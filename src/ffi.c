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

#include "ffi.h"
#include "ffi_platform.h"
#include "common.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * A loaded-library slot.  Handle IDs are the index into `slots`; a NULL `lib`
 * marks a never-used or already-closed (retired) slot.  This mirrors the
 * ModuleEntry-style bookkeeping used elsewhere in the interpreter but is kept
 * as a growable array for O(1) handle lookup.
 */
/*
 * A raw memory block (Phase3.1).  Block IDs are the index into `blocks`; a
 * NULL `ptr` marks a never-used or already-freed slot.  Mirrors the loaded-
 * library slot bookkeeping above: IDs are handed out in allocation order and
 * never recycled once freed.
 */
typedef struct {
    void      *ptr;   /* NULL == free/retired slot */
    long long  size;  /* allocation size in bytes */
} FFIMemBlock;

/*
 * A named struct layout (Phase4.1, Step2).  Field offsets and the total size
 * are precomputed at definition time using natural alignment.  Definitions are
 * stored in a simple growable array keyed by name; redefining a name reuses its
 * slot.  Names are never removed (there is no undefine operation).
 */
typedef struct {
    char         *name;         /* owned; NULL marks an unused slot */
    FFIFieldKind *field_kinds;  /* owned, length field_count */
    long long    *field_offsets;/* owned, length field_count */
    int           field_count;
    long long     size;         /* total size incl. tail padding */
} FFIStructDef;

struct FFIState {
    FFILib **slots;
    int      count;    /* number of allocated slots (== next handle ID) */
    int      capacity;

    /* Phase3.1 raw memory blocks. */
    FFIMemBlock *blocks;
    int          block_count;    /* == next block ID */
    int          block_capacity;

    /* Phase4.1 struct layout definitions. */
    FFIStructDef *structs;
    int           struct_count;
    int           struct_capacity;
};

FFIState *ffi_state_create(void) {
    FFIState *st = (FFIState *)myon_xmalloc(sizeof(FFIState));
    st->slots = NULL;
    st->count = 0;
    st->capacity = 0;
    st->blocks = NULL;
    st->block_count = 0;
    st->block_capacity = 0;
    st->structs = NULL;
    st->struct_count = 0;
    st->struct_capacity = 0;
    return st;
}

void ffi_state_free(FFIState *st) {
    if (!st) return;
    for (int i = 0; i < st->count; i++) {
        if (st->slots[i]) ffi_platform_close(st->slots[i]);
    }
    free(st->slots);
    /* free any memory blocks the script forgot to release (leak guard) */
    for (int i = 0; i < st->block_count; i++) {
        if (st->blocks[i].ptr) free(st->blocks[i].ptr);
    }
    free(st->blocks);
    /* free struct layout definitions */
    for (int i = 0; i < st->struct_count; i++) {
        free(st->structs[i].name);
        free(st->structs[i].field_kinds);
        free(st->structs[i].field_offsets);
    }
    free(st->structs);
    free(st);
}

/* ---- Phase3.1 raw memory blocks ---- */

long long ffi_mem_alloc(FFIState *st, long long size) {
    if (size <= 0) return -1;

    void *mem = malloc((size_t)size);
    if (!mem) return -1;
    memset(mem, 0, (size_t)size); /* zero-clear (calloc-equivalent) */

    if (st->block_count >= st->block_capacity) {
        int newcap = st->block_capacity ? st->block_capacity * 2 : 4;
        st->blocks = (FFIMemBlock *)myon_xrealloc(
            st->blocks, sizeof(FFIMemBlock) * (size_t)newcap);
        st->block_capacity = newcap;
    }
    long long id = st->block_count;
    st->blocks[st->block_count].ptr = mem;
    st->blocks[st->block_count].size = size;
    st->block_count++;
    return id;
}

void *ffi_mem_ptr(FFIState *st, long long block_id) {
    if (block_id < 0 || block_id >= st->block_count) return NULL;
    return st->blocks[block_id].ptr; /* NULL if freed/retired */
}

long long ffi_mem_size(FFIState *st, long long block_id) {
    if (block_id < 0 || block_id >= st->block_count) return -1;
    if (!st->blocks[block_id].ptr) return -1; /* freed */
    return st->blocks[block_id].size;
}

int ffi_mem_free(FFIState *st, long long block_id) {
    if (block_id < 0 || block_id >= st->block_count) return 0;
    if (!st->blocks[block_id].ptr) return 0; /* already freed */
    free(st->blocks[block_id].ptr);
    st->blocks[block_id].ptr = NULL; /* retire; never reused */
    st->blocks[block_id].size = 0;
    return 1;
}

