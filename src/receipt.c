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

#include "receipt.h"

#include <inttypes.h>
#include <stdio.h>

static const char *active_engine;
static uint64_t conversions;
static uint64_t stdout_bytes;

void receipt_start(const char *engine) {
    active_engine = engine;
    conversions = stdout_bytes = 0;
}

void receipt_conversion(void) {
    if (active_engine) conversions++;
}

void receipt_stdout(size_t bytes) {
    if (active_engine) stdout_bytes += bytes;
}

void receipt_finish(int status) {
    if (!active_engine) return;
    fprintf(stderr, "%-24s %s\n"
                    "%-24s %8" PRIu64 "\n"
                    "%-24s %8" PRIu64 "\n"
                    "%-24s %8d\n",
            "engine", active_engine, "explicit conversions", conversions,
            "stdout bytes", stdout_bytes, "exit status", status);
    active_engine = NULL;
}
