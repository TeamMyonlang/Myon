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

#include "parser.h"
#include "common.h"
#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

typedef struct {
    const Token *toks;
    int          count;
    int          pos;
    jmp_buf      escape;
    int          suppress_errors; /* >0 while doing speculative parsing */
} Parser;

static Expr *parse_expression(Parser *p);
static void  parse_block(Parser *p, StmtList *out);
static Stmt *parse_statement(Parser *p);
static TypeSpec *parse_type(Parser *p);
static FuncDecl *parse_func_decl(Parser *p, int is_async);

/* ---- token access ---- */
static const Token *peek(Parser *p)  { return &p->toks[p->pos]; }
static const Token *prev(Parser *p)  { return &p->toks[p->pos - 1]; }
static TokenType    cur(Parser *p)   { return p->toks[p->pos].type; }
static TokenType    peek_type(Parser *p, int k) { return p->toks[p->pos + k].type; }
static int          at_eof(Parser *p){ return cur(p) == TOK_EOF; }
static int          check(Parser *p, TokenType t) { return cur(p) == t; }

static const Token *advance(Parser *p) {
    if (!at_eof(p)) p->pos++;
    return prev(p);
}

static int match(Parser *p, TokenType t) {
    if (check(p, t)) { advance(p); return 1; }
    return 0;
}

static void perror_at(Parser *p, const Token *t, const char *msg) {
    /* During speculative parsing (e.g. distinguishing a generic
     * instantiation "Foo<int>(...)" from the comparison "i < 10") a failed
     * parse is expected and recovered from, so don't leak its diagnostic. */
    if (p->suppress_errors == 0) {
        /* P5: include the column and a friendly name for the offending token,
         * then print the source line with a caret under the failure column. */
        fprintf(stderr,
                "myon: syntax error at line %d, column %d: %s (got %s)\n",
                t->line, t->col, msg, diag_token_friendly(t->type));
        diag_print_snippet(t->line, t->col);
    }
    longjmp(p->escape, 1);
}

static const Token *expect(Parser *p, TokenType t, const char *msg) {
    if (check(p, t)) return advance(p);
    /* P5: if the caller passed a bare/short message, still surface the
     * expected token in a human-friendly form.  The msg already carries the
     * caller's intent (e.g. "expected ')' to close arguments"), so we keep it
     * verbatim; perror_at appends the friendly "got X" tail. */
    (void)t;
    perror_at(p, peek(p), msg);
    return NULL;
}

static void skip_terminators(Parser *p) {
    while (check(p, TOK_NEWLINE) || check(p, TOK_SEMICOLON)) advance(p);
}

/* ------------------------------------------------------------------ */
/* Type parsing (spec 13 `type`)                                       */
/* ------------------------------------------------------------------ */

static Type prim_token_to_type(TokenType t) {
    switch (t) {
        case TOK_KW_INT:   return TYPE_INT;
        case TOK_KW_FLOAT: return TYPE_FLOAT;
        case TOK_KW_STR:   return TYPE_STR;
        case TOK_KW_CHAR:  return TYPE_CHAR;
        case TOK_KW_BOOL:  return TYPE_BOOL;
        case TOK_KW_VOID:  return TYPE_VOID;
        case TOK_KW_ERROR: return TYPE_ERROR;
        default:           return TYPE_UNKNOWN;
    }
}

/* parse a comma-separated list of types inside < ... > */
static void parse_type_args(Parser *p, TypeSpec ***args_out, int *count_out) {
    TypeSpec **args = NULL;
    int count = 0;
    expect(p, TOK_LT, "expected '<'");
    do {
        TypeSpec *t = parse_type(p);
        args = (TypeSpec **)myon_xrealloc(args, sizeof(TypeSpec *) * (count + 1));
        args[count++] = t;
    } while (match(p, TOK_COMMA));
    expect(p, TOK_GT, "expected '>' to close type arguments");
    *args_out = args;
    *count_out = count;
}

