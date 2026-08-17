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

#include "mvm_chunk.h"
#include "common.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

/* ================================================================== */
/* Opcode metadata (mvm_bytecode.h declarations)                       */
/* ================================================================== */

const char *mvm_opcode_name(uint8_t op) {
    switch (op) {
        case MOP_NOP:          return "NOP";
        case MOP_PUSH_CONST:   return "PUSH_CONST";
        case MOP_PUSH_TRUE:    return "PUSH_TRUE";
        case MOP_PUSH_FALSE:   return "PUSH_FALSE";
        case MOP_PUSH_NIL:     return "PUSH_NIL";
        case MOP_POP:          return "POP";
        case MOP_DUP:          return "DUP";
        case MOP_ADD:          return "ADD";
        case MOP_SUB:          return "SUB";
        case MOP_MUL:          return "MUL";
        case MOP_DIV:          return "DIV";
        case MOP_EQ:           return "EQ";
        case MOP_NEQ:          return "NEQ";
        case MOP_LT:           return "LT";
        case MOP_GT:           return "GT";
        case MOP_LE:           return "LE";
        case MOP_GE:           return "GE";
        case MOP_NEG:          return "NEG";
        case MOP_NOT:          return "NOT";
        case MOP_LOAD_LOCAL:   return "LOAD_LOCAL";
        case MOP_STORE_LOCAL:  return "STORE_LOCAL";
        case MOP_LOAD_GLOBAL:  return "LOAD_GLOBAL";
        case MOP_STORE_GLOBAL: return "STORE_GLOBAL";
        case MOP_JUMP:         return "JUMP";
        case MOP_JUMP_IF_FALSE:return "JUMP_IF_FALSE";
        case MOP_JUMP_IF_TRUE: return "JUMP_IF_TRUE";
        case MOP_MAKE_CLOSURE: return "MAKE_CLOSURE";
        case MOP_CALL:         return "CALL";
        case MOP_RET:          return "RET";
        case MOP_NEW_ARRAY:    return "NEW_ARRAY";
        case MOP_NEW_MAP:      return "NEW_MAP";
        case MOP_ARRAY_PUSH:   return "ARRAY_PUSH";
        case MOP_INDEX_GET:    return "INDEX_GET";
        case MOP_INDEX_SET:    return "INDEX_SET";
        case MOP_NEW_STRUCT:   return "NEW_STRUCT";
        case MOP_GET_FIELD:    return "GET_FIELD";
        case MOP_SET_FIELD:    return "SET_FIELD";
        case MOP_INVOKE:       return "INVOKE";
        case MOP_STR_CONCAT:   return "STR_CONCAT";
        case MOP_TO_STR:       return "TO_STR";
        case MOP_CAST_STR:     return "CAST_STR";
        case MOP_CAST_INT:     return "CAST_INT";
        case MOP_CAST_CHAR:    return "CAST_CHAR";
        case MOP_MAKE_ERROR:   return "MAKE_ERROR";
        case MOP_CALL_NATIVE:  return "CALL_NATIVE";
        case MOP_UNPACK:       return "UNPACK";
        case MOP_CHECK_NOT_NIL: return "CHECK_NOT_NIL";
        default:              return "???";
    }
}

