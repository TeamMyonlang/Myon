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

#ifndef MYON_AST_H
#define MYON_AST_H

/*
 * Abstract syntax tree node definitions.
 * Reference: spec.md section 13 (EBNF).
 *
 * Covers Steps 0-18: expressions, control flow, functions (6), arrays (7),
 * scope/expose (8), structs (9), string interpolation (10), modules (11),
 * maps (13), generics (15), async/await (17).
 */

#include "types.h"

typedef struct Expr Expr;
typedef struct Stmt Stmt;

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    EXPR_INT_LIT,
    EXPR_FLOAT_LIT,
    EXPR_BOOL_LIT,
    EXPR_STR_CTOR,     /* str(expr)   */
    EXPR_CHAR_CTOR,    /* char(expr)  */
    EXPR_INT_CTOR,     /* int(expr)   */
    EXPR_ERROR_CTOR,   /* error(expr) */
    EXPR_STRING,       /* raw "..." string literal (interpolation at eval, Step 10) */
    EXPR_NIL,          /* myon.nil    */
    EXPR_IDENT,
    EXPR_BINARY,
    EXPR_UNARY,
    EXPR_LOGICAL,      /* myon.and / myon.or */
    EXPR_CALL,
    EXPR_INDEX,        /* a[b]        */
    EXPR_MEMBER,       /* a.b         */
    EXPR_ARRAY_CTOR,   /* myon.array(T)          (Step 7) */
    EXPR_MAP_CTOR,     /* myon.map(K, V)         (Step 13) */
    EXPR_LAMBDA,       /* myon.lambda(...) ret T { } (Step 6) */
    EXPR_AWAIT,        /* myon.await expr        (Step 17) */
    EXPR_GENERIC       /* Ident<T,...>  generic instantiation (Step 15) */
} ExprKind;

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR,
    OP_NEG, OP_NOT
} OpKind;

typedef struct {
    Expr *callee;
    Expr **args;      /* positional arg expressions */
    char **arg_names; /* parallel to args; NULL entry => positional */
    int    arg_count;
    /* explicit generic type args, e.g. first<int>(...) — may be NULL */
    TypeSpec **type_args;
    int        type_arg_count;
} CallExpr;

/* ---- function / lambda declaration (shared by Step 6 & 9 methods) ---- */
typedef struct { char *name; TypeSpec *type; } Param;

typedef struct FuncDecl {
    char      *name;          /* NULL for a lambda */
    int        is_async;      /* myon.async (Step 17) */
    Param     *params;
    int        param_count;
    TypeSpec **ret_types;     /* one or more (multiple return, 6.2) */
    int        ret_count;
    /* generic type parameters, e.g. <T, U> (Step 15) */
    char     **tparams;
    int        tparam_count;
    struct StmtList *body;    /* pointer so Value/Obj can hold it opaquely */
} FuncDecl;

struct Expr {
    ExprKind kind;
    int      line;
    union {
        long long   int_val;     /* EXPR_INT_LIT  */
        double      float_val;   /* EXPR_FLOAT_LIT */
        int         bool_val;    /* EXPR_BOOL_LIT */
        char       *str_val;     /* EXPR_STRING (raw body)  */
        char       *ident;       /* EXPR_IDENT */
        Expr       *operand;     /* EXPR_*_CTOR / EXPR_AWAIT */
        struct { OpKind op; Expr *left; Expr *right; } binary; /* BINARY/LOGICAL */
        struct { OpKind op; Expr *operand; } unary;
        CallExpr    call;
        struct { Expr *target; Expr *index; } index;
        struct { Expr *target; char *name; } member;
        TypeSpec   *array_elem;  /* EXPR_ARRAY_CTOR */
        struct { TypeSpec *key; TypeSpec *val; } map_types; /* EXPR_MAP_CTOR */
        FuncDecl   *lambda;      /* EXPR_LAMBDA */
        struct { char *name; TypeSpec **args; int arg_count; } generic; /* EXPR_GENERIC */
    } as;
};

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    STMT_SYSTEM,
    STMT_MODULE,
    STMT_ASSIGN,
    STMT_EXPR,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_BLOCK,
    STMT_EXPOSE,
    STMT_FUNC,        /* function declaration (Step 6) */
    STMT_STRUCT,      /* struct declaration (Step 9) */
    STMT_RETURN       /* ret expr, expr, ... (Step 6) */
} StmtKind;

