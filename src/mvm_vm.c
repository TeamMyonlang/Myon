/*
 * Copyright 2026 TeamMyonlang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Step 6: the MVM bytecode execution engine (docs/mvm_spec.md §1-§4, §8).
 *
 * See src/mvm_vm.h for the high-level design.  The VM never re-implements a
 * single built-in: myon.print / myon.math.* / array & map methods / casts /
 * arithmetic all flow through the myon_bridge_* functions in interpreter.c so
 * `.myon` (tree-walk) and `.myc` (VM) execution are behaviourally identical
 * (spec §2, §4.14).
 */

#include "mvm_vm.h"

#include "value.h"
#include "types.h"
#include "common.h"
#include "diag.h"
#include "interpreter.h"
#include "mvm_bytecode.h"

#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>

/* --- limits (spec §3.4) --------------------------------------------------- */

/*
 * Call-frame cap (spec §3.4 suggests ~1024).  The tree-walk interpreter caps
 * native C recursion at MYON_MAX_CALL_DEPTH (4000); the VM keeps its own
 * (heap) frame stack so it cannot blow the C stack, but we still bound it so a
 * runaway recursion turns into a clean Myon error rather than eating memory
 * until the OS kills the process (spec §3.4, task item 6).
 */
#define MVM_MAX_FRAMES 1024
/*
 * Operand-stack cap.  Chosen generously relative to the frame cap so ordinary
 * programs never hit it; it exists to bound pathological bytecode / very deep
 * recursion (each frame reserves num_locals slots plus its temporaries).
 */
#define MVM_STACK_MAX  (256 * 1024)

/* --- VM state ------------------------------------------------------------- */

typedef struct {
    Chunk    *chunk;   /* chunk currently executing in this frame */
    uint8_t  *ip;      /* instruction pointer into chunk->code */
    int       base;    /* operand-stack index of this frame's slot 0 */
    int       fn_slot; /* stack index of the callee fn value (base-1), or -1 */
    /*
     * Closures (spec §7.3).  `upvalues` are the boxed cells this frame's
     * function captured (borrowed from the FuncData that was called; NULL for
     * <main>/methods).  `cells` holds, for each captured *local* slot, the
     * shared UpvalueCell that backs it: when chunk->captured[slot] is set,
     * cells[slot] owns the slot's value and the operand-stack slot is unused.
     * `cells` is NULL when the chunk captures nothing.
     */
    UpvalueCell **upvalues;   /* borrowed from the closure FuncData */
    int           upvalue_count;
    UpvalueCell **cells;      /* owned array [num_locals], entries owned */
    /*
     * The <main> chunk's locals *are* the program globals (spec §4.6): within
     * <main> a top-level var is a LOAD_LOCAL, but a nested function sees the
     * same storage as a LOAD_GLOBAL.  For both to hit the same cell, the entry
     * frame stores/loads its locals in VMShared.globals rather than on the
     * operand stack.  `globals_frame` marks that frame.
     */
    int           globals_frame;
} Frame;

/*
 * Process-wide VM state shared by the main execution and every async task
 * (spec §14.9).  Async tasks each get their own operand/frame stack (a VM),
 * but they must observe the same top-level globals and the same stdlib bridge
 * (event loop, PRNG, net/ffi state), so those live here and are pointed at by
 * every VM.  Globals are a heap array (not frame 0) precisely so a suspended
 * task's stack does not shadow them.
 */
typedef struct {
    Module  *module;
    Program *program;     /* for struct/method decls (may be NULL) */
    Interp  *bridge;      /* keeps stdlib state alive across native calls */

    Value   *globals;     /* top-level (<main>) variable slots (spec §4.6) */
    int      global_count;
} VMShared;

typedef struct {
    VMShared *sh;

    Value   *stack;
    int      sp;          /* next free operand-stack slot */
    int      stack_cap;

    Frame    frames[MVM_MAX_FRAMES];
    int      frame_count;

    jmp_buf  on_error;    /* runtime-error unwind target */
    int      cur_line;    /* line of the instruction under execution (diag) */
    /*
     * When the outermost frame returns (frame_count 1->0), the <main> entry
     * chunk discards its return value.  An async task body, however, runs its
     * function chunk *as* the outermost frame and needs that value delivered to
     * async_vm_body.  This flag tells RET to leave the return value(s) on the
     * operand stack instead of discarding them (spec §14.9).
     */
    int      keep_top_return;
} VM;

/* Convenience accessors so most code keeps reading VM_MODULE(vm) etc. */
#define VM_MODULE(vm)  ((vm)->sh->module)
#define VM_PROGRAM(vm) ((vm)->sh->program)
#define VM_BRIDGE(vm)  ((vm)->sh->bridge)

/* --- struct-decl registry (spec §4.11) ------------------------------------ */
/*
 * NEW_STRUCT / INVOKE need field order and method bodies.  Those live in the
 * AST (StructDecl), not in the .myc, so they are only available when the VM is
 * handed the source Program.  We build a flat name->StructDecl lookup here and
 * resolve parent_name links exactly like the compiler / tree-walker do.
 */
static StructDecl *find_struct_decl(VM *vm, const char *name) {
    if (!VM_PROGRAM(vm)) return NULL;
    for (int i = 0; i < VM_PROGRAM(vm)->stmts.count; i++) {
        Stmt *s = VM_PROGRAM(vm)->stmts.items[i];
        if (s->kind == STMT_STRUCT && s->as.struct_decl &&
            strcmp(s->as.struct_decl->name, name) == 0)
            return s->as.struct_decl;
    }
    return NULL;
}

static void resolve_struct_parents(VM *vm, StructDecl *sd) {
    for (StructDecl *cur = sd; cur; cur = cur->parent) {
        if (cur->parent_name && !cur->parent)
            cur->parent = find_struct_decl(vm, cur->parent_name);
    }
}

