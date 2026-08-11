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

#ifndef MYON_MVM_COMPILER_H
#define MYON_MVM_COMPILER_H

#include "ast.h"
#include "mvm_chunk.h"

/*
 * Step 5: AST -> MVM bytecode compiler (docs/mvm_spec.md).
 *
 * The compiler walks the Parser's Program (src/ast.h) and produces a Module
 * (src/mvm_chunk.h) of bytecode chunks + a shared constant pool, following the
 * instruction set fixed in the MVM spec.  It performs the static scope
 * resolution of §5 (compile-time local-slot assignment, shadowing rules,
 * myon.expose lifetime promotion) so the VM never has to search an Env at
 * runtime.
 *
 * Features that the MVM spec (§7) marks as out of scope — async/await,
 * myon.net / myon.http, myon.ffi, closures over outer locals, and generics —
 * are rejected here with a clear, diag-formatted compile error (the .myc is
 * not produced).  These are all funnelled through a single "unsupported"
 * reporting path so they are easy to lift later.
 */

/*
 * Compile a whole program to a fresh Module.  On success returns the Module
 * (caller owns it; free with module_free).  On a compile error, prints a
 * diagnostic (line + source snippet, matching src/diag.c) to stderr and
 * returns NULL.
 *
 * `source_path` (may be NULL) is recorded in the .myc Source Info (spec §6.5)
 * and used to fill mtime/size/hash; it does not need to be re-read.
 */
Module *mvm_compile_program(Program *program, const char *source_path);

#endif /* MYON_MVM_COMPILER_H */