static TypeSpec *parse_type(Parser *p) {
    TokenType tt = cur(p);
    Type prim = prim_token_to_type(tt);
    if (prim != TYPE_UNKNOWN) {
        advance(p);
        return typespec_prim(prim);
    }
    if (tt == TOK_MYON_ARRAY) {
        advance(p);
        expect(p, TOK_LPAREN, "expected '(' after myon.array");
        TypeSpec *elem = parse_type(p);
        expect(p, TOK_RPAREN, "expected ')' after array element type");
        TypeSpec *ts = typespec_new(TYPE_ARRAY);
        ts->elem = elem;
        return ts;
    }
    if (tt == TOK_MYON_MAP) {
        advance(p);
        expect(p, TOK_LPAREN, "expected '(' after myon.map");
        TypeSpec *k = parse_type(p);
        expect(p, TOK_COMMA, "expected ',' in myon.map(K, V)");
        TypeSpec *v = parse_type(p);
        expect(p, TOK_RPAREN, "expected ')' after map types");
        TypeSpec *ts = typespec_new(TYPE_MAP);
        ts->key = k;
        ts->elem = v;
        return ts;
    }
    if (tt == TOK_IDENT) {
        const Token *name = advance(p);
        TypeSpec *ts = typespec_named(name->lexeme);
        if (check(p, TOK_LT)) {
            parse_type_args(p, &ts->args, &ts->arg_count);
        }
        return ts;
    }
    perror_at(p, peek(p), "expected a type name");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Primary / postfix expressions                                       */
/* ------------------------------------------------------------------ */

static Expr *parse_ctor(Parser *p, ExprKind kind) {
    int line = peek(p)->line;
    advance(p);
    expect(p, TOK_LPAREN, "expected '(' after type constructor");
    Expr *inner = parse_expression(p);
    expect(p, TOK_RPAREN, "expected ')' to close constructor");
    return expr_ctor(kind, inner, line);
}

static void parse_arg_list(Parser *p, CallExpr *call);

/* myon.array(T) or myon.map(K,V) constructors */
static Expr *parse_array_ctor(Parser *p) {
    int line = peek(p)->line;
    advance(p); /* myon.array */
    expect(p, TOK_LPAREN, "expected '(' after myon.array");
    TypeSpec *elem = parse_type(p);
    expect(p, TOK_RPAREN, "expected ')' after array element type");
    return expr_array_ctor(elem, line);
}

static Expr *parse_map_ctor(Parser *p) {
    int line = peek(p)->line;
    advance(p); /* myon.map */
    expect(p, TOK_LPAREN, "expected '(' after myon.map");
    TypeSpec *k = parse_type(p);
    expect(p, TOK_COMMA, "expected ',' in myon.map(K, V)");
    TypeSpec *v = parse_type(p);
    expect(p, TOK_RPAREN, "expected ')' after map types");
    return expr_map_ctor(k, v, line);
}

/* myon.lambda(params) ret T { body } */
static Expr *parse_lambda(Parser *p) {
    int line = peek(p)->line;
    advance(p); /* myon.lambda */
    FuncDecl *fd = (FuncDecl *)myon_xmalloc(sizeof(FuncDecl));
    memset(fd, 0, sizeof(FuncDecl));

    expect(p, TOK_LPAREN, "expected '(' after myon.lambda");
    if (!check(p, TOK_RPAREN)) {
        do {
            const Token *pn = expect(p, TOK_IDENT, "expected parameter name");
            expect(p, TOK_COLON, "expected ':' after parameter name");
            TypeSpec *pt = parse_type(p);
            fd->params = (Param *)myon_xrealloc(fd->params, sizeof(Param) * (fd->param_count + 1));
            fd->params[fd->param_count].name = myon_strdup(pn->lexeme);
            fd->params[fd->param_count].type = pt;
            fd->param_count++;
        } while (match(p, TOK_COMMA));
    }
    expect(p, TOK_RPAREN, "expected ')' after lambda parameters");
    expect(p, TOK_KW_RET, "expected 'ret' after lambda parameters");
    /* one or more return types */
    do {
        TypeSpec *rt = parse_type(p);
        fd->ret_types = (TypeSpec **)myon_xrealloc(fd->ret_types, sizeof(TypeSpec *) * (fd->ret_count + 1));
        fd->ret_types[fd->ret_count++] = rt;
    } while (match(p, TOK_COMMA));

    fd->body = (StmtList *)myon_xmalloc(sizeof(StmtList));
    stmtlist_init(fd->body);
    parse_block(p, fd->body);
    return expr_lambda(fd, line);
}

/* True when a token can serve as a trailing segment in a dotted
 * "myon.<...>" name.  Besides plain identifiers, a handful of stdlib members
 * reuse type-name keywords as function names (myon.random.int / .float), so
 * those keyword tokens are accepted here as well. */
static int seg_is_name(TokenType t) {
    switch (t) {
        case TOK_IDENT:
        case TOK_KW_INT:
        case TOK_KW_FLOAT:
        case TOK_KW_STR:
        case TOK_KW_CHAR:
        case TOK_KW_BOOL:
            return 1;
        default:
            return 0;
    }
}

static Expr *parse_primary(Parser *p) {
    const Token *t = peek(p);
    int line = t->line;

    switch (t->type) {
        case TOK_INT: {
            advance(p);
            long long v;
            if (t->lexeme[0] == '0' && (t->lexeme[1] == 'o' || t->lexeme[1] == 'O'))
                v = strtoll(t->lexeme + 2, NULL, 8);
            else
                v = strtoll(t->lexeme, NULL, 0);
            return expr_int(v, line);
        }
        case TOK_FLOAT:
            advance(p);
            return expr_float(strtod(t->lexeme, NULL), line);
        case TOK_KW_TRUE:  advance(p); return expr_bool(1, line);
        case TOK_KW_FALSE: advance(p); return expr_bool(0, line);
        case TOK_STRING:
            advance(p);
            return expr_string(myon_strdup(t->lexeme), line);
        case TOK_MYON_NIL: advance(p); return expr_nil(line);
        case TOK_KW_STR:   return parse_ctor(p, EXPR_STR_CTOR);
        case TOK_KW_CHAR:  return parse_ctor(p, EXPR_CHAR_CTOR);
        case TOK_KW_INT:   return parse_ctor(p, EXPR_INT_CTOR);
        case TOK_KW_ERROR: return parse_ctor(p, EXPR_ERROR_CTOR);
        case TOK_MYON_ARRAY: return parse_array_ctor(p);
        case TOK_MYON_MAP:   return parse_map_ctor(p);
        case TOK_MYON_LAMBDA: return parse_lambda(p);
        case TOK_MYON_AWAIT: {
            advance(p);
            Expr *inner = parse_expression(p);
            return expr_await(inner, line);
        }
        case TOK_IDENT: {
            advance(p);
            /* generic instantiation: Ident < type , ... > ( ... )
             * Only treat as generic if the '<' is followed by a type and the
             * matching '>' is immediately followed by '(' (a constructor call)
             * so we don't misparse the comparison operator. */
            if (check(p, TOK_LT)) {
                int save = p->pos;
                /* lookahead: try parsing type args; if it fails, rewind */
                TypeSpec **args = NULL; int count = 0;
                jmp_buf saved; memcpy(&saved, &p->escape, sizeof(jmp_buf));
                p->suppress_errors++;
                if (setjmp(p->escape) == 0) {
                    parse_type_args(p, &args, &count);
                    memcpy(&p->escape, &saved, sizeof(jmp_buf));
                    p->suppress_errors--;
                    if (check(p, TOK_LPAREN)) {
                        return expr_generic(myon_strdup(t->lexeme), args, count, line);
                    }
                    /* Not a constructor call: rewind, treat '<' as comparison. */
                    for (int i = 0; i < count; i++) typespec_free(args[i]);
                    free(args);
                    p->pos = save;
                } else {
                    memcpy(&p->escape, &saved, sizeof(jmp_buf));
                    p->suppress_errors--;
                    p->pos = save;
                }
            }
            return expr_ident(myon_strdup(t->lexeme), line);
        }
        case TOK_KW_SELF:
            advance(p);
            return expr_ident(myon_strdup("self"), line);
        case TOK_MYON_PRINT:
            advance(p);
            return expr_ident(myon_strdup("myon.print"), line);
        case TOK_MYON_INPUT:
            advance(p);
            return expr_ident(myon_strdup("myon.input"), line);
        case TOK_KW_MYON: {
            /* bare "myon" followed by ".<ident>" — module-qualified access,
             * e.g. myon.math.sqrt or an aliased module member. Represent as a
             * dotted identifier so the interpreter can resolve it. */
            advance(p);
            char buf[128];
            size_t len = (size_t)snprintf(buf, sizeof(buf), "myon");
            /* A dotted segment is normally an identifier, but some stdlib
             * members reuse type-name keywords as function names
             * (e.g. myon.random.int / myon.random.float).  Accept those
             * keyword tokens as trailing segments too, using their lexeme. */
            while (check(p, TOK_DOT) && seg_is_name(peek_type(p, 1))) {
                advance(p); /* '.' */
                const Token *seg = advance(p);
                len += (size_t)snprintf(buf + len, sizeof(buf) - len, ".%s", seg->lexeme);
            }
            return expr_ident(myon_strdup(buf), line);
        }
        case TOK_KW_RANGE:
            perror_at(p, t, "'range' is only valid in a myon.for header");
            return NULL;
        case TOK_LPAREN: {
            advance(p);
            Expr *e = parse_expression(p);
            expect(p, TOK_RPAREN, "expected ')'");
            return e;
        }
        default:
            perror_at(p, t, "expected an expression");
            return NULL;
    }
}

static void parse_arg_list(Parser *p, CallExpr *call) {
    if (check(p, TOK_RPAREN)) return;
    do {
        char *name = NULL;
        if (check(p, TOK_IDENT) && peek_type(p, 1) == TOK_ASSIGN) {
            name = myon_strdup(peek(p)->lexeme);
            advance(p); /* ident */
            advance(p); /* '=' */
        }
        Expr *arg = parse_expression(p);
        call_add_arg(call, name, arg);
    } while (match(p, TOK_COMMA));
}

static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        int line = peek(p)->line;
        if (match(p, TOK_DOT)) {
            const Token *name = expect(p, TOK_IDENT, "expected member name after '.'");
            e = expr_member(e, myon_strdup(name->lexeme), line);
        } else if (match(p, TOK_LBRACKET)) {
            Expr *idx = parse_expression(p);
            expect(p, TOK_RBRACKET, "expected ']'");
            e = expr_index(e, idx, line);
        } else if (match(p, TOK_LPAREN)) {
            Expr *call = expr_call(e, line);
            parse_arg_list(p, &call->as.call);
            expect(p, TOK_RPAREN, "expected ')' to close call");
            e = call;
        } else {
            break;
        }
    }
    return e;
}

