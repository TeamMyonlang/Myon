/*
 * Copyright 2026 nyan<(nyan4)
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

#include "mvm_compiler.h"
#include "common.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <sys/stat.h>

/* ================================================================== */
/* Compiler state                                                      */
/* ================================================================== */

#define MAX_LOCALS   1024
#define MAX_LOOP     64
#define MAX_DEPTH    256

/* A resolved local variable (spec §5.2). */
typedef struct {
    char *name;     /* borrowed from AST (not freed here) */
    int   depth;    /* block depth at which it was declared */
    int   slot;     /* stack slot */
} Local;

/* Per-loop backpatch bookkeeping for break/continue (spec §4.7).
 *
 * `continue` must land on the loop's *step* (the increment in a `for`, or the
 * condition re-check in a `while`), NOT on the condition top of a `for`:
 * jumping to the top of a range/iterable `for` would skip the counter
 * increment and spin forever.  Because the step is emitted *after* the body,
 * `continue` sites can't know its offset yet, so — like `break` — they emit a
 * placeholder forward JUMP whose operand is backpatched once the step label is
 * known (Step 7-b fix). */
typedef struct {
    int  continue_jumps[64];   /* operand offsets of pending continue JUMPs */
    int  continue_count;
    int  break_jumps[64];      /* operand offsets of pending break JUMPs */
    int  break_count;
    int  scope_depth;          /* block depth the loop body opened at */
} LoopCtx;

/* One function/chunk being compiled.  Chained so nested funcs know they are
 * nested (closures over outer locals are rejected, spec §7.3). */
typedef struct FnComp {
    struct FnComp *enclosing;
    int    chunk_idx;
    Local  locals[MAX_LOCALS];
    int    local_count;
    int    scope_depth;
    /*
     * Per-depth flag mirroring the tree-walk interpreter's Env.is_block
     * (src/env.c / src/interpreter.c §9.2): only an *explicit* `{ }` block
     * (STMT_BLOCK) enforces the shadowing prohibition.  The implicit body
     * scopes of myon.if / myon.while / myon.for behave like a function body
     * and assign through to an outer binding instead of erroring.
     * Index 0 is the function-body root (never a block). */
    int    depth_is_block[MAX_DEPTH];
    int    next_slot;          /* next slot to hand out */
    int    max_slot;           /* high-water mark => num_locals */
    LoopCtx loops[MAX_LOOP];
    int    loop_count;
} FnComp;

typedef struct {
    Module   *module;
    FnComp   *fn;              /* current function being compiled */
    /* struct declarations seen at top level (for static dispatch, spec §4.11) */
    StructDecl **structs;
    int          struct_count;
    int          struct_cap;
    jmp_buf   on_error;        /* longjmp target on a compile error */
    int        had_error;
} Compiler;

/* ================================================================== */
/* Error reporting (diag-formatted, spec §7.2)                         */
/* ================================================================== */

static void compile_error(Compiler *c, int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "myon: compile error");
    if (line > 0) fprintf(stderr, " (line %d)", line);
    fprintf(stderr, ": ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (line > 0) diag_print_snippet(line, 1);
    c->had_error = 1;
    longjmp(c->on_error, 1);
}

/* Single funnel for MVM-out-of-scope features (spec §7).  Easy to lift later. */
static void unsupported(Compiler *c, int line, const char *what, const char *hint) {
    compile_error(c, line, "MVM does not support %s. %s", what,
                  hint ? hint : "Run it as .myon (tree-walking) instead.");
}

/* ================================================================== */
/* forward decls                                                       */
/* ================================================================== */

static void compile_expr(Compiler *c, Expr *e);
static void compile_stmt(Compiler *c, Stmt *s);
/* open_scope: 0 = reuse current scope (function-body root),
 *             1 = open an implicit control-flow body scope (if/while/for),
 *             2 = open an explicit `{ }` block scope (STMT_BLOCK). */
static void compile_block(Compiler *c, StmtList *list, int open_scope);

static Chunk *cur_chunk(Compiler *c) { return module_chunk(c->module, c->fn->chunk_idx); }

/* ================================================================== */
/* scope resolution (spec §5)                                          */
/* ================================================================== */

/* Search all locals from innermost/newest to oldest (spec §5.3). */
static int resolve_local(FnComp *fn, const char *name) {
    for (int i = fn->local_count - 1; i >= 0; i--)
        if (strcmp(fn->locals[i].name, name) == 0)
            return fn->locals[i].slot;
    return -1;
}

/*
 * Name-resolution result (spec §5.3 / §4.6 note).
 *   NAME_LOCAL   : in the current function frame -> LOAD_LOCAL/STORE_LOCAL
 *   NAME_GLOBAL  : a top-level (<main>) binding    -> LOAD_GLOBAL/STORE_GLOBAL
 *   NAME_CAPTURE : found in an *intermediate* enclosing function -> the
 *                  closure-over-outer-locals case that §7.3 defers.
 *   NAME_NONE    : not a variable at all (maybe a native / struct / error).
 */
typedef enum { NAME_NONE, NAME_LOCAL, NAME_GLOBAL, NAME_CAPTURE } NameKind;

/*
 * Resolve a read/reference of `name` walking outward from the current
 * function.  A hit in the current frame is a local; a hit in the top-level
 * <main> frame (enclosing == NULL) is a global; a hit in any frame in between
 * would require a real closure upvalue, which the initial MVM does not support
 * (§7.3).  `*out_slot` receives the slot within whichever frame matched.
 */
static NameKind resolve_name(FnComp *fn, const char *name, int *out_slot) {
    int slot = resolve_local(fn, name);
    if (slot >= 0) { *out_slot = slot; return NAME_LOCAL; }
    for (FnComp *up = fn->enclosing; up; up = up->enclosing) {
        int s = resolve_local(up, name);
        if (s >= 0) {
            *out_slot = s;
            /* the outermost frame (no enclosing) is the global <main> frame */
            return up->enclosing ? NAME_CAPTURE : NAME_GLOBAL;
        }
    }
    return NAME_NONE;
}

/* Find a local declared *in the current scope depth* only. */
static int find_in_current_scope(FnComp *fn, const char *name) {
    for (int i = fn->local_count - 1; i >= 0; i--) {
        if (fn->locals[i].depth < fn->scope_depth) break; /* older scopes */
        if (strcmp(fn->locals[i].name, name) == 0) return fn->locals[i].slot;
    }
    return -1;
}

/* Find a local declared in an outer (shallower) scope. */
static int find_in_outer_scope(FnComp *fn, const char *name) {
    for (int i = fn->local_count - 1; i >= 0; i--)
        if (fn->locals[i].depth < fn->scope_depth &&
            strcmp(fn->locals[i].name, name) == 0)
            return fn->locals[i].slot;
    return -1;
}

