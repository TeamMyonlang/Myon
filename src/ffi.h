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

#ifndef MYON_FFI_H
#define MYON_FFI_H

/*
 * Phase3 C FFI — type / handle management layer.
 *
 * Maps opaque library handle IDs (exposed to Myon as plain int values) to the
 * loaded FFILib* objects from ffi_platform.  One FFIState lives per interpreter
 * instance.  Handle IDs are handed out in load order (0, 1, 2, ...) and are
 * never recycled once closed.
 */

typedef struct FFIState FFIState;

FFIState *ffi_state_create(void);
void      ffi_state_free(FFIState *st);

/*
 * Load a shared library.  On success returns a non-negative handle ID.  On
 * failure returns -1 and stores a heap-allocated error message in *err_msg
 * (the caller must free it).
 */
long long ffi_load(FFIState *st, const char *path, char **err_msg);

/*
 * Resolve a symbol in a previously loaded library.  Returns NULL if the handle
 * is invalid/closed or the symbol is missing.
 */
void *ffi_lookup_symbol(FFIState *st, long long handle,
                        const char *symbol_name);

/* Close a handle.  Returns 1 on success, 0 for an invalid/closed handle. */
int ffi_close(FFIState *st, long long handle);

/*
 * Phase3.1 — raw memory blocks.
 *
 * These let Myon scripts obtain writable scratch memory to hand to C
 * functions (out-parameters, byte buffers, ...) without ever exposing a raw
 * pointer to the script layer.  Blocks are tracked inside FFIState with the
 * same handle-ID discipline as loaded libraries: alloc hands out a
 * non-negative block ID, and everything else is addressed through that ID.
 * The real address is only materialised internally (via ffi_mem_ptr) when a
 * block is passed to a C call, so an invalid ID can never crash the callee.
 */

/*
 * Allocate `size` zero-initialised bytes and return a non-negative block ID.
 * Returns -1 on failure (size <= 0 or out of memory).
 */
long long ffi_mem_alloc(FFIState *st, long long size);

/*
 * Resolve a block ID to its raw pointer.  Returns NULL for an invalid or
 * already-freed ID.  Internal use only — never exposed to Myon scripts.
 */
void *ffi_mem_ptr(FFIState *st, long long block_id);

/* Size of the block in bytes, or -1 for an invalid/freed ID. */
long long ffi_mem_size(FFIState *st, long long block_id);

/* Free a block.  Returns 1 on success, 0 for an invalid/already-freed ID. */
int ffi_mem_free(FFIState *st, long long block_id);

/*
 * Phase3.1 — read a NUL-terminated C string from a raw address.
 *
 * `addr` is a *raw C address value* (as returned by call_p or another
 * call_*), NOT a block ID — the two are separate namespaces.  Reads at most
 * `max_len` bytes and returns a freshly heap-allocated copy (caller frees).
 * Returns NULL if addr is 0 (NULL).  Passing a bogus non-zero address may
 * crash — this phase does not install signal handlers.
 */
char *ffi_read_cstring(long long addr, long long max_len);

/*
 * Phase3.1 — byte-level read/write on a memory block.
 *
 * These operate on a block ID (from ffi_mem_alloc), NOT a raw address, so an
 * invalid ID or an out-of-range span can never corrupt memory.  A block plus
 * a separately-tracked valid length is Myon's stand-in for a length-prefixed
 * byte string (no dedicated byte-string value type is introduced in this
 * phase).
 */

/*
 * Write `len` bytes from `data` into block `block_id` starting at `offset`.
 * Fails (returns 0) if the ID is invalid/freed or [offset, offset+len) would
 * run past the end of the block.  Returns 1 on success.
 */
int ffi_mem_write(FFIState *st, long long block_id, long long offset,
                  const unsigned char *data, long long len);

/*
 * Read `len` bytes from block `block_id` starting at `offset` into a freshly
 * heap-allocated buffer (caller frees).  Returns NULL for an invalid/freed ID
 * or an out-of-range span.
 */
unsigned char *ffi_mem_read(FFIState *st, long long block_id, long long offset,
                            long long len);

/*
 * Phase3.1 — read 8 bytes of block `block_id` at `offset` as a little-endian
 * int64 (x86-64 native order).  This is the convenience path for pulling an
 * out-parameter pointer (e.g. the `sqlite3*` written by sqlite3_open) back out
 * of a block as a plain address value.  Returns 1 on success and stores the
 * value in *out; returns 0 for an invalid ID or an out-of-range span.
 */
int ffi_mem_read_i64(FFIState *st, long long block_id, long long offset,
                     long long *out);

/*
 * Phase5.1, Step7 — read 4 bytes of block `block_id` at `offset` as a
 * little-endian int32 (x86-64 native order), sign-extended into the returned
 * int64.  This is the read-side counterpart of ffi_mem_write_i32 and the
 * primitive used to pull 4-byte fields (e.g. the `Uint32 type` at offset 0 of
 * an SDL_Event, or the `sym` keycode at offset 20) out of an out-parameter
 * struct.  Returns 1 on success and stores the value in *out; returns 0 for an
 * invalid ID or an out-of-range span.
 */
