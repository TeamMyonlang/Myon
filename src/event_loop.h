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

#ifndef MYON_EVENT_LOOP_H
#define MYON_EVENT_LOOP_H

#include <stddef.h>
#include "net.h"   /* myon_fd_t (known-issue #5: raw fds carried at native width) */

/*
 * Phase5, Step1: a single-threaded, cooperative event loop.
 *
 * This module is deliberately independent of the interpreter's value system:
 * it knows nothing about `Value`, `Obj`, or refcounts.  Tasks carry an opaque
 * `void *result` that interpreter.c casts to/from `Value *`.
 *
 * The implementation uses POSIX `ucontext.h` (makecontext/swapcontext) to give
 * each task its own C stack so the tree-walking interpreter's recursive
 * eval_expr/exec_stmt do not need any changes: a task simply runs on its own
 * stack and, at an `await` / I/O point, swaps back to the loop, preserving its
 * whole C call stack until it is resumed.
 *
 * Platform support mirrors the FFI subsystem's "Linux is the primary target"
 * policy: ucontext is available on Linux glibc.  On platforms lacking it the
 * module compiles as an unsupported stub.
 */

typedef struct EventLoop EventLoop;
typedef struct Task Task;

typedef enum {
    TASK_READY,        /* runnable: will run at the next scheduling turn */
    TASK_RUNNING,      /* currently executing (at most one at a time)    */
    TASK_WAITING_IO,   /* blocked on an fd becoming readable/writable    */
    TASK_WAITING_TASK, /* blocked on another Task reaching TASK_DONE     */
    TASK_DONE          /* finished (result / error retained)             */
} TaskState;

/* Create/destroy a loop.  One loop per process is expected. */
EventLoop *event_loop_create(void);
void       event_loop_destroy(EventLoop *loop);

/*
 * Spawn a new task.  `entry` is the C function run first on the task's own
 * stack; `ud` is passed straight through to it.  The returned handle stays
 * valid until event_loop_destroy() (tasks are not individually freed while the
 * loop is alive, because their C stacks may still be referenced).
 */
Task *event_loop_spawn(EventLoop *loop, void (*entry)(void *ud), void *ud);

/* From the running task: suspend until `fd` is readable/writable.  Returns
 * >=0 on readiness, <0 on select() error.  `fd` is a myon_fd_t so a Windows
 * SOCKET survives without truncation (known-issue #5). */
int event_loop_wait_readable(EventLoop *loop, myon_fd_t fd);
int event_loop_wait_writable(EventLoop *loop, myon_fd_t fd);

/* From the running task: suspend for at least `ms` milliseconds. */
void event_loop_sleep_ms(EventLoop *loop, long long ms);

/* From the running task: suspend until `target` reaches TASK_DONE. */
void event_loop_wait_task(EventLoop *loop, Task *target);

/* Non-blocking: 1 if `target` has finished, else 0. */
int event_loop_task_done(Task *target);

/* Accessors for a finished task's payload (set by the task's entry via
 * event_loop_task_set_result). */
void *event_loop_task_result(Task *target);
int   event_loop_task_has_error(Task *target);

/* Called from within a task's entry to record its outcome before returning. */
void event_loop_task_set_result(Task *task, void *result, int has_error);

/* The task currently executing, or NULL if control is in the loop core. */
Task *event_loop_current(EventLoop *loop);

/*
 * Advance the loop by one turn: pick a READY task and resume it until it
 * suspends again or finishes.  If nothing is READY, block in select() on all
 * WAITING_IO fds (with a timeout derived from the nearest sleeping task) and
 * mark newly-ready tasks READY.  Returns the number of not-yet-DONE tasks
 * remaining (0 means everything finished).
 */
int event_loop_run_once(EventLoop *loop);

/* Convenience: run_once until every task is DONE. */
void event_loop_run_until_all_done(EventLoop *loop);

/*
 * Mark a task as a "daemon": long-lived background work (e.g. an HTTP server's
 * accept loop) that should not, by itself, keep the program alive.  The
 * program-exit drain (event_loop_run_until_foreground_done) ignores daemon
 * tasks, so the process can terminate once all foreground tasks finish even
 * though a daemon accept loop is still pending.
 */
void event_loop_set_daemon(Task *task, int is_daemon);

/*
 * Like event_loop_run_until_all_done, but stops once every *non-daemon* task
 * has finished (daemon tasks may still be pending).  Used at program exit so a
 * `myon.http.serve*` accept loop does not hang a script whose foreground work
 * is already complete.
 */
void event_loop_run_until_foreground_done(EventLoop *loop);

/* 1 if this build actually has a working event loop (ucontext present). */
int event_loop_supported(void);

#endif /* MYON_EVENT_LOOP_H */