int ffi_mem_write(FFIState *st, long long block_id, long long offset,
                  const unsigned char *data, long long len) {
    if (len < 0 || offset < 0) return 0;
    long long size = ffi_mem_size(st, block_id);
    if (size < 0) return 0;                 /* invalid / freed */
    if (offset > size || len > size - offset) return 0; /* out of range */
    void *base = ffi_mem_ptr(st, block_id);
    if (!base) return 0;
    if (len > 0) memcpy((unsigned char *)base + offset, data, (size_t)len);
    return 1;
}

unsigned char *ffi_mem_read(FFIState *st, long long block_id, long long offset,
                            long long len) {
    if (len < 0 || offset < 0) return NULL;
    long long size = ffi_mem_size(st, block_id);
    if (size < 0) return NULL;              /* invalid / freed */
    if (offset > size || len > size - offset) return NULL; /* out of range */
    void *base = ffi_mem_ptr(st, block_id);
    if (!base) return NULL;
    unsigned char *dst = (unsigned char *)myon_xmalloc((size_t)len + 1);
    if (len > 0) memcpy(dst, (unsigned char *)base + offset, (size_t)len);
    dst[len] = '\0'; /* NUL guard so callers may treat it as a C string */
    return dst;
}

int ffi_mem_read_i64(FFIState *st, long long block_id, long long offset,
                     long long *out) {
    if (offset < 0) return 0;
    long long size = ffi_mem_size(st, block_id);
    if (size < 0) return 0;                 /* invalid / freed */
    if (offset > size || (long long)sizeof(long long) > size - offset) return 0;
    const unsigned char *base = (const unsigned char *)ffi_mem_ptr(st, block_id);
    if (!base) return 0;
    /* assemble little-endian (x86-64 native) */
    unsigned long long v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (unsigned long long)base[offset + i] << (8 * i);
    }
    *out = (long long)v;
    return 1;
}

int ffi_mem_read_i32(FFIState *st, long long block_id, long long offset,
                     long long *out) {
    if (offset < 0) return 0;
    long long size = ffi_mem_size(st, block_id);
    if (size < 0) return 0;                 /* invalid / freed */
    if (offset > size || (long long)sizeof(int32_t) > size - offset) return 0;
    const unsigned char *base = (const unsigned char *)ffi_mem_ptr(st, block_id);
    if (!base) return 0;
    /* assemble little-endian (x86-64 native), then sign-extend to int64 */
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= (uint32_t)base[offset + i] << (8 * i);
    }
    *out = (long long)(int32_t)v;
    return 1;
}

/* ---- Phase4.1, Step1: typed memory writes ---- */

/*
 * Shared bounds check + destination pointer resolution for a fixed-width write.
 * Returns a writable pointer to [offset, offset+width) inside the block, or
 * NULL if the ID is invalid/freed, the offset is negative, or the span runs
 * past the end of the block.  Mirrors the guard logic in ffi_mem_write.
 */
static void *ffi_mem_span_ptr(FFIState *st, long long block_id,
                              long long offset, long long width) {
    if (offset < 0 || width < 0) return NULL;
    long long size = ffi_mem_size(st, block_id);
    if (size < 0) return NULL;                     /* invalid / freed */
    if (offset > size || width > size - offset) return NULL; /* out of range */
    void *base = ffi_mem_ptr(st, block_id);
    if (!base) return NULL;
    return (unsigned char *)base + offset;
}

int ffi_mem_write_i64(FFIState *st, long long block_id,
                      long long offset, long long v) {
    void *dest = ffi_mem_span_ptr(st, block_id, offset, 8);
    if (!dest) return 0;
    memcpy(dest, &v, 8);
    return 1;
}

int ffi_mem_write_i32(FFIState *st, long long block_id,
                      long long offset, long long v) {
    void *dest = ffi_mem_span_ptr(st, block_id, offset, 4);
    if (!dest) return 0;
    int32_t v32 = (int32_t)v;
    memcpy(dest, &v32, 4);
    return 1;
}

int ffi_mem_write_f64(FFIState *st, long long block_id,
                      long long offset, double v) {
    void *dest = ffi_mem_span_ptr(st, block_id, offset, 8);
    if (!dest) return 0;
    memcpy(dest, &v, 8);
    return 1;
}