/* Declare a new local at the current scope depth; returns its slot. */
static int declare_local(Compiler *c, const char *name) {
    FnComp *fn = c->fn;
    if (fn->local_count >= MAX_LOCALS)
        compile_error(c, 0, "too many local variables in one function");
    int slot = fn->next_slot++;
    if (fn->next_slot > fn->max_slot) fn->max_slot = fn->next_slot;
    fn->locals[fn->local_count].name = (char *)name;
    fn->locals[fn->local_count].depth = fn->scope_depth;
    fn->locals[fn->local_count].slot = slot;
    fn->local_count++;
    return slot;
}

/* Open a new lexical scope.  `is_block` marks an explicit `{ }` block so the
 * §9.2 shadowing rule applies (see FnComp.depth_is_block). */
/* Emit a load for a resolved variable (LOAD_LOCAL vs LOAD_GLOBAL). */
static void emit_var_load(Compiler *c, int line, NameKind k, int slot) {
    chunk_emit_op_u16(cur_chunk(c),
        k == NAME_GLOBAL ? MOP_LOAD_GLOBAL : MOP_LOAD_LOCAL, (uint16_t)slot, line);
}

static void begin_scope(Compiler *c, int is_block) {
    FnComp *fn = c->fn;
    fn->scope_depth++;
    if (fn->scope_depth >= MAX_DEPTH)
        compile_error(c, 0, "block nesting too deep");
    fn->depth_is_block[fn->scope_depth] = is_block ? 1 : 0;
}

/* Is the current innermost scope an explicit `{ }` block? (spec §5.3 2a) */
static int cur_scope_is_block(Compiler *c) {
    FnComp *fn = c->fn;
    return fn->scope_depth > 0 && fn->depth_is_block[fn->scope_depth];
}

/* Leave the current lexical scope.
 *
 * Local slots are NOT stack-allocated at runtime: push_frame() pre-reserves
 * num_locals (== max_slot) slots for the whole frame lifetime, and
 * STORE_LOCAL pops its computed value and writes it into that reserved slot.
 * Therefore leaving a scope must only roll back the *compile-time* slot
 * bookkeeping (so the slots can be reused by a sibling scope) — it must NOT
 * emit runtime POPs.  Emitting a POP per block-local (the old behaviour) drove
 * the operand stack below the reserved-locals region, which surfaced as an
 * "operand stack underflow" the first time a variable was assigned inside a
 * loop/if body (Step 7-b fix). */
static void end_scope(Compiler *c) {
    FnComp *fn = c->fn;
    int depth = fn->scope_depth;
    while (fn->local_count > 0 && fn->locals[fn->local_count - 1].depth == depth) {
        fn->local_count--;
        fn->next_slot--;
    }
    fn->scope_depth--;
}

/*
 * myon.expose name (spec §5.4 / language §9.1): promote the named local's
 * lifetime to the enclosing scope so end_scope() does not discard it.  We
 * lower its recorded depth by one; this both keeps it alive across the closing
 * brace and prevents a spurious POP.
 */
static void expose_local(Compiler *c, int line, const char *name) {
    FnComp *fn = c->fn;
    for (int i = fn->local_count - 1; i >= 0; i--) {
        if (fn->locals[i].depth == fn->scope_depth &&
            strcmp(fn->locals[i].name, name) == 0) {
            if (fn->locals[i].depth > 0) fn->locals[i].depth--;
            return;
        }
    }
    compile_error(c, line, "myon.expose: '%s' is not declared in this block", name);
}

/* ================================================================== */
/* native / struct helpers                                             */
/* ================================================================== */

static StructDecl *find_struct(Compiler *c, const char *name) {
    for (int i = 0; i < c->struct_count; i++)
        if (strcmp(c->structs[i]->name, name) == 0) return c->structs[i];
    return NULL;
}

static void register_struct(Compiler *c, StructDecl *sd) {
    if (c->struct_count == c->struct_cap) {
        c->struct_cap = c->struct_cap ? c->struct_cap * 2 : 8;
        c->structs = (StructDecl **)myon_xrealloc(c->structs, sizeof(StructDecl *) * c->struct_cap);
    }
    c->structs[c->struct_count++] = sd;
}

/* Reject the MVM-out-of-scope stdlib namespaces up front (spec §7.1). */
static void check_native_supported(Compiler *c, int line, const char *name) {
    if (strncmp(name, "myon.net.", 9) == 0 || strncmp(name, "myon.http.", 10) == 0)
        unsupported(c, line, "myon.net / myon.http", NULL);
    if (strncmp(name, "myon.ffi.", 9) == 0)
        unsupported(c, line, "myon.ffi", NULL);
    if (strncmp(name, "myon.async", 10) == 0)
        unsupported(c, line, "async/await", NULL);
}

/* ================================================================== */
/* expression compilation                                              */
/* ================================================================== */

/*
 * Parse a single interpolation sub-expression `src` into a standalone
 * Program (which must contain exactly one STMT_EXPR).  Returns the Program
 * (caller frees with program_free), or reports a compile error and longjmps.
 * Implemented at the bottom of this file where lexer/parser are included.
 */
static Program *parse_interp_expr(Compiler *c, int line, const char *src);

/* Compile a string literal that may contain {expr} interpolation (spec §4.12).
 * Mirrors the tree-walk interpolation splitting (src/interpreter.c) but emits
 * PUSH_CONST / <expr> / TO_STR / STR_CONCAT.  `{{` and `}}` are literal braces. */
static void compile_string(Compiler *c, int line, const char *raw) {
    Chunk *ch = cur_chunk(c);
    /* Fast path: no interpolation -> single str constant (spec §1.3). */
    if (strchr(raw, '{') == NULL && strchr(raw, '}') == NULL) {
        int idx = module_add_const_str(c->module, raw);
        chunk_emit_op_u16(ch, MOP_PUSH_CONST, (uint16_t)idx, line);
        return;
    }

    /* General path: build up pieces and concatenate left-to-right. */
    int pieces = 0;
    size_t n = strlen(raw);
    char *lit = (char *)myon_xmalloc(n + 1);
    size_t li = 0;

    #define FLUSH_LIT() do {                                            \
        if (li > 0) {                                                   \
            lit[li] = '\0';                                            \
            int idx = module_add_const_str(c->module, lit);            \
            chunk_emit_op_u16(ch, MOP_PUSH_CONST, (uint16_t)idx, line); \
            if (pieces > 0) chunk_emit_op(ch, MOP_STR_CONCAT, line);    \
            pieces++;                                                   \
            li = 0;                                                    \
        }                                                              \
    } while (0)

    const char *s = raw;
    while (*s) {
        if (s[0] == '{' && s[1] == '{') { lit[li++] = '{'; s += 2; continue; }
        if (s[0] == '}' && s[1] == '}') { lit[li++] = '}'; s += 2; continue; }
        if (s[0] == '{') {
            const char *end = strchr(s + 1, '}');
            if (!end) compile_error(c, line, "unterminated '{' in string interpolation");
            FLUSH_LIT();
            char *sub = myon_strndup(s + 1, (size_t)(end - (s + 1)));
            Program *p = parse_interp_expr(c, line, sub);
            free(sub);
            compile_expr(c, p->stmts.items[0]->as.expr);
            program_free(p);
            chunk_emit_op(ch, MOP_TO_STR, line);
            if (pieces > 0) chunk_emit_op(ch, MOP_STR_CONCAT, line);
            pieces++;
            s = end + 1;
            continue;
        }
        lit[li++] = *s++;
    }
    FLUSH_LIT();
    if (pieces == 0) {
        int idx = module_add_const_str(c->module, "");
        chunk_emit_op_u16(ch, MOP_PUSH_CONST, (uint16_t)idx, line);
    }
    #undef FLUSH_LIT
    free(lit);
}

