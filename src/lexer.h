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

#ifndef MYON_LEXER_H
#define MYON_LEXER_H

#include "token.h"

/*
 * The lexer scans the whole source at once and returns a dynamic
 * array of tokens terminated by a TOK_EOF token.
 * Reference: spec.md section 1.
 */
typedef struct {
    Token *items;
    int    count;
    int    capacity;
} TokenList;

/*
 * Tokenize `source`. On success returns 1 and fills `out`.
 * On a lexical error returns 0 and prints a diagnostic to stderr.
 * The caller owns `out` and must free it with token_list_free().
 */
int lexer_tokenize(const char *source, TokenList *out);

void token_list_free(TokenList *list);

#endif /* MYON_LEXER_H */
