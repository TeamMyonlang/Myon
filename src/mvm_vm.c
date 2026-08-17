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
} Frame;

typedef struct {
    Module  *module;
    Program *program;     /* for struct/method decls (may be NULL) */
    Interp  *bridge;      /* keeps stdlib state alive across native calls */

    Value   *stack;
    int      sp;          /* next free operand-stack slot */
    int      stack_cap;

    Frame    frames[MVM_MAX_FRAMES];
    int      frame_count;

    jmp_buf  on_error;    /* runtime-error unwind target */
    int      cur_line;    /* line of the instruction under execution (diag) */
} VM;

/* --- struct-decl registry (spec §4.11) ------------------------------------ */
/*
 * NEW_STRUCT / INVOKE need field order and method bodies.  Those live in the
 * AST (StructDecl), not in the .myc, so they are only available when the VM is
 * handed the source Program.  We build a flat name->StructDecl lookup here and
 * resolve parent_name links exactly like the compiler / tree-walker do.
 */
static StructDecl *find_struct_decl(VM *vm, const char *name) {
    if (!vm->program) return NULL;
    for (int i = 0; i < vm->program->stmts.count; i++) {
        Stmt *s = vm->program->stmts.items[i];
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
        for (int i = 0; i < vm->module->chunk_count; i++) {
            Chunk *c = vm->module->chunks[i];
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

/* --- VM function values (spec §2, §4.8) ----------------------------------- */
/*
 * An MVM closure is fully described by its chunk index (closures cannot capture
 * outer locals — spec §7.3).  We reuse OBJ_FUNC so value_copy/value_free's
 * refcounting Just Works, and stash the chunk index in the (unowned, never
 * dereferenced by value.c) `decl` pointer slot.  No per-instance heap state, so
 * nothing leaks.
 */
static Value vm_make_closure(int chunk_idx) {
    Value v = value_func(NULL, NULL);
    v.as.obj->as.fn.decl = (struct FuncDecl *)(intptr_t)chunk_idx;
    return v;
}

static int vm_closure_chunk(const Value *v) {
    return (int)(intptr_t)v->as.obj->as.fn.decl;
}

/* --- native dispatch (spec §4.14) ----------------------------------------- */

static void do_call_native(VM *vm, int line, const char *name, int argc) {
    Value *args = &vm->stack[vm->sp - argc];
    Value out = value_nil();
    int handled = myon_bridge_call_native(vm->bridge, name, args, argc, line, &out);
    if (!handled) {
        vm_error(vm, line, "unknown native function '%s'", name);
    }
    for (int i = 0; i < argc; i++) value_free(&vm->stack[vm->sp - 1 - i]);
    vm->sp -= argc;
    vm_push(vm, out);
}

/* --- call setup (spec §4.9) ----------------------------------------------- */

static void push_frame(VM *vm, int chunk_idx, int argc, int fn_slot, int line) {
    Chunk *c = module_chunk(vm->module, chunk_idx);
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
}

/* --- the dispatch loop ---------------------------------------------------- */

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
            if (idx >= (uint16_t)vm->module->const_count)
                vm_error(vm, line, "internal: const index %u out of range", idx);
            vm_push(vm, value_copy(&vm->module->consts[idx].value));
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
            vm_push(vm, myon_bridge_binary(vm->bridge, line, to_opkind[op], l, r));
            break;
        }
        case MOP_NEG: { Value v = vm_pop(vm); vm_push(vm, myon_bridge_neg(vm->bridge, line, v)); break; }
        case MOP_NOT: { Value v = vm_pop(vm); vm_push(vm, myon_bridge_not(vm->bridge, line, v)); break; }

        /* local variables (frame-relative) */
        case MOP_LOAD_LOCAL: {
            uint16_t slot = read_u16(f);
            vm_push(vm, value_copy(&vm->stack[f->base + slot]));
            break;
        }
        case MOP_STORE_LOCAL: {
            uint16_t slot = read_u16(f);
            Value v = vm_pop(vm);
            value_free(&vm->stack[f->base + slot]);
            vm->stack[f->base + slot] = v;
            break;
        }
        /* globals: absolute slots in the entry (<main>) frame (spec §4.6 note) */
        case MOP_LOAD_GLOBAL: {
            uint16_t slot = read_u16(f);
            int gbase = vm->frames[0].base;   /* entry frame base (== 0) */
            vm_push(vm, value_copy(&vm->stack[gbase + slot]));
            break;
        }
        case MOP_STORE_GLOBAL: {
            uint16_t slot = read_u16(f);
            int gbase = vm->frames[0].base;
            Value v = vm_pop(vm);
            value_free(&vm->stack[gbase + slot]);
            vm->stack[gbase + slot] = v;
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
            vm_push(vm, vm_make_closure(cidx));
            break;
        }
        case MOP_CALL: {
            uint8_t argc = read_u8(f);
            Value *callee = vm_peek(vm, argc);   /* fn sits below the args */
            if (callee->type != TYPE_FUNC)
                vm_error(vm, line, "cannot call a %s value", value_type_name(callee));
            int cidx = vm_closure_chunk(callee);
            int fn_slot = vm->sp - argc - 1;
            push_frame(vm, cidx, argc, fn_slot, line);
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
            /* tear down: free locals, then the callee fn value */
            while (vm->sp > f->base) { Value d = vm_pop(vm); value_free(&d); }
            if (f->fn_slot >= 0) {
                /* pop the callee fn value that the caller pushed at base-1 */
                Value d = vm_pop(vm);
                value_free(&d);
            }
            vm->frame_count--;
            for (int i = 0; i < n; i++) vm_push(vm, rets[i]);
            if (vm->frame_count == 0) {
                /* entry chunk returned: done. discard any straggler returns. */
                for (int i = 0; i < n; i++) { Value d = vm_pop(vm); value_free(&d); }
                return 0;
            }
            f = &vm->frames[vm->frame_count - 1];
            break;
        }

        /* arrays / maps */
        case MOP_NEW_ARRAY: {
            uint16_t tidx = read_u16(f);
            TypeSpec *ts = vm->module->consts[tidx].typespec;
            vm_push(vm, value_array(ts ? typespec_clone(ts) : NULL));
            break;
        }
        case MOP_NEW_MAP: {
            uint16_t tidx = read_u16(f);
            TypeSpec *ms = vm->module->consts[tidx].typespec;
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
            const char *sname = vm->module->consts[nidx].value.as.obj->as.str;
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
            const char *fname = vm->module->consts[nidx].value.as.obj->as.str;
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
            const char *fname = vm->module->consts[nidx].value.as.obj->as.str;
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
            const char *method = vm->module->consts[nidx].value.as.obj->as.str;
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
                Chunk *mc = module_chunk(vm->module, cidx);
                if ((int)mc->num_params != argc + 1)
                    vm_error(vm, line, "method '%s' expects %d argument%s but got %d",
                             method, mc->num_params - 1,
                             (mc->num_params - 1) == 1 ? "" : "s", argc);
                if (vm->frame_count >= MVM_MAX_FRAMES)
                    vm_error(vm, line, "stack overflow (call depth exceeded %d)", MVM_MAX_FRAMES);
                for (int i = (int)mc->num_params; i < (int)mc->num_locals; i++) vm_push(vm, value_nil());
                Frame *nf = &vm->frames[vm->frame_count++];
                nf->chunk = mc; nf->ip = mc->code; nf->base = base; nf->fn_slot = -1;
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
                myon_bridge_call_method(vm->bridge, recv_copy, method, args, argc, line, &out);
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
            vm_push(vm, myon_bridge_binary(vm->bridge, line, OP_ADD, a, b));
            break;
        }
        case MOP_TO_STR:   { Value v = vm_pop(vm); vm_push(vm, myon_bridge_cast_str(vm->bridge, line, v)); break; }
        case MOP_CAST_STR: { Value v = vm_pop(vm); vm_push(vm, myon_bridge_cast_str(vm->bridge, line, v)); break; }
        case MOP_CAST_INT: { Value v = vm_pop(vm); vm_push(vm, myon_bridge_cast_int(vm->bridge, line, v)); break; }
        case MOP_CAST_CHAR:{ Value v = vm_pop(vm); vm_push(vm, myon_bridge_cast_char(vm->bridge, line, v)); break; }
        case MOP_MAKE_ERROR:{ Value v = vm_pop(vm); vm_push(vm, myon_bridge_make_error(vm->bridge, line, v)); break; }

        /* native / module calls */
        case MOP_CALL_NATIVE: {
            uint16_t nid = read_u16(f);
            uint8_t argc = read_u8(f);
            if (nid >= (uint16_t)vm->module->native_count)
                vm_error(vm, line, "internal: native id %u out of range", nid);
            do_call_native(vm, line, vm->module->natives[nid], argc);
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

    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.module  = m;
    vm.program = program;
    vm.bridge  = myon_bridge_interp_new();

    int entry = m->entry_chunk;
    Chunk *ec = module_chunk(m, entry);
    if (!ec) { myon_bridge_interp_free(vm.bridge); fprintf(stderr, "myon: bad entry chunk\n"); return 70; }

    int rc = 0;
    if (setjmp(vm.on_error) == 0) {
        /* Point the shared stdlib's runtime_error() at our barrier so any
         * error raised deep inside a native unwinds cleanly back to us. */
        jmp_buf *bridge_buf = (jmp_buf *)myon_bridge_error_buf(vm.bridge);
        if (setjmp(*bridge_buf) == 0) {
            /* entry frame: <main> has no callee fn value (fn_slot = -1). */
            for (int i = 0; i < (int)ec->num_locals; i++) vm_push(&vm, value_nil());
            Frame *f = &vm.frames[vm.frame_count++];
            f->chunk = ec; f->ip = ec->code; f->base = 0; f->fn_slot = -1;
            rc = run(&vm);
        } else {
            rc = 1;   /* native raised (already reported by tree-walk diag) */
        }
    } else {
        rc = 1;       /* VM raised (already reported by vm_error) */
    }

    /* clean up any values left on the operand stack */
    while (vm.sp > 0) { Value d = vm.stack[--vm.sp]; value_free(&d); }
    free(vm.stack);
    myon_bridge_interp_free(vm.bridge);
    if (source) diag_clear_source();
    return rc;
}
