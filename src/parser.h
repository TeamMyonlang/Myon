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

#ifndef MYON_PARSER_H
#define MYON_PARSER_H

#include "lexer.h"
#include "ast.h"

/*
 * Recursive-descent parser following spec.md section 13 (EBNF).
 * Operator precedence follows the table in section 3.2.
 *
 * Returns a heap-allocated Program on success (caller frees with
 * program_free), or NULL on a syntax error (diagnostic printed).
 */
Program *parser_parse(const TokenList *tokens);

#endif /* MYON_PARSER_H */
