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

#include "lexer.h"
#include "common.h"

#include <ctype.h>
#include <string.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Lexer state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *src;
    size_t      pos;
    int         line;
    int         col;
    TokenList  *out;
    bool        error;
    /* Nesting depth of "()" and "[]".  While this is > 0 the lexer treats
     * newlines as insignificant whitespace so that argument lists / array
     * literals can span multiple lines (spec 1.4).  Curly braces "{}" are
     * intentionally NOT counted here: block bodies use newlines as statement
     * separators, so a newline inside "{}" must remain significant. */
    int         paren_depth;
} Lexer;

static void tl_push(TokenList *list, Token tok) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 16;
        list->items = (Token *)myon_xrealloc(list->items,
                                             sizeof(Token) * list->capacity);
    }
    list->items[list->count++] = tok;
}

void token_list_free(TokenList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->items[i].lexeme);
    }
    free(list->items);
    list->items = NULL;
    list->count = list->capacity = 0;
}

/* ------------------------------------------------------------------ */
/* Character helpers                                                   */
/* ------------------------------------------------------------------ */

static char peek(Lexer *lx)      { return lx->src[lx->pos]; }
static char peek2(Lexer *lx)     { return lx->src[lx->pos] ? lx->src[lx->pos + 1] : '\0'; }
static bool at_end(Lexer *lx)    { return lx->src[lx->pos] == '\0'; }

static char advance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else           { lx->col++; }
    return c;
}

static bool ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool ident_part(char c)  { return isalnum((unsigned char)c) || c == '_'; }

static void emit(Lexer *lx, TokenType type, char *lexeme, int line, int col) {
    Token t;
    t.type = type;
    t.lexeme = lexeme;
    t.line = line;
    t.col = col;
    tl_push(lx->out, t);
}

static void lex_error(Lexer *lx, const char *msg) {
    fprintf(stderr, "myon: lexical error at line %d, col %d: %s\n",
            lx->line, lx->col, msg);
    lx->error = true;
}

/* ------------------------------------------------------------------ */
/* Keyword tables                                                      */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; TokenType type; } KwEntry;

/* plain keywords (not prefixed with "myon.") */
static const KwEntry PLAIN_KW[] = {
    {"system", TOK_KW_SYSTEM}, {"module", TOK_KW_MODULE},
    {"ret",    TOK_KW_RET},    {"str",    TOK_KW_STR},
    {"char",   TOK_KW_CHAR},   {"int",    TOK_KW_INT},
    {"float",  TOK_KW_FLOAT},  {"bool",   TOK_KW_BOOL},
    {"void",   TOK_KW_VOID},   {"error",  TOK_KW_ERROR},
    {"then",   TOK_KW_THEN},   {"range",  TOK_KW_RANGE},
    {"self",   TOK_KW_SELF},   {"as",     TOK_KW_AS},
    {"true",   TOK_KW_TRUE},   {"false",  TOK_KW_FALSE},
    {NULL, 0}
};

/* "myon.<member>" compound keywords */
static const KwEntry MYON_KW[] = {
    {"if", TOK_MYON_IF},       {"elif", TOK_MYON_ELIF},
    {"else", TOK_MYON_ELSE},   {"while", TOK_MYON_WHILE},
    {"for", TOK_MYON_FOR},     {"in", TOK_MYON_IN},
    {"break", TOK_MYON_BREAK}, {"continue", TOK_MYON_CONTINUE},
    {"func", TOK_MYON_FUNC},   {"struct", TOK_MYON_STRUCT},
    {"array", TOK_MYON_ARRAY}, {"map", TOK_MYON_MAP},
    {"print", TOK_MYON_PRINT}, {"input", TOK_MYON_INPUT},
    {"and", TOK_MYON_AND},     {"or", TOK_MYON_OR},
    {"not", TOK_MYON_NOT},     {"nil", TOK_MYON_NIL},
    {"expose", TOK_MYON_EXPOSE},{"lambda", TOK_MYON_LAMBDA},
    {"extends", TOK_MYON_EXTENDS},{"async", TOK_MYON_ASYNC},
    {"await", TOK_MYON_AWAIT},
    {NULL, 0}
};

