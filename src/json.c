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

#include "json.h"
#include "common.h"
#include "unicode.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    JSON_MISSING, JSON_NULL, JSON_BOOL, JSON_STRING, JSON_INT,
    JSON_FLOAT, JSON_ARRAY, JSON_OBJECT
} JsonKind;

typedef struct {
    JsonKind kind;
    const unsigned char *start, *end;
    size_t count;
} JsonValue;

typedef struct {
    const unsigned char *next, *end;
    const Value *selector;
    JsonValue selected;
} JsonParser;

static void whitespace(JsonParser *p) {
    while (p->next < p->end &&
           (*p->next == ' ' || *p->next == '\t' ||
            *p->next == '\r' || *p->next == '\n')) p->next++;
}

static int take(JsonParser *p, unsigned char ch) {
    if (p->next == p->end || *p->next != ch) return 0;
    p->next++;
    return 1;
}

static int literal(JsonParser *p, const char *s) {
    size_t n = strlen(s);
    if ((size_t)(p->end - p->next) < n || memcmp(p->next, s, n) != 0) return 0;
    p->next += n;
    return 1;
}

static int hex4(JsonParser *p, uint32_t *cp) {
    if ((size_t)(p->end - p->next) < 4) return 0;
    *cp = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = *p->next++;
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return 0;
        *cp = (*cp << 4) | digit;
    }
    return 1;
}