/* ------------------------------------------------------------------ */
/* Precedence climbing (spec 3.2)                                      */
/* ------------------------------------------------------------------ */

static Expr *parse_unary(Parser *p) {
    int line = peek(p)->line;
    if (match(p, TOK_MINUS))
        return expr_unary(OP_NEG, parse_unary(p), line);
    return parse_postfix(p);
}

static Expr *parse_multiplicative(Parser *p) {
    Expr *e = parse_unary(p);
    for (;;) {
        int line = peek(p)->line;
        if (match(p, TOK_STAR))       e = expr_binary(OP_MUL, e, parse_unary(p), line);
        else if (match(p, TOK_SLASH)) e = expr_binary(OP_DIV, e, parse_unary(p), line);
        else break;
    }
    return e;
}

static Expr *parse_additive(Parser *p) {
    Expr *e = parse_multiplicative(p);
    for (;;) {
        int line = peek(p)->line;
        if (match(p, TOK_PLUS))       e = expr_binary(OP_ADD, e, parse_multiplicative(p), line);
        else if (match(p, TOK_MINUS)) e = expr_binary(OP_SUB, e, parse_multiplicative(p), line);
        else break;
    }
    return e;
}

static Expr *parse_comparison(Parser *p) {
    Expr *e = parse_additive(p);
    int line = peek(p)->line;
    OpKind op;
    switch (cur(p)) {
        case TOK_EQ:  op = OP_EQ;  break;
        case TOK_NEQ: op = OP_NEQ; break;
        case TOK_LT:  op = OP_LT;  break;
        case TOK_GT:  op = OP_GT;  break;
        case TOK_LE:  op = OP_LE;  break;
        case TOK_GE:  op = OP_GE;  break;
        default: return e;
    }
    advance(p);
    Expr *r = parse_additive(p);
    return expr_binary(op, e, r, line);
}

