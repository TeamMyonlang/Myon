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

#ifndef MYON_MVM_VM_H
#define MYON_MVM_VM_H

#include "mvm_chunk.h"
#include "ast.h"

/*
 * Step 6: the Myon Virtual Machine (MVM) bytecode execution engine
 * (docs/mvm_spec.md §1-§4, §8).
 *
 * The VM consumes a Module produced by the Step 5 compiler (fresh, in memory)
 * or reloaded from a `.myc` file, and executes it directly.  It reuses the
 * exact same runtime value representation (src/value.h) and — crucially — the
 * exact same built-in / stdlib implementations as the tree-walking interpreter
 * through the `myon_bridge_*` seam (src/interpreter.h), so that `.myon`
 * (tree-walk) and `.myc` (VM) execution behave identically (spec §2, §4.14).
 *
 * Design summary (spec §3):
 *   - two heap-allocated stacks: an operand stack (Value LIFO) and a call-frame
 *     stack.  Locals live at the base of the operand stack, addressed by slot;
 *   - a bounded call-frame stack (MVM_MAX_FRAMES) turns runaway recursion into
 *     a clean Myon runtime error instead of an OS SIGSEGV (spec §3.4);
 *   - a plain `switch` dispatch loop (spec §4.1 recommendation: portable C,
 *     no GCC computed-goto extension, so MSVC etc. can build it too).
 *
 * Structs / methods (spec §4.11) need their declarations (field order, method
 * bodies) which the current `.myc` format does not serialize; therefore struct
 * support is only available when the VM is given the source Program (the
 * "run compiled-in-memory" path).  See mvm_vm.c for the reported limitation.
 */

/*
 * Execute `m`.  `program` (may be NULL) supplies struct/method declarations for
 * NEW_STRUCT / GET_FIELD / SET_FIELD / INVOKE when the module was compiled in
 * memory from source; pass NULL when running a reloaded `.myc` (structs then
 * raise a clear runtime error).  `source` (may be NULL) is the original source
 * text, registered with diag so runtime errors can show a source snippet.
 *
 * Returns a process exit code: 0 on success, non-zero on a runtime error
 * (message already printed in the tree-walker's format, spec §8).
 */
int mvm_run_module(Module *m, Program *program, const char *source);

#endif /* MYON_MVM_VM_H */