/* Map an AST OpKind (src/ast.h) to the corresponding MVM opcode. */
static OpCode binary_opcode(OpKind k) {
    switch (k) {
        case OP_ADD: return MOP_ADD; case OP_SUB: return MOP_SUB;
        case OP_MUL: return MOP_MUL; case OP_DIV: return MOP_DIV;
        case OP_EQ:  return MOP_EQ;  case OP_NEQ: return MOP_NEQ;
        case OP_LT:  return MOP_LT;  case OP_GT:  return MOP_GT;
        case OP_LE:  return MOP_LE;  case OP_GE:  return MOP_GE;
        default:     return MOP_NOP;
    }
}

/* Compile a call whose callee is a builtin/native namespace (myon.*). */
static void compile_native_call(Compiler *c, Expr *e, const char *name) {
    Chunk *ch = cur_chunk(c);
    check_native_supported(c, e->line, name);
    if (e->as.call.type_arg_count > 0)
        unsupported(c, e->line, "generics", "Explicit type arguments are not supported by MVM.");
    for (int i = 0; i < e->as.call.arg_count; i++) {
        if (e->as.call.arg_names && e->as.call.arg_names[i])
            compile_error(c, e->line, "MVM: named arguments are only allowed on struct constructors");
        compile_expr(c, e->as.call.args[i]);
    }
    int nid = module_add_native(c->module, name);
    chunk_emit_op_u16(ch, MOP_CALL_NATIVE, (uint16_t)nid, e->line);
    chunk_emit_byte(ch, (uint8_t)e->as.call.arg_count, e->line);
}

/*
 * Collect a struct's fields across the whole inheritance chain, parent-first,
 * mirroring the tree-walk interpreter's collect_fields() (src/interpreter.c).
 * `out` (caller frees) receives borrowed StructField pointers in the exact
 * order NEW_STRUCT expects the values on the stack (spec §4.11).
 */
static void collect_struct_fields(StructDecl *sd, StructField ***out, int *count, int *cap) {
    if (!sd) return;
    collect_struct_fields(sd->parent, out, count, cap);
    for (int i = 0; i < sd->field_count; i++) {
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 8;
            *out = (StructField **)myon_xrealloc(*out, sizeof(StructField *) * (*cap));
        }
        (*out)[(*count)++] = &sd->fields[i];
    }
}

/* Compile a struct constructor Name(field=expr, ...) (spec §4.11). */
static void compile_struct_ctor(Compiler *c, Expr *e, StructDecl *sd) {
    Chunk *ch = cur_chunk(c);

    /*
     * Resolve the parent chain if not already linked (the compiler pre-pass
     * only registers top-level structs; parent_name -> parent is done here so
     * inherited fields are visible, matching the interpreter's resolve pass).
     */
    for (StructDecl *cur = sd; cur; cur = cur->parent) {
        if (cur->parent_name && !cur->parent) {
            cur->parent = find_struct(c, cur->parent_name);
            if (!cur->parent)
                compile_error(c, e->line, "struct '%s' extends unknown struct '%s'",
                              cur->name, cur->parent_name);
        }
    }

    StructField **fields = NULL;
    int fc = 0, cap = 0;
    collect_struct_fields(sd, &fields, &fc, &cap);

    /*
     * Evaluate field initializers in collected order (parent fields first),
     * matching NEW_STRUCT's pop order and the interpreter's construction.
     * Named args bind by field name; a positional arg binds to the field at
     * the same ordinal when it has no name.
     */
    for (int f = 0; f < fc; f++) {
        int found = -1;
        for (int a = 0; a < e->as.call.arg_count; a++) {
            const char *an = e->as.call.arg_names ? e->as.call.arg_names[a] : NULL;
            if (an && strcmp(an, fields[f]->name) == 0) { found = a; break; }
            if (!an && a == f) found = a; /* positional fallback */
        }
        if (found < 0) {
            free(fields);
            compile_error(c, e->line, "struct %s: missing field '%s'",
                          sd->name, fields[f]->name);
        }
        compile_expr(c, e->as.call.args[found]);
    }
    free(fields);
    int tidx = module_add_const_str(c->module, sd->name);
    chunk_emit_op_u16(ch, MOP_NEW_STRUCT, (uint16_t)tidx, e->line);
}

static void compile_call(Compiler *c, Expr *e) {
    Chunk *ch = cur_chunk(c);
    Expr *callee = e->as.call.callee;

    if (callee->kind == EXPR_IDENT) {
        const char *name = callee->as.ident;
        /* builtins & stdlib namespaces */
        if (strncmp(name, "myon.", 5) == 0) { compile_native_call(c, e, name); return; }
        /* struct constructor */
        StructDecl *sd = find_struct(c, name);
        if (sd) { compile_struct_ctor(c, e, sd); return; }
        /* user function value referenced by name -> load + CALL.  Resolving
         * through to the <main> globals is what makes recursion and mutually
         * recursive top-level functions work (spec §4.6 note / M4). */
        int slot = -1;
        NameKind k = resolve_name(c->fn, name, &slot);
        if (k == NAME_CAPTURE)
            unsupported(c, e->line,
                "calling an outer function's local (closures)",
                "Define the function at top level so it is a global.");
        if (k == NAME_NONE)
            compile_error(c, e->line, "undefined function or variable '%s'", name);
        emit_var_load(c, e->line, k, slot);
        for (int i = 0; i < e->as.call.arg_count; i++)
            compile_expr(c, e->as.call.args[i]);
        chunk_emit_op_u8(ch, MOP_CALL, (uint8_t)e->as.call.arg_count, e->line);
        return;
    }

    if (callee->kind == EXPR_GENERIC)
        unsupported(c, e->line, "generics", "Generic instantiation is not supported by MVM.");

    /* method call: obj.method(args) -> INVOKE (spec §4.11) */
    if (callee->kind == EXPR_MEMBER) {
        compile_expr(c, callee->as.member.target);          /* receiver */
        for (int i = 0; i < e->as.call.arg_count; i++)
            compile_expr(c, e->as.call.args[i]);
        int nidx = module_add_const_str(c->module, callee->as.member.name);
        chunk_emit_op_u16(ch, MOP_INVOKE, (uint16_t)nidx, e->line);
        chunk_emit_byte(ch, (uint8_t)e->as.call.arg_count, e->line);
        return;
    }

    /* first-class function value expression */
    compile_expr(c, callee);
    for (int i = 0; i < e->as.call.arg_count; i++)
        compile_expr(c, e->as.call.args[i]);
    chunk_emit_op_u8(ch, MOP_CALL, (uint8_t)e->as.call.arg_count, e->line);
}

