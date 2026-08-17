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

#ifndef MYON_FFI_CALLBACK_H
#define MYON_FFI_CALLBACK_H

/*
 * Phase4.1, Step4 — C-to-Myon callbacks (limited).
 *
 * Lets a Myon function value be handed to a C library as a plain function
 * pointer that the library can call back later (e.g. SDL_SetEventFilter).
 *
 * Scope (intentionally narrow, do not widen):
 *   - up to 4 arguments, each int64 / pointer class only (no double args);
 *   - the return value is int64 only (a void-returning callback simply ignores
 *     the returned value);
 *   - a small fixed number of live callbacks at a time (MYON_FFI_CB_SLOTS).
 *
 * Design: libffi closures are NOT used (to avoid a new dependency).  Instead a
 * fixed grid of static C trampoline functions — one per (slot, arg-count)
 * combination — is compiled ahead of time.  Each trampoline knows its own slot
 * index at compile time, looks up the Myon function value registered in that
 * slot, and invokes it via the interpreter's call_function().  Single-threaded
 * use is assumed (Myon runs single-threaded).
 */

#include "value.h"

typedef struct Interp Interp;

/* Number of simultaneously-registerable callback slots. */
#define MYON_FFI_CB_SLOTS 16
/* Maximum arguments per callback (0..MYON_FFI_CB_MAX_ARGS). */
#define MYON_FFI_CB_MAX_ARGS 4

/*
 * Register `fn` (must be TYPE_FUNC) as a callback taking `arg_count`
 * (0..MYON_FFI_CB_MAX_ARGS) int64/pointer arguments and returning int64.
 * `it` is the interpreter context used when the callback fires.
 *
 * On success returns the raw C function pointer to hand to a C call (pass it as
 * a 'p' argument — carried as an int address on the Myon side).  Returns NULL
 * on failure (no free slot, arg_count out of range, fn not a function).
 */
void *ffi_callback_register(Interp *it, Value fn, int arg_count);

/*
 * Release a callback previously returned by ffi_callback_register, freeing its
 * slot for reuse.  A NULL or unknown pointer is a no-op.
 */
void ffi_callback_unregister(void *ptr);

/* Release every live callback (called from ffi_state_free / interp teardown). */
void ffi_callback_reset_all(void);

#endif /* MYON_FFI_CALLBACK_H */
