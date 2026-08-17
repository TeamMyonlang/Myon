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

#include "diag.h"

#include <stdio.h>
#include <string.h>

/* Borrowed pointer to the source text currently being processed. */
static const char *g_source = NULL;

void diag_set_source(const char *src)   { g_source = src; }
void diag_clear_source(void)            { g_source = NULL; }

/* Return a pointer to the start of 1-based line `line`, or NULL. */
static const char *find_line_start(const char *src, int line) {
    if (line < 1) return NULL;
    const char *p = src;
    int cur = 1;
    while (cur < line && *p) {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line) return NULL;
    return p;
}

void diag_print_snippet(int line, int col) {
    if (!g_source || line < 1) return;
    const char *start = find_line_start(g_source, line);
    if (!start) return;

    /* Determine the length of this line (up to newline or EOF). */
    const char *end = start;
    while (*end && *end != '\n') end++;
    int len = (int)(end - start);

    /* Print the source line with a small line-number gutter. */
    fprintf(stderr, "  %4d | %.*s\n", line, len, start);

    /* Build the caret line: same indentation as the gutter + code. */
    fprintf(stderr, "       | ");
    if (col < 1) col = 1;
    for (int i = 1; i < col && i <= len + 1; i++) {
        /* Preserve tabs so the caret lines up with tab-indented code. */
        char c = (i - 1 < len) ? start[i - 1] : ' ';
        fputc(c == '\t' ? '\t' : ' ', stderr);
    }
    fputs("^\n", stderr);
}

const char *diag_token_friendly(TokenType type) {
    switch (type) {
        case TOK_INT:       return "an integer literal";
        case TOK_FLOAT:     return "a float literal";
        case TOK_STRING:    return "a string literal";
        case TOK_IDENT:     return "an identifier";
        case TOK_EQ:        return "'=='";
        case TOK_NEQ:       return "'!='";
        case TOK_LT:        return "'<'";
        case TOK_GT:        return "'>'";
        case TOK_LE:        return "'<='";
        case TOK_GE:        return "'>='";
        case TOK_PLUS:      return "'+'";
        case TOK_MINUS:     return "'-'";
        case TOK_STAR:      return "'*'";
        case TOK_SLASH:     return "'/'";
        case TOK_ASSIGN:    return "'='";
        case TOK_DOT:       return "'.'";
        case TOK_COMMA:     return "','";
        case TOK_COLON:     return "':'";
        case TOK_SEMICOLON: return "';'";
        case TOK_LPAREN:    return "'('";
        case TOK_RPAREN:    return "')'";
        case TOK_LBRACE:    return "'{'";
        case TOK_RBRACE:    return "'}'";
        case TOK_LBRACKET:  return "'['";
        case TOK_RBRACKET:  return "']'";
        case TOK_NEWLINE:   return "end of line";
        case TOK_EOF:       return "end of file";
        default:            return token_type_name(type);
    }
}