/* Compile a lambda/func body into a fresh chunk; leaves a closure on stack.
 * `self_struct` != NULL marks a struct method: `self` is bound to slot 0
 * (the first argument position) before the declared params (spec §4.11). */
static int compile_function(Compiler *c, FuncDecl *decl, const char *dbg_name,
                            StructDecl *self_struct);

static void compile_expr(Compiler *c, Expr *e) {
    Chunk *ch = cur_chunk(c);
    switch (e->kind) {
        case EXPR_INT_LIT: {
            int idx = module_add_const_int(c->module, e->as.int_val);
            chunk_emit_op_u16(ch, MOP_PUSH_CONST, (uint16_t)idx, e->line);
            break;
        }
        case EXPR_FLOAT_LIT: {
            int idx = module_add_const_float(c->module, e->as.float_val);
            chunk_emit_op_u16(ch, MOP_PUSH_CONST, (uint16_t)idx, e->line);
            break;
        }
        case EXPR_BOOL_LIT:
            chunk_emit_op(ch, e->as.bool_val ? MOP_PUSH_TRUE : MOP_PUSH_FALSE, e->line);
            break;
        case EXPR_NIL:
            chunk_emit_op(ch, MOP_PUSH_NIL, e->line);
            break;
        case EXPR_STRING:
            compile_string(c, e->line, e->as.str_val ? e->as.str_val : "");
            break;
        case EXPR_IDENT: {
            int slot = -1;
            NameKind k = resolve_name(c->fn, e->as.ident, &slot);
            if (k == NAME_CAPTURE)
                unsupported(c, e->line,
                    "capturing an outer function's local variable (closures)",
                    "Only top-level globals and the function's own locals are visible.");
            if (k == NAME_NONE)
                compile_error(c, e->line, "undefined variable '%s'", e->as.ident);
            emit_var_load(c, e->line, k, slot);
            break;
        }
        case EXPR_BINARY: {
            compile_expr(c, e->as.binary.left);
            compile_expr(c, e->as.binary.right);
            OpCode op = binary_opcode(e->as.binary.op);
            if (op == MOP_NOP)
                compile_error(c, e->line, "MVM: unsupported binary operator");
            chunk_emit_op(ch, op, e->line);
            break;
        }
        case EXPR_UNARY:
            compile_expr(c, e->as.unary.operand);
            chunk_emit_op(ch, e->as.unary.op == OP_NOT ? MOP_NOT : MOP_NEG, e->line);
            break;
        case EXPR_LOGICAL: {
            /* short-circuit via jumps (spec §4.5) */
            compile_expr(c, e->as.binary.left);
            if (e->as.binary.op == OP_AND) {
                /* a and b : if a false -> result false */
                chunk_emit_op(ch, MOP_DUP, e->line);
                int jf = chunk_emit_jump(ch, MOP_JUMP_IF_FALSE, e->line);
                chunk_emit_op(ch, MOP_POP, e->line);   /* drop the duped a */
                compile_expr(c, e->as.binary.right);
                chunk_patch_jump(ch, jf);
            } else { /* OP_OR */
                chunk_emit_op(ch, MOP_DUP, e->line);
                int jt = chunk_emit_jump(ch, MOP_JUMP_IF_TRUE, e->line);
                chunk_emit_op(ch, MOP_POP, e->line);
                compile_expr(c, e->as.binary.right);
                chunk_patch_jump(ch, jt);
            }
            break;
        }
        case EXPR_CALL:
            compile_call(c, e);
            break;
        case EXPR_INDEX:
            compile_expr(c, e->as.index.target);
            compile_expr(c, e->as.index.index);
            chunk_emit_op(ch, MOP_INDEX_GET, e->line);
            break;
        case EXPR_MEMBER: {
            compile_expr(c, e->as.member.target);
            int nidx = module_add_const_str(c->module, e->as.member.name);
            chunk_emit_op_u16(ch, MOP_GET_FIELD, (uint16_t)nidx, e->line);
            break;
        }
        case EXPR_ARRAY_CTOR: {
            int tidx = module_add_const_typespec(c->module, e->as.array_elem);
            chunk_emit_op_u16(ch, MOP_NEW_ARRAY, (uint16_t)tidx, e->line);
            break;
        }
        case EXPR_MAP_CTOR: {
            /* store a synthetic map typespec whose elem=val, key=key */
            TypeSpec *ms = typespec_new(TYPE_MAP);
            ms->key  = typespec_clone(e->as.map_types.key);
            ms->elem = typespec_clone(e->as.map_types.val);
            int tidx = module_add_const_typespec(c->module, ms);
            typespec_free(ms);
            chunk_emit_op_u16(ch, MOP_NEW_MAP, (uint16_t)tidx, e->line);
            break;
        }
        case EXPR_LAMBDA: {
            /* A lambda must be self-contained (no outer capture, spec §7.3). */
            int cidx = compile_function(c, e->as.lambda, "<lambda>", NULL);
            chunk_emit_op_u16(cur_chunk(c), MOP_MAKE_CLOSURE, (uint16_t)cidx, e->line);
            break;
        }
        case EXPR_STR_CTOR:
            compile_expr(c, e->as.operand);
            chunk_emit_op(ch, MOP_CAST_STR, e->line);
            break;
        case EXPR_INT_CTOR:
            compile_expr(c, e->as.operand);
            chunk_emit_op(ch, MOP_CAST_INT, e->line);
            break;
        case EXPR_CHAR_CTOR:
            compile_expr(c, e->as.operand);
            chunk_emit_op(ch, MOP_CAST_CHAR, e->line);
            break;
        case EXPR_ERROR_CTOR:
            compile_expr(c, e->as.operand);
            chunk_emit_op(ch, MOP_MAKE_ERROR, e->line);
            break;
        case EXPR_AWAIT:
            unsupported(c, e->line, "async/await", NULL);
            break;
        case EXPR_GENERIC:
            unsupported(c, e->line, "generics", "Generic instantiation is not supported by MVM.");
            break;
        default:
            compile_error(c, e->line, "MVM: unsupported expression kind %d", (int)e->kind);
            break;
    }
}

/* ================================================================== */
/* assignment (spec §5.3 resolve_assign, §4.6/§4.10/§4.11)             */
/* ================================================================== */

