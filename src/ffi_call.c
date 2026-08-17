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

#include "ffi_call.h"

#include <stddef.h> /* size_t */

/*
 * Casting the dlsym-returned object pointer to a function pointer is
 * implementation-defined per ISO C but guaranteed to work on every platform
 * that provides a POSIX dlopen/dlsym (and on Windows via GetProcAddress).  It
 * is unavoidable for any dlopen-based FFI, so we locally silence the pedantic
 * diagnostic for this translation unit.
 */
#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif

/*
 * Dynamic C call without libffi.
 *
 * The four Myon-visible argument kinds collapse to two ABI classes:
 *   - integer class : i64 / ptr / str  -> general-purpose registers
 *   - float class   : f64              -> XMM registers
 *
 * We therefore only need one wrapper per (integer-count, double-count,
 * return-kind) triple.  Each wrapper casts the raw pointer to the concrete
 * C signature (integer-class parameters first, then doubles) and calls it.
 *
 * Supported ordering: all integer-class arguments precede all double
 * arguments.  This is the overwhelmingly common shape (all of SDL2's core
 * entry points fit it).  ffi_call_dispatch() rejects any interleaved ordering
 * or an arity above 6 by returning 0.
 *
 * NOTE: This file's wrapper table is machine-generated (see the generator in
 * the Phase3 commit history); edit the generator rather than by hand.
 */


