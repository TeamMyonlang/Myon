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

/*
 * Phase5 Step1 smoke test (NOT part of `make`).  Verifies two tasks interleave
 * cooperatively by yielding on a readable pipe fd.
 *
 * Build & run manually from the repo root:
 *   cc -std=c11 -o /tmp/elsmoke tests/fixtures/event_loop_smoke.c src/event_loop.c
 *   /tmp/elsmoke
 * Expected output (interleaved A/B, not A-run-to-completion then B):
 *   A1 B1 A2 B2 A3 B3 done
 */

#include "../../src/event_loop.h"
#include <stdio.h>
#include <unistd.h>

static EventLoop *g_loop;
static int g_pipe[2];

static void worker(void *ud) {
    const char *tag = (const char *)ud;
    for (int i = 1; i <= 3; i++) {
        printf("%s%d ", tag, i);
        fflush(stdout);
        /* Yield by waiting for the read end of a pipe that always has data
         * available (we pre-load one byte and never drain it), forcing a
         * cooperative turn back to the loop. */
        event_loop_wait_readable(g_loop, g_pipe[0]);
    }
}

int main(void) {
    if (!event_loop_supported()) {
        printf("event loop unsupported on this platform\n");
        return 0;
    }
    if (pipe(g_pipe) != 0) { perror("pipe"); return 1; }
    /* keep a readable byte in the pipe so the read fd is always "ready" */
    write(g_pipe[1], "x", 1);

    g_loop = event_loop_create();
    event_loop_spawn(g_loop, worker, (void *)"A");
    event_loop_spawn(g_loop, worker, (void *)"B");
    event_loop_run_until_all_done(g_loop);
    event_loop_destroy(g_loop);
    printf("done\n");
    return 0;
}
