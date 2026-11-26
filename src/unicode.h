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

#ifndef MYON_UNICODE_H
#define MYON_UNICODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decode one UTF-8 scalar, returning its width or zero on invalid input.
 * Encode writes one valid scalar into an output buffer of at least 4 bytes. */
size_t myon_utf8_decode(const unsigned char *s, const unsigned char *end, uint32_t *cp);
size_t myon_utf8_encode(unsigned char *out, uint32_t cp);

/* Owned NUL-terminated UTF-8 result, or NULL on size overflow. Invalid bytes
 * are preserved and break casing context. No locale tailoring/normalization. */
char *myon_unicode_case(const char *input, bool upper);

#endif