static Expr *parse_not(Parser *p) {
    int line = peek(p)->line;
    if (match(p, TOK_MYON_NOT))
        return expr_unary(OP_NOT, parse_not(p), line);
    return parse_comparison(p);
}

static Expr *parse_and(Parser *p) {
    Expr *e = parse_not(p);
    while (check(p, TOK_MYON_AND)) {
        int line = peek(p)->line;
        advance(p);
        e = expr_logical(OP_AND, e, parse_not(p), line);
    }
    return e;
}

static Expr *parse_or(Parser *p) {
    Expr *e = parse_and(p);
    while (check(p, TOK_MYON_OR)) {
        int line = peek(p)->line;
        advance(p);
        e = expr_logical(OP_OR, e, parse_and(p), line);
    }
    return e;
}

static Expr *parse_expression(Parser *p) {
    return parse_or(p);
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

static Stmt *parse_system(Parser *p) {
    int line = peek(p)->line;
    advance(p); /* system */
    expect(p, TOK_KW_MYON, "expected 'myon' after 'system'");
    expect(p, TOK_DOT, "expected '.' after 'myon'");
    const Token *field = expect(p, TOK_IDENT, "expected 'useversion'");
    if (strcmp(field->lexeme, "useversion") != 0)
        perror_at(p, field, "expected 'useversion'");
    expect(p, TOK_ASSIGN, "expected '=' in system declaration");
    const Token *v = expect(p, TOK_INT, "expected integer version");
    Stmt *s = stmt_new(STMT_SYSTEM, line);
    s->as.system_decl.version = (int)strtol(v->lexeme, NULL, 0);
    return s;
}

static Stmt *parse_module(Parser *p) {
    int line = peek(p)->line;
    advance(p); /* module */

    char buf[256];
    size_t len = 0;
    buf[0] = '\0';

    const Token *head = peek(p);
    if (head->type == TOK_KW_MYON || head->type == TOK_IDENT) {
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s", head->lexeme);
        advance(p);
    } else {
        perror_at(p, head, "expected module path");
    }
    while (match(p, TOK_DOT)) {
        const Token *seg = expect(p, TOK_IDENT, "expected identifier in module path");
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, ".%s", seg->lexeme);
    }

    char *alias = NULL;
    if (match(p, TOK_KW_AS)) {
        const Token *a = expect(p, TOK_IDENT, "expected alias identifier after 'as'");
        alias = myon_strdup(a->lexeme);
    }

    Stmt *s = stmt_new(STMT_MODULE, line);
    s->as.module_decl.path = myon_strdup(buf);
    s->as.module_decl.alias = alias;
    return s;
}