/* Find the "<Struct>.<method>" chunk index, honouring inheritance (nearest
 * definition wins), mirroring the tree-walker's method search. Returns -1. */
static int find_method_chunk(VM *vm, StructDecl *sd, const char *method) {
    for (StructDecl *cur = sd; cur; cur = cur->parent) {
        char key[256];
        snprintf(key, sizeof(key), "%s.%s", cur->name, method);
        for (int i = 0; i < VM_MODULE(vm)->chunk_count; i++) {
            Chunk *c = VM_MODULE(vm)->chunks[i];
            if (c->name && strcmp(c->name, key) == 0) return i;
        }
    }
    return -1;
}

/* --- error handling (spec §8) --------------------------------------------- */

/*
 * Raise a Myon runtime error in the tree-walker's exact format (line + source
 * snippet), then unwind to mvm_run_module's barrier.  We print here (so the VM
 * controls the line/snippet) rather than going through the bridge, but the
 * message wording matches the interpreter for shared cases.
 */
static void vm_error(VM *vm, int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "runtime error");
    if (line > 0) fprintf(stderr, " (line %d)", line);
    fprintf(stderr, ": ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    diag_print_snippet(line, 1);
    longjmp(vm->on_error, 1);
}

/* --- operand stack -------------------------------------------------------- */

static void vm_push(VM *vm, Value v) {
    if (vm->sp >= vm->stack_cap) {
        if (vm->stack_cap >= MVM_STACK_MAX) {
            /* free the value we could not store, then bail */
            value_free(&v);
            vm_error(vm, vm->cur_line,
                     "stack overflow (operand stack exceeded %d slots; "
                     "recursion too deep?)", MVM_STACK_MAX);
        }
        int ncap = vm->stack_cap ? vm->stack_cap * 2 : 256;
        if (ncap > MVM_STACK_MAX) ncap = MVM_STACK_MAX;
        vm->stack = (Value *)myon_xrealloc(vm->stack, sizeof(Value) * (size_t)ncap);
        vm->stack_cap = ncap;
    }
    vm->stack[vm->sp++] = v;
}

static Value vm_pop(VM *vm) {
    /* Bytecode from our own compiler is balanced; guard anyway. */
    if (vm->sp <= 0) vm_error(vm, vm->cur_line, "internal: operand stack underflow");
    return vm->stack[--vm->sp];
}

static Value *vm_peek(VM *vm, int back) {
    return &vm->stack[vm->sp - 1 - back];
}

/* --- bytecode readers ----------------------------------------------------- */

static uint8_t read_u8(Frame *f) { return *f->ip++; }

static uint16_t read_u16(Frame *f) {
    uint16_t lo = *f->ip++;
    uint16_t hi = *f->ip++;
    return (uint16_t)(lo | (hi << 8));
}

static int16_t read_s16(Frame *f) { return (int16_t)read_u16(f); }

/* Map the current chunk offset to a source line via the chunk's line table
 * (spec §6.4).  Returns 0 when no mapping is available. */
static int line_for(Chunk *c, size_t off) {
    int best = 0;
    for (uint32_t i = 0; i < c->line_count; i++) {
        if (c->lines[i].code_off <= off) best = (int)c->lines[i].line;
        else break;
    }
    return best;
}

/* --- VM function values (spec §2, §4.8, §7.3) ----------------------------- */
/*
 * An MVM closure is an OBJ_FUNC whose `mvm_chunk` field holds (chunk index + 1)
 * so 0 means "not an MVM closure" (a tree-walk fn), and whose `upvalues` array
 * holds the boxed cells it captured from enclosing frames (spec §7.3).
 * value_copy/value_free refcount the object and (in value.c) ref/unref each
 * captured cell, so sharing and lifetime Just Work.
 */
static Value vm_make_closure(int chunk_idx, int is_async,
                             UpvalueCell **ups, int nups) {
    Value v = value_func(NULL, NULL);
    FuncData *fn = &v.as.obj->as.fn;
    fn->mvm_chunk    = chunk_idx + 1;
    fn->mvm_is_async = is_async ? 1 : 0;
    fn->upvalue_count = nups;
    if (nups > 0) {
        fn->upvalues = (UpvalueCell **)myon_xmalloc(sizeof(UpvalueCell *) * (size_t)nups);
        for (int i = 0; i < nups; i++) fn->upvalues[i] = upvalue_cell_ref(ups[i]);
    } else {
        fn->upvalues = NULL;
    }
    return v;
}

/* Is `v` an MVM (bytecode) function value?  (vs. a tree-walk FuncDecl fn) */
static int vm_is_mvm_fn(const Value *v) {
    return v->type == TYPE_FUNC && v->as.obj->as.fn.mvm_chunk != 0;
}

static int vm_closure_chunk(const Value *v) {
    return v->as.obj->as.fn.mvm_chunk - 1;
}

/* --- native dispatch (spec §4.14) ----------------------------------------- */

static void do_call_native(VM *vm, int line, const char *name, int argc) {
    Value *args = &vm->stack[vm->sp - argc];
    /* An MVM closure encodes a chunk index in as.fn.mvm_chunk, not a real
     * FuncDecl*, so a tree-walk native that would call it back (only
     * myon.ffi.make_callback does) cannot execute it and would misbehave.
     * Refuse cleanly and halt here rather than letting the program proceed
     * with a bogus callback pointer (which crashes the foreign call). Passing
     * a VM function/lambda into a native is an MVM cross-engine limitation
     * (spec §7); run the .myon source instead. */
    for (int i = 0; i < argc; i++) {
        if (vm_is_mvm_fn(&args[i]))
            vm_error(vm, line,
                "MVM does not support passing a function/lambda to the native "
                "'%s' (cross-engine callback; run the .myon source instead)", name);
    }
    Value out = value_nil();
    int handled = myon_bridge_call_native(VM_BRIDGE(vm), name, args, argc, line, &out);
    if (!handled) {
        vm_error(vm, line, "unknown native function '%s'", name);
    }
    for (int i = 0; i < argc; i++) value_free(&vm->stack[vm->sp - 1 - i]);
    vm->sp -= argc;
    vm_push(vm, out);
}

