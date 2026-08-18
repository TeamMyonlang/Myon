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

/* Feature-test macros must precede every system header: glibc gates
 * makecontext/swapcontext, clock_gettime and CLOCK_MONOTONIC behind these,
 * and macOS gates the ucontext family behind _XOPEN_SOURCE as well (without it
 * <ucontext.h> is an empty header on Darwin). */
#ifndef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif
/* macOS: _XOPEN_SOURCE alone hides makecontext() from <ucontext.h>; the Darwin
 * headers additionally require _DARWIN_C_SOURCE to re-expose the ucontext
 * family (and the BSD socket extras used elsewhere). */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE 1
#endif

#include "platform.h"
#include "event_loop.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Cooperative-coroutine backend selection.
 *
 *   * POSIX (Linux, macOS, the BSDs) use the ucontext family
 *     (getcontext/makecontext/swapcontext).  macOS/BSD are full POSIX targets
 *     here -- the previous `#if defined(__linux__)` guard wrongly excluded them
 *     and sent macOS to the unsupported stub, silently disabling the whole
 *     async event loop.  See platform.h (MYON_HAVE_UCONTEXT).
 *   * Windows lacks ucontext; the loop is built on the Win32 Fiber API
 *     (ConvertThreadToFiber/CreateFiber/SwitchToFiber/DeleteFiber).  See the
 *     #elif defined(_WIN32) block far below and docs/myon_spec.md 14.9.
 */
#if defined(MYON_HAVE_UCONTEXT)
#  define MYON_EVENT_LOOP_UCONTEXT 1
#elif defined(MYON_OS_WINDOWS)
#  define MYON_EVENT_LOOP_FIBER 1
#endif

#ifdef MYON_EVENT_LOOP_UCONTEXT

/* macOS marks the ucontext routines "deprecated"; <sys/ucontext.h> is the
 * documented way to pull them in without the deprecation attribute, and works
 * identically on Linux/BSD where it simply forwards to <ucontext.h>. */
#if defined(__APPLE__)
#  include <sys/ucontext.h>
#endif
#include <ucontext.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#define TASK_STACK_SIZE (256 * 1024)

struct Task {
    ucontext_t  ctx;         /* this task's own context               */
    ucontext_t *return_ctx;  /* the loop core's context (resume point) */
    void       *stack;       /* malloc'd C stack for this task         */
    size_t      stack_size;

    void      (*entry)(void *ud);
    void       *ud;

    TaskState   state;
    int         waiting_fd;        /* WAITING_IO fd being watched      */
    int         waiting_for_write; /* 0=read, 1=write                  */
    long long   wake_at_ms;        /* absolute wake time, 0 if unused  */
    Task       *waiting_on;        /* WAITING_TASK target              */

    void       *result;            /* opaque payload (Value* on the interp side) */
    int         has_error;
    int         started;           /* has entry begun executing?       */
    int         is_daemon;         /* background task; ignored at exit  */

    EventLoop  *loop;
    Task       *next;              /* singly-linked list of all tasks  */
};

struct EventLoop {
    ucontext_t  core_ctx;   /* context we swap back to from a task     */
    Task       *tasks;      /* all tasks (never removed while alive)   */
    Task       *current;    /* task currently running, or NULL         */
};

/* ------------------------------------------------------------------ */
/* time helpers                                                        */
/* ------------------------------------------------------------------ */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

EventLoop *event_loop_create(void) {
    EventLoop *loop = (EventLoop *)calloc(1, sizeof(EventLoop));
    if (!loop) { fprintf(stderr, "myon: out of memory (event loop)\n"); abort(); }
    return loop;
}

void event_loop_destroy(EventLoop *loop) {
    if (!loop) return;
    Task *t = loop->tasks;
    while (t) {
        Task *n = t->next;
        free(t->stack);
        free(t);
        t = n;
    }
    free(loop);
}

int event_loop_supported(void) { return 1; }

/* ------------------------------------------------------------------ */
/* task trampoline                                                     */
/* ------------------------------------------------------------------ */

/*
 * makecontext entry points cannot portably take a pointer argument in one
 * word on all ABIs, so we stash the running task in a file-scope pointer set
 * just before swapping into the task for the first time.
 */