/* Does the current position start a simple-target assignment?
 * Handles: IDENT (":" ...)? "=" | IDENT compound_op | IDENT ("," IDENT)+ "=" */
static int looks_like_assignment(Parser *p) {
    if (!check(p, TOK_IDENT)) return 0;
    int i = p->pos + 1;
    if (p->toks[i].type == TOK_COLON) return 1;
    if (p->toks[i].type == TOK_ASSIGN) return 1;
    switch (p->toks[i].type) {
        case TOK_PLUS_EQ: case TOK_MINUS_EQ:
        case TOK_STAR_EQ: case TOK_SLASH_EQ: return 1;
        default: break;
    }
    if (p->toks[i].type == TOK_COMMA) {
        while (p->toks[i].type == TOK_COMMA) {
            if (p->toks[i + 1].type != TOK_IDENT) return 0;
            i += 2;
        }
        return p->toks[i].type == TOK_ASSIGN;
    }
    return 0;
}

static Stmt *parse_assignment(Parser *p) {
    int line = peek(p)->line;
    const Token *name = expect(p, TOK_IDENT, "expected identifier");
    Stmt *s = stmt_new(STMT_ASSIGN, line);
    s->as.assign.name = myon_strdup(name->lexeme);
    s->as.assign.annotated = NULL;
    s->as.assign.compound = (OpKind)-1;

    while (match(p, TOK_COMMA)) {
        const Token *extra = expect(p, TOK_IDENT, "expected identifier in assignment target");
        int n = s->as.assign.extra_count + 1;
        s->as.assign.extra_names = (char **)myon_xrealloc(
            s->as.assign.extra_names, sizeof(char *) * n);
        s->as.assign.extra_names[s->as.assign.extra_count] = myon_strdup(extra->lexeme);
        s->as.assign.extra_count = n;
    }

    if (match(p, TOK_COLON)) {
        s->as.assign.annotated = parse_type(p);
        s->as.assign.has_type = 1;
    }

    if (match(p, TOK_ASSIGN)) {
        /* plain */
    } else if (check(p, TOK_PLUS_EQ) || check(p, TOK_MINUS_EQ) ||
               check(p, TOK_STAR_EQ) || check(p, TOK_SLASH_EQ)) {
        switch (cur(p)) {
            case TOK_PLUS_EQ:  s->as.assign.compound = OP_ADD; break;
            case TOK_MINUS_EQ: s->as.assign.compound = OP_SUB; break;
            case TOK_STAR_EQ:  s->as.assign.compound = OP_MUL; break;
            case TOK_SLASH_EQ: s->as.assign.compound = OP_DIV; break;
            default: break;
        }
        s->as.assign.is_compound = 1;
        advance(p);
    } else {
        perror_at(p, peek(p), "expected '=' or compound assignment operator");
    }

    s->as.assign.value = parse_expression(p);
    return s;
}