int mvm_opcode_operand_bytes(uint8_t op) {
    switch (op) {
        /* no operands */
        case MOP_NOP: case MOP_PUSH_TRUE: case MOP_PUSH_FALSE: case MOP_PUSH_NIL:
        case MOP_POP: case MOP_DUP:
        case MOP_ADD: case MOP_SUB: case MOP_MUL: case MOP_DIV:
        case MOP_EQ: case MOP_NEQ: case MOP_LT: case MOP_GT: case MOP_LE: case MOP_GE:
        case MOP_NEG: case MOP_NOT:
        case MOP_ARRAY_PUSH: case MOP_INDEX_GET: case MOP_INDEX_SET:
        case MOP_STR_CONCAT: case MOP_TO_STR:
        case MOP_CAST_STR: case MOP_CAST_INT: case MOP_CAST_CHAR: case MOP_MAKE_ERROR:
        case MOP_CHECK_NOT_NIL:
            return 0;
        /* one u16 */
        case MOP_PUSH_CONST:
        case MOP_LOAD_LOCAL: case MOP_STORE_LOCAL:
        case MOP_LOAD_GLOBAL: case MOP_STORE_GLOBAL:
        case MOP_JUMP: case MOP_JUMP_IF_FALSE: case MOP_JUMP_IF_TRUE:
        case MOP_MAKE_CLOSURE:
        case MOP_NEW_ARRAY: case MOP_NEW_MAP:
        case MOP_NEW_STRUCT: case MOP_GET_FIELD: case MOP_SET_FIELD:
            return 2;
        /* one u8 */
        case MOP_CALL: case MOP_RET: case MOP_UNPACK:
            return 1;
        /* u16 + u8 */
        case MOP_INVOKE:
        case MOP_CALL_NATIVE:
            return 3;
        default:
            return -1;
    }
}

/* ================================================================== */
/* Module + Chunk lifecycle                                            */
/* ================================================================== */

Module *module_new(void) {
    Module *m = (Module *)myon_xmalloc(sizeof(Module));
    memset(m, 0, sizeof(*m));
    return m;
}

static void chunk_free(Chunk *c) {
    if (!c) return;
    free(c->name);
    free(c->code);
    free(c->lines);
    free(c);
}

void module_free(Module *m) {
    if (!m) return;
    for (int i = 0; i < m->const_count; i++) {
        value_free(&m->consts[i].value);
        if (m->consts[i].typespec) typespec_free(m->consts[i].typespec);
    }
    free(m->consts);
    for (int i = 0; i < m->native_count; i++) free(m->natives[i]);
    free(m->natives);
    for (int i = 0; i < m->chunk_count; i++) chunk_free(m->chunks[i]);
    free(m->chunks);
    free(m->src_path);
    free(m);
}

int module_add_chunk(Module *m, const char *name) {
    if (m->chunk_count == m->chunk_cap) {
        m->chunk_cap = m->chunk_cap ? m->chunk_cap * 2 : 4;
        m->chunks = (Chunk **)myon_xrealloc(m->chunks, sizeof(Chunk *) * m->chunk_cap);
    }
    Chunk *c = (Chunk *)myon_xmalloc(sizeof(Chunk));
    memset(c, 0, sizeof(*c));
    c->name = myon_strdup(name ? name : "<anon>");
    int idx = m->chunk_count++;
    m->chunks[idx] = c;
    return idx;
}

Chunk *module_chunk(Module *m, int idx) {
    if (idx < 0 || idx >= m->chunk_count) return NULL;
    return m->chunks[idx];
}

/* ================================================================== */
/* Constant pool (dedup, spec §1.3 / §5.3)                             */
/* ================================================================== */

static int const_reserve(Module *m) {
    if (m->const_count == m->const_cap) {
        m->const_cap = m->const_cap ? m->const_cap * 2 : 8;
        m->consts = (ConstEntry *)myon_xrealloc(m->consts, sizeof(ConstEntry) * m->const_cap);
    }
    return m->const_count;
}

static int const_add(Module *m, MvmConstTag tag, Value v, TypeSpec *ts) {
    int idx = const_reserve(m);
    m->consts[idx].tag = tag;
    m->consts[idx].value = v;
    m->consts[idx].typespec = ts;
    m->const_count++;
    return idx;
}

int module_add_const_int(Module *m, long long v) {
    for (int i = 0; i < m->const_count; i++)
        if (m->consts[i].tag == MVM_CONST_INT && m->consts[i].value.as.i == v)
            return i;
    return const_add(m, MVM_CONST_INT, value_int(v), NULL);
}

int module_add_const_float(Module *m, double v) {
    for (int i = 0; i < m->const_count; i++)
        if (m->consts[i].tag == MVM_CONST_FLOAT && m->consts[i].value.as.f == v)
            return i;
    return const_add(m, MVM_CONST_FLOAT, value_float(v), NULL);
}