int ffi_mem_read_i32(FFIState *st, long long block_id, long long offset,
                     long long *out);

/*
 * Phase4.1, Step1 — typed memory writes.
 *
 * These are the write-side counterparts of ffi_mem_read_i64.  Unlike
 * ffi_mem_write (which copies the bytes of a NUL-terminated Myon str and
 * therefore stops at the first embedded NUL), these encode a scalar directly,
 * so values whose little-endian byte pattern contains NUL bytes (e.g. small
 * ints) are written in full.  All values are laid out in x86-64 native
 * (little-endian) order.  Each returns 1 on success and 0 for an invalid /
 * freed block ID or an out-of-range span (never corrupts memory).
 */

/* Write `v` at `offset` as a little-endian 8-byte int64. */
int ffi_mem_write_i64(FFIState *st, long long block_id,
                      long long offset, long long v);

/* Write `v` at `offset` as a 4-byte int32 (upper 32 bits truncated). */
int ffi_mem_write_i32(FFIState *st, long long block_id,
                      long long offset, long long v);

/* Write `v` at `offset` as an 8-byte IEEE-754 double (native order). */
int ffi_mem_write_f64(FFIState *st, long long block_id,
                      long long offset, double v);

/* Write `v` at `offset` as a 4-byte IEEE-754 float (v converted first). */
int ffi_mem_write_f32(FFIState *st, long long block_id,
                      long long offset, double v);

/*
 * Phase4.1, Step2 — struct layout DSL.
 *
 * A struct layout is a named list of field kinds; the interpreter computes each
 * field's offset using natural alignment (a 4-byte field aligns to 4 bytes, an
 * 8-byte field to 8 bytes) and pads the total size up to the largest field's
 * alignment — the same rules a C compiler applies to a plain struct.  No real C
 * header is ever consulted: the layout is whatever the Myon script declares.
 * Layouts are stored inside FFIState alongside memory blocks and libraries.
 */

typedef enum {
    FFI_FIELD_I32,
    FFI_FIELD_I64,
    FFI_FIELD_F32,
    FFI_FIELD_F64
} FFIFieldKind;

/*
 * Define (or redefine) the struct layout `name` from `field_kinds` (length
 * `field_count`).  A pre-existing definition with the same name is overwritten.
 * Returns the total size in bytes, or -1 on invalid arguments (NULL name, empty
 * field list, unknown kind).
 */
long long ffi_struct_define(FFIState *st, const char *name,
                            const FFIFieldKind *field_kinds,
                            int field_count);

/*
 * Offset (in bytes) of field `field_index` in the layout `name`, or -1 for an
 * undefined name or an out-of-range index.
 */
long long ffi_struct_field_offset(FFIState *st, const char *name,
                                  int field_index);

/* Total size in bytes of the layout `name`, or -1 if undefined. */
long long ffi_struct_size(FFIState *st, const char *name);

/* Number of fields in layout `name`, or -1 if undefined. */
int ffi_struct_field_count(FFIState *st, const char *name);

/* Kind of field `field_index` in layout `name`.  Returns 1 and stores the kind
 * in *out on success; 0 for an undefined name or out-of-range index. */
int ffi_struct_field_kind(FFIState *st, const char *name, int field_index,
                          FFIFieldKind *out);

/*
 * Phase4.1, Step3 — bulk array read/write.
 *
 * Read/write `count` same-typed scalars contiguously starting at `offset`.  The
 * i32/i64 variants marshal a Myon int array (carried as long long*), while the
 * f32/f64 variants marshal a Myon float array (carried as double*).  Each
 * returns 1 on success, 0 for an invalid/freed block or an out-of-range span
 * (offset + count*elem_size must fit inside the block).  Reads write into a
 * caller-provided out buffer of `count` elements.
 */
int ffi_mem_write_array_i32(FFIState *st, long long block_id,
                            long long offset,
                            const long long *values, long long count);
int ffi_mem_write_array_i64(FFIState *st, long long block_id,
                            long long offset,
                            const long long *values, long long count);
int ffi_mem_write_array_f32(FFIState *st, long long block_id,
                            long long offset,
                            const double *values, long long count);
int ffi_mem_write_array_f64(FFIState *st, long long block_id,
                            long long offset,
                            const double *values, long long count);

int ffi_mem_read_array_i32(FFIState *st, long long block_id,
                           long long offset,
                           long long *out_values, long long count);
int ffi_mem_read_array_i64(FFIState *st, long long block_id,
                           long long offset,
                           long long *out_values, long long count);
int ffi_mem_read_array_f32(FFIState *st, long long block_id,
                           long long offset,
                           double *out_values, long long count);
int ffi_mem_read_array_f64(FFIState *st, long long block_id,
                           long long offset,
                           double *out_values, long long count);

#endif /* MYON_FFI_H */
