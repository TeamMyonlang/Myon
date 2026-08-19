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

#ifndef MYON_MVM_CHUNK_H
#define MYON_MVM_CHUNK_H

#include <stdint.h>
#include <stdio.h>

#include "value.h"
#include "types.h"
#include "mvm_bytecode.h"

/*
 * Bytecode chunk + constant pool container and the .myc (de)serializer
 * (docs/mvm_spec.md §1.4, §6).
 *
 * A program compiles to a Module that owns:
 *   - a single, shared constant pool (spec §6.3: one pool for the whole file)
 *   - a native-function name table (spec §6.6)
 *   - one or more chunks: the top-level "<main>" chunk plus one per
 *     myon.func / myon.lambda / struct method (spec §1.4, §4.8)
 *
 * The VM (Step 6) will consume the same Module structure produced here, either
 * fresh from the compiler or reloaded from a .myc file.
 */

/* One (bytecode offset -> source line) mapping row (spec §6.4 lines[]). */
typedef struct {
    uint32_t code_off;
    uint32_t line;
} LineEntry;

typedef struct Chunk {
    char     *name;        /* diagnostic name; "<main>" for top level */
    uint16_t  num_params;
    uint16_t  num_locals;  /* total local slots this chunk uses (spec §5.5) */
    uint16_t  ret_count;
    uint8_t   is_async;    /* 1 if compiled from a `myon.async myon.func` (spec §14.9) */

    /*
     * Per-local-slot capture bitmap (spec §7.3).  captured[s] != 0 means slot
     * `s` is captured by some nested closure, so the VM must box it in a shared
     * UpvalueCell for the frame's whole lifetime (rather than holding the value
     * inline on the operand stack) so a closure's mutation is visible to the
     * defining frame and to sibling closures.  NULL when no slot is captured.
     * Length is num_locals.  Serialized after `code` in the .myc (minor 1).
     */
    uint8_t  *captured;
    uint16_t  captured_len;   /* number of valid bytes in `captured` */

    uint8_t  *code;        /* bytecode bytes */
    uint32_t  code_len;
    uint32_t  code_cap;

    LineEntry *lines;      /* optional instr->line table */
    uint32_t   line_count;
    uint32_t   line_cap;
} Chunk;

/* A constant-pool entry.  Wraps either a runtime Value or a TypeSpec. */
typedef struct {
    MvmConstTag tag;
    Value       value;     /* for INT/FLOAT/STR/BOOL/NIL (owned) */
    TypeSpec   *typespec;  /* for TYPESPEC (owned), else NULL */
} ConstEntry;

typedef struct Module {
    /* shared constant pool (spec §6.3) */
    ConstEntry *consts;
    int         const_count;
    int         const_cap;

    /* native function name table (spec §6.6) */
    char      **natives;
    int         native_count;
    int         native_cap;

    /* chunk table (spec §6.4) */
    Chunk     **chunks;
    int         chunk_count;
    int         chunk_cap;

    int         entry_chunk;  /* usually 0 (main) */

    /* Source Info (spec §6.5) — best-effort, filled by the compiler driver. */
    int64_t     src_mtime;
    uint64_t    src_size;
    uint8_t     src_hash[32];
    char       *src_path;     /* owned, may be NULL */
} Module;

/* ---- Module lifecycle ---- */
Module *module_new(void);
void    module_free(Module *m);

/* Append a fresh, empty chunk and return its index. Chunk name is copied. */
int     module_add_chunk(Module *m, const char *name);
Chunk  *module_chunk(Module *m, int idx);

/*
 * Intern a constant into the shared pool with duplicate elimination
 * (spec §1.3 / §5.3 dedup).  The Value is copied (value_copy); the pool owns
 * its copy.  Returns the pool index.
 */
int module_add_const_int(Module *m, long long v);
int module_add_const_float(Module *m, double v);
int module_add_const_str(Module *m, const char *s);   /* copies s */
int module_add_const_bool(Module *m, int b);
int module_add_const_nil(Module *m);
/* TypeSpec constant (array/map element types etc.); clones t. Not deduped. */
int module_add_const_typespec(Module *m, const TypeSpec *t);

/* Intern a native function name (spec §6.6), deduplicated. Returns index. */
int module_add_native(Module *m, const char *name);

/* ---- code emission (used by the compiler) ---- */
void chunk_emit_byte(Chunk *c, uint8_t b, int line);
void chunk_emit_op(Chunk *c, OpCode op, int line);
void chunk_emit_u16(Chunk *c, uint16_t v);       /* little-endian */
void chunk_emit_op_u16(Chunk *c, OpCode op, uint16_t a, int line);
void chunk_emit_op_u8(Chunk *c, OpCode op, uint8_t a, int line);

/*
 * Emit a jump instruction with a placeholder 16-bit offset and return the
 * bytecode offset of the operand so it can be patched later (spec §4.7
 * backpatching).  Use chunk_patch_jump() once the target is known.
 */
int  chunk_emit_jump(Chunk *c, OpCode op, int line);
void chunk_patch_jump(Chunk *c, int operand_off);   /* patch to current end */

/*
 * Emit a backward jump to an absolute code offset already known (loop tops).
 */
void chunk_emit_loop(Chunk *c, OpCode op, int target_off, int line);

/* Current end-of-code offset (next instruction address). */
int  chunk_here(const Chunk *c);

/* ---- disassembler (spec Step5 verification method #1) ---- */
void mvm_chunk_disassemble(const Module *m, const Chunk *c, FILE *out);
void mvm_module_disassemble(const Module *m, FILE *out);

/* ---- .myc serialization (spec §6) ---- */
/* Returns 0 on success, non-zero on I/O error. */
int  mvm_module_write_file(const Module *m, const char *path);
/* Loads a .myc into a fresh Module (VM/tooling side). NULL on error. */
Module *mvm_module_read_file(const char *path);

#endif /* MYON_MVM_CHUNK_H */