int module_add_const_str(Module *m, const char *s) {
    for (int i = 0; i < m->const_count; i++)
        if (m->consts[i].tag == MVM_CONST_STR &&
            m->consts[i].value.as.obj && m->consts[i].value.as.obj->as.str &&
            strcmp(m->consts[i].value.as.obj->as.str, s) == 0)
            return i;
    return const_add(m, MVM_CONST_STR, value_str(myon_strdup(s)), NULL);
}

int module_add_const_bool(Module *m, int b) {
    for (int i = 0; i < m->const_count; i++)
        if (m->consts[i].tag == MVM_CONST_BOOL && m->consts[i].value.as.b == (b ? 1 : 0))
            return i;
    return const_add(m, MVM_CONST_BOOL, value_bool(b), NULL);
}

int module_add_const_nil(Module *m) {
    for (int i = 0; i < m->const_count; i++)
        if (m->consts[i].tag == MVM_CONST_NIL) return i;
    return const_add(m, MVM_CONST_NIL, value_nil(), NULL);
}

int module_add_const_typespec(Module *m, const TypeSpec *t) {
    /* Not deduped (structural equality is cheap enough to skip for now). */
    Value nilv = value_nil();
    return const_add(m, MVM_CONST_TYPESPEC, nilv, typespec_clone(t));
}

int module_add_native(Module *m, const char *name) {
    for (int i = 0; i < m->native_count; i++)
        if (strcmp(m->natives[i], name) == 0) return i;
    if (m->native_count == m->native_cap) {
        m->native_cap = m->native_cap ? m->native_cap * 2 : 8;
        m->natives = (char **)myon_xrealloc(m->natives, sizeof(char *) * m->native_cap);
    }
    int idx = m->native_count++;
    m->natives[idx] = myon_strdup(name);
    return idx;
}

/* ================================================================== */
/* Code emission                                                       */
/* ================================================================== */

static void chunk_reserve(Chunk *c, uint32_t extra) {
    if (c->code_len + extra > c->code_cap) {
        uint32_t cap = c->code_cap ? c->code_cap : 32;
        while (c->code_len + extra > cap) cap *= 2;
        c->code = (uint8_t *)myon_xrealloc(c->code, cap);
        c->code_cap = cap;
    }
}

static void chunk_note_line(Chunk *c, uint32_t off, int line) {
    if (line <= 0) return;
    /* Only record when the line changes to keep the table compact. */
    if (c->line_count && c->lines[c->line_count - 1].line == (uint32_t)line)
        return;
    if (c->line_count == c->line_cap) {
        c->line_cap = c->line_cap ? c->line_cap * 2 : 16;
        c->lines = (LineEntry *)myon_xrealloc(c->lines, sizeof(LineEntry) * c->line_cap);
    }
    c->lines[c->line_count].code_off = off;
    c->lines[c->line_count].line = (uint32_t)line;
    c->line_count++;
}

void chunk_emit_byte(Chunk *c, uint8_t b, int line) {
    chunk_reserve(c, 1);
    if (line > 0) chunk_note_line(c, c->code_len, line);
    c->code[c->code_len++] = b;
}

void chunk_emit_op(Chunk *c, OpCode op, int line) {
    chunk_emit_byte(c, (uint8_t)op, line);
}

void chunk_emit_u16(Chunk *c, uint16_t v) {
    chunk_reserve(c, 2);
    c->code[c->code_len++] = (uint8_t)(v & 0xFF);
    c->code[c->code_len++] = (uint8_t)((v >> 8) & 0xFF);
}

void chunk_emit_op_u16(Chunk *c, OpCode op, uint16_t a, int line) {
    chunk_emit_op(c, op, line);
    chunk_emit_u16(c, a);
}

void chunk_emit_op_u8(Chunk *c, OpCode op, uint8_t a, int line) {
    chunk_emit_op(c, op, line);
    chunk_reserve(c, 1);
    c->code[c->code_len++] = a;
}