int ffi_mem_write_f32(FFIState *st, long long block_id,
                      long long offset, double v) {
    void *dest = ffi_mem_span_ptr(st, block_id, offset, 4);
    if (!dest) return 0;
    float f = (float)v;
    memcpy(dest, &f, 4);
    return 1;
}

/* ---- Phase4.1, Step3: bulk array read/write ---- */

int ffi_mem_write_array_i32(FFIState *st, long long block_id,
                            long long offset,
                            const long long *values, long long count) {
    if (count < 0) return 0;
    for (long long i = 0; i < count; i++)
        if (!ffi_mem_write_i32(st, block_id, offset + i * 4, values[i])) return 0;
    return 1;
}

int ffi_mem_write_array_i64(FFIState *st, long long block_id,
                            long long offset,
                            const long long *values, long long count) {
    if (count < 0) return 0;
    for (long long i = 0; i < count; i++)
        if (!ffi_mem_write_i64(st, block_id, offset + i * 8, values[i])) return 0;
    return 1;
}

int ffi_mem_write_array_f32(FFIState *st, long long block_id,
                            long long offset,
                            const double *values, long long count) {
    if (count < 0) return 0;
    for (long long i = 0; i < count; i++)
        if (!ffi_mem_write_f32(st, block_id, offset + i * 4, values[i])) return 0;
    return 1;
}

int ffi_mem_write_array_f64(FFIState *st, long long block_id,
                            long long offset,
                            const double *values, long long count) {
    if (count < 0) return 0;
    for (long long i = 0; i < count; i++)
        if (!ffi_mem_write_f64(st, block_id, offset + i * 8, values[i])) return 0;
    return 1;
}

int ffi_mem_read_array_i32(FFIState *st, long long block_id,
                           long long offset,
                           long long *out_values, long long count) {
    if (count < 0) return 0;
    for (long long i = 0; i < count; i++) {
        const void *src = ffi_mem_span_ptr(st, block_id, offset + i * 4, 4);
        if (!src) return 0;
        int32_t v32;
        memcpy(&v32, src, 4);
        out_values[i] = (long long)v32;   /* sign-extend */
    }
    return 1;
}

int ffi_mem_read_array_i64(FFIState *st, long long block_id,
                           long long offset,
                           long long *out_values, long long count) {
    if (count < 0) return 0;
    for (long long i = 0; i < count; i++) {
        const void *src = ffi_mem_span_ptr(st, block_id, offset + i * 8, 8);
        if (!src) return 0;
        long long v;
        memcpy(&v, src, 8);
        out_values[i] = v;
    }
    return 1;
}

int ffi_mem_read_array_f32(FFIState *st, long long block_id,
                           long long offset,
                           double *out_values, long long count) {
    if (count < 0) return 0;
    for (long long i = 0; i < count; i++) {
        const void *src = ffi_mem_span_ptr(st, block_id, offset + i * 4, 4);
        if (!src) return 0;
        float f;
        memcpy(&f, src, 4);
        out_values[i] = (double)f;
    }
    return 1;
}

int ffi_mem_read_array_f64(FFIState *st, long long block_id,
                           long long offset,
                           double *out_values, long long count) {
    if (count < 0) return 0;
    for (long long i = 0; i < count; i++) {
        const void *src = ffi_mem_span_ptr(st, block_id, offset + i * 8, 8);
        if (!src) return 0;
        double d;
        memcpy(&d, src, 8);
        out_values[i] = d;
    }
    return 1;
}

/* ---- Phase4.1, Step2: struct layout DSL ---- */

/* Byte size / alignment of a field kind (natural alignment == size here). */
static long long ffi_field_size(FFIFieldKind k) {
    switch (k) {
        case FFI_FIELD_I32: case FFI_FIELD_F32: return 4;
        case FFI_FIELD_I64: case FFI_FIELD_F64: return 8;
    }
    return 0;
}

/* Find an existing definition by name (NULL if none). */
static FFIStructDef *ffi_struct_find(FFIState *st, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < st->struct_count; i++) {
        if (st->structs[i].name && strcmp(st->structs[i].name, name) == 0)
            return &st->structs[i];
    }
    return NULL;
}