static TokenType lookup_kw(const KwEntry *table, const char *name, size_t len) {
    for (int i = 0; table[i].name; i++) {
        if (strlen(table[i].name) == len && strncmp(table[i].name, name, len) == 0)
            return table[i].type;
    }
    return TOK_EOF; /* sentinel: not found */
}

/* ------------------------------------------------------------------ */
/* Comment handling (1.1)                                              */
/* ------------------------------------------------------------------ */

/* Returns true if a comment was consumed. */
static bool skip_comment(Lexer *lx) {
    char c = peek(lx);
    if (c == '#') {
        /* '#' comment: no newline allowed => runs to end of line */
        while (!at_end(lx) && peek(lx) != '\n') advance(lx);
        return true;
    }
    if (c == '/' && peek2(lx) == '/') {
        /* '//' comment: runs to end of line */
        while (!at_end(lx) && peek(lx) != '\n') advance(lx);
        return true;
    }
    if (c == '/' && peek2(lx) == '*') {
        /* slash-star ... star-slash multi-line comment */
        advance(lx); advance(lx); /* consume the opening slash-star */
        while (!at_end(lx)) {
            if (peek(lx) == '*' && peek2(lx) == '/') {
                advance(lx); advance(lx);
                return true;
            }
            advance(lx);
        }
        lex_error(lx, "unterminated block comment");
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Scanners for individual token kinds                                 */
/* ------------------------------------------------------------------ */

/* number: int (dec/hex/oct) or float (with optional exponent). Ref 1.3, 14.1 */
static void scan_number(Lexer *lx) {
    int line = lx->line, col = lx->col;
    size_t start = lx->pos;

    if (peek(lx) == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
        advance(lx); advance(lx); /* 0x */
        if (!isxdigit((unsigned char)peek(lx))) { lex_error(lx, "invalid hex literal"); return; }
        while (isxdigit((unsigned char)peek(lx))) advance(lx);
        emit(lx, TOK_INT, myon_strndup(lx->src + start, lx->pos - start), line, col);
        return;
    }
    if (peek(lx) == '0' && (peek2(lx) == 'o' || peek2(lx) == 'O')) {
        advance(lx); advance(lx); /* 0o */
        if (peek(lx) < '0' || peek(lx) > '7') { lex_error(lx, "invalid octal literal"); return; }
        while (peek(lx) >= '0' && peek(lx) <= '7') advance(lx);
        emit(lx, TOK_INT, myon_strndup(lx->src + start, lx->pos - start), line, col);
        return;
    }

    while (isdigit((unsigned char)peek(lx))) advance(lx);

    bool is_float = false;
    if (peek(lx) == '.' && isdigit((unsigned char)peek2(lx))) {
        is_float = true;
        advance(lx); /* '.' */
        while (isdigit((unsigned char)peek(lx))) advance(lx);
    }
    if (peek(lx) == 'e' || peek(lx) == 'E') {
        char n = peek2(lx);
        if (isdigit((unsigned char)n) || ((n == '+' || n == '-'))) {
            is_float = true;
            advance(lx); /* e/E */
            if (peek(lx) == '+' || peek(lx) == '-') advance(lx);
            if (!isdigit((unsigned char)peek(lx))) { lex_error(lx, "invalid exponent"); return; }
            while (isdigit((unsigned char)peek(lx))) advance(lx);
        }
    }

    emit(lx, is_float ? TOK_FLOAT : TOK_INT,
         myon_strndup(lx->src + start, lx->pos - start), line, col);
}

/* string literal: raw body preserved, interpolation handled later (Step 10). */
static void scan_string(Lexer *lx) {
    int line = lx->line, col = lx->col;
    advance(lx); /* opening quote */
    size_t start = lx->pos;
    while (!at_end(lx) && peek(lx) != '"') {
        if (peek(lx) == '\\') {
            advance(lx); /* backslash */
            if (at_end(lx)) break;
            advance(lx); /* escaped char */
        } else {
            advance(lx);
        }
    }
    if (at_end(lx)) { lex_error(lx, "unterminated string literal"); return; }
    size_t len = lx->pos - start;
    advance(lx); /* closing quote */
    emit(lx, TOK_STRING, myon_strndup(lx->src + start, len), line, col);
}

/* identifier or keyword; folds "myon.<member>" into a compound token. */
static void scan_ident(Lexer *lx) {
    int line = lx->line, col = lx->col;
    size_t start = lx->pos;
    while (ident_part(peek(lx))) advance(lx);
    size_t len = lx->pos - start;
    const char *word = lx->src + start;

    /* Special handling for the "myon" namespace token. */
    if (len == 4 && strncmp(word, "myon", 4) == 0) {
        /* If followed by ".<member>" where member is a known myon keyword,
         * fold into a single compound token. Otherwise emit bare "myon". */
        if (peek(lx) == '.' && ident_start(peek2(lx))) {
            size_t save_pos = lx->pos;
            int save_line = lx->line, save_col = lx->col;
            advance(lx); /* '.' */
            size_t mstart = lx->pos;
            while (ident_part(peek(lx))) advance(lx);
            size_t mlen = lx->pos - mstart;
            TokenType t = lookup_kw(MYON_KW, lx->src + mstart, mlen);
            if (t != TOK_EOF) {
                emit(lx, t, myon_strndup(word, lx->pos - start), line, col);
                return;
            }
            /* Not a known myon keyword (e.g. myon.stdio, myon.useversion,
             * myon.print already handled): rewind and emit bare "myon";
             * the parser deals with the ".ident" postfix. */
            lx->pos = save_pos; lx->line = save_line; lx->col = save_col;
        }
        emit(lx, TOK_KW_MYON, myon_strndup(word, len), line, col);
        return;
    }

    TokenType kw = lookup_kw(PLAIN_KW, word, len);
    if (kw != TOK_EOF) {
        emit(lx, kw, myon_strndup(word, len), line, col);
    } else {
        emit(lx, TOK_IDENT, myon_strndup(word, len), line, col);
    }
}

/* ------------------------------------------------------------------ */
/* Operator / symbol scanning                                          */
/* ------------------------------------------------------------------ */

static void scan_operator(Lexer *lx) {
    int line = lx->line, col = lx->col;
    char c = advance(lx);
    char n = peek(lx);

    switch (c) {
        case '=':
            if (n == '=') { advance(lx); emit(lx, TOK_EQ, myon_strdup("=="), line, col); }
            else { emit(lx, TOK_ASSIGN, myon_strdup("="), line, col); }
            return;
        case '!':
            if (n == '=') { advance(lx); emit(lx, TOK_NEQ, myon_strdup("!="), line, col); }
            else { lex_error(lx, "unexpected '!'"); }
            return;
        case '<':
            if (n == '=') { advance(lx); emit(lx, TOK_LE, myon_strdup("<="), line, col); }
            else { emit(lx, TOK_LT, myon_strdup("<"), line, col); }
            return;
        case '>':
            if (n == '=') { advance(lx); emit(lx, TOK_GE, myon_strdup(">="), line, col); }
            else { emit(lx, TOK_GT, myon_strdup(">"), line, col); }
            return;
        case '+':
            if (n == '=') { advance(lx); emit(lx, TOK_PLUS_EQ, myon_strdup("+="), line, col); }
            else { emit(lx, TOK_PLUS, myon_strdup("+"), line, col); }
            return;
        case '-':
            if (n == '=') { advance(lx); emit(lx, TOK_MINUS_EQ, myon_strdup("-="), line, col); }
            else { emit(lx, TOK_MINUS, myon_strdup("-"), line, col); }
            return;
        case '*':
            if (n == '=') { advance(lx); emit(lx, TOK_STAR_EQ, myon_strdup("*="), line, col); }
            else { emit(lx, TOK_STAR, myon_strdup("*"), line, col); }
            return;
        case '/':
            if (n == '=') { advance(lx); emit(lx, TOK_SLASH_EQ, myon_strdup("/="), line, col); }
            else { emit(lx, TOK_SLASH, myon_strdup("/"), line, col); }
            return;
        case '.': emit(lx, TOK_DOT, myon_strdup("."), line, col); return;
        case ',': emit(lx, TOK_COMMA, myon_strdup(","), line, col); return;
        case ':': emit(lx, TOK_COLON, myon_strdup(":"), line, col); return;
        case ';': emit(lx, TOK_SEMICOLON, myon_strdup(";"), line, col); return;
        /* "(" and "[" increase paren depth so that newlines inside argument
         * lists / array literals are not treated as statement separators
         * (spec 1.4).  "{" does NOT: block bodies rely on newlines. */
        case '(': lx->paren_depth++; emit(lx, TOK_LPAREN, myon_strdup("("), line, col); return;
        case '[': lx->paren_depth++; emit(lx, TOK_LBRACKET, myon_strdup("["), line, col); return;
        case '{': emit(lx, TOK_LBRACE, myon_strdup("{"), line, col); return;
        /* matching closers decrease depth (clamped at 0 for malformed input). */
        case ')': if (lx->paren_depth > 0) lx->paren_depth--; emit(lx, TOK_RPAREN, myon_strdup(")"), line, col); return;
        case ']': if (lx->paren_depth > 0) lx->paren_depth--; emit(lx, TOK_RBRACKET, myon_strdup("]"), line, col); return;
        case '}': emit(lx, TOK_RBRACE, myon_strdup("}"), line, col); return;
        default: {
            char buf[64];
            snprintf(buf, sizeof(buf), "unexpected character '%c' (0x%02x)", c, (unsigned char)c);
            lex_error(lx, buf);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int lexer_tokenize(const char *source, TokenList *out) {
    Lexer lx;
    lx.src = source;
    lx.pos = 0;
    lx.line = 1;
    lx.col = 1;
    lx.out = out;
    lx.error = false;
    lx.paren_depth = 0;

    out->items = NULL;
    out->count = 0;
    out->capacity = 0;

    while (!at_end(&lx) && !lx.error) {
        char c = peek(&lx);

        /* whitespace (except newline) */
        if (c == ' ' || c == '\t' || c == '\r') { advance(&lx); continue; }

        /* newline => statement separator token (collapse consecutive).
         * Inside "()" or "[]" (paren_depth > 0) newlines are insignificant
         * and simply skipped, so argument lists and array literals may span
         * multiple lines (spec 1.4). */
        if (c == '\n') {
            int line = lx.line, col = lx.col;
            advance(&lx);
            if (lx.paren_depth > 0) continue; /* line continuation inside ()/[] */
            /* avoid emitting leading/duplicate newlines */
            if (out->count > 0 && out->items[out->count - 1].type != TOK_NEWLINE)
                emit(&lx, TOK_NEWLINE, myon_strdup("\\n"), line, col);
            continue;
        }

        /* comments */
        if (c == '#' || (c == '/' && (peek2(&lx) == '/' || peek2(&lx) == '*'))) {
            if (skip_comment(&lx)) continue;
        }

        if (isdigit((unsigned char)c)) { scan_number(&lx); continue; }
        if (c == '"')                  { scan_string(&lx); continue; }
        if (ident_start(c))            { scan_ident(&lx); continue; }

        scan_operator(&lx);
    }

    if (lx.error) {
        token_list_free(out);
        return 0;
    }

    /* strip a trailing newline token, then append EOF */
    if (out->count > 0 && out->items[out->count - 1].type == TOK_NEWLINE) {
        free(out->items[out->count - 1].lexeme);
        out->count--;
    }
    emit(&lx, TOK_EOF, NULL, lx.line, lx.col);
    return 1;
}
