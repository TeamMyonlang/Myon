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

#include "ffi_callback.h"
#include "value.h"

#include <stddef.h>

/*
 * We store trampoline *function* pointers in a void* table and hand them out as
 * generic pointers for the C call layer.  ISO C forbids function<->object
 * pointer conversion, but every POSIX/Windows FFI relies on it (as ffi_call.c
 * already does), so silence the pedantic diagnostic for this file.
 */
#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif

/*
 * Implemented in interpreter.c (where the static call_function() lives).  Given
 * a slot index, an argument count and an int64 argument array, it looks up the
 * Myon function registered in that slot, invokes it, and returns the result
 * coerced to int64 (0 if the function did not return an int, or on error — a
 * message is printed to stderr rather than crashing the process).
 */
long long myon_ffi_callback_dispatch(int slot, int argc, const long long *args);

/*
 * One registration slot.  `active` guards against firing a trampoline whose
 * slot has been unregistered.  The Myon function value (fn) and interpreter
 * context (it) are owned by interpreter.c via the dispatch path; this file only
 * records the arg_count and active flag and hands trampoline pointers out.
 */
typedef struct {
    int active;
    int arg_count;
    void *fn_ptr;   /* the trampoline pointer handed to C for this slot */
} CBSlot;

static CBSlot g_slots[MYON_FFI_CB_SLOTS];

/*
 * Trampoline grid.  For every (slot, arg-count) pair we need a distinct static
 * function whose *identity* encodes the slot number, because a C function
 * pointer's target cannot be rewritten at run time.  Each trampoline packs its
 * fixed arguments into an int64 array and forwards to the interpreter dispatch.
 */

/* arg-count 0 */
#define DEF_TRAMP0(SLOT) \
    static long long tramp_s##SLOT##_a0(void) { \
        long long _a[MYON_FFI_CB_MAX_ARGS] = {0}; (void)_a; \
        return myon_ffi_callback_dispatch((SLOT), 0, _a); \
    }
/* arg-count 1 */
#define DEF_TRAMP1(SLOT) \
    static long long tramp_s##SLOT##_a1(long long a0) { \
        long long _a[MYON_FFI_CB_MAX_ARGS] = {0}; _a[0]=a0; \
        return myon_ffi_callback_dispatch((SLOT), 1, _a); \
    }
/* arg-count 2 */
#define DEF_TRAMP2(SLOT) \
    static long long tramp_s##SLOT##_a2(long long a0, long long a1) { \
        long long _a[MYON_FFI_CB_MAX_ARGS] = {0}; _a[0]=a0; _a[1]=a1; \
        return myon_ffi_callback_dispatch((SLOT), 2, _a); \
    }
/* arg-count 3 */
#define DEF_TRAMP3(SLOT) \
    static long long tramp_s##SLOT##_a3(long long a0, long long a1, long long a2) { \
        long long _a[MYON_FFI_CB_MAX_ARGS] = {0}; _a[0]=a0; _a[1]=a1; _a[2]=a2; \
        return myon_ffi_callback_dispatch((SLOT), 3, _a); \
    }
/* arg-count 4 */
#define DEF_TRAMP4(SLOT) \
    static long long tramp_s##SLOT##_a4(long long a0, long long a1, long long a2, long long a3) { \
        long long _a[MYON_FFI_CB_MAX_ARGS] = {0}; _a[0]=a0; _a[1]=a1; _a[2]=a2; _a[3]=a3; \
        return myon_ffi_callback_dispatch((SLOT), 4, _a); \
    }

#define DEF_TRAMP_SLOT(SLOT) \
    DEF_TRAMP0(SLOT) DEF_TRAMP1(SLOT) DEF_TRAMP2(SLOT) \
    DEF_TRAMP3(SLOT) DEF_TRAMP4(SLOT)

/* MYON_FFI_CB_SLOTS == 16 slots, indices 0..15. */
DEF_TRAMP_SLOT(0)  DEF_TRAMP_SLOT(1)  DEF_TRAMP_SLOT(2)  DEF_TRAMP_SLOT(3)
DEF_TRAMP_SLOT(4)  DEF_TRAMP_SLOT(5)  DEF_TRAMP_SLOT(6)  DEF_TRAMP_SLOT(7)
DEF_TRAMP_SLOT(8)  DEF_TRAMP_SLOT(9)  DEF_TRAMP_SLOT(10) DEF_TRAMP_SLOT(11)
DEF_TRAMP_SLOT(12) DEF_TRAMP_SLOT(13) DEF_TRAMP_SLOT(14) DEF_TRAMP_SLOT(15)


/* Table: g_tramp[slot][arg_count] -> trampoline function pointer. */
#define TRAMP_ROW(SLOT) \
    { (void *)tramp_s##SLOT##_a0, (void *)tramp_s##SLOT##_a1, \
      (void *)tramp_s##SLOT##_a2, (void *)tramp_s##SLOT##_a3, \
      (void *)tramp_s##SLOT##_a4 }

static void *g_tramp[MYON_FFI_CB_SLOTS][MYON_FFI_CB_MAX_ARGS + 1] = {
    TRAMP_ROW(0),  TRAMP_ROW(1),  TRAMP_ROW(2),  TRAMP_ROW(3),
    TRAMP_ROW(4),  TRAMP_ROW(5),  TRAMP_ROW(6),  TRAMP_ROW(7),
    TRAMP_ROW(8),  TRAMP_ROW(9),  TRAMP_ROW(10), TRAMP_ROW(11),
    TRAMP_ROW(12), TRAMP_ROW(13), TRAMP_ROW(14), TRAMP_ROW(15)
};

/*
 * interpreter.c owns the actual Value fn + Interp* per slot.  These accessors
 * let it register/clear without exposing its internals here.  Defined in
 * interpreter.c.
 */
void myon_ffi_callback_bind_slot(int slot, Interp *it, Value fn, int arg_count);
void myon_ffi_callback_clear_slot(int slot);

void *ffi_callback_register(Interp *it, Value fn, int arg_count) {
    if (fn.type != TYPE_FUNC) return NULL;
    if (arg_count < 0 || arg_count > MYON_FFI_CB_MAX_ARGS) return NULL;

    for (int s = 0; s < MYON_FFI_CB_SLOTS; s++) {
        if (!g_slots[s].active) {
            g_slots[s].active = 1;
            g_slots[s].arg_count = arg_count;
            g_slots[s].fn_ptr = g_tramp[s][arg_count];
            myon_ffi_callback_bind_slot(s, it, fn, arg_count);
            return g_slots[s].fn_ptr;
        }
    }
    return NULL; /* slots exhausted */
}

void ffi_callback_unregister(void *ptr) {
    if (!ptr) return;
    for (int s = 0; s < MYON_FFI_CB_SLOTS; s++) {
        if (g_slots[s].active && g_slots[s].fn_ptr == ptr) {
            g_slots[s].active = 0;
            g_slots[s].arg_count = 0;
            g_slots[s].fn_ptr = NULL;
            myon_ffi_callback_clear_slot(s);
            return;
        }
    }
}

void ffi_callback_reset_all(void) {
    for (int s = 0; s < MYON_FFI_CB_SLOTS; s++) {
        if (g_slots[s].active) {
            g_slots[s].active = 0;
            g_slots[s].arg_count = 0;
            g_slots[s].fn_ptr = NULL;
            myon_ffi_callback_clear_slot(s);
        }
    }
}