int chunk_here(const Chunk *c) { return (int)c->code_len; }

int chunk_emit_jump(Chunk *c, OpCode op, int line) {
    chunk_emit_op(c, op, line);
    int operand_off = (int)c->code_len;
    chunk_emit_u16(c, 0xFFFF); /* placeholder */
    return operand_off;
}

void chunk_patch_jump(Chunk *c, int operand_off) {
    /* base for the relative offset is the address right after the operand */
    int base = operand_off + 2;
    int target = (int)c->code_len;
    int rel = target - base;
    /* store as little-endian s16 */
    c->code[operand_off]     = (uint8_t)(rel & 0xFF);
    c->code[operand_off + 1] = (uint8_t)((rel >> 8) & 0xFF);
}

void chunk_emit_loop(Chunk *c, OpCode op, int target_off, int line) {
    chunk_emit_op(c, op, line);
    int operand_off = (int)c->code_len;
    chunk_emit_u16(c, 0);
    int base = operand_off + 2;
    int rel = target_off - base;   /* negative for a backward loop */
    c->code[operand_off]     = (uint8_t)(rel & 0xFF);
    c->code[operand_off + 1] = (uint8_t)((rel >> 8) & 0xFF);
}

/* ================================================================== */
/* Disassembler (spec Step5 verification #1)                           */
/* ================================================================== */