static void compile_assign_target_name(Compiler *c, int line, const char *name) {
    /* value to store is already on the stack top. */
    FnComp *fn = c->fn;
    int i = find_in_current_scope(fn, name);
    if (i >= 0) {                          /* (1) update current scope */
        chunk_emit_op_u16(cur_chunk(c), MOP_STORE_LOCAL, (uint16_t)i, line);
        return;
    }
    int j = find_in_outer_scope(fn, name);
    if (j >= 0) {
        if (cur_scope_is_block(c)) {       /* (2a) inside an explicit {} block => shadow error */
            compile_error(c, line,
                "redefinition of '%s' shadows an outer variable (forbidden, spec 9.2)", name);
        }
        chunk_emit_op_u16(cur_chunk(c), MOP_STORE_LOCAL, (uint16_t)j, line); /* (2b) */
        return;
    }
    /*
     * Not in this function at all.  If it names an existing top-level global,
     * assign through to it (mirrors the interpreter's env_set() walking the
     * parent chain, spec §9.2 (2b)); if it lives in an intermediate function
     * that is the unsupported closure case (§7.3).
     */
    if (fn->enclosing) {
        int gslot = -1;
        NameKind gk = resolve_name(fn, name, &gslot);
        if (gk == NAME_GLOBAL) {
            chunk_emit_op_u16(cur_chunk(c), MOP_STORE_GLOBAL, (uint16_t)gslot, line);
            return;
        }
        if (gk == NAME_CAPTURE)
            unsupported(c, line,
                "assigning to an outer function's local (closures)",
                "Only top-level globals and the function's own locals are visible.");
    }
    int slot = declare_local(c, name);     /* (3) new local */
    chunk_emit_op_u16(cur_chunk(c), MOP_STORE_LOCAL, (uint16_t)slot, line);
}

static void compile_assign(Compiler *c, Stmt *s) {
    int line = s->line;
    Chunk *ch = cur_chunk(c);

    /* member / index target: a.b = v  or  a[i] = v (spec §4.10/§4.11) */
    if (s->as.assign.target) {
        Expr *t = s->as.assign.target;
        if (s->as.assign.is_compound)
            unsupported(c, line, "compound assignment to member/index targets", NULL);
        if (t->kind == EXPR_MEMBER) {
            compile_expr(c, t->as.member.target);   /* st */
            compile_expr(c, s->as.assign.value);    /* v  */
            int nidx = module_add_const_str(c->module, t->as.member.name);
            chunk_emit_op_u16(ch, MOP_SET_FIELD, (uint16_t)nidx, line);
            return;
        }
        if (t->kind == EXPR_INDEX) {
            compile_expr(c, t->as.index.target);    /* c */
            compile_expr(c, t->as.index.index);     /* i */
            compile_expr(c, s->as.assign.value);    /* v */
            chunk_emit_op(ch, MOP_INDEX_SET, line);
            return;
        }
        compile_error(c, line, "MVM: unsupported assignment target");
    }

    /* multi-target: a, b = f()  (spec §4.9) */
    if (s->as.assign.extra_count > 0) {
        if (s->as.assign.is_compound)
            compile_error(c, line, "compound assignment cannot have multiple targets");
        /* Evaluate the RHS which must leave N values on the stack (a call). */
        compile_expr(c, s->as.assign.value);
        int total = s->as.assign.extra_count + 1;
        /* A native stdlib call returns its multi-value result as a single
         * tuple array (spec §6.2), not as N stack values like a user
         * function's multi-`ret`.  Detect that RHS shape and spread it with
         * MOP_UNPACK so the STORE_LOCALs below receive the N values.  User
         * function / method calls already leave N values, so they need no
         * unpack. (Step 7-b) */
        {
            Expr *rhs = s->as.assign.value;
            int rhs_is_native_call =
                rhs && rhs->kind == EXPR_CALL &&
                rhs->as.call.callee &&
                rhs->as.call.callee->kind == EXPR_IDENT &&
                strncmp(rhs->as.call.callee->as.ident, "myon.", 5) == 0;
            /* A built-in method call (obj.method(...) where obj is an
             * array/map/str, e.g. `mid, merr = ss.slice(1, 3)`) is executed
             * by MOP_INVOKE, which reuses the tree-walk built-in and therefore
             * leaves the multi-return as a single tuple array too (spec §6.2),
             * exactly like a native call.  We cannot statically tell a built-in
             * receiver from a user-struct receiver here, but struct methods
             * returning multiple values into a multi-target assignment are not
             * part of the MVM-supported surface, so emitting UNPACK for any
             * method-call RHS is the correct, net-positive choice. (Step 7-b) */
            int rhs_is_method_call =
                rhs && rhs->kind == EXPR_CALL &&
                rhs->as.call.callee &&
                rhs->as.call.callee->kind == EXPR_MEMBER;
            if (rhs_is_native_call || rhs_is_method_call)
                chunk_emit_op_u8(ch, MOP_UNPACK, (uint8_t)total, line);
        }
        /* Values are on the stack in order; store in reverse (spec §4.9). */
        /* First resolve/declare all target slots left-to-right so slot numbers
         * are stable, then emit STORE in reverse.  Each target is resolved with
         * the same rule as a single-target assignment (compile_assign_target_name):
         * an existing local/outer slot is reused, an existing top-level global is
         * assigned through with MOP_STORE_GLOBAL, otherwise a new local is
         * declared.  Without the global path, `x, y = pair()` inside a function
         * would wrongly create shadowing locals instead of updating the outer
         * x, y (spec §9.2). (Step 7-b fix) */
        char *names[64];
        int   slots[64];
        int   is_global[64];
        if (total > 64) compile_error(c, line, "too many assignment targets");
        names[0] = s->as.assign.name;
        for (int k = 0; k < s->as.assign.extra_count; k++)
            names[k + 1] = s->as.assign.extra_names[k];
        for (int k = 0; k < total; k++) {
            FnComp *fn = c->fn;
            is_global[k] = 0;
            int idx = find_in_current_scope(fn, names[k]);
            if (idx < 0) {
                int jj = find_in_outer_scope(fn, names[k]);
                if (jj >= 0 && cur_scope_is_block(c))
                    compile_error(c, line,
                        "redefinition of '%s' shadows an outer variable (forbidden, spec 9.2)", names[k]);
                if (jj >= 0) {
                    idx = jj;
                } else if (fn->enclosing) {
                    /* not a local of this function: try a top-level global */
                    int gslot = -1;
                    NameKind gk = resolve_name(fn, names[k], &gslot);
                    if (gk == NAME_GLOBAL) {
                        is_global[k] = 1;
                        idx = gslot;
                    } else if (gk == NAME_CAPTURE) {
                        unsupported(c, line,
                            "assigning to an outer function's local (closures)",
                            "Only top-level globals and the function's own locals are visible.");
                    } else {
                        idx = declare_local(c, names[k]);
                    }
                } else {
                    idx = declare_local(c, names[k]);
                }
            }
            slots[k] = idx;
        }
        for (int k = total - 1; k >= 0; k--)
            chunk_emit_op_u16(ch,
                is_global[k] ? MOP_STORE_GLOBAL : MOP_STORE_LOCAL,
                (uint16_t)slots[k], line);
        return;
    }

    /* simple / compound single-target assignment */
    if (s->as.assign.is_compound) {
        /* x op= v  ->  x = x op v */
        int slot = resolve_local(c->fn, s->as.assign.name);
        if (slot < 0)
            compile_error(c, line, "compound assignment to undefined variable '%s'",
                          s->as.assign.name);
        chunk_emit_op_u16(ch, MOP_LOAD_LOCAL, (uint16_t)slot, line);
        compile_expr(c, s->as.assign.value);
        chunk_emit_op(ch, binary_opcode(s->as.assign.compound), line);
        chunk_emit_op_u16(ch, MOP_STORE_LOCAL, (uint16_t)slot, line);
        return;
    }

    compile_expr(c, s->as.assign.value);
    /* Assigning myon.nil to a normal (single-target) variable is forbidden
     * (spec §2.4).  The tree-walk enforces this at runtime on the produced
     * value, so emit a runtime guard here to mirror it exactly — this covers
     * both the literal `x = myon.nil` and a computed nil RHS.  Error slots of
     * a multiple-target assignment are exempt and never reach this path. */
    chunk_emit_op(cur_chunk(c), MOP_CHECK_NOT_NIL, line);
    compile_assign_target_name(c, line, s->as.assign.name);
}