/* A block is a dynamic list of statements. */
struct StmtList {
    Stmt **items;
    int    count;
    int    capacity;
};
typedef struct StmtList StmtList;

/* One "myon.elif expr then { ... }" arm. */
typedef struct {
    Expr     *cond;
    StmtList  body;
} ElifArm;

/* ---- struct declaration (Step 9) ---- */
typedef struct StructField { char *name; TypeSpec *type; } StructField;

typedef struct StructDecl {
    char        *name;
    char        *parent_name;   /* myon.extends (may be NULL) */
    struct StructDecl *parent;  /* resolved at runtime (not owned) */
    StructField *fields;
    int          field_count;
    FuncDecl   **methods;
    int          method_count;
    char       **tparams;       /* generic type params (Step 15) */
    int          tparam_count;
} StructDecl;

struct Stmt {
    StmtKind kind;
    int      line;
    union {
        struct { int version; } system_decl;               /* STMT_SYSTEM */
        struct { char *path; char *alias; } module_decl;    /* STMT_MODULE */

        struct {
            char    *name;
            TypeSpec *annotated;   /* declared type (owned) or NULL */
            int      has_type;
            OpKind   compound;     /* OP_ADD/SUB/MUL/DIV for +=,... else -1 */
            int      is_compound;
            Expr    *value;
            /* multi-target assignment: `a, b = f()` */
            char   **extra_names;
            int      extra_count;
            /* assignment to a member/index target (a.b = v, a[i] = v) */
            Expr    *target;       /* non-NULL => LHS is target, `name` unused */
        } assign;                                           /* STMT_ASSIGN */

        Expr *expr;                                         /* STMT_EXPR */

        struct {
            Expr     *cond;
            StmtList  then_body;
            ElifArm  *elifs;
            int       elif_count;
            int       has_else;
            StmtList  else_body;
        } if_stmt;                                          /* STMT_IF */

        struct { Expr *cond; StmtList body; } while_stmt;   /* STMT_WHILE */

        struct {
            char    *var;
            int      is_range;
            Expr    *range_start;
            Expr    *range_end;
            Expr    *iterable;
            StmtList body;
        } for_stmt;                                         /* STMT_FOR */

        StmtList block;                                     /* STMT_BLOCK */
        char    *expose_name;                               /* STMT_EXPOSE */

        FuncDecl  *func;                                    /* STMT_FUNC */
        StructDecl *struct_decl;                            /* STMT_STRUCT */

        struct { Expr **values; int count; } ret;           /* STMT_RETURN */
    } as;
};

/* Whole program. */
typedef struct {
    StmtList stmts;
} Program;

/* ---- expression constructors ---- */
Expr *expr_int(long long v, int line);
Expr *expr_float(double v, int line);
Expr *expr_bool(int v, int line);
Expr *expr_string(char *raw, int line);
Expr *expr_nil(int line);
Expr *expr_ident(char *name, int line);
Expr *expr_ctor(ExprKind kind, Expr *operand, int line);
Expr *expr_binary(OpKind op, Expr *l, Expr *r, int line);
Expr *expr_logical(OpKind op, Expr *l, Expr *r, int line);
Expr *expr_unary(OpKind op, Expr *operand, int line);
Expr *expr_call(Expr *callee, int line);
Expr *expr_index(Expr *target, Expr *index, int line);
Expr *expr_member(Expr *target, char *name, int line);
Expr *expr_array_ctor(TypeSpec *elem, int line);
Expr *expr_map_ctor(TypeSpec *key, TypeSpec *val, int line);
Expr *expr_lambda(FuncDecl *decl, int line);
Expr *expr_await(Expr *operand, int line);
Expr *expr_generic(char *name, TypeSpec **args, int arg_count, int line);
void  call_add_arg(CallExpr *call, char *name, Expr *arg);

/* ---- statement constructors / lists ---- */
Stmt *stmt_new(StmtKind kind, int line);
void  stmtlist_init(StmtList *list);
void  stmtlist_push(StmtList *list, Stmt *s);
void  stmtlist_free(StmtList *list);

/* ---- destructors ---- */
void  expr_free(Expr *e);
void  stmt_free(Stmt *s);
void  program_free(Program *p);
void  funcdecl_free(FuncDecl *f);
void  structdecl_free(StructDecl *s);

#endif /* MYON_AST_H */