static long long call_0i0d_to_i(void *fn) {
    long long (*f)(void) = (long long (*)(void))fn;
    return f();
}
static long long call_0i1d_to_i(void *fn, double d0) {
    long long (*f)(double) = (long long (*)(double))fn;
    return f(d0);
}
static long long call_0i2d_to_i(void *fn, double d0, double d1) {
    long long (*f)(double,double) = (long long (*)(double,double))fn;
    return f(d0, d1);
}
static long long call_0i3d_to_i(void *fn, double d0, double d1, double d2) {
    long long (*f)(double,double,double) = (long long (*)(double,double,double))fn;
    return f(d0, d1, d2);
}
static long long call_0i4d_to_i(void *fn, double d0, double d1, double d2, double d3) {
    long long (*f)(double,double,double,double) = (long long (*)(double,double,double,double))fn;
    return f(d0, d1, d2, d3);
}
static long long call_0i5d_to_i(void *fn, double d0, double d1, double d2, double d3, double d4) {
    long long (*f)(double,double,double,double,double) = (long long (*)(double,double,double,double,double))fn;
    return f(d0, d1, d2, d3, d4);
}
static long long call_0i6d_to_i(void *fn, double d0, double d1, double d2, double d3, double d4, double d5) {
    long long (*f)(double,double,double,double,double,double) = (long long (*)(double,double,double,double,double,double))fn;
    return f(d0, d1, d2, d3, d4, d5);
}
static long long call_1i0d_to_i(void *fn, long long i0) {
    long long (*f)(long long) = (long long (*)(long long))fn;
    return f(i0);
}
static long long call_1i1d_to_i(void *fn, long long i0, double d0) {
    long long (*f)(long long,double) = (long long (*)(long long,double))fn;
    return f(i0, d0);
}
static long long call_1i2d_to_i(void *fn, long long i0, double d0, double d1) {
    long long (*f)(long long,double,double) = (long long (*)(long long,double,double))fn;
    return f(i0, d0, d1);
}
static long long call_1i3d_to_i(void *fn, long long i0, double d0, double d1, double d2) {
    long long (*f)(long long,double,double,double) = (long long (*)(long long,double,double,double))fn;
    return f(i0, d0, d1, d2);
}
static long long call_1i4d_to_i(void *fn, long long i0, double d0, double d1, double d2, double d3) {
    long long (*f)(long long,double,double,double,double) = (long long (*)(long long,double,double,double,double))fn;
    return f(i0, d0, d1, d2, d3);
}
static long long call_1i5d_to_i(void *fn, long long i0, double d0, double d1, double d2, double d3, double d4) {
    long long (*f)(long long,double,double,double,double,double) = (long long (*)(long long,double,double,double,double,double))fn;
    return f(i0, d0, d1, d2, d3, d4);
}
static long long call_2i0d_to_i(void *fn, long long i0, long long i1) {
    long long (*f)(long long,long long) = (long long (*)(long long,long long))fn;
    return f(i0, i1);
}
static long long call_2i1d_to_i(void *fn, long long i0, long long i1, double d0) {
    long long (*f)(long long,long long,double) = (long long (*)(long long,long long,double))fn;
    return f(i0, i1, d0);
}
static long long call_2i2d_to_i(void *fn, long long i0, long long i1, double d0, double d1) {
    long long (*f)(long long,long long,double,double) = (long long (*)(long long,long long,double,double))fn;
    return f(i0, i1, d0, d1);
}
static long long call_2i3d_to_i(void *fn, long long i0, long long i1, double d0, double d1, double d2) {
    long long (*f)(long long,long long,double,double,double) = (long long (*)(long long,long long,double,double,double))fn;
    return f(i0, i1, d0, d1, d2);
}
static long long call_2i4d_to_i(void *fn, long long i0, long long i1, double d0, double d1, double d2, double d3) {
    long long (*f)(long long,long long,double,double,double,double) = (long long (*)(long long,long long,double,double,double,double))fn;
    return f(i0, i1, d0, d1, d2, d3);
}
static long long call_3i0d_to_i(void *fn, long long i0, long long i1, long long i2) {
    long long (*f)(long long,long long,long long) = (long long (*)(long long,long long,long long))fn;
    return f(i0, i1, i2);
}
static long long call_3i1d_to_i(void *fn, long long i0, long long i1, long long i2, double d0) {
    long long (*f)(long long,long long,long long,double) = (long long (*)(long long,long long,long long,double))fn;
    return f(i0, i1, i2, d0);
}
static long long call_3i2d_to_i(void *fn, long long i0, long long i1, long long i2, double d0, double d1) {
    long long (*f)(long long,long long,long long,double,double) = (long long (*)(long long,long long,long long,double,double))fn;
    return f(i0, i1, i2, d0, d1);
}
static long long call_3i3d_to_i(void *fn, long long i0, long long i1, long long i2, double d0, double d1, double d2) {
    long long (*f)(long long,long long,long long,double,double,double) = (long long (*)(long long,long long,long long,double,double,double))fn;
    return f(i0, i1, i2, d0, d1, d2);
}
static long long call_4i0d_to_i(void *fn, long long i0, long long i1, long long i2, long long i3) {
    long long (*f)(long long,long long,long long,long long) = (long long (*)(long long,long long,long long,long long))fn;
    return f(i0, i1, i2, i3);
}
static long long call_4i1d_to_i(void *fn, long long i0, long long i1, long long i2, long long i3, double d0) {
    long long (*f)(long long,long long,long long,long long,double) = (long long (*)(long long,long long,long long,long long,double))fn;
    return f(i0, i1, i2, i3, d0);
}
static long long call_4i2d_to_i(void *fn, long long i0, long long i1, long long i2, long long i3, double d0, double d1) {
    long long (*f)(long long,long long,long long,long long,double,double) = (long long (*)(long long,long long,long long,long long,double,double))fn;
    return f(i0, i1, i2, i3, d0, d1);
}
static long long call_5i0d_to_i(void *fn, long long i0, long long i1, long long i2, long long i3, long long i4) {
    long long (*f)(long long,long long,long long,long long,long long) = (long long (*)(long long,long long,long long,long long,long long))fn;
    return f(i0, i1, i2, i3, i4);
}
static long long call_5i1d_to_i(void *fn, long long i0, long long i1, long long i2, long long i3, long long i4, double d0) {
    long long (*f)(long long,long long,long long,long long,long long,double) = (long long (*)(long long,long long,long long,long long,long long,double))fn;
    return f(i0, i1, i2, i3, i4, d0);
}
static long long call_6i0d_to_i(void *fn, long long i0, long long i1, long long i2, long long i3, long long i4, long long i5) {
    long long (*f)(long long,long long,long long,long long,long long,long long) = (long long (*)(long long,long long,long long,long long,long long,long long))fn;
    return f(i0, i1, i2, i3, i4, i5);
}
static double call_0i0d_to_d(void *fn) {
    double (*f)(void) = (double (*)(void))fn;
    return f();
}
static double call_0i1d_to_d(void *fn, double d0) {
    double (*f)(double) = (double (*)(double))fn;
    return f(d0);
}
static double call_0i2d_to_d(void *fn, double d0, double d1) {
    double (*f)(double,double) = (double (*)(double,double))fn;
    return f(d0, d1);
}
static double call_0i3d_to_d(void *fn, double d0, double d1, double d2) {
    double (*f)(double,double,double) = (double (*)(double,double,double))fn;
    return f(d0, d1, d2);
}
static double call_0i4d_to_d(void *fn, double d0, double d1, double d2, double d3) {
    double (*f)(double,double,double,double) = (double (*)(double,double,double,double))fn;
    return f(d0, d1, d2, d3);
}
static double call_0i5d_to_d(void *fn, double d0, double d1, double d2, double d3, double d4) {
    double (*f)(double,double,double,double,double) = (double (*)(double,double,double,double,double))fn;
    return f(d0, d1, d2, d3, d4);
}
static double call_0i6d_to_d(void *fn, double d0, double d1, double d2, double d3, double d4, double d5) {
    double (*f)(double,double,double,double,double,double) = (double (*)(double,double,double,double,double,double))fn;
    return f(d0, d1, d2, d3, d4, d5);
}
static double call_1i0d_to_d(void *fn, long long i0) {
    double (*f)(long long) = (double (*)(long long))fn;
    return f(i0);
}
static double call_1i1d_to_d(void *fn, long long i0, double d0) {
    double (*f)(long long,double) = (double (*)(long long,double))fn;
    return f(i0, d0);
}
static double call_1i2d_to_d(void *fn, long long i0, double d0, double d1) {
    double (*f)(long long,double,double) = (double (*)(long long,double,double))fn;
    return f(i0, d0, d1);
}
static double call_1i3d_to_d(void *fn, long long i0, double d0, double d1, double d2) {
    double (*f)(long long,double,double,double) = (double (*)(long long,double,double,double))fn;
    return f(i0, d0, d1, d2);
}
static double call_1i4d_to_d(void *fn, long long i0, double d0, double d1, double d2, double d3) {
    double (*f)(long long,double,double,double,double) = (double (*)(long long,double,double,double,double))fn;
    return f(i0, d0, d1, d2, d3);
}
static double call_1i5d_to_d(void *fn, long long i0, double d0, double d1, double d2, double d3, double d4) {
    double (*f)(long long,double,double,double,double,double) = (double (*)(long long,double,double,double,double,double))fn;
    return f(i0, d0, d1, d2, d3, d4);
}
static double call_2i0d_to_d(void *fn, long long i0, long long i1) {
    double (*f)(long long,long long) = (double (*)(long long,long long))fn;
    return f(i0, i1);
}
static double call_2i1d_to_d(void *fn, long long i0, long long i1, double d0) {
    double (*f)(long long,long long,double) = (double (*)(long long,long long,double))fn;
    return f(i0, i1, d0);
}
static double call_2i2d_to_d(void *fn, long long i0, long long i1, double d0, double d1) {
    double (*f)(long long,long long,double,double) = (double (*)(long long,long long,double,double))fn;
    return f(i0, i1, d0, d1);
}
static double call_2i3d_to_d(void *fn, long long i0, long long i1, double d0, double d1, double d2) {
    double (*f)(long long,long long,double,double,double) = (double (*)(long long,long long,double,double,double))fn;
    return f(i0, i1, d0, d1, d2);
}
static double call_2i4d_to_d(void *fn, long long i0, long long i1, double d0, double d1, double d2, double d3) {
    double (*f)(long long,long long,double,double,double,double) = (double (*)(long long,long long,double,double,double,double))fn;
    return f(i0, i1, d0, d1, d2, d3);
}
static double call_3i0d_to_d(void *fn, long long i0, long long i1, long long i2) {
    double (*f)(long long,long long,long long) = (double (*)(long long,long long,long long))fn;
    return f(i0, i1, i2);
}
static double call_3i1d_to_d(void *fn, long long i0, long long i1, long long i2, double d0) {
    double (*f)(long long,long long,long long,double) = (double (*)(long long,long long,long long,double))fn;
    return f(i0, i1, i2, d0);
}
static double call_3i2d_to_d(void *fn, long long i0, long long i1, long long i2, double d0, double d1) {
    double (*f)(long long,long long,long long,double,double) = (double (*)(long long,long long,long long,double,double))fn;
    return f(i0, i1, i2, d0, d1);
}
static double call_3i3d_to_d(void *fn, long long i0, long long i1, long long i2, double d0, double d1, double d2) {
    double (*f)(long long,long long,long long,double,double,double) = (double (*)(long long,long long,long long,double,double,double))fn;
    return f(i0, i1, i2, d0, d1, d2);
}
static double call_4i0d_to_d(void *fn, long long i0, long long i1, long long i2, long long i3) {
    double (*f)(long long,long long,long long,long long) = (double (*)(long long,long long,long long,long long))fn;
    return f(i0, i1, i2, i3);
}
static double call_4i1d_to_d(void *fn, long long i0, long long i1, long long i2, long long i3, double d0) {
    double (*f)(long long,long long,long long,long long,double) = (double (*)(long long,long long,long long,long long,double))fn;
    return f(i0, i1, i2, i3, d0);
}
static double call_4i2d_to_d(void *fn, long long i0, long long i1, long long i2, long long i3, double d0, double d1) {
    double (*f)(long long,long long,long long,long long,double,double) = (double (*)(long long,long long,long long,long long,double,double))fn;
    return f(i0, i1, i2, i3, d0, d1);
}
static double call_5i0d_to_d(void *fn, long long i0, long long i1, long long i2, long long i3, long long i4) {
    double (*f)(long long,long long,long long,long long,long long) = (double (*)(long long,long long,long long,long long,long long))fn;
    return f(i0, i1, i2, i3, i4);
}
static double call_5i1d_to_d(void *fn, long long i0, long long i1, long long i2, long long i3, long long i4, double d0) {
    double (*f)(long long,long long,long long,long long,long long,double) = (double (*)(long long,long long,long long,long long,long long,double))fn;
    return f(i0, i1, i2, i3, i4, d0);
}
static double call_6i0d_to_d(void *fn, long long i0, long long i1, long long i2, long long i3, long long i4, long long i5) {
    double (*f)(long long,long long,long long,long long,long long,long long) = (double (*)(long long,long long,long long,long long,long long,long long))fn;
    return f(i0, i1, i2, i3, i4, i5);
}
static void * call_0i0d_to_p(void *fn) {
    void * (*f)(void) = (void * (*)(void))fn;
    return f();
}
static void * call_0i1d_to_p(void *fn, double d0) {
    void * (*f)(double) = (void * (*)(double))fn;
    return f(d0);
}
static void * call_0i2d_to_p(void *fn, double d0, double d1) {
    void * (*f)(double,double) = (void * (*)(double,double))fn;
    return f(d0, d1);
}
static void * call_0i3d_to_p(void *fn, double d0, double d1, double d2) {
    void * (*f)(double,double,double) = (void * (*)(double,double,double))fn;
    return f(d0, d1, d2);
}
static void * call_0i4d_to_p(void *fn, double d0, double d1, double d2, double d3) {
    void * (*f)(double,double,double,double) = (void * (*)(double,double,double,double))fn;
    return f(d0, d1, d2, d3);
}
static void * call_0i5d_to_p(void *fn, double d0, double d1, double d2, double d3, double d4) {
    void * (*f)(double,double,double,double,double) = (void * (*)(double,double,double,double,double))fn;
    return f(d0, d1, d2, d3, d4);
}
static void * call_0i6d_to_p(void *fn, double d0, double d1, double d2, double d3, double d4, double d5) {
    void * (*f)(double,double,double,double,double,double) = (void * (*)(double,double,double,double,double,double))fn;
    return f(d0, d1, d2, d3, d4, d5);
}
static void * call_1i0d_to_p(void *fn, long long i0) {
    void * (*f)(long long) = (void * (*)(long long))fn;
    return f(i0);
}
static void * call_1i1d_to_p(void *fn, long long i0, double d0) {
    void * (*f)(long long,double) = (void * (*)(long long,double))fn;
    return f(i0, d0);
}
static void * call_1i2d_to_p(void *fn, long long i0, double d0, double d1) {
    void * (*f)(long long,double,double) = (void * (*)(long long,double,double))fn;
    return f(i0, d0, d1);
}
static void * call_1i3d_to_p(void *fn, long long i0, double d0, double d1, double d2) {
    void * (*f)(long long,double,double,double) = (void * (*)(long long,double,double,double))fn;
    return f(i0, d0, d1, d2);
}
static void * call_1i4d_to_p(void *fn, long long i0, double d0, double d1, double d2, double d3) {
    void * (*f)(long long,double,double,double,double) = (void * (*)(long long,double,double,double,double))fn;
    return f(i0, d0, d1, d2, d3);
}
static void * call_1i5d_to_p(void *fn, long long i0, double d0, double d1, double d2, double d3, double d4) {
    void * (*f)(long long,double,double,double,double,double) = (void * (*)(long long,double,double,double,double,double))fn;
    return f(i0, d0, d1, d2, d3, d4);
}
static void * call_2i0d_to_p(void *fn, long long i0, long long i1) {
    void * (*f)(long long,long long) = (void * (*)(long long,long long))fn;
    return f(i0, i1);
}
static void * call_2i1d_to_p(void *fn, long long i0, long long i1, double d0) {
    void * (*f)(long long,long long,double) = (void * (*)(long long,long long,double))fn;
    return f(i0, i1, d0);
}
static void * call_2i2d_to_p(void *fn, long long i0, long long i1, double d0, double d1) {
    void * (*f)(long long,long long,double,double) = (void * (*)(long long,long long,double,double))fn;
    return f(i0, i1, d0, d1);
}
static void * call_2i3d_to_p(void *fn, long long i0, long long i1, double d0, double d1, double d2) {
    void * (*f)(long long,long long,double,double,double) = (void * (*)(long long,long long,double,double,double))fn;
    return f(i0, i1, d0, d1, d2);
}
static void * call_2i4d_to_p(void *fn, long long i0, long long i1, double d0, double d1, double d2, double d3) {
    void * (*f)(long long,long long,double,double,double,double) = (void * (*)(long long,long long,double,double,double,double))fn;
    return f(i0, i1, d0, d1, d2, d3);
}
static void * call_3i0d_to_p(void *fn, long long i0, long long i1, long long i2) {
    void * (*f)(long long,long long,long long) = (void * (*)(long long,long long,long long))fn;
    return f(i0, i1, i2);
}
static void * call_3i1d_to_p(void *fn, long long i0, long long i1, long long i2, double d0) {
    void * (*f)(long long,long long,long long,double) = (void * (*)(long long,long long,long long,double))fn;
    return f(i0, i1, i2, d0);
}
static void * call_3i2d_to_p(void *fn, long long i0, long long i1, long long i2, double d0, double d1) {
    void * (*f)(long long,long long,long long,double,double) = (void * (*)(long long,long long,long long,double,double))fn;
    return f(i0, i1, i2, d0, d1);
}
static void * call_3i3d_to_p(void *fn, long long i0, long long i1, long long i2, double d0, double d1, double d2) {
    void * (*f)(long long,long long,long long,double,double,double) = (void * (*)(long long,long long,long long,double,double,double))fn;
    return f(i0, i1, i2, d0, d1, d2);
}
static void * call_4i0d_to_p(void *fn, long long i0, long long i1, long long i2, long long i3) {
    void * (*f)(long long,long long,long long,long long) = (void * (*)(long long,long long,long long,long long))fn;
    return f(i0, i1, i2, i3);
}
static void * call_4i1d_to_p(void *fn, long long i0, long long i1, long long i2, long long i3, double d0) {
    void * (*f)(long long,long long,long long,long long,double) = (void * (*)(long long,long long,long long,long long,double))fn;
    return f(i0, i1, i2, i3, d0);
}
static void * call_4i2d_to_p(void *fn, long long i0, long long i1, long long i2, long long i3, double d0, double d1) {
    void * (*f)(long long,long long,long long,long long,double,double) = (void * (*)(long long,long long,long long,long long,double,double))fn;
    return f(i0, i1, i2, i3, d0, d1);
}
static void * call_5i0d_to_p(void *fn, long long i0, long long i1, long long i2, long long i3, long long i4) {
    void * (*f)(long long,long long,long long,long long,long long) = (void * (*)(long long,long long,long long,long long,long long))fn;
    return f(i0, i1, i2, i3, i4);
}
static void * call_5i1d_to_p(void *fn, long long i0, long long i1, long long i2, long long i3, long long i4, double d0) {
    void * (*f)(long long,long long,long long,long long,long long,double) = (void * (*)(long long,long long,long long,long long,long long,double))fn;
    return f(i0, i1, i2, i3, i4, d0);
}
static void * call_6i0d_to_p(void *fn, long long i0, long long i1, long long i2, long long i3, long long i4, long long i5) {
    void * (*f)(long long,long long,long long,long long,long long,long long) = (void * (*)(long long,long long,long long,long long,long long,long long))fn;
    return f(i0, i1, i2, i3, i4, i5);
}