/* ================================================================== */
/* statements                                                          */
/* ================================================================== */

static LoopCtx *cur_loop(Compiler *c) {
    if (c->fn->loop_count == 0) return NULL;
    return &c->fn->loops[c->fn->loop_count - 1];
}

static void compile_if(Compiler *c, Stmt *s) {
    Chunk *ch = cur_chunk(c);
    /* if cond then { ... } elif ... else ... */
    compile_expr(c, s->as.if_stmt.cond);
    int else_jump = chunk_emit_jump(ch, MOP_JUMP_IF_FALSE, s->line);
    compile_block(c, &s->as.if_stmt.then_body, 1);

    /* collect end jumps to patch after the whole chain */
    int end_jumps[128];
    int end_count = 0;
    end_jumps[end_count++] = chunk_emit_jump(ch, MOP_JUMP, s->line);
    chunk_patch_jump(ch, else_jump);

    for (int i = 0; i < s->as.if_stmt.elif_count; i++) {
        compile_expr(c, s->as.if_stmt.elifs[i].cond);
        int nxt = chunk_emit_jump(ch, MOP_JUMP_IF_FALSE, s->line);
        compile_block(c, &s->as.if_stmt.elifs[i].body, 1);
        if (end_count < 128) end_jumps[end_count++] = chunk_emit_jump(ch, MOP_JUMP, s->line);
        chunk_patch_jump(ch, nxt);
    }

    if (s->as.if_stmt.has_else)
        compile_block(c, &s->as.if_stmt.else_body, 1);

    for (int i = 0; i < end_count; i++) chunk_patch_jump(ch, end_jumps[i]);
}

static void loop_push(Compiler *c) {
    FnComp *fn = c->fn;
    if (fn->loop_count >= MAX_LOOP) compile_error(c, 0, "loops nested too deeply");
    LoopCtx *l = &fn->loops[fn->loop_count++];
    l->continue_count = 0;
    l->break_count = 0;
    l->scope_depth = fn->scope_depth;
}

/* Backpatch every pending `continue` JUMP in the current loop so it lands on
 * the loop's step label (which is `chunk_here` at the call site).  Must be
 * called after the body and before the step code is emitted. */
static void loop_patch_continues(Compiler *c) {
    Chunk *ch = cur_chunk(c);
    LoopCtx *l = cur_loop(c);
    for (int i = 0; i < l->continue_count; i++) chunk_patch_jump(ch, l->continue_jumps[i]);
}

static void loop_pop(Compiler *c) {
    Chunk *ch = cur_chunk(c);
    LoopCtx *l = cur_loop(c);
    for (int i = 0; i < l->break_count; i++) chunk_patch_jump(ch, l->break_jumps[i]);
    c->fn->loop_count--;
}

static void compile_while(Compiler *c, Stmt *s) {
    Chunk *ch = cur_chunk(c);
    int top = chunk_here(ch);
    compile_expr(c, s->as.while_stmt.cond);
    int exit_jump = chunk_emit_jump(ch, MOP_JUMP_IF_FALSE, s->line);
    loop_push(c);
    compile_block(c, &s->as.while_stmt.body, 1);
    /* `continue` in a while loop re-checks the condition: patch it to `top`. */
    loop_patch_continues(c);
    chunk_emit_loop(ch, MOP_JUMP, top, s->line);
    chunk_patch_jump(ch, exit_jump);
    loop_pop(c);
}

