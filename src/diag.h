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

#ifndef MYON_DIAG_H
#define MYON_DIAG_H

#include "token.h"

/*
 * Diagnostics helpers (spec P5 — detailed error messages).
 *
 * The lexer, parser and interpreter all share a single "current source"
 * pointer so that error reporting can print the offending source line with a
 * caret ('^') under the failing column, in the style of gcc/rustc.
 *
 * The pointer is borrowed, not owned: the caller (main.c / REPL) sets it to
 * the buffer it is about to tokenize and clears it when that buffer is freed.
 */

/* Register / clear the source text used for caret snippets. Borrowed pointer. */
void diag_set_source(const char *src);
void diag_clear_source(void);

/*
 * Print, to stderr, the source line `line` (1-based) followed by a caret
 * line that points at column `col` (1-based).  A no-op if no source has been
 * registered or the line/col are out of range.  Emits a trailing newline.
 */
void diag_print_snippet(int line, int col);

/*
 * Human-readable name for a token type, for user-facing "expected X" messages
 * (e.g. TOK_RPAREN -> "')'", TOK_NEWLINE -> "end of line").  Falls back to the
 * internal token_type_name() for tokens without a friendly form.
 */
const char *diag_token_friendly(TokenType type);

#endif /* MYON_DIAG_H */