/* --- call setup (spec §4.9) ----------------------------------------------- */

/*
 * Set up `f->cells` for a freshly-pushed frame: if the chunk has captured
 * slots (spec §7.3), box each captured slot's current value into a shared
 * UpvalueCell and clear the operand-stack slot.  Thereafter LOAD_LOCAL /
 * STORE_LOCAL on a captured slot go through the cell (see the dispatch loop),
 * so a nested closure that captures the slot and the frame share one cell.
 */
static void frame_open_cells(VM *vm, Frame *f) {
    Chunk *c = f->chunk;
    f->cells = NULL;
    if (!c->captured || c->captured_len == 0) return;
    f->cells = (UpvalueCell **)myon_xmalloc(sizeof(UpvalueCell *) * (size_t)c->num_locals);
    for (int i = 0; i < (int)c->num_locals; i++) {
        if (i < (int)c->captured_len && c->captured[i]) {
            /* move the slot's value into a fresh cell */
            f->cells[i] = upvalue_cell_new(vm->stack[f->base + i]);
            vm->stack[f->base + i] = value_nil();
        } else {
            f->cells[i] = NULL;
        }
    }
}

/* Tear down a frame's owned cells (drop our ref; the closure keeps its own). */
static void frame_close_cells(Frame *f) {
    if (!f->cells) return;
    for (int i = 0; i < (int)f->chunk->num_locals; i++)
        if (f->cells[i]) upvalue_cell_unref(f->cells[i]);
    free(f->cells);
    f->cells = NULL;
}

/* Push a call frame for `callee` (an MVM fn value; may be NULL for methods). */
static void push_frame(VM *vm, const Value *callee, int chunk_idx, int argc,
                       int fn_slot, int line) {
    Chunk *c = module_chunk(VM_MODULE(vm), chunk_idx);
    if (!c) vm_error(vm, line, "internal: bad chunk index %d", chunk_idx);
    if ((int)c->num_params != argc)
        vm_error(vm, line, "function '%s' expects %d argument%s but got %d",
                 c->name ? c->name : "<fn>", c->num_params,
                 c->num_params == 1 ? "" : "s", argc);
    if (vm->frame_count >= MVM_MAX_FRAMES)
        vm_error(vm, line, "stack overflow (call depth exceeded %d; "
                 "recursion too deep?)", MVM_MAX_FRAMES);

    int base = vm->sp - argc;   /* first arg becomes slot 0 */
    /* Reserve the remaining local slots (params already occupy 0..argc-1). */
    for (int i = argc; i < (int)c->num_locals; i++) vm_push(vm, value_nil());

    Frame *f = &vm->frames[vm->frame_count++];
    f->chunk   = c;
    f->ip      = c->code;
    f->base    = base;
    f->fn_slot = fn_slot;
    f->upvalues = NULL;
    f->upvalue_count = 0;
    if (callee && callee->type == TYPE_FUNC) {
        f->upvalues      = callee->as.obj->as.fn.upvalues;
        f->upvalue_count = callee->as.obj->as.fn.upvalue_count;
    }
    frame_open_cells(vm, f);
}

/* --- the dispatch loop ---------------------------------------------------- */

/* --- async tasks (spec §14.9) --------------------------------------------- */
static int run(VM *vm);   /* forward: the dispatch loop, reused by task bodies */

/*
 * Per-async-task VM invocation.  A spawned async task runs its function chunk
 * on its OWN operand/frame stack (so concurrently-suspended tasks never share
 * stack slots) while pointing at the same VMShared (globals + stdlib bridge).
 * Its C stack is the event-loop coroutine's, so a suspend at sleep/I-O parks
 * the whole nested run() intact.
 */
typedef struct {
    VMShared *sh;
    int       chunk_idx;
    Value     fn;          /* owned closure value (for its upvalues) */
    Value    *args;        /* owned */
    int       argc;
} AsyncVMCtx;

static void async_vm_body(void *ud, Value *out_result, int *out_has_error) {
    AsyncVMCtx *ctx = (AsyncVMCtx *)ud;
    VM tvm;
    memset(&tvm, 0, sizeof(tvm));
    tvm.sh = ctx->sh;
    tvm.keep_top_return = 1;   /* deliver the task's return value (spec §14.9) */

    /* Build the task's frame: push args as slots 0..argc-1, then locals. */
    Chunk *c = module_chunk(ctx->sh->module, ctx->chunk_idx);
    Value result = value_nil();
    int has_error = 0;

    if (setjmp(tvm.on_error) == 0) {
        for (int i = 0; i < ctx->argc; i++) vm_push(&tvm, value_copy(&ctx->args[i]));
        for (int i = ctx->argc; i < (int)c->num_locals; i++) vm_push(&tvm, value_nil());
        Frame *f = &tvm.frames[tvm.frame_count++];
        f->chunk = c; f->ip = c->code; f->base = 0; f->fn_slot = -1;
        f->upvalues = ctx->fn.as.obj->as.fn.upvalues;
        f->upvalue_count = ctx->fn.as.obj->as.fn.upvalue_count;
        f->globals_frame = 0;
        frame_open_cells(&tvm, f);
        run(&tvm);
        /* run() leaves the single return value (RET normalizes 0->nil) on top */
        if (tvm.sp > 0) result = tvm.stack[--tvm.sp];
    } else {
        has_error = 1;
        result = value_error(myon_strdup("async task failed"));
    }

    /* drain any stragglers on the task stack */
    while (tvm.sp > 0) { Value d = tvm.stack[--tvm.sp]; value_free(&d); }
    free(tvm.stack);

    value_free(&ctx->fn);
    for (int i = 0; i < ctx->argc; i++) value_free(&ctx->args[i]);
    free(ctx->args);
    /* Deliver the outcome via the task-local out-params (see bridge header):
     * out_result already holds a value_nil() we may overwrite. */
    value_free(out_result);
    *out_result = result;         /* ownership transfers to the task */
    *out_has_error = has_error;
    free(ctx);
}

