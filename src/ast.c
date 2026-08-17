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

#include "ast.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Expression constructors                                             */
/* ------------------------------------------------------------------ */

static Expr *expr_alloc(ExprKind kind, int line) {
    Expr *e = (Expr *)myon_xmalloc(sizeof(Expr));
    memset(e, 0, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    return e;
}

Expr *expr_int(long long v, int line)   { Expr *e = expr_alloc(EXPR_INT_LIT, line);   e->as.int_val = v; return e; }
Expr *expr_float(double v, int line)    { Expr *e = expr_alloc(EXPR_FLOAT_LIT, line); e->as.float_val = v; return e; }
Expr *expr_bool(int v, int line)        { Expr *e = expr_alloc(EXPR_BOOL_LIT, line);  e->as.bool_val = v; return e; }
Expr *expr_string(char *raw, int line)  { Expr *e = expr_alloc(EXPR_STRING, line);    e->as.str_val = raw; return e; }
Expr *expr_nil(int line)                { return expr_alloc(EXPR_NIL, line); }
Expr *expr_ident(char *name, int line)  { Expr *e = expr_alloc(EXPR_IDENT, line);     e->as.ident = name; return e; }

Expr *expr_ctor(ExprKind kind, Expr *operand, int line) {
    Expr *e = expr_alloc(kind, line);
    e->as.operand = operand;
    return e;
}

Expr *expr_binary(OpKind op, Expr *l, Expr *r, int line) {
    Expr *e = expr_alloc(EXPR_BINARY, line);
    e->as.binary.op = op; e->as.binary.left = l; e->as.binary.right = r;
    return e;
}

Expr *expr_logical(OpKind op, Expr *l, Expr *r, int line) {
    Expr *e = expr_alloc(EXPR_LOGICAL, line);
    e->as.binary.op = op; e->as.binary.left = l; e->as.binary.right = r;
    return e;
}

Expr *expr_unary(OpKind op, Expr *operand, int line) {
    Expr *e = expr_alloc(EXPR_UNARY, line);
    e->as.unary.op = op; e->as.unary.operand = operand;
    return e;
}

Expr *expr_call(Expr *callee, int line) {
    Expr *e = expr_alloc(EXPR_CALL, line);
    e->as.call.callee = callee;
    return e;
}

Expr *expr_index(Expr *target, Expr *index, int line) {
    Expr *e = expr_alloc(EXPR_INDEX, line);
    e->as.index.target = target; e->as.index.index = index;
    return e;
}

Expr *expr_member(Expr *target, char *name, int line) {
    Expr *e = expr_alloc(EXPR_MEMBER, line);
    e->as.member.target = target; e->as.member.name = name;
    return e;
}

Expr *expr_array_ctor(TypeSpec *elem, int line) {
    Expr *e = expr_alloc(EXPR_ARRAY_CTOR, line);
    e->as.array_elem = elem;
    return e;
}

Expr *expr_map_ctor(TypeSpec *key, TypeSpec *val, int line) {
    Expr *e = expr_alloc(EXPR_MAP_CTOR, line);
    e->as.map_types.key = key; e->as.map_types.val = val;
    return e;
}

Expr *expr_lambda(FuncDecl *decl, int line) {
    Expr *e = expr_alloc(EXPR_LAMBDA, line);
    e->as.lambda = decl;
    return e;
}

Expr *expr_await(Expr *operand, int line) {
    Expr *e = expr_alloc(EXPR_AWAIT, line);
    e->as.operand = operand;
    return e;
}

Expr *expr_generic(char *name, TypeSpec **args, int arg_count, int line) {
    Expr *e = expr_alloc(EXPR_GENERIC, line);
    e->as.generic.name = name;
    e->as.generic.args = args;
    e->as.generic.arg_count = arg_count;
    return e;
}

void call_add_arg(CallExpr *call, char *name, Expr *arg) {
    int n = call->arg_count + 1;
    call->args = (Expr **)myon_xrealloc(call->args, sizeof(Expr *) * n);
    call->arg_names = (char **)myon_xrealloc(call->arg_names, sizeof(char *) * n);
    call->args[call->arg_count] = arg;
    call->arg_names[call->arg_count] = name;
    call->arg_count = n;
}

/* ------------------------------------------------------------------ */
/* Statement constructors / lists                                      */
/* ------------------------------------------------------------------ */

Stmt *stmt_new(StmtKind kind, int line) {
    Stmt *s = (Stmt *)myon_xmalloc(sizeof(Stmt));
    memset(s, 0, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    return s;
}

void stmtlist_init(StmtList *list) {
    list->items = NULL; list->count = 0; list->capacity = 0;
}

void stmtlist_push(StmtList *list, Stmt *s) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->items = (Stmt **)myon_xrealloc(list->items, sizeof(Stmt *) * list->capacity);
    }
    list->items[list->count++] = s;
}

/* ------------------------------------------------------------------ */
/* Destructors                                                         */
/* ------------------------------------------------------------------ */

