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

#ifndef MYON_INTERPRETER_H
#define MYON_INTERPRETER_H

#include "ast.h"
#include "value.h"

/*
 * Tree-walking interpreter.
 * Implements Steps 3-5: assignments, myon.print, arithmetic with strict
 * type checking (spec 2.2), casts (2.3), and control flow (section 5).
 *
 * Returns 0 on success, non-zero on a runtime error (message printed).
 */
int interpret(Program *program);

/*
 * Persistent interpreter handle used by the interactive REPL (spec 12).
 *
 * Unlike interpret(), which builds and tears down a fresh interpreter for a
 * single program, these functions let a caller keep one interpreter alive
 * across many independently-parsed programs so that variables, functions and
 * structs defined earlier stay visible later.
 */
typedef struct Interp Interp;

/* Create an interpreter with an empty global environment. */
Interp *interp_create(void);

/* Free an interpreter and all state it owns. */
void interp_free(Interp *it);

/*
 * Run one parsed program against a persistent interpreter.  A setjmp barrier
 * is installed inside, so a runtime error aborts only the current program and
 * leaves the interpreter usable for the next call (returns non-zero).  On
 * success returns 0.  The program's AST is retained by the interpreter for
 * its lifetime (function/struct values may reference its nodes).
 */
int interp_run(Interp *it, Program *program);

/* ------------------------------------------------------------------ */
/* myon_bridge_* — shared runtime seam between the tree-walking          */
/* interpreter (`.myon`) and the MVM bytecode VM (`.myc`, Step 6).       */
/*                                                                       */
/* The MVM VM (src/mvm_vm.c) must never re-implement a built-in: every   */
/* arithmetic op, cast, error constructor, stdlib function (myon.math /  */
/* myon.string / myon.array / myon.map / myon.time / myon.random /       */
/* myon.file) and container method (array/map/str) it needs is executed  */
/* by the *exact* C code the tree-walker uses, reached through the thin  */
/* wrappers below.  This guarantees `.myon` and `.myc` behave            */
/* identically (docs/mvm_spec.md §2, §4.14) and keeps the standard       */
/* library single-sourced.                                               */
/*                                                                       */
/* These wrappers do not change the tree-walk execution path in any way; */
/* they only expose already-existing internal helpers under a stable     */
/* calling convention that works from pre-evaluated Values (the VM has   */
/* Values on its operand stack, not AST Exprs).                          */
/* ------------------------------------------------------------------ */

/*
 * Create/free an interpreter instance to host the shared stdlib state
 * (globals, PRNG seed, FFI/net/loop subsystems) for a VM run.  Same object
 * as interp_create()/interp_free(); named separately so the seam reads
 * clearly at the VM call sites.
 */
Interp *myon_bridge_interp_new(void);
void    myon_bridge_interp_free(Interp *it);

/*
 * The interpreter's runtime-error longjmp target.  The VM installs its own
 * setjmp on this buffer so that a runtime_error() raised deep inside a shared
 * stdlib function unwinds cleanly back to the VM driver (which then returns a
 * non-zero exit code, the error already printed in the tree-walker's format).
 * Returns a pointer to the interpreter's jmp_buf (as void* to avoid leaking
 * <setjmp.h> into this header).
 */
void *myon_bridge_error_buf(Interp *it);

/*
 * Binary / unary operators with the strict-typing semantics of spec §2.2.
 * myon_bridge_binary consumes (frees) both l and r and returns a fresh Value;
 * on a type error it prints a diagnostic and longjmps to the error buffer.
 * `op` is an OpKind (OP_ADD..OP_GE); MOP_STR_CONCAT maps to OP_ADD.
 */
Value myon_bridge_binary(Interp *it, int line, int op, Value l, Value r);
Value myon_bridge_neg(Interp *it, int line, Value v);   /* consumes v */
Value myon_bridge_not(Interp *it, int line, Value v);   /* consumes v */

/* Casts (spec §2.3).  Each consumes v and returns a fresh Value. */
Value myon_bridge_cast_str(Interp *it, int line, Value v);
Value myon_bridge_cast_int(Interp *it, int line, Value v);
Value myon_bridge_cast_char(Interp *it, int line, Value v);
/* error(v): build an error value from v (consumes v). */
Value myon_bridge_make_error(Interp *it, int line, Value v);

/*
 * Call a namespaced stdlib function (e.g. "myon.math.sqrt", "myon.print").
 * `args` are argc already-evaluated Values owned by the caller (the VM);
 * this wrapper does NOT free them (the VM frees its stack slots afterwards).
 * On success returns 1 and stores the result in *out; returns 0 if `name`
 * is not a known stdlib function.  A runtime error longjmps to the error buf.
 */
int myon_bridge_call_native(Interp *it, const char *name,
                            Value *args, int argc, int line, Value *out);

/*
 * Call a container method (array/map/str builtin methods such as push, len,
 * keys, ...) on receiver `recv` (borrowed; not freed here) with argc already-
 * evaluated Values in `args` (borrowed).  Stores the result in *out.  A
 * runtime error longjmps to the error buffer.
 */
void myon_bridge_call_method(Interp *it, Value recv, const char *method,
                             Value *args, int argc, int line, Value *out);

#endif /* MYON_INTERPRETER_H */