/*
 * MOP_CALL of an async function value: pop fn + argc args off the *current*
 * stack, hand them to a freshly spawned task, and push the Task handle.  The
 * task does not run yet (cooperative: it runs when the loop next schedules it).
 */
static void do_spawn_async(VM *vm, int line, int argc) {
    (void)line;
    Value *callee = vm_peek(vm, argc);
    int cidx = vm_closure_chunk(callee);

    AsyncVMCtx *ctx = (AsyncVMCtx *)myon_xmalloc(sizeof(AsyncVMCtx));
    ctx->sh = vm->sh;
    ctx->chunk_idx = cidx;
    ctx->fn = value_copy(callee);
    ctx->argc = argc;
    ctx->args = argc ? (Value *)myon_xmalloc(sizeof(Value) * (size_t)argc) : NULL;
    /* args sit above the callee: [callee][arg0..arg(argc-1)] */
    for (int i = 0; i < argc; i++) ctx->args[i] = value_copy(&vm->stack[vm->sp - argc + i]);

    /* pop args + callee off the operand stack */
    for (int i = 0; i < argc; i++) { Value d = vm_pop(vm); value_free(&d); }
    { Value d = vm_pop(vm); value_free(&d); }

    Value task = myon_bridge_spawn_task(vm->sh->bridge, async_vm_body, ctx);
    vm_push(vm, task);
}

