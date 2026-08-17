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

#ifndef MYON_FFI_CALL_H
#define MYON_FFI_CALL_H

/*
 * Phase3 C FFI — dynamic call dispatch.
 *
 * Calls an arbitrary C function resolved via dlsym given a signature (kinds of
 * each argument) and an argument array, without libffi.  Implemented with a
 * table of per-arity C wrappers that cast the raw function pointer to the
 * concrete signature before calling (see ffi_call.c).
 *
 * ABI note (x86-64 System V): integer / pointer / string arguments travel in
 * the general-purpose register file while double arguments travel in the XMM
 * register file, independently and each in source order.  This layer supports
 * the common ordering where all integer-class arguments come before all
 * double arguments (which covers virtually every SDL2 function).  Any other
 * interleaving, or more than 6 arguments, is reported as unsupported.
 */

typedef enum {
    FFI_ARG_I64,
    FFI_ARG_F64,
    FFI_ARG_PTR,
    FFI_ARG_STR,
} FFIArgKind;

typedef union {
    long long   i;
    double      d;
    void       *p;
    const char *s;
} FFIArgValue;

typedef enum {
    FFI_RET_I64,
    FFI_RET_F64,
    FFI_RET_PTR,
    FFI_RET_VOID,
} FFIRetKind;

typedef union {
    long long i;
    double    d;
    void     *p;
} FFIRetValue;

/*
 * Invoke `fn` with the given arguments.
 *
 *   fn         : raw function pointer from dlsym
 *   args       : argument values (max 6)
 *   arg_kinds  : kind of each argument
 *   arg_count  : number of arguments (0..6)
 *   ret_kind   : expected return type
 *   out        : receives the return value (untouched for FFI_RET_VOID)
 *
 * Returns 1 on success, 0 if the arity/argument ordering is unsupported.
 */
int ffi_call_dispatch(void *fn,
                      const FFIArgValue *args,
                      const FFIArgKind *arg_kinds,
                      int arg_count,
                      FFIRetKind ret_kind,
                      FFIRetValue *out);

#endif /* MYON_FFI_CALL_H */