static const char *const_render(const Module *m, int idx, char *buf, size_t n) {
    if (idx < 0 || idx >= m->const_count) { snprintf(buf, n, "<bad const %d>", idx); return buf; }
    const ConstEntry *e = &m->consts[idx];
    switch (e->tag) {
        case MVM_CONST_INT:   snprintf(buf, n, "%lld", e->value.as.i); break;
        case MVM_CONST_FLOAT: snprintf(buf, n, "%g", e->value.as.f); break;
        case MVM_CONST_BOOL:  snprintf(buf, n, "%s", e->value.as.b ? "true" : "false"); break;
        case MVM_CONST_NIL:   snprintf(buf, n, "nil"); break;
        case MVM_CONST_STR: {
            const char *s = (e->value.as.obj && e->value.as.obj->as.str)
                                ? e->value.as.obj->as.str : "";
            snprintf(buf, n, "\"%s\"", s);
            break;
        }
        case MVM_CONST_TYPESPEC: {
            char *ts = e->typespec ? typespec_to_cstr(e->typespec) : NULL;
            snprintf(buf, n, "type %s", ts ? ts : "?");
            free(ts);
            break;
        }
        default: snprintf(buf, n, "<const %d>", idx); break;
    }
    return buf;
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static int16_t rd_s16(const uint8_t *p) {
    return (int16_t)(p[0] | (p[1] << 8));
}

void mvm_chunk_disassemble(const Module *m, const Chunk *c, FILE *out) {
    fprintf(out, "== chunk %s  (params=%u locals=%u ret=%u, %u bytes) ==\n",
            c->name, c->num_params, c->num_locals, c->ret_count, c->code_len);

    char buf[256];
    uint32_t li = 0;      /* line-table cursor */
    int last_line = -1;
    uint32_t off = 0;
    while (off < c->code_len) {
        /* resolve source line for this offset */
        while (li + 1 < c->line_count && c->lines[li + 1].code_off <= off) li++;
        int line = (c->line_count && c->lines[li].code_off <= off)
                       ? (int)c->lines[li].line : -1;

        fprintf(out, "  %04u ", off);
        if (line != last_line && line > 0) {
            fprintf(out, "%4d ", line);
            last_line = line;
        } else {
            fprintf(out, "   | ");
        }

        uint8_t op = c->code[off];
        const char *name = mvm_opcode_name(op);
        int nb = mvm_opcode_operand_bytes(op);
        if (nb < 0) {
            fprintf(out, "%-14s <unknown opcode 0x%02X>\n", "???", op);
            off += 1;
            continue;
        }
        const uint8_t *p = &c->code[off + 1];

        switch (op) {
            case MOP_PUSH_CONST:
            case MOP_NEW_ARRAY:
            case MOP_NEW_MAP: {
                uint16_t idx = rd_u16(p);
                fprintf(out, "%-14s %u\t; %s\n", name, idx, const_render(m, idx, buf, sizeof buf));
                break;
            }
            case MOP_GET_FIELD:
            case MOP_SET_FIELD: {
                uint16_t idx = rd_u16(p);
                fprintf(out, "%-14s %u\t; %s\n", name, idx, const_render(m, idx, buf, sizeof buf));
                break;
            }
            case MOP_NEW_STRUCT: {
                uint16_t idx = rd_u16(p);
                fprintf(out, "%-14s %u\t; %s\n", name, idx, const_render(m, idx, buf, sizeof buf));
                break;
            }
            case MOP_LOAD_LOCAL:
            case MOP_STORE_LOCAL:
            case MOP_LOAD_GLOBAL:
            case MOP_STORE_GLOBAL: {
                uint16_t slot = rd_u16(p);
                fprintf(out, "%-14s %u\n", name, slot);
                break;
            }
            case MOP_JUMP:
            case MOP_JUMP_IF_FALSE:
            case MOP_JUMP_IF_TRUE: {
                int16_t rel = rd_s16(p);
                uint32_t target = off + 3 + rel;
                fprintf(out, "%-14s %+d\t; -> %04u\n", name, rel, target);
                break;
            }
            case MOP_MAKE_CLOSURE: {
                uint16_t idx = rd_u16(p);
                const char *cn = (idx < (uint16_t)m->chunk_count) ? m->chunks[idx]->name : "?";
                fprintf(out, "%-14s %u\t; chunk %s\n", name, idx, cn);
                break;
            }
            case MOP_CALL:
            case MOP_RET:
            case MOP_UNPACK: {
                uint8_t a = p[0];
                fprintf(out, "%-14s %u\n", name, a);
                break;
            }
            case MOP_INVOKE: {
                uint16_t idx = rd_u16(p);
                uint8_t argc = p[2];
                fprintf(out, "%-14s %u, %u\t; .%s\n", name, idx, argc,
                        const_render(m, idx, buf, sizeof buf));
                break;
            }
            case MOP_CALL_NATIVE: {
                uint16_t nid = rd_u16(p);
                uint8_t argc = p[2];
                const char *nn = (nid < (uint16_t)m->native_count) ? m->natives[nid] : "?";
                fprintf(out, "%-14s %u, %u\t; %s\n", name, nid, argc, nn);
                break;
            }
            default:
                fprintf(out, "%s\n", name);
                break;
        }
        off += 1 + (uint32_t)nb;
    }
}

void mvm_module_disassemble(const Module *m, FILE *out) {
    fprintf(out, "; MVM module: %d chunk(s), %d const(s), %d native(s), entry=%d\n",
            m->chunk_count, m->const_count, m->native_count, m->entry_chunk);
    if (m->native_count) {
        fprintf(out, "; natives:");
        for (int i = 0; i < m->native_count; i++)
            fprintf(out, " [%d]%s", i, m->natives[i]);
        fprintf(out, "\n");
    }
    for (int i = 0; i < m->chunk_count; i++) {
        fprintf(out, "\n; chunk #%d\n", i);
        mvm_chunk_disassemble(m, m->chunks[i], out);
    }
}

/* ================================================================== */
/* .myc serialization (spec §6). Little-endian throughout.             */
/* ================================================================== */

/* ---- growable byte buffer for building the file image ---- */
typedef struct { uint8_t *data; size_t len, cap; } Buf;

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + extra > cap) cap *= 2;
        b->data = (uint8_t *)myon_xrealloc(b->data, cap);
        b->cap = cap;
    }
}
static void buf_u8(Buf *b, uint8_t v)  { buf_reserve(b, 1); b->data[b->len++] = v; }
static void buf_u16(Buf *b, uint16_t v){ buf_u8(b, v & 0xFF); buf_u8(b, (v >> 8) & 0xFF); }
static void buf_u32(Buf *b, uint32_t v){ for (int i = 0; i < 4; i++) buf_u8(b, (v >> (8*i)) & 0xFF); }
static void buf_u64(Buf *b, uint64_t v){ for (int i = 0; i < 8; i++) buf_u8(b, (v >> (8*i)) & 0xFF); }
static void buf_bytes(Buf *b, const void *p, size_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static void buf_str(Buf *b, const char *s) {
    uint32_t n = s ? (uint32_t)strlen(s) : 0;
    buf_u32(b, n);
    if (n) buf_bytes(b, s, n);
}

static void ser_typespec(Buf *b, const TypeSpec *t) {
    /* spec §6.3.1: base(u8) [name] [elem] [key] [argc(u16) args...] */
    uint8_t base = t ? (uint8_t)t->base : (uint8_t)TYPE_UNKNOWN;
    buf_u8(b, base);
    if (!t) { buf_u16(b, 0); return; }
    if (t->base == TYPE_STRUCT || t->base == TYPE_TYPEPARAM)
        buf_str(b, t->name ? t->name : "");
    if (t->base == TYPE_ARRAY || t->base == TYPE_MAP)
        ser_typespec(b, t->elem);
    if (t->base == TYPE_MAP)
        ser_typespec(b, t->key);
    buf_u16(b, (uint16_t)t->arg_count);
    for (int i = 0; i < t->arg_count; i++)
        ser_typespec(b, t->args[i]);
}

static void ser_const_pool(Buf *b, const Module *m) {
    buf_u32(b, (uint32_t)m->const_count);
    for (int i = 0; i < m->const_count; i++) {
        const ConstEntry *e = &m->consts[i];
        buf_u8(b, (uint8_t)e->tag);
        switch (e->tag) {
            case MVM_CONST_INT:   buf_u64(b, (uint64_t)e->value.as.i); break;
            case MVM_CONST_FLOAT: {
                uint64_t bits; memcpy(&bits, &e->value.as.f, 8); buf_u64(b, bits); break;
            }
            case MVM_CONST_STR:
                buf_str(b, (e->value.as.obj && e->value.as.obj->as.str)
                              ? e->value.as.obj->as.str : "");
                break;
            case MVM_CONST_BOOL:  buf_u8(b, e->value.as.b ? 1 : 0); break;
            case MVM_CONST_NIL:   break;
            case MVM_CONST_TYPESPEC: ser_typespec(b, e->typespec); break;
            default: break;
        }
    }
}

static void ser_native_table(Buf *b, const Module *m) {
    buf_u16(b, (uint16_t)m->native_count);
    for (int i = 0; i < m->native_count; i++)
        buf_str(b, m->natives[i]);
}

static void ser_chunk(Buf *b, const Chunk *c) {
    buf_str(b, c->name);
    buf_u16(b, c->num_params);
    buf_u16(b, c->num_locals);
    buf_u16(b, c->ret_count);
    buf_u32(b, c->code_len);
    buf_bytes(b, c->code, c->code_len);
    buf_u32(b, c->line_count);
    for (uint32_t i = 0; i < c->line_count; i++) {
        buf_u32(b, c->lines[i].code_off);
        buf_u32(b, c->lines[i].line);
    }
}

int mvm_module_write_file(const Module *m, const char *path) {
    Buf b = {0};

    /* --- header (spec §6.2), 32 bytes; offsets patched after layout --- */
    buf_u8(&b, MVM_MAGIC0); buf_u8(&b, MVM_MAGIC1);
    buf_u8(&b, MVM_MAGIC2); buf_u8(&b, MVM_MAGIC3);
    buf_u8(&b, MVM_VERSION_MAJOR);
    buf_u8(&b, MVM_VERSION_MINOR);
    buf_u8(&b, MVM_ENDIAN_LITTLE);
    buf_u8(&b, 0);                 /* flags */
    size_t off_const_off = b.len;   buf_u32(&b, 0); /* const pool offset  */
    size_t off_const_sz  = b.len;   buf_u32(&b, 0); /* const pool size    */
    size_t off_chunk_off = b.len;   buf_u32(&b, 0); /* chunk table offset */
    buf_u32(&b, (uint32_t)m->chunk_count);          /* chunk count        */
    buf_u32(&b, (uint32_t)m->entry_chunk);          /* entry chunk        */
    size_t off_srcinfo   = b.len;   buf_u32(&b, 0); /* source info offset */

    /* --- source info (spec §6.5) --- */
    uint32_t srcinfo_off = (uint32_t)b.len;
    buf_u64(&b, (uint64_t)m->src_mtime);
    buf_u64(&b, m->src_size);
    buf_bytes(&b, m->src_hash, 32);
    buf_str(&b, m->src_path);

    /* --- constant pool (spec §6.3) --- */
    uint32_t const_off = (uint32_t)b.len;
    ser_const_pool(&b, m);
    uint32_t const_sz = (uint32_t)b.len - const_off;

    /* --- native table (spec §6.6) --- */
    ser_native_table(&b, m);

    /* --- chunk table (spec §6.4) --- */
    uint32_t chunk_off = (uint32_t)b.len;
    buf_u32(&b, (uint32_t)m->chunk_count);
    for (int i = 0; i < m->chunk_count; i++)
        ser_chunk(&b, m->chunks[i]);

    /* --- patch header offsets --- */
    #define PATCH_U32(pos, val) do { \
        b.data[(pos)]   = (uint8_t)((val) & 0xFF); \
        b.data[(pos)+1] = (uint8_t)(((val) >> 8) & 0xFF); \
        b.data[(pos)+2] = (uint8_t)(((val) >> 16) & 0xFF); \
        b.data[(pos)+3] = (uint8_t)(((val) >> 24) & 0xFF); \
    } while (0)
    PATCH_U32(off_const_off, const_off);
    PATCH_U32(off_const_sz,  const_sz);
    PATCH_U32(off_chunk_off, chunk_off);
    PATCH_U32(off_srcinfo,   srcinfo_off);
    #undef PATCH_U32

    FILE *f = fopen(path, "wb");
    if (!f) { free(b.data); return 1; }
    size_t wrote = fwrite(b.data, 1, b.len, f);
    fclose(f);
    free(b.data);
    return wrote == b.len ? 0 : 2;
}