static Stmt *parse_if(Parser *p) {
    int line = peek(p)->line;
    advance(p); /* myon.if */
    Stmt *s = stmt_new(STMT_IF, line);
    s->as.if_stmt.cond = parse_expression(p);
    match(p, TOK_KW_THEN);  /* accept optional 'then' (spec samples use it) */
    stmtlist_init(&s->as.if_stmt.then_body);
    parse_block(p, &s->as.if_stmt.then_body);

    s->as.if_stmt.elifs = NULL;
    s->as.if_stmt.elif_count = 0;

    while (check(p, TOK_MYON_ELIF)) {
        advance(p);
        int n = s->as.if_stmt.elif_count + 1;
        s->as.if_stmt.elifs = (ElifArm *)myon_xrealloc(
            s->as.if_stmt.elifs, sizeof(ElifArm) * n);
        ElifArm *arm = &s->as.if_stmt.elifs[s->as.if_stmt.elif_count];
        arm->cond = parse_expression(p);
        expect(p, TOK_KW_THEN, "expected 'then' after myon.elif condition");
        stmtlist_init(&arm->body);
        parse_block(p, &arm->body);
        s->as.if_stmt.elif_count = n;
    }

    if (check(p, TOK_MYON_ELSE)) {
        advance(p);
        s->as.if_stmt.has_else = 1;
        stmtlist_init(&s->as.if_stmt.else_body);
        parse_block(p, &s->as.if_stmt.else_body);
    }
    return s;
}

static Stmt *parse_while(Parser *p) {
    int line = peek(p)->line;
    advance(p);
    Stmt *s = stmt_new(STMT_WHILE, line);
    s->as.while_stmt.cond = parse_expression(p);
    stmtlist_init(&s->as.while_stmt.body);
    parse_block(p, &s->as.while_stmt.body);
    return s;
}

static Stmt *parse_for(Parser *p) {
    int line = peek(p)->line;
    advance(p);
    const Token *var = expect(p, TOK_IDENT, "expected loop variable after myon.for");
    expect(p, TOK_MYON_IN, "expected myon.in in for header");

    Stmt *s = stmt_new(STMT_FOR, line);
    s->as.for_stmt.var = myon_strdup(var->lexeme);

    if (check(p, TOK_KW_RANGE)) {
        advance(p);
        expect(p, TOK_LPAREN, "expected '(' after range");
        s->as.for_stmt.is_range = 1;
        s->as.for_stmt.range_start = parse_expression(p);
        expect(p, TOK_COMMA, "expected ',' in range(...)");
        s->as.for_stmt.range_end = parse_expression(p);
        expect(p, TOK_RPAREN, "expected ')' to close range(...)");
    } else {
        s->as.for_stmt.is_range = 0;
        s->as.for_stmt.iterable = parse_expression(p);
    }

    stmtlist_init(&s->as.for_stmt.body);
    parse_block(p, &s->as.for_stmt.body);
    return s;
}

/* type_params = "<" identifier { "," identifier } ">" */
static void parse_tparams(Parser *p, char ***names_out, int *count_out) {
    char **names = NULL;
    int count = 0;
    expect(p, TOK_LT, "expected '<'");
    do {
        const Token *n = expect(p, TOK_IDENT, "expected type parameter name");
        names = (char **)myon_xrealloc(names, sizeof(char *) * (count + 1));
        names[count++] = myon_strdup(n->lexeme);
    } while (match(p, TOK_COMMA));
    expect(p, TOK_GT, "expected '>' to close type parameters");
    *names_out = names;
    *count_out = count;
}

/* func_decl = [async] myon.func ident [tparams] "(" params ")" ret types block */
static FuncDecl *parse_func_decl(Parser *p, int is_async) {
    FuncDecl *fd = (FuncDecl *)myon_xmalloc(sizeof(FuncDecl));
    memset(fd, 0, sizeof(FuncDecl));
    fd->is_async = is_async;

    advance(p); /* myon.func */
    const Token *name = expect(p, TOK_IDENT, "expected function name");
    fd->name = myon_strdup(name->lexeme);

    if (check(p, TOK_LT))
        parse_tparams(p, &fd->tparams, &fd->tparam_count);

    expect(p, TOK_LPAREN, "expected '(' after function name");
    if (!check(p, TOK_RPAREN)) {
        do {
            const Token *pn = expect(p, TOK_IDENT, "expected parameter name");
            expect(p, TOK_COLON, "expected ':' after parameter name (types are required)");
            TypeSpec *pt = parse_type(p);
            fd->params = (Param *)myon_xrealloc(fd->params, sizeof(Param) * (fd->param_count + 1));
            fd->params[fd->param_count].name = myon_strdup(pn->lexeme);
            fd->params[fd->param_count].type = pt;
            fd->param_count++;
        } while (match(p, TOK_COMMA));
    }
    expect(p, TOK_RPAREN, "expected ')' after parameters");
    expect(p, TOK_KW_RET, "expected 'ret' return-type clause");
    do {
        TypeSpec *rt = parse_type(p);
        fd->ret_types = (TypeSpec **)myon_xrealloc(fd->ret_types, sizeof(TypeSpec *) * (fd->ret_count + 1));
        fd->ret_types[fd->ret_count++] = rt;
    } while (match(p, TOK_COMMA));

    fd->body = (StmtList *)myon_xmalloc(sizeof(StmtList));
    stmtlist_init(fd->body);
    parse_block(p, fd->body);
    return fd;
}