static void compile_for(Compiler *c, Stmt *s) {
    Chunk *ch = cur_chunk(c);
    /* for x in range(a, b) : lower to a hidden counter + LT + jumps (spec §4.7).
     * The loop-variable scope is an implicit body scope, not an explicit `{ }`
     * block, so it must not trigger the §9.2 shadowing rule. */
    begin_scope(c, 0);
    if (s->as.for_stmt.is_range) {
        /* declare loop var x = start */
        compile_expr(c, s->as.for_stmt.range_start);
        int vslot = declare_local(c, s->as.for_stmt.var);
        chunk_emit_op_u16(ch, MOP_STORE_LOCAL, (uint16_t)vslot, s->line);
        /* end value stored in a hidden local */
        compile_expr(c, s->as.for_stmt.range_end);
        static const char *end_name = " __for_end";
        int eslot = declare_local(c, end_name);
        chunk_emit_op_u16(ch, MOP_STORE_LOCAL, (uint16_t)eslot, s->line);

        int top = chunk_here(ch);
        chunk_emit_op_u16(ch, MOP_LOAD_LOCAL, (uint16_t)vslot, s->line);
        chunk_emit_op_u16(ch, MOP_LOAD_LOCAL, (uint16_t)eslot, s->line);
        chunk_emit_op(ch, MOP_LT, s->line);
        int exit_jump = chunk_emit_jump(ch, MOP_JUMP_IF_FALSE, s->line);

        loop_push(c);
        compile_block(c, &s->as.for_stmt.body, 1);
        /* `continue` must fall through to the increment, not back to the
         * condition top (which would skip the increment and spin forever). */
        loop_patch_continues(c);
        /* increment: x = x + 1 */
        chunk_emit_op_u16(ch, MOP_LOAD_LOCAL, (uint16_t)vslot, s->line);
        int one = module_add_const_int(c->module, 1);
        chunk_emit_op_u16(ch, MOP_PUSH_CONST, (uint16_t)one, s->line);
        chunk_emit_op(ch, MOP_ADD, s->line);
        chunk_emit_op_u16(ch, MOP_STORE_LOCAL, (uint16_t)vslot, s->line);
        chunk_emit_loop(ch, MOP_JUMP, top, s->line);
        chunk_patch_jump(ch, exit_jump);
        loop_pop(c);
    } else {
        /* for x in iterable : hidden index i, x = iterable[i] (spec §4.7). */
        compile_expr(c, s->as.for_stmt.iterable);
        static const char *it_name = " __for_it";
        int islot_it = declare_local(c, it_name);
        chunk_emit_op_u16(ch, MOP_STORE_LOCAL, (uint16_t)islot_it, s->line);
        /* i = 0 */
        int zero = module_add_const_int(c->module, 0);
        chunk_emit_op_u16(ch, MOP_PUSH_CONST, (uint16_t)zero, s->line);
        static const char *idx_name = " __for_idx";
        int islot = declare_local(c, idx_name);
        chunk_emit_op_u16(ch, MOP_STORE_LOCAL, (uint16_t)islot, s->line);
        /* loop var */
        chunk_emit_op(ch, MOP_PUSH_NIL, s->line);
        int vslot = declare_local(c, s->as.for_stmt.var);
        chunk_emit_op_u16(ch, MOP_STORE_LOCAL, (uint16_t)vslot, s->line);

        int top = chunk_here(ch);
        /* i < len(iterable) : len via native call */
        chunk_emit_op_u16(ch, MOP_LOAD_LOCAL, (uint16_t)islot, s->line);
        chunk_emit_op_u16(ch, MOP_LOAD_LOCAL, (uint16_t)islot_it, s->line);
        int nlen = module_add_native(c->module, "myon.len");
        chunk_emit_op_u16(ch, MOP_CALL_NATIVE, (uint16_t)nlen, s->line);
        chunk_emit_byte(ch, 1, s->line);
        chunk_emit_op(ch, MOP_LT, s->line);
        int exit_jump = chunk_emit_jump(ch, MOP_JUMP_IF_FALSE, s->line);
        /* x = it[i] */
        chunk_emit_op_u16(ch, MOP_LOAD_LOCAL, (uint16_t)islot_it, s->line);
        chunk_emit_op_u16(ch, MOP_LOAD_LOCAL, (uint16_t)islot, s->line);
        chunk_emit_op(ch, MOP_INDEX_GET, s->line);
        chunk_emit_op_u16(ch, MOP_STORE_LOCAL, (uint16_t)vslot, s->line);

        loop_push(c);
        compile_block(c, &s->as.for_stmt.body, 1);
        /* `continue` must fall through to the index increment below. */
        loop_patch_continues(c);
        /* i = i + 1 */
        chunk_emit_op_u16(ch, MOP_LOAD_LOCAL, (uint16_t)islot, s->line);
        int one = module_add_const_int(c->module, 1);
        chunk_emit_op_u16(ch, MOP_PUSH_CONST, (uint16_t)one, s->line);
        chunk_emit_op(ch, MOP_ADD, s->line);
        chunk_emit_op_u16(ch, MOP_STORE_LOCAL, (uint16_t)islot, s->line);
        chunk_emit_loop(ch, MOP_JUMP, top, s->line);
        chunk_patch_jump(ch, exit_jump);
        loop_pop(c);
    }
    end_scope(c);
}

static void compile_return(Compiler *c, Stmt *s) {
    Chunk *ch = cur_chunk(c);
    int n = s->as.ret.count;
    for (int i = 0; i < n; i++) compile_expr(c, s->as.ret.values[i]);
    chunk_emit_op_u8(ch, MOP_RET, (uint8_t)n, s->line);
}

static void compile_stmt(Compiler *c, Stmt *s) {
    Chunk *ch = cur_chunk(c);
    switch (s->kind) {
        case STMT_SYSTEM:
            /* compile-time metadata only (spec §4.14 note); no code emitted */
            break;
        case STMT_MODULE: {
            /*
             * Builtin (`myon.*`) modules are compile-time metadata only — no
             * code is emitted, matching the tree-walker.  *External* modules
             * (`module external.*`), however, must actually load and inject
             * their definitions; the MVM backend does not yet implement that
             * (see docs/known-issues.md).  Previously these were silently
             * skipped, so any use of an external symbol later compiled to an
             * "undefined" error or, worse, produced a .myc that behaved
             * differently from the .myon.  Reject them explicitly instead so
             * the divergence is loud, not silent (spec §11 / §2 equivalence). */
            const char *mpath = s->as.module_decl.path;
            if (mpath && strncmp(mpath, "external.", 9) == 0)
                unsupported(c, s->line, "external module imports (module external.*)",
                    "The MVM backend cannot yet load external modules; run it as "
                    ".myon (tree-walking) instead.");
            break;
        }
        case STMT_ASSIGN:
            compile_assign(c, s);
            break;
        case STMT_EXPR:
            compile_expr(c, s->as.expr);
            chunk_emit_op(ch, MOP_POP, s->line);  /* discard result (spec appendix) */
            break;
        case STMT_IF:
            compile_if(c, s);
            break;
        case STMT_WHILE:
            compile_while(c, s);
            break;
        case STMT_FOR:
            compile_for(c, s);
            break;
        case STMT_BREAK: {
            LoopCtx *l = cur_loop(c);
            if (!l) compile_error(c, s->line, "myon.break outside a loop");
            int j = chunk_emit_jump(ch, MOP_JUMP, s->line);
            if (l->break_count < 64) l->break_jumps[l->break_count++] = j;
            break;
        }
        case STMT_CONTINUE: {
            LoopCtx *l = cur_loop(c);
            if (!l) compile_error(c, s->line, "myon.continue outside a loop");
            /* Emit a forward placeholder JUMP; loop_patch_continues() rewrites
             * it to the loop's step label once that offset is known.  This is
             * what makes `continue` skip to the increment in a `for` loop
             * rather than back to the condition (which would loop forever). */
            int j = chunk_emit_jump(ch, MOP_JUMP, s->line);
            if (l->continue_count < 64) l->continue_jumps[l->continue_count++] = j;
            break;
        }
        case STMT_BLOCK:
            compile_block(c, &s->as.block, 2);   /* explicit `{ }` block (spec §9.2) */
            break;
        case STMT_EXPOSE:
            expose_local(c, s->line, s->as.expose_name);
            break;
        case STMT_FUNC: {
            /* declare the function name as a local, build closure, store it. */
            int slot = declare_local(c, s->as.func->name);
            int cidx = compile_function(c, s->as.func, s->as.func->name, NULL);
            chunk_emit_op_u16(cur_chunk(c), MOP_MAKE_CLOSURE, (uint16_t)cidx, s->line);
            chunk_emit_op_u16(cur_chunk(c), MOP_STORE_LOCAL, (uint16_t)slot, s->line);
            break;
        }
        case STMT_STRUCT: {
            /* Structs are pre-registered in mvm_compile_program; here we only
             * compile their methods into their own chunks (spec §4.11).  Each
             * method chunk is named "Struct.method" so the disassembler is
             * readable and the future VM (Step 6) can build its method table
             * for static dispatch by name (spec §4.11 継承/self note). */
            StructDecl *sd = s->as.struct_decl;
            for (int i = 0; i < sd->method_count; i++) {
                FuncDecl *md = sd->methods[i];
                size_t need = strlen(sd->name) + 1 + strlen(md->name) + 1;
                char *dbg = (char *)myon_xmalloc(need);
                snprintf(dbg, need, "%s.%s", sd->name, md->name);
                compile_function(c, md, dbg, sd);  /* sd => bind `self` at slot 0 */
                free(dbg);
            }
            break;
        }
        case STMT_RETURN:
            compile_return(c, s);
            break;
        default:
            compile_error(c, s->line, "MVM: unsupported statement kind %d", (int)s->kind);
            break;
    }
}