static int run(VM *vm) {
    Frame *f = &vm->frames[vm->frame_count - 1];

    for (;;) {
        size_t off = (size_t)(f->ip - f->chunk->code);
        int line = line_for(f->chunk, off);
        vm->cur_line = line;
        uint8_t op = read_u8(f);

        switch (op) {
        case MOP_NOP:
            break;

        case MOP_PUSH_CONST: {
            uint16_t idx = read_u16(f);
            if (idx >= (uint16_t)VM_MODULE(vm)->const_count)
                vm_error(vm, line, "internal: const index %u out of range", idx);
            vm_push(vm, value_copy(&VM_MODULE(vm)->consts[idx].value));
            break;
        }
        case MOP_PUSH_TRUE:  vm_push(vm, value_bool(1)); break;
        case MOP_PUSH_FALSE: vm_push(vm, value_bool(0)); break;
        case MOP_PUSH_NIL:   vm_push(vm, value_nil());   break;
        case MOP_POP: { Value v = vm_pop(vm); value_free(&v); break; }
        case MOP_DUP: { Value *t = vm_peek(vm, 0); vm_push(vm, value_copy(t)); break; }

        /* arithmetic / comparison via the shared tree-walk semantics */
        case MOP_ADD: case MOP_SUB: case MOP_MUL: case MOP_DIV:
        case MOP_EQ:  case MOP_NEQ: case MOP_LT:  case MOP_GT:
        case MOP_LE:  case MOP_GE: {
            static const int to_opkind[] = {
                [MOP_ADD]=OP_ADD, [MOP_SUB]=OP_SUB, [MOP_MUL]=OP_MUL, [MOP_DIV]=OP_DIV,
                [MOP_EQ]=OP_EQ,   [MOP_NEQ]=OP_NEQ, [MOP_LT]=OP_LT,  [MOP_GT]=OP_GT,
                [MOP_LE]=OP_LE,   [MOP_GE]=OP_GE,
            };
            Value r = vm_pop(vm);
            Value l = vm_pop(vm);
            /* myon_bridge_binary consumes l and r and may longjmp on error */
            vm_push(vm, myon_bridge_binary(VM_BRIDGE(vm), line, to_opkind[op], l, r));
            break;
        }
        case MOP_NEG: { Value v = vm_pop(vm); vm_push(vm, myon_bridge_neg(VM_BRIDGE(vm), line, v)); break; }
        case MOP_NOT: { Value v = vm_pop(vm); vm_push(vm, myon_bridge_not(VM_BRIDGE(vm), line, v)); break; }

        /* local variables (frame-relative).  A captured slot lives in a boxed
         * cell (spec §7.3) so closures share it; read/write through the cell. */
        case MOP_LOAD_LOCAL: {
            uint16_t slot = read_u16(f);
            if (f->cells && f->cells[slot])
                vm_push(vm, value_copy(&f->cells[slot]->value));
            else if (f->globals_frame)
                vm_push(vm, value_copy(&vm->sh->globals[slot]));
            else
                vm_push(vm, value_copy(&vm->stack[f->base + slot]));
            break;
        }
        case MOP_STORE_LOCAL: {
            uint16_t slot = read_u16(f);
            Value v = vm_pop(vm);
            if (f->cells && f->cells[slot]) {
                value_free(&f->cells[slot]->value);
                f->cells[slot]->value = v;
            } else if (f->globals_frame) {
                value_free(&vm->sh->globals[slot]);
                vm->sh->globals[slot] = v;
            } else {
                value_free(&vm->stack[f->base + slot]);
                vm->stack[f->base + slot] = v;
            }
            break;
        }
        /* upvalues: captured cells threaded in via the closure (spec §7.3) */
        case MOP_LOAD_UPVALUE: {
            uint16_t idx = read_u16(f);
            if (idx >= (uint16_t)f->upvalue_count || !f->upvalues || !f->upvalues[idx])
                vm_error(vm, line, "internal: bad upvalue index %u", idx);
            vm_push(vm, value_copy(&f->upvalues[idx]->value));
            break;
        }
        case MOP_STORE_UPVALUE: {
            uint16_t idx = read_u16(f);
            if (idx >= (uint16_t)f->upvalue_count || !f->upvalues || !f->upvalues[idx])
                vm_error(vm, line, "internal: bad upvalue index %u", idx);
            Value v = vm_pop(vm);
            value_free(&f->upvalues[idx]->value);
            f->upvalues[idx]->value = v;
            break;
        }
        /* globals: shared top-level (<main>) slots (spec §4.6 note).  Held in a
         * heap array (VMShared.globals), not frame 0, so a suspended async
         * task's own stack cannot shadow them (spec §14.9). */
        case MOP_LOAD_GLOBAL: {
            uint16_t slot = read_u16(f);
            if (slot >= (uint16_t)vm->sh->global_count)
                vm_error(vm, line, "internal: global slot %u out of range", slot);
            vm_push(vm, value_copy(&vm->sh->globals[slot]));
            break;
        }
        case MOP_STORE_GLOBAL: {
            uint16_t slot = read_u16(f);
            if (slot >= (uint16_t)vm->sh->global_count)
                vm_error(vm, line, "internal: global slot %u out of range", slot);
            Value v = vm_pop(vm);
            value_free(&vm->sh->globals[slot]);
            vm->sh->globals[slot] = v;
            break;
        }

        /* branches (offset base = address right after the operand, spec §4.7) */
        case MOP_JUMP: { int16_t o = read_s16(f); f->ip += o; break; }
        case MOP_JUMP_IF_FALSE: {
            int16_t o = read_s16(f);
            Value v = vm_pop(vm);
            int t = value_truthy(&v);
            value_free(&v);
            if (!t) f->ip += o;
            break;
        }
        case MOP_JUMP_IF_TRUE: {
            int16_t o = read_s16(f);
            Value v = vm_pop(vm);
            int t = value_truthy(&v);
            value_free(&v);
            if (t) f->ip += o;
            break;
        }

        /* functions */
        case MOP_MAKE_CLOSURE: {
            uint16_t cidx = read_u16(f);
            uint8_t nupval = read_u8(f);
            Chunk *tc = module_chunk(VM_MODULE(vm), cidx);
            /* Resolve each capture against the *current* frame (spec §7.3):
             * kind 1 = box/borrow this frame's local cell, kind 0 = re-share
             * one of this frame's own upvalue cells. */
            UpvalueCell *ups[256];
            for (int i = 0; i < (int)nupval; i++) {
                uint8_t kind = read_u8(f);
                uint16_t index = read_u16(f);
                if (kind == 1) {
                    if (!f->cells || !f->cells[index])
                        vm_error(vm, line,
                            "internal: captured local slot %u is not boxed", index);
                    ups[i] = f->cells[index];
                } else {
                    if (index >= (uint16_t)f->upvalue_count || !f->upvalues)
                        vm_error(vm, line, "internal: bad re-captured upvalue %u", index);
                    ups[i] = f->upvalues[index];
                }
            }
            vm_push(vm, vm_make_closure(cidx, tc ? tc->is_async : 0, ups, (int)nupval));
            break;
        }
        case MOP_CALL: {
            uint8_t argc = read_u8(f);
            Value *callee = vm_peek(vm, argc);   /* fn sits below the args */
            if (!vm_is_mvm_fn(callee))
                vm_error(vm, line, "cannot call a %s value", value_type_name(callee));
            int cidx = vm_closure_chunk(callee);
            /*
             * Calling an `myon.async` function does not run it: it spawns a
             * cooperative task and yields a Task value immediately (spec
             * §14.9), exactly like the tree-walker.  Async-ness is a property
             * of the function value (its chunk's is_async flag), so first-class
             * async fn values behave the same as a bare named call.
             */
            if (callee->as.obj->as.fn.mvm_is_async) {
                do_spawn_async(vm, line, argc);
                break;
            }
            int fn_slot = vm->sp - argc - 1;
            push_frame(vm, callee, cidx, argc, fn_slot, line);
            f = &vm->frames[vm->frame_count - 1];
            break;
        }
        case MOP_RET: {
            uint8_t n = read_u8(f);
            /* return values are the top n operands (n is a u8, so <= 255) */
            Value rets[256];
            for (int i = (int)n - 1; i >= 0; i--) rets[i] = vm_pop(vm);
            /* Step 7-b fix: a call expression must always yield at least one
             * value, because a call used as a statement is compiled as
             * `<call> ; POP` (mvm_compiler.c STMT_EXPR).  A `ret void` /
             * implicit return emits `RET 0`, which previously left nothing on
             * the stack, so the trailing POP consumed an unrelated slot (e.g. a
             * global), corrupting the frame.  Normalize a 0-value return of a
             * *called* function to a single nil; the entry <main> chunk (handled
             * below when frame_count hits 0) still discards it.  Multi-value
             * returns (n >= 1, used by `a, b = f()`) are unchanged. */
            if (n == 0) { rets[0] = value_nil(); n = 1; }
            /* tear down: drop boxed cells (spec §7.3), free locals, then the
             * callee fn value */
            frame_close_cells(f);
            while (vm->sp > f->base) { Value d = vm_pop(vm); value_free(&d); }
            if (f->fn_slot >= 0) {
                /* pop the callee fn value that the caller pushed at base-1 */
                Value d = vm_pop(vm);
                value_free(&d);
            }
            vm->frame_count--;
            for (int i = 0; i < n; i++) vm_push(vm, rets[i]);
            if (vm->frame_count == 0) {
                /* Outermost frame returned.  For the <main> entry chunk the
                 * return value is discarded; for an async task body it must be
                 * left on the stack so async_vm_body can hand it to the awaiter
                 * (spec §14.9).  RET normalized n>=1 above, so the task result
                 * is the single top value; drop any extra straggler returns. */
                if (vm->keep_top_return) {
                    for (int i = 0; i < n - 1; i++) { Value d = vm_pop(vm); value_free(&d); }
                } else {
                    for (int i = 0; i < n; i++) { Value d = vm_pop(vm); value_free(&d); }
                }
                return 0;
            }
            f = &vm->frames[vm->frame_count - 1];
            break;
        }

        /* arrays / maps */
        case MOP_NEW_ARRAY: {
            uint16_t tidx = read_u16(f);
            TypeSpec *ts = VM_MODULE(vm)->consts[tidx].typespec;
            vm_push(vm, value_array(ts ? typespec_clone(ts) : NULL));
            break;
        }
        case MOP_NEW_MAP: {
            uint16_t tidx = read_u16(f);
            TypeSpec *ms = VM_MODULE(vm)->consts[tidx].typespec;
            TypeSpec *kt = (ms && ms->key)  ? typespec_clone(ms->key)  : NULL;
            TypeSpec *vt = (ms && ms->elem) ? typespec_clone(ms->elem) : NULL;
            vm_push(vm, value_map(kt, vt));
            break;
        }
        case MOP_ARRAY_PUSH: {
            Value v = vm_pop(vm);
            Value *arr = vm_peek(vm, 0);          /* array stays on the stack */
            if (arr->type != TYPE_ARRAY) { value_free(&v); vm_error(vm, line, "ARRAY_PUSH on non-array"); }
            array_push(arr, v);
            break;
        }
        case MOP_INDEX_GET: {
            Value idx = vm_pop(vm);
            Value cont = vm_pop(vm);
            if (cont.type == TYPE_ARRAY) {
                if (idx.type != TYPE_INT) { value_free(&cont); value_free(&idx); vm_error(vm, line, "array index must be int"); }
                ArrayData *a = &cont.as.obj->as.arr;
                long long i = idx.as.i;
                if (i < 0 || i >= a->count) {
                    long long ii = i; int n = a->count;
                    value_free(&cont); value_free(&idx);
                    vm_error(vm, line, "array index %lld out of bounds (length %d)", ii, n);
                }
                Value r = value_copy(&a->items[i]);
                value_free(&cont); value_free(&idx);
                vm_push(vm, r);
            } else if (cont.type == TYPE_MAP) {
                Value out;
                int ok = map_get(&cont, &idx, &out);
                value_free(&cont); value_free(&idx);
                vm_push(vm, ok ? out : value_nil());
            } else {
                value_free(&cont); value_free(&idx);
                vm_error(vm, line, "cannot index type");
            }
            break;
        }
        case MOP_INDEX_SET: {
            Value v = vm_pop(vm);
            Value idx = vm_pop(vm);
            Value cont = vm_pop(vm);
            if (cont.type == TYPE_ARRAY) {
                if (idx.type != TYPE_INT) { value_free(&cont); value_free(&idx); value_free(&v); vm_error(vm, line, "array index must be int"); }
                ArrayData *a = &cont.as.obj->as.arr;
                long long i = idx.as.i;
                if (i < 0 || i >= a->count) { value_free(&cont); value_free(&idx); value_free(&v); vm_error(vm, line, "array index out of bounds"); }
                value_free(&a->items[i]);
                a->items[i] = v;
            } else if (cont.type == TYPE_MAP) {
                map_set(&cont, idx, v);   /* map_set takes ownership of key+val */
                idx = value_nil();        /* consumed */
            } else {
                value_free(&cont); value_free(&idx); value_free(&v);
                vm_error(vm, line, "cannot index-assign type");
            }
            value_free(&cont); value_free(&idx);
            break;
        }

        /* structs / members / methods (spec §4.11) */
        case MOP_NEW_STRUCT: {
            uint16_t nidx = read_u16(f);
            const char *sname = VM_MODULE(vm)->consts[nidx].value.as.obj->as.str;
            StructDecl *sd = find_struct_decl(vm, sname);
            if (!sd)
                vm_error(vm, line,
                    "struct '%s' is not available to the VM "
                    "(struct definitions are not stored in .myc; run the .myon "
                    "source instead)", sname);
            resolve_struct_parents(vm, sd);
            /*
             * Collect fields parent-first (matches the compiler's push order,
             * spec §4.11 / note 4): gather the inheritance chain leaf..root,
             * then emit root..leaf.
             */
            StructField *fields = NULL; int fc = 0, cap = 0;
            {
                StructDecl *chain[64]; int depth = 0;
                for (StructDecl *cur = sd; cur && depth < 64; cur = cur->parent)
                    chain[depth++] = cur;
                for (int d = depth - 1; d >= 0; d--) {
                    StructDecl *cur = chain[d];
                    for (int i = 0; i < cur->field_count; i++) {
                        if (fc == cap) { cap = cap ? cap * 2 : 8; fields = (StructField *)myon_xrealloc(fields, sizeof(StructField) * cap); }
                        fields[fc++] = cur->fields[i];
                    }
                }
            }
            /* field values are on the stack in collected order */
            Value sv = value_struct(sd->name, sd);
            for (int i = 0; i < fc; i++) {
                Value fv = vm->stack[vm->sp - fc + i];
                struct_add_field(&sv, fields[i].name, fv);
            }
            vm->sp -= fc;
            free(fields);
            vm_push(vm, sv);
            break;
        }
        case MOP_GET_FIELD: {
            uint16_t nidx = read_u16(f);
            const char *fname = VM_MODULE(vm)->consts[nidx].value.as.obj->as.str;
            Value st = vm_pop(vm);
            if (st.type != TYPE_STRUCT) { value_free(&st); vm_error(vm, line, "member access on non-struct value"); }
            Value *fp = struct_field_ptr(&st, fname);
            if (!fp) { const char *tn = st.as.obj->as.st.type_name; char nm[128]; snprintf(nm,sizeof(nm),"%s",tn); value_free(&st); vm_error(vm, line, "struct '%s' has no field '%s'", nm, fname); }
            Value r = value_copy(fp);
            value_free(&st);
            vm_push(vm, r);
            break;
        }
        case MOP_SET_FIELD: {
            uint16_t nidx = read_u16(f);
            const char *fname = VM_MODULE(vm)->consts[nidx].value.as.obj->as.str;
            Value v = vm_pop(vm);
            Value st = vm_pop(vm);
            if (st.type != TYPE_STRUCT) { value_free(&st); value_free(&v); vm_error(vm, line, "cannot set field on non-struct"); }
            Value *fp = struct_field_ptr(&st, fname);
            if (!fp) { const char *tn = st.as.obj->as.st.type_name; char nm[128]; snprintf(nm,sizeof(nm),"%s",tn); value_free(&st); value_free(&v); vm_error(vm, line, "struct '%s' has no field '%s'", nm, fname); }
            value_free(fp);
            *fp = v;                 /* struct Obj is shared: mutation visible */
            value_free(&st);
            break;
        }
        case MOP_INVOKE: {
            uint16_t nidx = read_u16(f);
            uint8_t argc = read_u8(f);
            const char *method = VM_MODULE(vm)->consts[nidx].value.as.obj->as.str;
            Value *recv = vm_peek(vm, argc);   /* receiver below the args */
            if (recv->type == TYPE_STRUCT) {
                /* static dispatch to the Struct.method chunk (self at slot 0) */
                StructDecl *sd = recv->as.obj->as.st.decl;
                if (!sd) sd = find_struct_decl(vm, recv->as.obj->as.st.type_name);
                resolve_struct_parents(vm, sd);
                int cidx = find_method_chunk(vm, sd, method);
                if (cidx < 0)
                    vm_error(vm, line, "struct '%s' has no method '%s'",
                             recv->as.obj->as.st.type_name, method);
                /* the receiver already occupies the self (slot 0) position;
                 * treat it as fn_slot-less: base = recv position. */
                int base = vm->sp - argc - 1;
                Chunk *mc = module_chunk(VM_MODULE(vm), cidx);
                if ((int)mc->num_params != argc + 1)
                    vm_error(vm, line, "method '%s' expects %d argument%s but got %d",
                             method, mc->num_params - 1,
                             (mc->num_params - 1) == 1 ? "" : "s", argc);
                if (vm->frame_count >= MVM_MAX_FRAMES)
                    vm_error(vm, line, "stack overflow (call depth exceeded %d)", MVM_MAX_FRAMES);
                for (int i = (int)mc->num_params; i < (int)mc->num_locals; i++) vm_push(vm, value_nil());
                Frame *nf = &vm->frames[vm->frame_count++];
                nf->chunk = mc; nf->ip = mc->code; nf->base = base; nf->fn_slot = -1;
                nf->upvalues = NULL; nf->upvalue_count = 0;
                frame_open_cells(vm, nf);   /* methods may capture in nested lambdas */
                f = &vm->frames[vm->frame_count - 1];
            } else {
                /* built-in method (array/map/str): reuse the tree-walker */
                Value *args = &vm->stack[vm->sp - argc];
                /* An MVM closure encodes its chunk index in as.fn.decl (see
                 * vm_make_closure), NOT a real FuncDecl*.  The tree-walk
                 * built-ins (.map/.filter/.reduce, ...) would dereference that
                 * as a pointer and crash.  Passing a VM function value into a
                 * native higher-order method is therefore unsupported for now;
                 * detect it and raise a clean error instead of segfaulting.
                 * (Documented Step 7-b limitation.) */
                for (int i = 0; i < argc; i++) {
                    if (args[i].type == TYPE_FUNC) {
                        vm_error(vm, line,
                            "MVM does not support passing a function/lambda to the "
                            "built-in method '%s' (higher-order native methods; "
                            "run the .myon source instead)", method);
                    }
                }
                Value recv_copy = value_copy(recv);
                Value out = value_nil();
                myon_bridge_call_method(VM_BRIDGE(vm), recv_copy, method, args, argc, line, &out);
                value_free(&recv_copy);
                for (int i = 0; i < argc; i++) value_free(&vm->stack[vm->sp - 1 - i]);
                vm->sp -= argc;
                Value r = vm_pop(vm);   /* pop the receiver */
                value_free(&r);
                vm_push(vm, out);
            }
            break;
        }

        /* string interpolation / casts */
        case MOP_STR_CONCAT: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            /* both are already str (compiler emits TO_STR as needed); reuse
             * the tree-walker's ADD-on-str semantics for identical behaviour */
            vm_push(vm, myon_bridge_binary(VM_BRIDGE(vm), line, OP_ADD, a, b));
            break;
        }
        case MOP_TO_STR:   { Value v = vm_pop(vm); vm_push(vm, myon_bridge_cast_str(VM_BRIDGE(vm), line, v)); break; }
        case MOP_CAST_STR: { Value v = vm_pop(vm); vm_push(vm, myon_bridge_cast_str(VM_BRIDGE(vm), line, v)); break; }
        case MOP_CAST_INT: { Value v = vm_pop(vm); vm_push(vm, myon_bridge_cast_int(VM_BRIDGE(vm), line, v)); break; }
        case MOP_CAST_CHAR:{ Value v = vm_pop(vm); vm_push(vm, myon_bridge_cast_char(VM_BRIDGE(vm), line, v)); break; }
        case MOP_MAKE_ERROR:{ Value v = vm_pop(vm); vm_push(vm, myon_bridge_make_error(VM_BRIDGE(vm), line, v)); break; }

        /* native / module calls */
        case MOP_CALL_NATIVE: {
            uint16_t nid = read_u16(f);
            uint8_t argc = read_u8(f);
            if (nid >= (uint16_t)VM_MODULE(vm)->native_count)
                vm_error(vm, line, "internal: native id %u out of range", nid);
            do_call_native(vm, line, VM_MODULE(vm)->natives[nid], argc);
            break;
        }

        /* async/await (spec §14.9): await drives/suspends the event loop until
         * the awaited Task resolves, then leaves its result on the stack.  A
         * non-task value passes straight through (myon.await of a plain value). */
        case MOP_AWAIT: {
            Value t = vm_pop(vm);
            vm_push(vm, myon_bridge_await(VM_BRIDGE(vm), line, t));
            break;
        }

        /* Spread a multi-return tuple across N assignment targets (spec §6.2).
         * Native stdlib calls represent a `(value, error)` multi-return as a
         * single untyped tuple array (interpreter.c make_result_pair), whereas
         * a user function's multi-`ret` already pushes N separate stack values.
         * The compiler emits MOP_UNPACK only after a native-call RHS in a
         * multiple-target assignment, so here we pop that one tuple and push
         * its elements, giving the following STORE_LOCALs the N values they
         * expect. */
        /* Reject `x = myon.nil` on a normal single-target variable (spec §2.4).
         * Peeks (does not consume) the value about to be stored. */
        case MOP_CHECK_NOT_NIL: {
            Value *top = vm_peek(vm, 0);
            if (top->type == TYPE_NIL)
                vm_error(vm, line,
                    "cannot assign myon.nil to a normal variable (spec 2.4)");
            break;
        }

        case MOP_UNPACK: {
            uint8_t n = read_u8(f);
            Value tup = vm_pop(vm);
            if (tup.type != TYPE_ARRAY || tup.as.obj->as.arr.elem_type != NULL) {
                value_free(&tup);
                vm_error(vm, line,
                    "multiple-target assignment requires a multi-value return (spec 6.2)");
            }
            ArrayData *a = &tup.as.obj->as.arr;
            if (a->count != (int)n) {
                int got = a->count; value_free(&tup);
                vm_error(vm, line,
                    "assignment target count (%d) does not match return count (%d)",
                    (int)n, got);
            }
            for (int i = 0; i < (int)n; i++) vm_push(vm, value_copy(&a->items[i]));
            value_free(&tup);
            break;
        }

        default:
            vm_error(vm, line, "internal: unknown opcode 0x%02X", op);
        }
    }
}