static Stmt *parse_func_stmt(Parser *p, int is_async) {
    int line = peek(p)->line;
    Stmt *s = stmt_new(STMT_FUNC, line);
    s->as.func = parse_func_decl(p, is_async);
    return s;
}

/* struct_decl = myon.struct ident [tparams] [extends ident] "{" ... "}" */
static Stmt *parse_struct(Parser *p) {
    int line = peek(p)->line;
    advance(p); /* myon.struct */
    StructDecl *sd = (StructDecl *)myon_xmalloc(sizeof(StructDecl));
    memset(sd, 0, sizeof(StructDecl));

    const Token *name = expect(p, TOK_IDENT, "expected struct name");
    sd->name = myon_strdup(name->lexeme);

    if (check(p, TOK_LT))
        parse_tparams(p, &sd->tparams, &sd->tparam_count);

    if (match(p, TOK_MYON_EXTENDS)) {
        const Token *parent = expect(p, TOK_IDENT, "expected parent struct name after myon.extends");
        sd->parent_name = myon_strdup(parent->lexeme);
    }

    expect(p, TOK_LBRACE, "expected '{' to open struct body");
    skip_terminators(p);
    while (!check(p, TOK_RBRACE) && !at_eof(p)) {
        if (check(p, TOK_MYON_FUNC)) {
            FuncDecl *m = parse_func_decl(p, 0);
            sd->methods = (FuncDecl **)myon_xrealloc(sd->methods, sizeof(FuncDecl *) * (sd->method_count + 1));
            sd->methods[sd->method_count++] = m;
        } else {
            const Token *fn = expect(p, TOK_IDENT, "expected field name");
            expect(p, TOK_COLON, "expected ':' after field name");
            TypeSpec *ft = parse_type(p);
            sd->fields = (StructField *)myon_xrealloc(sd->fields, sizeof(StructField) * (sd->field_count + 1));
            sd->fields[sd->field_count].name = myon_strdup(fn->lexeme);
            sd->fields[sd->field_count].type = ft;
            sd->field_count++;
        }
        skip_terminators(p);
    }
    expect(p, TOK_RBRACE, "expected '}' to close struct body");

    Stmt *s = stmt_new(STMT_STRUCT, line);
    s->as.struct_decl = sd;
    return s;
}

/* return_stmt = "ret" expression { "," expression } */
static Stmt *parse_return(Parser *p) {
    int line = peek(p)->line;
    advance(p); /* ret */
    Stmt *s = stmt_new(STMT_RETURN, line);
    s->as.ret.values = NULL;
    s->as.ret.count = 0;
    /* An empty ret (bare) is not in the grammar; ret void uses the 'void'
     * type keyword as an expression sentinel handled by the interpreter. */
    if (check(p, TOK_KW_VOID)) {
        advance(p);
        return s; /* count 0 => void */
    }
    if (!check(p, TOK_NEWLINE) && !check(p, TOK_SEMICOLON) &&
        !check(p, TOK_RBRACE) && !at_eof(p)) {
        do {
            Expr *v = parse_expression(p);
            s->as.ret.values = (Expr **)myon_xrealloc(s->as.ret.values, sizeof(Expr *) * (s->as.ret.count + 1));
            s->as.ret.values[s->as.ret.count++] = v;
        } while (match(p, TOK_COMMA));
    }
    return s;
}

static void parse_block(Parser *p, StmtList *out) {
    expect(p, TOK_LBRACE, "expected '{' to open block");
    skip_terminators(p);
    while (!check(p, TOK_RBRACE) && !at_eof(p)) {
        Stmt *s = parse_statement(p);
        if (s) stmtlist_push(out, s);
        skip_terminators(p);
    }
    expect(p, TOK_RBRACE, "expected '}' to close block");
}