static Task *g_bootstrapping_task = NULL;

static void task_trampoline(void) {
    Task *self = g_bootstrapping_task;
    self->entry(self->ud);
    /* If the entry did not explicitly set a result, default to none. */
    self->state = TASK_DONE;
    /* return to the loop core; never falls through (uc_link is core_ctx). */
}

Task *event_loop_spawn(EventLoop *loop, void (*entry)(void *ud), void *ud) {
    Task *t = (Task *)calloc(1, sizeof(Task));
    if (!t) { fprintf(stderr, "myon: out of memory (task)\n"); abort(); }
    t->stack_size = TASK_STACK_SIZE;
    t->stack = malloc(t->stack_size);
    if (!t->stack) { fprintf(stderr, "myon: out of memory (task stack)\n"); abort(); }
    t->entry = entry;
    t->ud = ud;
    t->state = TASK_READY;
    t->waiting_fd = -1;
    t->loop = loop;
    t->return_ctx = &loop->core_ctx;

    /* link into the all-tasks list (append to preserve spawn order) */
    if (!loop->tasks) {
        loop->tasks = t;
    } else {
        Task *p = loop->tasks;
        while (p->next) p = p->next;
        p->next = t;
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* suspension primitives (called from within the running task)         */
/* ------------------------------------------------------------------ */

static void task_yield_to_core(Task *t) {
    /* Save this task's context and jump back to the loop core. */
    swapcontext(&t->ctx, t->return_ctx);
}

int event_loop_wait_readable(EventLoop *loop, int fd) {
    Task *t = loop->current;
    if (!t) return -1;
    t->waiting_fd = fd;
    t->waiting_for_write = 0;
    t->state = TASK_WAITING_IO;
    task_yield_to_core(t);
    return 0;
}

int event_loop_wait_writable(EventLoop *loop, int fd) {
    Task *t = loop->current;
    if (!t) return -1;
    t->waiting_fd = fd;
    t->waiting_for_write = 1;
    t->state = TASK_WAITING_IO;
    task_yield_to_core(t);
    return 0;
}

void event_loop_sleep_ms(EventLoop *loop, long long ms) {
    Task *t = loop->current;
    if (!t) return;
    if (ms < 0) ms = 0;
    t->wake_at_ms = now_ms() + ms;
    t->state = TASK_WAITING_IO; /* reuse IO-wait bucket; no fd (-1) */
    t->waiting_fd = -1;
    task_yield_to_core(t);
    t->wake_at_ms = 0;
}

void event_loop_wait_task(EventLoop *loop, Task *target) {
    Task *t = loop->current;
    if (!t || !target) return;
    if (target->state == TASK_DONE) return;
    t->waiting_on = target;
    t->state = TASK_WAITING_TASK;
    task_yield_to_core(t);
    t->waiting_on = NULL;
}

int event_loop_task_done(Task *target) {
    return target && target->state == TASK_DONE;
}

void *event_loop_task_result(Task *target) {
    return target ? target->result : NULL;
}

int event_loop_task_has_error(Task *target) {
    return target ? target->has_error : 0;
}

void event_loop_task_set_result(Task *task, void *result, int has_error) {
    if (!task) return;
    task->result = result;
    task->has_error = has_error;
}

Task *event_loop_current(EventLoop *loop) {
    return loop ? loop->current : NULL;
}

/* ------------------------------------------------------------------ */
/* scheduler core                                                      */
/* ------------------------------------------------------------------ */

/* Resume a task: switch onto its C stack until it yields/finishes. */
static void resume_task(EventLoop *loop, Task *t) {
    loop->current = t;
    t->state = TASK_RUNNING;

    if (!t->started) {
        t->started = 1;
        getcontext(&t->ctx);
        t->ctx.uc_stack.ss_sp = t->stack;
        t->ctx.uc_stack.ss_size = t->stack_size;
        t->ctx.uc_link = &loop->core_ctx; /* return here when entry returns */
        makecontext(&t->ctx, task_trampoline, 0);
        g_bootstrapping_task = t;
    }
    /* Jump into the task; control returns here when the task yields (via
     * swapcontext) or finishes (via uc_link). */
    swapcontext(&loop->core_ctx, &t->ctx);
    loop->current = NULL;

    /* If the task ran off the end of its entry, uc_link brought us back with
     * the task still marked RUNNING; normalize it to DONE. */
    if (t->state == TASK_RUNNING)
        t->state = TASK_DONE;
}

/* Count tasks not yet DONE. */
static int count_pending(EventLoop *loop) {
    int n = 0;
    for (Task *t = loop->tasks; t; t = t->next)
        if (t->state != TASK_DONE) n++;
    return n;
}

/* Count non-DONE tasks that are NOT daemons (foreground work). */
static int count_foreground_pending(EventLoop *loop) {
    int n = 0;
    for (Task *t = loop->tasks; t; t = t->next)
        if (t->state != TASK_DONE && !t->is_daemon) n++;
    return n;
}

void event_loop_set_daemon(Task *task, int is_daemon) {
    if (task) task->is_daemon = is_daemon ? 1 : 0;
}

/* Wake WAITING_TASK tasks whose target has finished. */
static void wake_task_waiters(EventLoop *loop) {
    for (Task *t = loop->tasks; t; t = t->next) {
        if (t->state == TASK_WAITING_TASK && t->waiting_on &&
            t->waiting_on->state == TASK_DONE) {
            t->state = TASK_READY;
        }
    }
}

/* Wake sleeping tasks whose deadline has passed. */
static void wake_sleepers(EventLoop *loop, long long now) {
    for (Task *t = loop->tasks; t; t = t->next) {
        if (t->state == TASK_WAITING_IO && t->waiting_fd < 0 &&
            t->wake_at_ms != 0 && now >= t->wake_at_ms) {
            t->state = TASK_READY;
        }
    }
}

static Task *pick_ready(EventLoop *loop) {
    for (Task *t = loop->tasks; t; t = t->next)
        if (t->state == TASK_READY) return t;
    return NULL;
}

int event_loop_run_once(EventLoop *loop) {
    wake_task_waiters(loop);
    wake_sleepers(loop, now_ms());

    Task *ready = pick_ready(loop);
    if (ready) {
        resume_task(loop, ready);
        return count_pending(loop);
    }

    /* Nothing runnable right now: figure out what to wait on. */
    int pending = count_pending(loop);
    if (pending == 0) return 0;

    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxfd = -1;
    int have_fd = 0;
    long long soonest_wake = -1;

    for (Task *t = loop->tasks; t; t = t->next) {
        if (t->state != TASK_WAITING_IO) continue;
        if (t->waiting_fd >= 0) {
            if (t->waiting_for_write) FD_SET(t->waiting_fd, &wfds);
            else                       FD_SET(t->waiting_fd, &rfds);
            if (t->waiting_fd > maxfd) maxfd = t->waiting_fd;
            have_fd = 1;
        } else if (t->wake_at_ms != 0) {
            if (soonest_wake < 0 || t->wake_at_ms < soonest_wake)
                soonest_wake = t->wake_at_ms;
        }
    }

    struct timeval tv, *ptv = NULL;
    if (soonest_wake >= 0) {
        long long delta = soonest_wake - now_ms();
        if (delta < 0) delta = 0;
        tv.tv_sec  = (long)(delta / 1000);
        tv.tv_usec = (long)((delta % 1000) * 1000);
        ptv = &tv;
    }

    if (!have_fd && ptv == NULL) {
        /* Deadlock: pending tasks that can never wake.  Give up to avoid a
         * busy spin; mark them done so the caller can terminate. */
        for (Task *t = loop->tasks; t; t = t->next)
            if (t->state != TASK_DONE) { t->state = TASK_DONE; t->has_error = 1; }
        return 0;
    }

    int rc = select(maxfd + 1, have_fd ? &rfds : NULL,
                     have_fd ? &wfds : NULL, NULL, ptv);
    if (rc < 0) {
        if (errno == EINTR) return count_pending(loop);
        /* On a hard select() error, fail all IO waiters. */
        for (Task *t = loop->tasks; t; t = t->next)
            if (t->state == TASK_WAITING_IO && t->waiting_fd >= 0) {
                t->state = TASK_READY;
            }
        return count_pending(loop);
    }

    /* Mark tasks whose fd is ready as READY. */
    for (Task *t = loop->tasks; t; t = t->next) {
        if (t->state != TASK_WAITING_IO || t->waiting_fd < 0) continue;
        int ready_now = t->waiting_for_write ? FD_ISSET(t->waiting_fd, &wfds)
                                             : FD_ISSET(t->waiting_fd, &rfds);
        if (ready_now) t->state = TASK_READY;
    }
    /* Also wake any sleepers whose timeout elapsed during select(). */
    wake_sleepers(loop, now_ms());

    return count_pending(loop);
}

void event_loop_run_until_all_done(EventLoop *loop) {
    while (event_loop_run_once(loop) > 0) { /* keep turning */ }
}

void event_loop_run_until_foreground_done(EventLoop *loop) {
    /* Turn the loop while any non-daemon task is still pending.  We check the
     * foreground count before each turn so that a still-running daemon accept
     * loop (blocked on select) does not keep us here forever. */
    while (count_foreground_pending(loop) > 0) {
        if (event_loop_run_once(loop) == 0) break;
    }
}

#elif defined(MYON_EVENT_LOOP_FIBER) /* Windows Fiber implementation */

/* ================================================================== */
/* Windows (Win32 Fiber) implementation                               */
/* ================================================================== */
/*
 * Phase5, Step3 (Windows): a Win32-Fiber port of the cooperative event loop
 * above.  The Linux block (#ifdef MYON_EVENT_LOOP_UCONTEXT) is left completely
 * unchanged; this branch only compiles on _WIN32 (native MSYS2/MinGW-w64 or a
 * MinGW-w64 cross build).
 *
 * Design rationale (verified against Microsoft Learn, "Fibers" and the
 * winbase.h / winsock2.h reference pages):
 *
 *   * ucontext (getcontext/makecontext/swapcontext) does not exist on Windows.
 *     Its exact user-mode-stack-switching analogue is the Fiber API:
 *        - ConvertThreadToFiber(NULL)  <->  the loop core context
 *          (== `main_fiber`, analogous to `core_ctx` in the ucontext build).
 *        - CreateFiber(stack, proc, param)  <->  makecontext: it both allocates
 *          the task's stack AND sets its entry point, so the ucontext build's
 *          separate malloc'd `stack`/`stack_size` fields are unnecessary here.
 *        - SwitchToFiber(fiber)  <->  swapcontext: transfer control; execution
 *          resumes in the target fiber at the point it last SwitchToFiber'd
 *          away, and returns to *us* only when someone SwitchToFiber's back.
 *        - DeleteFiber(fiber)  <->  freeing the task stack.
 *     CreateFiber passes `param` straight to the fiber proc, so the ucontext
 *     build's g_bootstrapping_task global (a workaround for makecontext not
 *     portably taking a pointer arg) is not needed: we pass the Task* directly.
 *
 *   * CRITICAL Fiber lifecycle constraint: a fiber's start routine must NEVER
 *     return -- doing so calls ExitThread and terminates the whole thread
 *     (undefined/fatal for us), because a fiber has no uc_link-style automatic
 *     "return to caller" the way ucontext does.  We therefore make the fiber
 *     proc, after the task's entry() finishes, enter an infinite loop that
 *     marks the task DONE and SwitchToFiber(main_fiber) forever.  The scheduler
 *     never SwitchToFiber's into a DONE task again, so this loop body executes
 *     exactly once per completion; the fiber then simply sits parked until
 *     event_loop_destroy() calls DeleteFiber on it.  This is the single biggest
 *     structural difference from the ucontext build and is called out in the
 *     Step3 completion report.
 *
 *   * Time: clock_gettime(CLOCK_MONOTONIC) is unavailable; GetTickCount64()
 *     gives a monotonic millisecond tick since boot, which is exactly what
 *     now_ms() needs (it is only ever used for relative deadlines).
 *
 *   * select(): Winsock provides select() in <winsock2.h> with an fd_set that
 *     is an array of SOCKET handles (not a bitmask over int fds), and its
 *     first `nfds` argument is documented as *ignored* (kept only for BSD
 *     source compatibility).  FD_SETSIZE defaults to 64.  A SOCKET is a
 *     UINT_PTR, but the fd threaded in from net_raw_fd()/event_loop.h is an
 *     `int` (see the fd-width note below).  We keep the public `int fd`
 *     interface unchanged and reconstruct the SOCKET locally where select()
 *     needs it -- see fd_to_socket().
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN 1
#endif
/* winsock2.h MUST precede windows.h, otherwise windows.h pulls in the older
 * winsock.h and the two clash (same policy as src/net.c's Windows branch). */
#include <winsock2.h>
#include <windows.h>

#define TASK_STACK_SIZE (256 * 1024)

struct Task {
    LPVOID      fiber;       /* this task's fiber (CreateFiber result)  */

    void      (*entry)(void *ud);
    void       *ud;

    TaskState   state;
    int         waiting_fd;        /* WAITING_IO fd being watched      */
    int         waiting_for_write; /* 0=read, 1=write                  */
    long long   wake_at_ms;        /* absolute wake time, 0 if unused  */
    Task       *waiting_on;        /* WAITING_TASK target              */

    void       *result;            /* opaque payload (Value* on the interp side) */
    int         has_error;
    int         started;           /* has the fiber been created/entered? */
    int         is_daemon;         /* background task; ignored at exit  */

    EventLoop  *loop;
    Task       *next;              /* singly-linked list of all tasks  */
};

struct EventLoop {
    LPVOID      main_fiber; /* ConvertThreadToFiber result; loop core  */
    Task       *tasks;      /* all tasks (never removed while alive)   */
    Task       *current;    /* task currently running, or NULL         */
};

/* ------------------------------------------------------------------ */
/* fd <-> SOCKET bridging                                              */
/* ------------------------------------------------------------------ */
/*
 * The public event-loop / net_raw_fd() interface carries an `int` fd on every
 * platform.  On Windows the underlying kernel object is a SOCKET (UINT_PTR).
 * Winsock socket handles are documented to fit within 32 bits ("Socket
 * Handles" in the Winsock reference), so the int<->SOCKET round trip used here
 * is lossless in practice.  Reconstructing via (unsigned) first ensures the
 * value is zero-extended rather than sign-extended into the 64-bit SOCKET.
 *
 * This resolves the Step2 hand-off note in src/net.c / docs/myon_spec.md 10.7:
 * rather than widening the entire fd chain (event_loop.h + interpreter.c) to an
 * intptr_t-sized type -- which would be a gratuitous interface break for the
 * Linux side, where an int fd is exactly right -- we keep the int interface and
 * localise the SOCKET reconstruction to the two spots in this file that hand an
 * fd to Winsock (FD_SET and FD_ISSET).
 */
static SOCKET fd_to_socket(int fd) {
    return (SOCKET)(UINT_PTR)(unsigned int)fd;
}

/* ------------------------------------------------------------------ */
/* time helpers                                                        */
/* ------------------------------------------------------------------ */

static long long now_ms(void) {
    /* GetTickCount64: monotonic milliseconds since system start. */
    return (long long)GetTickCount64();
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

EventLoop *event_loop_create(void) {
    EventLoop *loop = (EventLoop *)calloc(1, sizeof(EventLoop));
    if (!loop) { fprintf(stderr, "myon: out of memory (event loop)\n"); abort(); }
    /* Turn the current thread into a fiber so it can SwitchToFiber into tasks.
     * This is the Fiber-API analogue of having a core_ctx to swapcontext into.
     * Passing NULL means "no fiber data".  Idempotency: converting an
     * already-converted thread fails, but we only create one loop per process
     * (documented in event_loop.h), so a fresh conversion here is correct. */
    loop->main_fiber = ConvertThreadToFiber(NULL);
    if (!loop->main_fiber) {
        fprintf(stderr, "myon: ConvertThreadToFiber failed (err=%lu)\n",
                (unsigned long)GetLastError());
        abort();
    }
    return loop;
}

void event_loop_destroy(EventLoop *loop) {
    if (!loop) return;
    Task *t = loop->tasks;
    while (t) {
        Task *n = t->next;
        if (t->fiber) DeleteFiber(t->fiber);
        free(t);
        t = n;
    }
    /* Convert the thread back so a second loop (if ever created) can convert
     * again.  ConvertFiberToThread has no ill effect at process exit either. */
    if (loop->main_fiber) ConvertFiberToThread();
    free(loop);
}

int event_loop_supported(void) { return 1; }

/* ------------------------------------------------------------------ */
/* task fiber proc                                                     */
/* ------------------------------------------------------------------ */

/*
 * Fiber start routine.  Unlike the ucontext trampoline, this must never
 * return (see the file-header note): after entry() completes we mark the task
 * DONE and loop forever, yielding back to the loop core each time.  The
 * scheduler will not resume a DONE task, so the body after entry() runs at
 * most once per task; the trailing loop only re-executes if something
 * erroneously SwitchToFiber'd back in, in which case we simply bounce control
 * straight back to the core rather than corrupting state.
 */
static VOID CALLBACK task_fiber_proc(PVOID param) {
    Task *self = (Task *)param;
    self->entry(self->ud);
    /* If the entry did not explicitly set a result, it stays as calloc'd 0. */
    self->state = TASK_DONE;
    for (;;) {
        /* Park forever, always handing control back to the loop core. */
        SwitchToFiber(self->loop->main_fiber);
    }
}

Task *event_loop_spawn(EventLoop *loop, void (*entry)(void *ud), void *ud) {
    Task *t = (Task *)calloc(1, sizeof(Task));
    if (!t) { fprintf(stderr, "myon: out of memory (task)\n"); abort(); }
    t->entry = entry;
    t->ud = ud;
    t->state = TASK_READY;
    t->waiting_fd = -1;
    t->loop = loop;

    /* link into the all-tasks list (append to preserve spawn order) */
    if (!loop->tasks) {
        loop->tasks = t;
    } else {
        Task *p = loop->tasks;
        while (p->next) p = p->next;
        p->next = t;
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* suspension primitives (called from within the running task)         */
/* ------------------------------------------------------------------ */

static void task_yield_to_core(Task *t) {
    /* Hand control back to the loop core fiber.  Control resumes here when the
     * scheduler SwitchToFiber's back into this task. */
    SwitchToFiber(t->loop->main_fiber);
}

int event_loop_wait_readable(EventLoop *loop, int fd) {
    Task *t = loop->current;
    if (!t) return -1;
    t->waiting_fd = fd;
    t->waiting_for_write = 0;
    t->state = TASK_WAITING_IO;
    task_yield_to_core(t);
    return 0;
}

int event_loop_wait_writable(EventLoop *loop, int fd) {
    Task *t = loop->current;
    if (!t) return -1;
    t->waiting_fd = fd;
    t->waiting_for_write = 1;
    t->state = TASK_WAITING_IO;
    task_yield_to_core(t);
    return 0;
}

void event_loop_sleep_ms(EventLoop *loop, long long ms) {
    Task *t = loop->current;
    if (!t) return;
    if (ms < 0) ms = 0;
    t->wake_at_ms = now_ms() + ms;
    t->state = TASK_WAITING_IO; /* reuse IO-wait bucket; no fd (-1) */
    t->waiting_fd = -1;
    task_yield_to_core(t);
    t->wake_at_ms = 0;
}

void event_loop_wait_task(EventLoop *loop, Task *target) {
    Task *t = loop->current;
    if (!t || !target) return;
    if (target->state == TASK_DONE) return;
    t->waiting_on = target;
    t->state = TASK_WAITING_TASK;
    task_yield_to_core(t);
    t->waiting_on = NULL;
}

int event_loop_task_done(Task *target) {
    return target && target->state == TASK_DONE;
}

void *event_loop_task_result(Task *target) {
    return target ? target->result : NULL;
}

int event_loop_task_has_error(Task *target) {
    return target ? target->has_error : 0;
}

void event_loop_task_set_result(Task *task, void *result, int has_error) {
    if (!task) return;
    task->result = result;
    task->has_error = has_error;
}

Task *event_loop_current(EventLoop *loop) {
    return loop ? loop->current : NULL;
}

/* ------------------------------------------------------------------ */
/* scheduler core                                                      */
/* ------------------------------------------------------------------ */

/* Resume a task: switch onto its fiber until it yields/finishes. */
static void resume_task(EventLoop *loop, Task *t) {
    loop->current = t;
    t->state = TASK_RUNNING;

    if (!t->started) {
        t->started = 1;
        /* CreateFiber both allocates the stack and sets task_fiber_proc as the
         * entry, passing the Task* directly (no bootstrap global needed). */
        t->fiber = CreateFiber((SIZE_T)TASK_STACK_SIZE, task_fiber_proc, t);
        if (!t->fiber) {
            fprintf(stderr, "myon: CreateFiber failed (err=%lu)\n",
                    (unsigned long)GetLastError());
            abort();
        }
    }
    /* Jump into the task's fiber; control returns here when the task yields
     * (task_yield_to_core) or completes (task_fiber_proc's park loop). */
    SwitchToFiber(t->fiber);
    loop->current = NULL;

    /* Normalise a task that ran to the end of its entry to DONE (the fiber
     * proc already does this, but keep parity with the ucontext build). */
    if (t->state == TASK_RUNNING)
        t->state = TASK_DONE;
}

/* Count tasks not yet DONE. */
static int count_pending(EventLoop *loop) {
    int n = 0;
    for (Task *t = loop->tasks; t; t = t->next)
        if (t->state != TASK_DONE) n++;
    return n;
}

/* Count non-DONE tasks that are NOT daemons (foreground work). */
static int count_foreground_pending(EventLoop *loop) {
    int n = 0;
    for (Task *t = loop->tasks; t; t = t->next)
        if (t->state != TASK_DONE && !t->is_daemon) n++;
    return n;
}

void event_loop_set_daemon(Task *task, int is_daemon) {
    if (task) task->is_daemon = is_daemon ? 1 : 0;
}

/* Wake WAITING_TASK tasks whose target has finished. */
static void wake_task_waiters(EventLoop *loop) {
    for (Task *t = loop->tasks; t; t = t->next) {
        if (t->state == TASK_WAITING_TASK && t->waiting_on &&
            t->waiting_on->state == TASK_DONE) {
            t->state = TASK_READY;
        }
    }
}

/* Wake sleeping tasks whose deadline has passed. */
static void wake_sleepers(EventLoop *loop, long long now) {
    for (Task *t = loop->tasks; t; t = t->next) {
        if (t->state == TASK_WAITING_IO && t->waiting_fd < 0 &&
            t->wake_at_ms != 0 && now >= t->wake_at_ms) {
            t->state = TASK_READY;
        }
    }
}

static Task *pick_ready(EventLoop *loop) {
    for (Task *t = loop->tasks; t; t = t->next)
        if (t->state == TASK_READY) return t;
    return NULL;
}

int event_loop_run_once(EventLoop *loop) {
    wake_task_waiters(loop);
    wake_sleepers(loop, now_ms());

    Task *ready = pick_ready(loop);
    if (ready) {
        resume_task(loop, ready);
        return count_pending(loop);
    }

    /* Nothing runnable right now: figure out what to wait on. */
    int pending = count_pending(loop);
    if (pending == 0) return 0;

    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int have_fd = 0;
    long long soonest_wake = -1;

    for (Task *t = loop->tasks; t; t = t->next) {
        if (t->state != TASK_WAITING_IO) continue;
        if (t->waiting_fd >= 0) {
            SOCKET s = fd_to_socket(t->waiting_fd);
            if (t->waiting_for_write) FD_SET(s, &wfds);
            else                       FD_SET(s, &rfds);
            have_fd = 1;
        } else if (t->wake_at_ms != 0) {
            if (soonest_wake < 0 || t->wake_at_ms < soonest_wake)
                soonest_wake = t->wake_at_ms;
        }
    }

    struct timeval tv, *ptv = NULL;
    if (soonest_wake >= 0) {
        long long delta = soonest_wake - now_ms();
        if (delta < 0) delta = 0;
        tv.tv_sec  = (long)(delta / 1000);
        tv.tv_usec = (long)((delta % 1000) * 1000);
        ptv = &tv;
    }

    if (!have_fd && ptv == NULL) {
        /* Deadlock: pending tasks that can never wake.  Give up to avoid a
         * busy spin; mark them done so the caller can terminate. */
        for (Task *t = loop->tasks; t; t = t->next)
            if (t->state != TASK_DONE) { t->state = TASK_DONE; t->has_error = 1; }
        return 0;
    }

    /* Winsock select(): the first arg (nfds) is ignored on Windows and kept
     * only for BSD source compatibility, so we pass 0. */
    int rc = select(0, have_fd ? &rfds : NULL,
                     have_fd ? &wfds : NULL, NULL, ptv);
    if (rc == SOCKET_ERROR) {
        int werr = WSAGetLastError();
        if (werr == WSAEINTR) return count_pending(loop);
        /* On a hard select() error, wake all fd IO waiters so they can observe
         * the failure on their next socket call rather than hanging. */
        for (Task *t = loop->tasks; t; t = t->next)
            if (t->state == TASK_WAITING_IO && t->waiting_fd >= 0) {
                t->state = TASK_READY;
            }
        return count_pending(loop);
    }

    /* Mark tasks whose socket is ready as READY. */
    for (Task *t = loop->tasks; t; t = t->next) {
        if (t->state != TASK_WAITING_IO || t->waiting_fd < 0) continue;
        SOCKET s = fd_to_socket(t->waiting_fd);
        int ready_now = t->waiting_for_write ? FD_ISSET(s, &wfds)
                                             : FD_ISSET(s, &rfds);
        if (ready_now) t->state = TASK_READY;
    }
    /* Also wake any sleepers whose timeout elapsed during select(). */
    wake_sleepers(loop, now_ms());

    return count_pending(loop);
}

void event_loop_run_until_all_done(EventLoop *loop) {
    while (event_loop_run_once(loop) > 0) { /* keep turning */ }
}

void event_loop_run_until_foreground_done(EventLoop *loop) {
    /* Turn the loop while any non-daemon task is still pending. */
    while (count_foreground_pending(loop) > 0) {
        if (event_loop_run_once(loop) == 0) break;
    }
}

#else /* neither ucontext nor Fiber: unsupported-platform stub */

struct Task { int dummy; };
struct EventLoop { int dummy; };

EventLoop *event_loop_create(void) { return NULL; }
void       event_loop_destroy(EventLoop *loop) { (void)loop; }
int        event_loop_supported(void) { return 0; }

Task *event_loop_spawn(EventLoop *loop, void (*entry)(void *ud), void *ud) {
    (void)loop; (void)entry; (void)ud; return NULL;
}
int  event_loop_wait_readable(EventLoop *loop, int fd) { (void)loop; (void)fd; return -1; }
int  event_loop_wait_writable(EventLoop *loop, int fd) { (void)loop; (void)fd; return -1; }
void event_loop_sleep_ms(EventLoop *loop, long long ms) { (void)loop; (void)ms; }
void event_loop_wait_task(EventLoop *loop, Task *target) { (void)loop; (void)target; }
int  event_loop_task_done(Task *target) { (void)target; return 1; }
void *event_loop_task_result(Task *target) { (void)target; return NULL; }
int  event_loop_task_has_error(Task *target) { (void)target; return 0; }
void event_loop_task_set_result(Task *task, void *result, int has_error) {
    (void)task; (void)result; (void)has_error;
}
Task *event_loop_current(EventLoop *loop) { (void)loop; return NULL; }
int  event_loop_run_once(EventLoop *loop) { (void)loop; return 0; }
void event_loop_run_until_all_done(EventLoop *loop) { (void)loop; }
void event_loop_set_daemon(Task *task, int is_daemon) { (void)task; (void)is_daemon; }
void event_loop_run_until_foreground_done(EventLoop *loop) { (void)loop; }

#endif /* MYON_EVENT_LOOP_UCONTEXT / MYON_EVENT_LOOP_FIBER / stub */