/* --- public entry --------------------------------------------------------- */

int mvm_run_module(Module *m, Program *program, const char *source) {
    if (!m || m->chunk_count == 0) {
        fprintf(stderr, "myon: empty module\n");
        return 70;
    }
    if (source) diag_set_source(source);

    VMShared sh;
    memset(&sh, 0, sizeof(sh));
    sh.module  = m;
    sh.program = program;
    sh.bridge  = myon_bridge_interp_new();

    int entry = m->entry_chunk;
    Chunk *ec = module_chunk(m, entry);
    if (!ec) { myon_bridge_interp_free(sh.bridge); fprintf(stderr, "myon: bad entry chunk\n"); return 70; }

    /* <main>'s locals are the program globals; hold them in a shared heap array
     * so async tasks (each with their own operand stack) observe the same set
     * (spec §4.6 note, §14.9). */
    sh.global_count = ec->num_locals;
    sh.globals = sh.global_count
        ? (Value *)myon_xmalloc(sizeof(Value) * (size_t)sh.global_count) : NULL;
    for (int i = 0; i < sh.global_count; i++) sh.globals[i] = value_nil();

    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.sh = &sh;

    int rc = 0;
    if (setjmp(vm.on_error) == 0) {
        /* Point the shared stdlib's runtime_error() at our barrier so any
         * error raised deep inside a native unwinds cleanly back to us. */
        jmp_buf *bridge_buf = (jmp_buf *)myon_bridge_error_buf(sh.bridge);
        if (setjmp(*bridge_buf) == 0) {
            /* entry frame: <main> keeps its locals in sh.globals (globals_frame
             * = 1), and has no callee fn value (fn_slot = -1). */
            Frame *f = &vm.frames[vm.frame_count++];
            f->chunk = ec; f->ip = ec->code; f->base = 0; f->fn_slot = -1;
            f->upvalues = NULL; f->upvalue_count = 0;
            f->cells = NULL; f->globals_frame = 1;
            rc = run(&vm);
            /*
             * Drain any tasks still pending after <main> returns so background
             * / not-yet-awaited async work runs to completion, matching the
             * tree-walker's program-exit drain (spec §14.9).
             */
            myon_bridge_drain_tasks(sh.bridge);
        } else {
            rc = 1;   /* native raised (already reported by tree-walk diag) */
        }
    } else {
        rc = 1;       /* VM raised (already reported by vm_error) */
    }

    /* clean up any values left on the operand stack + the globals */
    while (vm.sp > 0) { Value d = vm.stack[--vm.sp]; value_free(&d); }
    free(vm.stack);
    for (int i = 0; i < sh.global_count; i++) value_free(&sh.globals[i]);
    free(sh.globals);
    myon_bridge_interp_free(sh.bridge);
    if (source) diag_clear_source();
    return rc;
}
