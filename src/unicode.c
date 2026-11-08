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

/* Unicode default full casing. Data license: third_party/unicode/LICENSE.txt. */
#include "unicode.h"
#include "common.h"
#include <string.h>

typedef struct {
    uint32_t code;
    uint32_t mapped[3];
    unsigned char length;
} CaseMapping;
typedef struct { uint32_t first, last; } CodeRange;
#include "unicode_case_data.inc"
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Strict RFC 3629 decoding. A zero result consumes no bytes; callers preserve
 * that invalid byte and advance once. Never read beyond the input's end. */
static size_t decode(const unsigned char *s, const unsigned char *end, uint32_t *cp) {
    unsigned char c = *s;
    if (c < 0x80) { *cp = c; return 1; }
    size_t n = c >= 0xC2 && c <= 0xDF ? 2 :
               c >= 0xE0 && c <= 0xEF ? 3 :
               c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!n || (size_t)(end - s) < n) return 0;
    uint32_t value = c & (0x7Fu >> n);
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        value = (value << 6) | (s[i] & 0x3F);
    }
    if ((n == 3 && value < 0x800) || (n == 4 && value < 0x10000) ||
        (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) return 0;
    *cp = value;
    return n;
}

static bool in_ranges(uint32_t cp, const CodeRange *ranges, size_t count) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid].first) hi = mid;
        else if (cp > ranges[mid].last) lo = mid + 1;
        else return true;
    }
    return false;
}
static bool ignorable(uint32_t cp) {
    return in_ranges(cp, case_ignorable_ranges, COUNT(case_ignorable_ranges));
}
static bool cased(uint32_t cp) {
    return in_ranges(cp, cased_ranges, COUNT(cased_ranges));
}
static bool next_cased(const unsigned char *s, const unsigned char *end) {
    while (s < end) {
        uint32_t cp;
        size_t n = decode(s, end, &cp);
        if (!n) return false;
        if (!ignorable(cp)) return cased(cp);
        s += n;
    }
    return false;
}
static const CaseMapping *lookup(uint32_t cp, bool upper) {
    const CaseMapping *table = upper ? upper_mappings : lower_mappings;
    size_t lo = 0, hi = upper ? COUNT(upper_mappings) : COUNT(lower_mappings);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < table[mid].code) hi = mid;
        else if (cp > table[mid].code) lo = mid + 1;
        else return &table[mid];
    }
    return NULL;
}
static size_t encoded_size(uint32_t cp) {
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}
static void encode(unsigned char *out, uint32_t cp, size_t n) {
    if (n == 1) { out[0] = (unsigned char)cp; return; }
    for (size_t i = n - 1; i > 0; i--) {
        out[i] = (unsigned char)(0x80 | (cp & 0x3F));
        cp >>= 6;
    }
    out[0] = (unsigned char)((n == 2 ? 0xC0 : n == 3 ? 0xE0 : 0xF0) | cp);
}

/* Two passes keep allocation exact and check expansion before writing. Sigma
 * lookahead scans only adjacent ignorable runs, so repeated sigmas stay linear. */
static bool convert(const unsigned char *s, const unsigned char *end, bool upper,
                    unsigned char *out, size_t *size) {
    size_t used = 0;
    bool preceding_cased = false;
    while (s < end) {
        uint32_t cp;
        size_t width = decode(s, end, &cp);
        if (!width) {
            if (!checked_add_size(used, 1, &used)) return false;
            if (out) out[used - 1] = *s;
            s++;
            preceding_cased = false;
            continue;
        }
        uint32_t mapped[3] = {cp, 0, 0};
        size_t count = 1;
        if (!upper && cp == 0x3A3 && preceding_cased && !next_cased(s + width, end)) {
            mapped[0] = 0x3C2;
        } else {
            const CaseMapping *mapping = lookup(cp, upper);
            if (mapping) {
                count = mapping->length;
                memcpy(mapped, mapping->mapped, sizeof(mapped));
            }
        }
        for (size_t i = 0; i < count; i++) {
            size_t n = encoded_size(mapped[i]), next;
            if (!checked_add_size(used, n, &next)) return false;
            if (out) encode(out + used, mapped[i], n);
            used = next;
        }
        if (!ignorable(cp)) preceding_cased = cased(cp);
        s += width;
    }
    *size = used;
    return true;
}
char *myon_unicode_case(const char *input, bool upper) {
    const unsigned char *s = (const unsigned char *)input;
    const unsigned char *end = s + strlen(input);
    size_t size, allocation;
    if (!convert(s, end, upper, NULL, &size) || !checked_add_size(size, 1, &allocation)) return NULL;
    char *out = myon_xmalloc(allocation);
    if (!convert(s, end, upper, (unsigned char *)out, &size)) { free(out); return NULL; }
    out[size] = '\0';
    return out;
}