int ffi_call_dispatch(void *fn,
                      const FFIArgValue *args,
                      const FFIArgKind *arg_kinds,
                      int arg_count,
                      FFIRetKind ret_kind,
                      FFIRetValue *out) {
    if (!fn || arg_count < 0 || arg_count > 6) return 0;

    long long ints[6];
    double    dbls[6];
    int ni = 0, nd = 0;
    int seen_double = 0;

    for (int k = 0; k < arg_count; k++) {
        switch (arg_kinds[k]) {
            case FFI_ARG_I64: ints[ni++] = args[k].i;                 break;
            case FFI_ARG_PTR: ints[ni++] = (long long)(size_t)args[k].p; break;
            case FFI_ARG_STR: ints[ni++] = (long long)(size_t)args[k].s; break;
            case FFI_ARG_F64: dbls[nd++] = args[k].d; seen_double = 1;  break;
            default: return 0;
        }
        /* Only the "integers first, then doubles" ordering is supported. */
        if (seen_double &&
            (arg_kinds[k] == FFI_ARG_I64 || arg_kinds[k] == FFI_ARG_PTR ||
             arg_kinds[k] == FFI_ARG_STR))
            return 0;
    }

    /* Convenience aliases for the generated wrapper argument lists. */
    long long *I = ints; (void)I;
    double    *D = dbls; (void)D;

    switch (ret_kind) {
        case FFI_RET_I64: {
            switch (ni * 8 + nd) {
                case 0: out->i = call_0i0d_to_i(fn); break;
                case 1: out->i = call_0i1d_to_i(fn, D[0]); break;
                case 2: out->i = call_0i2d_to_i(fn, D[0], D[1]); break;
                case 3: out->i = call_0i3d_to_i(fn, D[0], D[1], D[2]); break;
                case 4: out->i = call_0i4d_to_i(fn, D[0], D[1], D[2], D[3]); break;
                case 5: out->i = call_0i5d_to_i(fn, D[0], D[1], D[2], D[3], D[4]); break;
                case 6: out->i = call_0i6d_to_i(fn, D[0], D[1], D[2], D[3], D[4], D[5]); break;
                case 8: out->i = call_1i0d_to_i(fn, I[0]); break;
                case 9: out->i = call_1i1d_to_i(fn, I[0], D[0]); break;
                case 10: out->i = call_1i2d_to_i(fn, I[0], D[0], D[1]); break;
                case 11: out->i = call_1i3d_to_i(fn, I[0], D[0], D[1], D[2]); break;
                case 12: out->i = call_1i4d_to_i(fn, I[0], D[0], D[1], D[2], D[3]); break;
                case 13: out->i = call_1i5d_to_i(fn, I[0], D[0], D[1], D[2], D[3], D[4]); break;
                case 16: out->i = call_2i0d_to_i(fn, I[0], I[1]); break;
                case 17: out->i = call_2i1d_to_i(fn, I[0], I[1], D[0]); break;
                case 18: out->i = call_2i2d_to_i(fn, I[0], I[1], D[0], D[1]); break;
                case 19: out->i = call_2i3d_to_i(fn, I[0], I[1], D[0], D[1], D[2]); break;
                case 20: out->i = call_2i4d_to_i(fn, I[0], I[1], D[0], D[1], D[2], D[3]); break;
                case 24: out->i = call_3i0d_to_i(fn, I[0], I[1], I[2]); break;
                case 25: out->i = call_3i1d_to_i(fn, I[0], I[1], I[2], D[0]); break;
                case 26: out->i = call_3i2d_to_i(fn, I[0], I[1], I[2], D[0], D[1]); break;
                case 27: out->i = call_3i3d_to_i(fn, I[0], I[1], I[2], D[0], D[1], D[2]); break;
                case 32: out->i = call_4i0d_to_i(fn, I[0], I[1], I[2], I[3]); break;
                case 33: out->i = call_4i1d_to_i(fn, I[0], I[1], I[2], I[3], D[0]); break;
                case 34: out->i = call_4i2d_to_i(fn, I[0], I[1], I[2], I[3], D[0], D[1]); break;
                case 40: out->i = call_5i0d_to_i(fn, I[0], I[1], I[2], I[3], I[4]); break;
                case 41: out->i = call_5i1d_to_i(fn, I[0], I[1], I[2], I[3], I[4], D[0]); break;
                case 48: out->i = call_6i0d_to_i(fn, I[0], I[1], I[2], I[3], I[4], I[5]); break;
                default: return 0;
            }
            return 1;
        }
        case FFI_RET_PTR: {
            switch (ni * 8 + nd) {
                case 0: out->p = call_0i0d_to_p(fn); break;
                case 1: out->p = call_0i1d_to_p(fn, D[0]); break;
                case 2: out->p = call_0i2d_to_p(fn, D[0], D[1]); break;
                case 3: out->p = call_0i3d_to_p(fn, D[0], D[1], D[2]); break;
                case 4: out->p = call_0i4d_to_p(fn, D[0], D[1], D[2], D[3]); break;
                case 5: out->p = call_0i5d_to_p(fn, D[0], D[1], D[2], D[3], D[4]); break;
                case 6: out->p = call_0i6d_to_p(fn, D[0], D[1], D[2], D[3], D[4], D[5]); break;
                case 8: out->p = call_1i0d_to_p(fn, I[0]); break;
                case 9: out->p = call_1i1d_to_p(fn, I[0], D[0]); break;
                case 10: out->p = call_1i2d_to_p(fn, I[0], D[0], D[1]); break;
                case 11: out->p = call_1i3d_to_p(fn, I[0], D[0], D[1], D[2]); break;
                case 12: out->p = call_1i4d_to_p(fn, I[0], D[0], D[1], D[2], D[3]); break;
                case 13: out->p = call_1i5d_to_p(fn, I[0], D[0], D[1], D[2], D[3], D[4]); break;
                case 16: out->p = call_2i0d_to_p(fn, I[0], I[1]); break;
                case 17: out->p = call_2i1d_to_p(fn, I[0], I[1], D[0]); break;
                case 18: out->p = call_2i2d_to_p(fn, I[0], I[1], D[0], D[1]); break;
                case 19: out->p = call_2i3d_to_p(fn, I[0], I[1], D[0], D[1], D[2]); break;
                case 20: out->p = call_2i4d_to_p(fn, I[0], I[1], D[0], D[1], D[2], D[3]); break;
                case 24: out->p = call_3i0d_to_p(fn, I[0], I[1], I[2]); break;
                case 25: out->p = call_3i1d_to_p(fn, I[0], I[1], I[2], D[0]); break;
                case 26: out->p = call_3i2d_to_p(fn, I[0], I[1], I[2], D[0], D[1]); break;
                case 27: out->p = call_3i3d_to_p(fn, I[0], I[1], I[2], D[0], D[1], D[2]); break;
                case 32: out->p = call_4i0d_to_p(fn, I[0], I[1], I[2], I[3]); break;
                case 33: out->p = call_4i1d_to_p(fn, I[0], I[1], I[2], I[3], D[0]); break;
                case 34: out->p = call_4i2d_to_p(fn, I[0], I[1], I[2], I[3], D[0], D[1]); break;
                case 40: out->p = call_5i0d_to_p(fn, I[0], I[1], I[2], I[3], I[4]); break;
                case 41: out->p = call_5i1d_to_p(fn, I[0], I[1], I[2], I[3], I[4], D[0]); break;
                case 48: out->p = call_6i0d_to_p(fn, I[0], I[1], I[2], I[3], I[4], I[5]); break;
                default: return 0;
            }
            return 1;
        }
        case FFI_RET_F64: {
            switch (ni * 8 + nd) {
                case 0: out->d = call_0i0d_to_d(fn); break;
                case 1: out->d = call_0i1d_to_d(fn, D[0]); break;
                case 2: out->d = call_0i2d_to_d(fn, D[0], D[1]); break;
                case 3: out->d = call_0i3d_to_d(fn, D[0], D[1], D[2]); break;
                case 4: out->d = call_0i4d_to_d(fn, D[0], D[1], D[2], D[3]); break;
                case 5: out->d = call_0i5d_to_d(fn, D[0], D[1], D[2], D[3], D[4]); break;
                case 6: out->d = call_0i6d_to_d(fn, D[0], D[1], D[2], D[3], D[4], D[5]); break;
                case 8: out->d = call_1i0d_to_d(fn, I[0]); break;
                case 9: out->d = call_1i1d_to_d(fn, I[0], D[0]); break;
                case 10: out->d = call_1i2d_to_d(fn, I[0], D[0], D[1]); break;
                case 11: out->d = call_1i3d_to_d(fn, I[0], D[0], D[1], D[2]); break;
                case 12: out->d = call_1i4d_to_d(fn, I[0], D[0], D[1], D[2], D[3]); break;
                case 13: out->d = call_1i5d_to_d(fn, I[0], D[0], D[1], D[2], D[3], D[4]); break;
                case 16: out->d = call_2i0d_to_d(fn, I[0], I[1]); break;
                case 17: out->d = call_2i1d_to_d(fn, I[0], I[1], D[0]); break;
                case 18: out->d = call_2i2d_to_d(fn, I[0], I[1], D[0], D[1]); break;
                case 19: out->d = call_2i3d_to_d(fn, I[0], I[1], D[0], D[1], D[2]); break;
                case 20: out->d = call_2i4d_to_d(fn, I[0], I[1], D[0], D[1], D[2], D[3]); break;
                case 24: out->d = call_3i0d_to_d(fn, I[0], I[1], I[2]); break;
                case 25: out->d = call_3i1d_to_d(fn, I[0], I[1], I[2], D[0]); break;
                case 26: out->d = call_3i2d_to_d(fn, I[0], I[1], I[2], D[0], D[1]); break;
                case 27: out->d = call_3i3d_to_d(fn, I[0], I[1], I[2], D[0], D[1], D[2]); break;
                case 32: out->d = call_4i0d_to_d(fn, I[0], I[1], I[2], I[3]); break;
                case 33: out->d = call_4i1d_to_d(fn, I[0], I[1], I[2], I[3], D[0]); break;
                case 34: out->d = call_4i2d_to_d(fn, I[0], I[1], I[2], I[3], D[0], D[1]); break;
                case 40: out->d = call_5i0d_to_d(fn, I[0], I[1], I[2], I[3], I[4]); break;
                case 41: out->d = call_5i1d_to_d(fn, I[0], I[1], I[2], I[3], I[4], D[0]); break;
                case 48: out->d = call_6i0d_to_d(fn, I[0], I[1], I[2], I[3], I[4], I[5]); break;
                default: return 0;
            }
            return 1;
        }
        case FFI_RET_VOID: {
            switch (ni * 8 + nd) {
                case 0: (void)call_0i0d_to_i(fn); break;
                case 1: (void)call_0i1d_to_i(fn, D[0]); break;
                case 2: (void)call_0i2d_to_i(fn, D[0], D[1]); break;
                case 3: (void)call_0i3d_to_i(fn, D[0], D[1], D[2]); break;
                case 4: (void)call_0i4d_to_i(fn, D[0], D[1], D[2], D[3]); break;
                case 5: (void)call_0i5d_to_i(fn, D[0], D[1], D[2], D[3], D[4]); break;
                case 6: (void)call_0i6d_to_i(fn, D[0], D[1], D[2], D[3], D[4], D[5]); break;
                case 8: (void)call_1i0d_to_i(fn, I[0]); break;
                case 9: (void)call_1i1d_to_i(fn, I[0], D[0]); break;
                case 10: (void)call_1i2d_to_i(fn, I[0], D[0], D[1]); break;
                case 11: (void)call_1i3d_to_i(fn, I[0], D[0], D[1], D[2]); break;
                case 12: (void)call_1i4d_to_i(fn, I[0], D[0], D[1], D[2], D[3]); break;
                case 13: (void)call_1i5d_to_i(fn, I[0], D[0], D[1], D[2], D[3], D[4]); break;
                case 16: (void)call_2i0d_to_i(fn, I[0], I[1]); break;
                case 17: (void)call_2i1d_to_i(fn, I[0], I[1], D[0]); break;
                case 18: (void)call_2i2d_to_i(fn, I[0], I[1], D[0], D[1]); break;
                case 19: (void)call_2i3d_to_i(fn, I[0], I[1], D[0], D[1], D[2]); break;
                case 20: (void)call_2i4d_to_i(fn, I[0], I[1], D[0], D[1], D[2], D[3]); break;
                case 24: (void)call_3i0d_to_i(fn, I[0], I[1], I[2]); break;
                case 25: (void)call_3i1d_to_i(fn, I[0], I[1], I[2], D[0]); break;
                case 26: (void)call_3i2d_to_i(fn, I[0], I[1], I[2], D[0], D[1]); break;
                case 27: (void)call_3i3d_to_i(fn, I[0], I[1], I[2], D[0], D[1], D[2]); break;
                case 32: (void)call_4i0d_to_i(fn, I[0], I[1], I[2], I[3]); break;
                case 33: (void)call_4i1d_to_i(fn, I[0], I[1], I[2], I[3], D[0]); break;
                case 34: (void)call_4i2d_to_i(fn, I[0], I[1], I[2], I[3], D[0], D[1]); break;
                case 40: (void)call_5i0d_to_i(fn, I[0], I[1], I[2], I[3], I[4]); break;
                case 41: (void)call_5i1d_to_i(fn, I[0], I[1], I[2], I[3], I[4], D[0]); break;
                case 48: (void)call_6i0d_to_i(fn, I[0], I[1], I[2], I[3], I[4], I[5]); break;
                default: return 0;
            }
            return 1;
        }
        default: return 0;
    }
}