long long ffi_struct_define(FFIState *st, const char *name,
                            const FFIFieldKind *field_kinds,
                            int field_count) {
    if (!name || !field_kinds || field_count <= 0) return -1;
    /* validate kinds */
    for (int i = 0; i < field_count; i++) {
        long long fs = ffi_field_size(field_kinds[i]);
        if (fs != 4 && fs != 8) return -1;
    }

    /* compute offsets with natural alignment + tail padding */
    long long *offsets = (long long *)myon_xmalloc(sizeof(long long) * (size_t)field_count);
    long long cur = 0;
    long long max_align = 1;
    for (int i = 0; i < field_count; i++) {
        long long align = ffi_field_size(field_kinds[i]);
        if (align > max_align) max_align = align;
        if (cur % align != 0) cur += align - (cur % align); /* pad */
        offsets[i] = cur;
        cur += align;
    }
    /* pad total size up to the largest alignment requirement */
    if (cur % max_align != 0) cur += max_align - (cur % max_align);

    FFIFieldKind *kinds_copy =
        (FFIFieldKind *)myon_xmalloc(sizeof(FFIFieldKind) * (size_t)field_count);
    memcpy(kinds_copy, field_kinds, sizeof(FFIFieldKind) * (size_t)field_count);

    FFIStructDef *def = ffi_struct_find(st, name);
    if (def) {
        /* redefine: replace in place */
        free(def->field_kinds);
        free(def->field_offsets);
        def->field_kinds = kinds_copy;
        def->field_offsets = offsets;
        def->field_count = field_count;
        def->size = cur;
        return cur;
    }

    if (st->struct_count >= st->struct_capacity) {
        int newcap = st->struct_capacity ? st->struct_capacity * 2 : 4;
        st->structs = (FFIStructDef *)myon_xrealloc(
            st->structs, sizeof(FFIStructDef) * (size_t)newcap);
        st->struct_capacity = newcap;
    }
    FFIStructDef *slot = &st->structs[st->struct_count++];
    slot->name = myon_strdup(name);
    slot->field_kinds = kinds_copy;
    slot->field_offsets = offsets;
    slot->field_count = field_count;
    slot->size = cur;
    return cur;
}

long long ffi_struct_field_offset(FFIState *st, const char *name,
                                  int field_index) {
    FFIStructDef *def = ffi_struct_find(st, name);
    if (!def) return -1;
    if (field_index < 0 || field_index >= def->field_count) return -1;
    return def->field_offsets[field_index];
}

long long ffi_struct_size(FFIState *st, const char *name) {
    FFIStructDef *def = ffi_struct_find(st, name);
    if (!def) return -1;
    return def->size;
}

int ffi_struct_field_count(FFIState *st, const char *name) {
    FFIStructDef *def = ffi_struct_find(st, name);
    if (!def) return -1;
    return def->field_count;
}

int ffi_struct_field_kind(FFIState *st, const char *name, int field_index,
                          FFIFieldKind *out) {
    FFIStructDef *def = ffi_struct_find(st, name);
    if (!def) return 0;
    if (field_index < 0 || field_index >= def->field_count) return 0;
    *out = def->field_kinds[field_index];
    return 1;
}

char *ffi_read_cstring(long long addr, long long max_len) {
    if (addr == 0) return NULL; /* NULL pointer */
    if (max_len <= 0) return NULL;

    const char *src = (const char *)(size_t)addr;
    /* Bounded scan for the NUL terminator (avoids runaway reads). */
    long long n = 0;
    while (n < max_len && src[n] != '\0') n++;

    char *dst = (char *)myon_xmalloc((size_t)n + 1);
    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
    return dst;
}

long long ffi_load(FFIState *st, const char *path, char **err_msg) {
    FFILib *lib = ffi_platform_load(path, err_msg);
    if (!lib) return -1;

    if (st->count >= st->capacity) {
        int newcap = st->capacity ? st->capacity * 2 : 4;
        st->slots = (FFILib **)myon_xrealloc(st->slots,
                                             sizeof(FFILib *) * (size_t)newcap);
        st->capacity = newcap;
    }
    long long id = st->count;
    st->slots[st->count++] = lib;
    return id;
}

void *ffi_lookup_symbol(FFIState *st, long long handle,
                        const char *symbol_name) {
    if (handle < 0 || handle >= st->count) return NULL;
    FFILib *lib = st->slots[handle];
    if (!lib) return NULL; /* closed / retired */
    return ffi_platform_sym(lib, symbol_name);
}

int ffi_close(FFIState *st, long long handle) {
    if (handle < 0 || handle >= st->count) return 0;
    FFILib *lib = st->slots[handle];
    if (!lib) return 0; /* already closed */
    ffi_platform_close(lib);
    st->slots[handle] = NULL; /* retire; never reused */
    return 1;
}