static int escaped(JsonParser *p, uint32_t *cp) {
    if (p->next == p->end) return 0;
    switch (*p->next++) {
        case '"':  *cp = '"';  return 1;
        case '\\': *cp = '\\'; return 1;
        case '/':  *cp = '/';  return 1;
        case 'b':  *cp = '\b'; return 1;
        case 'f':  *cp = '\f'; return 1;
        case 'n':  *cp = '\n'; return 1;
        case 'r':  *cp = '\r'; return 1;
        case 't':  *cp = '\t'; return 1;
        case 'u':
            if (!hex4(p, cp)) return 0;
            if (*cp >= 0xD800 && *cp <= 0xDBFF) {
                uint32_t low;
                if (!literal(p, "\\u") || !hex4(p, &low) || low < 0xDC00 || low > 0xDFFF)
                    return 0;
                *cp = 0x10000 + ((*cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (*cp >= 0xDC00 && *cp <= 0xDFFF) {
                return 0;
            }
            return 1;
        default: return 0;
    }
}

/* A decoded string never needs more bytes than its JSON representation. */
static int string(JsonParser *p, unsigned char *out, size_t *length) {
    size_t used = 0;
    if (!take(p, '"')) return 0;
    while (p->next < p->end) {
        if (take(p, '"')) {
            if (out) {
                out[used] = 0;
                *length = used;
            }
            return 1;
        }
        uint32_t cp;
        if (take(p, '\\')) {
            if (!escaped(p, &cp)) return 0;
        } else {
            size_t width = myon_utf8_decode(p->next, p->end, &cp);
            if (!width || cp < 0x20) return 0;
            p->next += width;
        }
        if (out) used += myon_utf8_encode(out + used, cp);
    }
    return 0;
}

static char *decode_string(const unsigned char *start, const unsigned char *end,
                           size_t *length) {
    JsonParser p = { .next = start, .end = end };
    unsigned char *out = myon_xmalloc((size_t)(end - start));
    if (!string(&p, out, length)) {
        free(out);
        return NULL;
    }
    return (char *)out;
}

static int digits(JsonParser *p) {
    const unsigned char *start = p->next;
    while (p->next < p->end && *p->next >= '0' && *p->next <= '9') p->next++;
    return p->next != start;
}

static JsonKind number(JsonParser *p) {
    take(p, '-');
    if (!take(p, '0') && !digits(p)) return JSON_MISSING;
    JsonKind kind = JSON_INT;
    if (take(p, '.')) {
        if (!digits(p)) return JSON_MISSING;
        kind = JSON_FLOAT;
    }
    if (take(p, 'e') || take(p, 'E')) {
        if (!take(p, '+')) take(p, '-');
        if (!digits(p)) return JSON_MISSING;
        kind = JSON_FLOAT;
    }
    return kind;
}

static int parse_value(JsonParser *p, unsigned depth, JsonValue *out);

static int container(JsonParser *p, unsigned depth, JsonValue *out) {
    int object = out->kind == JSON_OBJECT;
    unsigned char close = object ? '}' : ']';
    const Value *selector = depth == 0 ? p->selector : NULL;
    const char *key = object && selector && selector->type == TYPE_STR
                    ? selector->as.obj->as.str : NULL;
    size_t key_length = key ? strlen(key) : 0;
    whitespace(p);
    if (take(p, close)) return 1;
    for (;;) {
        int match = 0;
        if (object) {
            const unsigned char *start = p->next;
            if (!string(p, NULL, NULL)) return 0;
            if (key) {
                size_t length;
                char *name = decode_string(start, p->next, &length);
                match = name && length == key_length && memcmp(name, key, length) == 0;
                free(name);
            }
            whitespace(p);
            if (!take(p, ':')) return 0;
        } else if (selector && selector->type == TYPE_INT && selector->as.i >= 0) {
            match = (uint64_t)selector->as.i == out->count;
        }
        JsonValue child;
        if (!parse_value(p, depth + 1, &child)) return 0;
        if (match) p->selected = child;
        out->count++;
        whitespace(p);
        if (take(p, close)) return 1;
        if (!take(p, ',')) return 0;
        whitespace(p);
    }
}

static int parse_value(JsonParser *p, unsigned depth, JsonValue *out) {
    whitespace(p);
    if (depth >= 128 || p->next == p->end) return 0;
    *out = (JsonValue){ .start = p->next };
    switch (*p->next) {
        case '{':
        case '[':
            out->kind = *p->next++ == '{' ? JSON_OBJECT : JSON_ARRAY;
            if (!container(p, depth, out)) return 0;
            break;
        case '"':
            if (!string(p, NULL, NULL)) return 0;
            out->kind = JSON_STRING;
            break;
        case 't':
        case 'f':
            if (!literal(p, "true") && !literal(p, "false")) return 0;
            out->kind = JSON_BOOL;
            break;
        case 'n':
            if (!literal(p, "null")) return 0;
            out->kind = JSON_NULL;
            break;
        default:
            out->kind = number(p);
            if (out->kind == JSON_MISSING) return 0;
    }
    out->end = p->next;
    return 1;
}

static char *raw_value(JsonValue v) {
    size_t n = (size_t)(v.end - v.start);
    char *text = myon_xmalloc(n + 1);
    memcpy(text, v.start, n);
    text[n] = 0;
    return text;
}

static const char *number_result(JsonValue v, int integer, Value *result) {
    char *s = raw_value(v), *end;
    errno = 0;
    if (integer) *result = value_int(strtoll(s, &end, 10));
    else *result = value_float(strtod(s, &end));
    const char *error = errno == ERANGE ? "number out of range" :
                        *end ? "invalid number" : NULL;
    free(s);
    return error;
}

const char *myon_json_read(const char *text, const char *operation,
                          const Value *selector, Value *result) {
    JsonParser p = { .next = (const unsigned char *)text,
                     .end = (const unsigned char *)text + strlen(text),
                     .selector = selector };
    JsonValue root;
    *result = value_nil();
    if (!parse_value(&p, 0, &root)) return "invalid JSON";
    whitespace(&p);
    if (p.next != p.end) return "unexpected text after JSON value";
    if (strcmp(operation, "validate") == 0) {
        *result = value_bool(1);
        return NULL;
    }
    if (selector && !((root.kind == JSON_OBJECT && selector->type == TYPE_STR) ||
                      (root.kind == JSON_ARRAY && selector->type == TYPE_INT)))
        return "expected an object with a string key or an array with an integer index";
    JsonValue v = selector ? p.selected : root;
    if (v.kind == JSON_MISSING) return "key or index not found";

    if (strcmp(operation, "length") == 0 && (v.kind == JSON_OBJECT || v.kind == JSON_ARRAY))
        *result = value_int((long long)v.count);
    else if (strcmp(operation, "is_null") == 0)
        *result = value_bool(v.kind == JSON_NULL);
    else if (strcmp(operation, "get_bool") == 0 && v.kind == JSON_BOOL)
        *result = value_bool(*v.start == 't');
    else if (strcmp(operation, "get_str") == 0 && v.kind == JSON_STRING) {
        size_t length;
        char *s = decode_string(v.start, v.end, &length);
        if (!s) return "invalid JSON string";
        if (memchr(s, 0, length)) {
            free(s);
            return "string contains U+0000";
        }
        *result = value_str(s);
    } else if ((strcmp(operation, "get_object") == 0 && v.kind == JSON_OBJECT) ||
               (strcmp(operation, "get_array") == 0 && v.kind == JSON_ARRAY))
        *result = value_str(raw_value(v));
    else if (strcmp(operation, "get_int") == 0 && v.kind == JSON_INT)
        return number_result(v, 1, result);
    else if (strcmp(operation, "get_float") == 0 && (v.kind == JSON_INT || v.kind == JSON_FLOAT))
        return number_result(v, 0, result);
    else
        return "JSON value has the wrong type";
    return NULL;
}