/* Convert a trailing expression statement into a member/index assignment
 * when it is followed by '=' (e.g. p.name = ..., xs[0] = ...). */
static Stmt *parse_expr_or_target_assign(Parser *p) {
    int line = peek(p)->line;
    Expr *e = parse_expression(p);
    if ((e->kind == EXPR_MEMBER || e->kind == EXPR_INDEX) && check(p, TOK_ASSIGN)) {
        advance(p); /* '=' */
        Stmt *s = stmt_new(STMT_ASSIGN, line);
        s->as.assign.target = e;
        s->as.assign.compound = (OpKind)-1;
        s->as.assign.value = parse_expression(p);
        return s;
    }
    if ((e->kind == EXPR_MEMBER || e->kind == EXPR_INDEX) &&
        (check(p, TOK_PLUS_EQ) || check(p, TOK_MINUS_EQ) ||
         check(p, TOK_STAR_EQ) || check(p, TOK_SLASH_EQ))) {
        Stmt *s = stmt_new(STMT_ASSIGN, line);
        s->as.assign.target = e;
        switch (cur(p)) {
            case TOK_PLUS_EQ:  s->as.assign.compound = OP_ADD; break;
            case TOK_MINUS_EQ: s->as.assign.compound = OP_SUB; break;
            case TOK_STAR_EQ:  s->as.assign.compound = OP_MUL; break;
            case TOK_SLASH_EQ: s->as.assign.compound = OP_DIV; break;
            default: break;
        }
        s->as.assign.is_compound = 1;
        advance(p);
        s->as.assign.value = parse_expression(p);
        return s;
    }
    Stmt *s = stmt_new(STMT_EXPR, line);
    s->as.expr = e;
    return s;
}

static Stmt *parse_statement(Parser *p) {
    switch (cur(p)) {
        case TOK_KW_SYSTEM:      return parse_system(p);
        case TOK_KW_MODULE:      return parse_module(p);
        case TOK_MYON_IF:        return parse_if(p);
        case TOK_MYON_WHILE:     return parse_while(p);
        case TOK_MYON_FOR:       return parse_for(p);
        case TOK_MYON_FUNC:      return parse_func_stmt(p, 0);
        case TOK_MYON_STRUCT:    return parse_struct(p);
        case TOK_KW_RET:         return parse_return(p);
        case TOK_MYON_ASYNC:
            advance(p); /* myon.async */
            if (!check(p, TOK_MYON_FUNC))
                perror_at(p, peek(p), "expected myon.func after myon.async");
            return parse_func_stmt(p, 1);
        case TOK_MYON_BREAK: {
            int line = peek(p)->line; advance(p);
            return stmt_new(STMT_BREAK, line);
        }
        case TOK_MYON_CONTINUE: {
            int line = peek(p)->line; advance(p);
            return stmt_new(STMT_CONTINUE, line);
        }
        case TOK_MYON_EXPOSE: {
            int line = peek(p)->line; advance(p);
            const Token *n = expect(p, TOK_IDENT, "expected identifier after myon.expose");
            Stmt *s = stmt_new(STMT_EXPOSE, line);
            s->as.expose_name = myon_strdup(n->lexeme);
            return s;
        }
        case TOK_LBRACE: {
            int line = peek(p)->line;
            Stmt *s = stmt_new(STMT_BLOCK, line);
            stmtlist_init(&s->as.block);
            parse_block(p, &s->as.block);
            return s;
        }
        default:
            if (looks_like_assignment(p))
                return parse_assignment(p);
            return parse_expr_or_target_assign(p);
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

Program *parser_parse(const TokenList *tokens) {
    Parser p;
    p.toks = tokens->items;
    p.count = tokens->count;
    p.pos = 0;
    p.suppress_errors = 0;

    Program *volatile prog = (Program *)myon_xmalloc(sizeof(Program));
    stmtlist_init(&prog->stmts);

    if (setjmp(p.escape)) {
        program_free(prog);
        return NULL;
    }

    skip_terminators(&p);
    while (!at_eof(&p)) {
        Stmt *s = parse_statement(&p);
        if (s) stmtlist_push(&prog->stmts, s);
        if (!at_eof(&p) && !check(&p, TOK_NEWLINE) && !check(&p, TOK_SEMICOLON)) {
            perror_at(&p, peek(&p), "expected end of statement");
        }
        skip_terminators(&p);
    }
    return prog;
}