/* ---- reader ---- */
typedef struct { const uint8_t *p; size_t len, pos; int err; } Rd;

static uint8_t  rd8(Rd *r)  { if (r->pos + 1 > r->len) { r->err = 1; return 0; } return r->p[r->pos++]; }
static uint16_t rd16(Rd *r) { uint16_t a = rd8(r); uint16_t b = rd8(r); return (uint16_t)(a | (b << 8)); }
static uint32_t rd32(Rd *r) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)rd8(r) << (8*i); return v; }
static uint64_t rd64(Rd *r) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)rd8(r) << (8*i); return v; }
static char *rdstr(Rd *r) {
    uint32_t n = rd32(r);
    if (r->err || r->pos + n > r->len) { r->err = 1; return NULL; }
    char *s = (char *)myon_xmalloc(n + 1);
    memcpy(s, r->p + r->pos, n);
    s[n] = '\0';
    r->pos += n;
    return s;
}

static TypeSpec *deser_typespec(Rd *r) {
    uint8_t base = rd8(r);
    TypeSpec *t = typespec_new((Type)base);
    if (base == TYPE_STRUCT || base == TYPE_TYPEPARAM) {
        char *nm = rdstr(r);
        t->name = nm;
    }
    if (base == TYPE_ARRAY || base == TYPE_MAP)
        t->elem = deser_typespec(r);
    if (base == TYPE_MAP)
        t->key = deser_typespec(r);
    uint16_t argc = rd16(r);
    t->arg_count = argc;
    if (argc) {
        t->args = (TypeSpec **)myon_xmalloc(sizeof(TypeSpec *) * argc);
        for (int i = 0; i < argc; i++) t->args[i] = deser_typespec(r);
    }
    return t;
}

