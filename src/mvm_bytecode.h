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

#ifndef MYON_MVM_BYTECODE_H
#define MYON_MVM_BYTECODE_H

#include <stdint.h>

/*
 * Myon Virtual Machine (MVM) bytecode definitions.
 *
 * This is the shared instruction-set header referenced by both the compiler
 * (Step 5, src/mvm_compiler.c) and the future VM (Step 6).  It follows the
 * design fixed in docs/mvm_spec.md:
 *
 *   - a stack machine (spec §1.1)
 *   - variable-length instructions: 1-byte opcode followed by fixed-width
 *     operands, all stored little-endian (spec §1.2)
 *   - a per-file constant pool referenced by u16 index (spec §1.3)
 *
 * Operand widths used by the instructions below (spec §1.2):
 *   u8   1 byte   arg count / small native id
 *   u16  2 bytes  constant index, local slot, name index, native id
 *   s16  2 bytes  signed relative jump offset (base = address right after the
 *                 operand, spec §4.7)
 *
 * The opcode numbers are assigned here (spec §4.0 delegated the concrete
 * numbers to the implementation).  The banding matches spec §4.0's table so
 * future additions stay within their reserved range.
 */

typedef enum {
    /* 0x00-0x0F : stack / constant load (spec §4.1) */
    MOP_NOP        = 0x00,
    MOP_PUSH_CONST = 0x01,  /* u16 idx : push constants[idx] */
    MOP_PUSH_TRUE  = 0x02,
    MOP_PUSH_FALSE = 0x03,
    MOP_PUSH_NIL   = 0x04,
    MOP_POP        = 0x05,
    MOP_DUP        = 0x06,

    /* 0x10-0x2F : arithmetic / comparison / logical / unary (spec §4.2-4.4) */
    MOP_ADD        = 0x10,
    MOP_SUB        = 0x11,
    MOP_MUL        = 0x12,
    MOP_DIV        = 0x13,

    MOP_EQ         = 0x18,
    MOP_NEQ        = 0x19,
    MOP_LT         = 0x1A,
    MOP_GT         = 0x1B,
    MOP_LE         = 0x1C,
    MOP_GE         = 0x1D,

    MOP_NEG        = 0x28,
    MOP_NOT        = 0x29,

    /* 0x30-0x3F : local variables (spec §4.6) */
    MOP_LOAD_LOCAL  = 0x30,  /* u16 slot */
    MOP_STORE_LOCAL = 0x31,  /* u16 slot */
    /*
     * Top-level ("global") variables live in the <main> chunk's frame.  A
     * nested function frame cannot reach them with LOAD_LOCAL (that is
     * frame-relative), so these address the entry frame's slot directly.
     * This realizes the spec §4.6 note "top-level variables are main-chunk
     * local slots" for the cross-frame case (recursion, mutually-recursive
     * top-level functions, and global reads/writes from inside a function).
     */
    MOP_LOAD_GLOBAL  = 0x32,  /* u16 slot (index into the entry/<main> frame) */
    MOP_STORE_GLOBAL = 0x33,  /* u16 slot */

    /* 0x40-0x4F : branches / jumps (spec §4.7) */
    MOP_JUMP          = 0x40,  /* s16 off */
    MOP_JUMP_IF_FALSE = 0x41,  /* s16 off : pop, jump if falsy */
    MOP_JUMP_IF_TRUE  = 0x42,  /* s16 off : pop, jump if truthy */

    /* 0x50-0x5F : function call / return (spec §4.8-4.9) */
    MOP_MAKE_CLOSURE = 0x50,  /* u16 chunk_idx */
    MOP_CALL         = 0x51,  /* u8 argc */
    MOP_RET          = 0x52,  /* u8 n */

    /* 0x60-0x6F : arrays / maps (spec §4.10) */
    MOP_NEW_ARRAY  = 0x60,  /* u16 type_idx */
    MOP_NEW_MAP    = 0x61,  /* u16 type_idx */
    MOP_ARRAY_PUSH = 0x62,
    MOP_INDEX_GET  = 0x63,
    MOP_INDEX_SET  = 0x64,

    /* 0x70-0x7F : structs / members / methods (spec §4.11) */
    MOP_NEW_STRUCT = 0x70,  /* u16 type_idx */
    MOP_GET_FIELD  = 0x71,  /* u16 name_idx */
    MOP_SET_FIELD  = 0x72,  /* u16 name_idx */
    MOP_INVOKE     = 0x73,  /* u16 name_idx, u8 argc */

    /* 0x80-0x8F : string interpolation / casts (spec §4.12-4.13) */
    MOP_STR_CONCAT = 0x80,
    MOP_TO_STR     = 0x81,
    MOP_CAST_STR   = 0x82,
    MOP_CAST_INT   = 0x83,
    MOP_CAST_CHAR  = 0x84,
    MOP_MAKE_ERROR = 0x85,

    /* 0x90-0x9F : native / module calls (spec §4.14) */
    MOP_CALL_NATIVE = 0x90,  /* u16 native_id, u8 argc */

    /* 0xA0-0xAF : misc (Step 7-b) */
    MOP_UNPACK        = 0xA0, /* u8 n : pop one tuple (array) value, push its n
                               * elements in order.  Used to spread a native
                               * call's (value,error) tuple across the N targets
                               * of a multiple-target assignment (spec §6.2). */
    MOP_CHECK_NOT_NIL = 0xA1  /* peek stack top; if it is myon.nil, raise the
                               * spec §2.4 "cannot assign myon.nil to a normal
                               * variable" error.  Emitted before the STORE of a
                               * single-target simple assignment so the VM
                               * mirrors the tree-walk's runtime rule. */
} OpCode;

/*
 * Constant-pool entry tags used in the .myc serialization (spec §6.3).
 */
typedef enum {
    MVM_CONST_INT      = 0x01,
    MVM_CONST_FLOAT    = 0x02,
    MVM_CONST_STR      = 0x03,
    MVM_CONST_BOOL     = 0x04,
    MVM_CONST_NIL      = 0x05,
    MVM_CONST_TYPESPEC = 0x10
} MvmConstTag;

/* .myc header constants (spec §6.2). */
#define MVM_MAGIC0        0x4D  /* 'M' */
#define MVM_MAGIC1        0x59  /* 'Y' */
#define MVM_MAGIC2        0x43  /* 'C' */
#define MVM_MAGIC3        0x31  /* '1' */
#define MVM_VERSION_MAJOR 1
#define MVM_VERSION_MINOR 0
#define MVM_ENDIAN_LITTLE 1

/*
 * Human-readable mnemonic for an opcode (used by the disassembler and any
 * diagnostics).  Returns a static string; "???" for unknown opcodes.
 */
const char *mvm_opcode_name(uint8_t op);

/*
 * Number of operand bytes that follow the given opcode (not counting the
 * opcode byte itself).  Used by the disassembler / VM to advance the ip.
 * Returns -1 for an unknown opcode.
 */
int mvm_opcode_operand_bytes(uint8_t op);

#endif /* MYON_MVM_BYTECODE_H */