void funcdecl_free(FuncDecl *f) {
    if (!f) return;
    free(f->name);
    for (int i = 0; i < f->param_count; i++) {
        free(f->params[i].name);
        typespec_free(f->params[i].type);
    }
    free(f->params);
    for (int i = 0; i < f->ret_count; i++)
        typespec_free(f->ret_types[i]);
    free(f->ret_types);
    for (int i = 0; i < f->tparam_count; i++)
        free(f->tparams[i]);
    free(f->tparams);
    if (f->body) {
        stmtlist_free(f->body);
        free(f->body);
    }
    free(f);
}

void structdecl_free(StructDecl *s) {
    if (!s) return;
    free(s->name);
    free(s->parent_name);
    for (int i = 0; i < s->field_count; i++) {
        free(s->fields[i].name);
        typespec_free(s->fields[i].type);
    }
    free(s->fields);
    for (int i = 0; i < s->method_count; i++)
        funcdecl_free(s->methods[i]);
    free(s->methods);
    for (int i = 0; i < s->tparam_count; i++)
        free(s->tparams[i]);
    free(s->tparams);
    free(s);
}

void expr_free(Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_STRING:    free(e->as.str_val); break;
        case EXPR_IDENT:     free(e->as.ident);   break;
        case EXPR_STR_CTOR:
        case EXPR_CHAR_CTOR:
        case EXPR_INT_CTOR:
        case EXPR_ERROR_CTOR:
        case EXPR_AWAIT:     expr_free(e->as.operand); break;
        case EXPR_BINARY:
        case EXPR_LOGICAL:
            expr_free(e->as.binary.left);
            expr_free(e->as.binary.right);
            break;
        case EXPR_UNARY:
            expr_free(e->as.unary.operand);
            break;
        case EXPR_CALL:
            expr_free(e->as.call.callee);
            for (int i = 0; i < e->as.call.arg_count; i++) {
                expr_free(e->as.call.args[i]);
                free(e->as.call.arg_names[i]);
            }
            free(e->as.call.args);
            free(e->as.call.arg_names);
            for (int i = 0; i < e->as.call.type_arg_count; i++)
                typespec_free(e->as.call.type_args[i]);
            free(e->as.call.type_args);
            break;
        case EXPR_INDEX:
            expr_free(e->as.index.target);
            expr_free(e->as.index.index);
            break;
        case EXPR_MEMBER:
            expr_free(e->as.member.target);
            free(e->as.member.name);
            break;
        case EXPR_ARRAY_CTOR:
            typespec_free(e->as.array_elem);
            break;
        case EXPR_MAP_CTOR:
            typespec_free(e->as.map_types.key);
            typespec_free(e->as.map_types.val);
            break;
        case EXPR_LAMBDA:
            funcdecl_free(e->as.lambda);
            break;
        case EXPR_GENERIC:
            free(e->as.generic.name);
            for (int i = 0; i < e->as.generic.arg_count; i++)
                typespec_free(e->as.generic.args[i]);
            free(e->as.generic.args);
            break;
        default: break;
    }
    free(e);
}

void stmt_free(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_MODULE:
            free(s->as.module_decl.path);
            free(s->as.module_decl.alias);
            break;
        case STMT_ASSIGN:
            free(s->as.assign.name);
            typespec_free(s->as.assign.annotated);
            expr_free(s->as.assign.value);
            expr_free(s->as.assign.target);
            for (int i = 0; i < s->as.assign.extra_count; i++)
                free(s->as.assign.extra_names[i]);
            free(s->as.assign.extra_names);
            break;
        case STMT_EXPR:
            expr_free(s->as.expr);
            break;
        case STMT_IF:
            expr_free(s->as.if_stmt.cond);
            stmtlist_free(&s->as.if_stmt.then_body);
            for (int i = 0; i < s->as.if_stmt.elif_count; i++) {
                expr_free(s->as.if_stmt.elifs[i].cond);
                stmtlist_free(&s->as.if_stmt.elifs[i].body);
            }
            free(s->as.if_stmt.elifs);
            if (s->as.if_stmt.has_else)
                stmtlist_free(&s->as.if_stmt.else_body);
            break;
        case STMT_WHILE:
            expr_free(s->as.while_stmt.cond);
            stmtlist_free(&s->as.while_stmt.body);
            break;
        case STMT_FOR:
            free(s->as.for_stmt.var);
            expr_free(s->as.for_stmt.range_start);
            expr_free(s->as.for_stmt.range_end);
            expr_free(s->as.for_stmt.iterable);
            stmtlist_free(&s->as.for_stmt.body);
            break;
        case STMT_BLOCK:
            stmtlist_free(&s->as.block);
            break;
        case STMT_EXPOSE:
            free(s->as.expose_name);
            break;
        case STMT_FUNC:
            funcdecl_free(s->as.func);
            break;
        case STMT_STRUCT:
            structdecl_free(s->as.struct_decl);
            break;
        case STMT_RETURN:
            for (int i = 0; i < s->as.ret.count; i++)
                expr_free(s->as.ret.values[i]);
            free(s->as.ret.values);
            break;
        default: break;
    }
    free(s);
}

void stmtlist_free(StmtList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++)
        stmt_free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = list->capacity = 0;
}

void program_free(Program *p) {
    if (!p) return;
    stmtlist_free(&p->stmts);
    free(p);
}