Module *mvm_module_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)myon_xmalloc((size_t)sz);
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(data); return NULL; }

    Rd r = { data, (size_t)sz, 0, 0 };
    if (rd8(&r) != MVM_MAGIC0 || rd8(&r) != MVM_MAGIC1 ||
        rd8(&r) != MVM_MAGIC2 || rd8(&r) != MVM_MAGIC3) { free(data); return NULL; }
    uint8_t major = rd8(&r);
    (void)rd8(&r);            /* minor */
    uint8_t endian = rd8(&r);
    (void)rd8(&r);            /* flags */
    if (major != MVM_VERSION_MAJOR || endian != MVM_ENDIAN_LITTLE) { free(data); return NULL; }

    uint32_t const_off = rd32(&r);
    (void)rd32(&r);           /* const size */
    uint32_t chunk_off = rd32(&r);
    uint32_t chunk_count = rd32(&r);
    uint32_t entry = rd32(&r);
    uint32_t srcinfo_off = rd32(&r);

    Module *m = module_new();
    m->entry_chunk = (int)entry;

    /* source info */
    r.pos = srcinfo_off;
    m->src_mtime = (int64_t)rd64(&r);
    m->src_size = rd64(&r);
    for (int i = 0; i < 32; i++) m->src_hash[i] = rd8(&r);
    m->src_path = rdstr(&r);

    /* constant pool */
    r.pos = const_off;
    uint32_t cc = rd32(&r);
    for (uint32_t i = 0; i < cc && !r.err; i++) {
        uint8_t tag = rd8(&r);
        switch (tag) {
            case MVM_CONST_INT:   module_add_const_int(m, (long long)rd64(&r)); break;
            case MVM_CONST_FLOAT: {
                uint64_t bits = rd64(&r); double d; memcpy(&d, &bits, 8);
                module_add_const_float(m, d); break;
            }
            case MVM_CONST_STR:   { char *s = rdstr(&r); module_add_const_str(m, s ? s : ""); free(s); break; }
            case MVM_CONST_BOOL:  module_add_const_bool(m, rd8(&r)); break;
            case MVM_CONST_NIL:   module_add_const_nil(m); break;
            case MVM_CONST_TYPESPEC: {
                TypeSpec *t = deser_typespec(&r);
                module_add_const_typespec(m, t);
                typespec_free(t);
                break;
            }
            default: r.err = 1; break;
        }
    }

    /* native table (immediately after const pool) */
    uint16_t nc = rd16(&r);
    for (uint16_t i = 0; i < nc && !r.err; i++) {
        char *s = rdstr(&r);
        module_add_native(m, s ? s : "");
        free(s);
    }

    /* chunk table */
    r.pos = chunk_off;
    uint32_t ct = rd32(&r);
    (void)chunk_count;
    for (uint32_t i = 0; i < ct && !r.err; i++) {
        char *name = rdstr(&r);
        int idx = module_add_chunk(m, name ? name : "<anon>");
        free(name);
        Chunk *c = m->chunks[idx];
        c->num_params = rd16(&r);
        c->num_locals = rd16(&r);
        c->ret_count  = rd16(&r);
        c->code_len   = rd32(&r);
        if (r.err || r.pos + c->code_len > r.len) { r.err = 1; break; }
        c->code_cap = c->code_len ? c->code_len : 1;
        c->code = (uint8_t *)myon_xmalloc(c->code_cap);
        memcpy(c->code, r.p + r.pos, c->code_len);
        r.pos += c->code_len;
        c->line_count = rd32(&r);
        if (c->line_count) {
            c->line_cap = c->line_count;
            c->lines = (LineEntry *)myon_xmalloc(sizeof(LineEntry) * c->line_cap);
            for (uint32_t j = 0; j < c->line_count; j++) {
                c->lines[j].code_off = rd32(&r);
                c->lines[j].line = rd32(&r);
            }
        }
    }

    free(data);
    if (r.err) { module_free(m); return NULL; }
    return m;
}