static void compile_block(Compiler *c, StmtList *list, int open_scope) {
    /* open_scope: 0 = none, 1 = implicit body scope, 2 = explicit `{ }` block */
    if (open_scope) begin_scope(c, open_scope == 2);
    for (int i = 0; i < list->count; i++)
        compile_stmt(c, list->items[i]);
    if (open_scope) end_scope(c);
}

/* ================================================================== */
/* function compilation                                                */
/* ================================================================== */

static int compile_function(Compiler *c, FuncDecl *decl, const char *dbg_name,
                            StructDecl *self_struct) {
    if (decl->is_async)
        unsupported(c, 0, "async functions", NULL);
    if (decl->tparam_count > 0)
        unsupported(c, 0, "generics", "Generic functions are not supported by MVM.");

    int cidx = module_add_chunk(c->module, dbg_name);

    FnComp fn;
    memset(&fn, 0, sizeof(fn));
    fn.enclosing = c->fn;
    fn.chunk_idx = cidx;
    fn.scope_depth = 0;

    FnComp *prev = c->fn;
    c->fn = &fn;

    /*
     * Methods bind `self` to slot 0 (first argument position); INVOKE places
     * the receiver there before the explicit arguments (spec §4.11).  We
     * remember the owning struct so `self.field` / `self.method()` inside the
     * body can be checked and dispatched statically.
     */
    int self_param = self_struct ? 1 : 0;
    if (self_struct) declare_local(c, "self");

    /* parameters occupy slots [self_param .. self_param+num_params-1] (§4.8) */
    for (int i = 0; i < decl->param_count; i++)
        declare_local(c, decl->params[i].name);

    compile_block(c, decl->body, 0);   /* function body is its own scope root */

    /* implicit return (spec §4.8): RET 0 */
    chunk_emit_op_u8(cur_chunk(c), MOP_RET, 0, 0);

    Chunk *ch = cur_chunk(c);
    ch->num_params = (uint16_t)(decl->param_count + self_param);
    ch->num_locals = (uint16_t)fn.max_slot;
    ch->ret_count  = (uint16_t)(decl->ret_count > 0 ? decl->ret_count : 0);

    c->fn = prev;
    return cidx;
}

/* ================================================================== */
/* interpolation sub-expression parsing helper                         */
/* ================================================================== */

static Program *parse_interp_expr(Compiler *c, int line, const char *src) {
    TokenList tl;
    if (!lexer_tokenize(src, &tl))
        compile_error(c, line, "lexical error in string interpolation");
    Program *p = parser_parse(&tl);
    token_list_free(&tl);
    if (!p || p->stmts.count != 1 || p->stmts.items[0]->kind != STMT_EXPR) {
        if (p) program_free(p);
        compile_error(c, line, "invalid expression in string interpolation");
    }
    return p;
}

/* ================================================================== */
/* Source Info hashing (spec §6.5) — FNV-1a 64 into the first 8 bytes  */
/* ================================================================== */

static void fill_source_info(Module *m, const char *path) {
    if (!path) return;
    m->src_path = myon_strdup(path);
    struct stat st;
    if (stat(path, &st) == 0) {
        m->src_mtime = (int64_t)st.st_mtime;
        m->src_size  = (uint64_t)st.st_size;
    }
    FILE *f = fopen(path, "rb");
    if (f) {
        uint64_t h = 1469598103934665603ULL;  /* FNV offset basis */
        int ch2;
        while ((ch2 = fgetc(f)) != EOF) {
            h ^= (uint8_t)ch2;
            h *= 1099511628211ULL;             /* FNV prime */
        }
        fclose(f);
        for (int i = 0; i < 8; i++) m->src_hash[i] = (uint8_t)((h >> (8 * i)) & 0xFF);
    }
}

/* ================================================================== */
/* entry point                                                         */
/* ================================================================== */

Module *mvm_compile_program(Program *program, const char *source_path) {
    Compiler c;
    memset(&c, 0, sizeof(c));
    c.module = module_new();

    /* top-level main chunk */
    int main_idx = module_add_chunk(c.module, "<main>");
    c.module->entry_chunk = main_idx;

    FnComp mainfn;
    memset(&mainfn, 0, sizeof(mainfn));
    mainfn.chunk_idx = main_idx;
    c.fn = &mainfn;

    if (setjmp(c.on_error)) {
        /* compile error already reported */
        if (c.structs) free(c.structs);
        module_free(c.module);
        return NULL;
    }

    /* Pre-pass: register all top-level struct declarations so forward
     * references (e.g. a method returning another struct) resolve. */
    for (int i = 0; i < program->stmts.count; i++)
        if (program->stmts.items[i]->kind == STMT_STRUCT)
            register_struct(&c, program->stmts.items[i]->as.struct_decl);

    /* Resolve parents and validate the inheritance chain, mirroring the
     * tree-walk interpreter's prescan (spec 14.6): a subclass may not
     * re-declare a field already present in any ancestor.  Enforcing this in
     * the compiler makes the .err case fail cleanly (no .myc emitted) exactly
     * as `.myon` does, keeping the two engines in agreement (Step 7-b). */
    for (int i = 0; i < c.struct_count; i++) {
        StructDecl *sd = c.structs[i];
        if (sd->parent_name && !sd->parent) {
            sd->parent = find_struct(&c, sd->parent_name);
            if (!sd->parent)
                compile_error(&c, 0, "struct '%s' extends unknown struct '%s'",
                              sd->name, sd->parent_name);
            for (int a = 0; a < sd->field_count; a++)
                for (StructDecl *pc = sd->parent; pc; pc = pc->parent)
                    for (int b = 0; b < pc->field_count; b++)
                        if (strcmp(sd->fields[a].name, pc->fields[b].name) == 0)
                            compile_error(&c, 0,
                                "field '%s' in struct '%s' collides with parent (spec 14.6)",
                                sd->fields[a].name, sd->name);
        }
    }

    for (int i = 0; i < program->stmts.count; i++)
        compile_stmt(&c, program->stmts.items[i]);

    /* main returns nothing */
    chunk_emit_op_u8(cur_chunk(&c), MOP_RET, 0, 0);

    Chunk *mc = cur_chunk(&c);
    mc->num_params = 0;
    mc->num_locals = (uint16_t)mainfn.max_slot;
    mc->ret_count  = 0;

    fill_source_info(c.module, source_path);

    if (c.structs) free(c.structs);
    return c.module;
}
