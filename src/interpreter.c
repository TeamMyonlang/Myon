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

/* Request POSIX.1b clocks/timers (clock_gettime, nanosleep, CLOCK_REALTIME)
 * from glibc while compiling under strict -std=c11 (Phase4 myon.time). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif
/* macOS: re-expose the BSD/POSIX extras (stat, select, arc4random) that the
 * strict _POSIX_C_SOURCE selection would otherwise hide on Darwin. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "platform.h"
#include "interpreter.h"
#include "value.h"
#include "env.h"
#include "common.h"
#include "builtin_modules.h"
#include "unicode.h"
#include "lexer.h"
#include "parser.h"
#include "diag.h"
#include "ffi.h"
#include "ffi_call.h"
#include "ffi_platform.h"
#include "ffi_callback.h"
#include "event_loop.h"
#include "net.h"
#include "http.h"
#include "tls.h"
/* Package-manager data model + filesystem layer, reused (read-only) by the
 * installed-package module resolver (spec §6.2).  The interpreter never
 * fetches or installs here — it only reads the already-written myon.lock and
 * the on-disk .myon/packages/ tree to resolve `module <pkg-module> as <alias>`. */
#include "package.h"
#include "pkg_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

/* TTY detection (myon.is_tty): POSIX isatty()/fileno() live in <unistd.h>,
 * while the Win32 CRT exposes _isatty()/_fileno() in <io.h>.  A tiny wrapper
 * (myon_stream_is_tty) below hides the spelling difference from the builtin. */
#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

/* Phase5 myon.net: the synchronous fallback path in net_wait_fd() uses
 * select(2) directly (outside a coroutine).  On Linux this lives behind
 * <sys/select.h>.  On Windows the equivalent Winsock select() lives in
 * <winsock2.h>, but that header transitively drags in <windows.h> ->
 * <winnt.h>, whose `TokenType` enum collides with Myon's own `TokenType`
 * (src/token.h) that interpreter.c already includes.  Rather than pull the
 * whole Win32 surface into this translation unit (a Step7 integration concern),
 * the Windows synchronous single-fd wait is delegated to net.c via
 * net_sync_wait_fd(), which owns the Winsock headers cleanly.  See
 * docs/myon_spec.md 10.7 / 14.9. */
#if defined(MYON_OS_POSIX)
#include <sys/select.h>
#include <sys/stat.h>
/* Known-issues #9: the call-depth cap is derived from the stack limit the
 * process actually runs with (getrlimit(RLIMIT_STACK)). */
#include <sys/resource.h>
#endif
/* Windows lacks nanosleep; Sleep() (Win32, in kernel32) is the blocking
 * millisecond primitive used by the synchronous myon.time.sleep_ms fallback.
 * Declare it locally with the exact kernel32 prototype so we avoid including
 * <windows.h> here (see the TokenType-collision note above). */
#if defined(_WIN32)
__declspec(dllimport) void __stdcall Sleep(unsigned long dwMilliseconds);
/* Windows has no clock_gettime(CLOCK_REALTIME).  GetSystemTimeAsFileTime gives
 * the wall clock as a FILETIME (100-ns ticks since 1601-01-01 UTC).  We declare
 * it (and the minimal FILETIME layout) locally with the exact kernel32
 * prototype so we still avoid pulling in <windows.h> (see the TokenType
 * collision note above). */
typedef struct { unsigned long dwLowDateTime; unsigned long dwHighDateTime; } MYON_FILETIME;
__declspec(dllimport) void __stdcall GetSystemTimeAsFileTime(MYON_FILETIME *lpSystemTimeAsFileTime);
#endif

/*
 * Wall-clock time in UNIX-epoch milliseconds, portable across POSIX and
 * Windows.  POSIX uses clock_gettime(CLOCK_REALTIME); Windows converts the
 * FILETIME (ticks since 1601) to the UNIX epoch.  Consolidating the three
 * former inline clock_gettime() sites here fixes the MinGW-w64 link failure
 * (undefined reference to clock_gettime) without changing Linux behaviour.
 */
static long long myon_wallclock_ms(void) {
#if defined(_WIN32)
    MYON_FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* 100-ns ticks since 1601-01-01 -> ms; 11644473600 s between 1601 and 1970. */
    unsigned long long ticks =
        ((unsigned long long)ft.dwHighDateTime << 32) | (unsigned long long)ft.dwLowDateTime;
    return (long long)(ticks / 10000ULL) - 11644473600000LL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
#endif
}

/* Some C standard libraries only expose M_PI / M_E under _GNU_SOURCE or
 * similar feature-test macros; provide portable fallbacks (Phase3.5). */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

/* ------------------------------------------------------------------ */
/* Interpreter state and control-flow signalling                       */
/* ------------------------------------------------------------------ */

/*
 * Phase5.2: maximum interpreter-level function-call nesting depth.
 *
 * The tree-walker evaluates a Myon function call by recursing on the native
 * C stack (call_function -> exec_block -> eval_expr -> call_function -> ...).
 * A deeply recursive Myon program therefore consumes the C stack directly and,
 * without a guard, overflows it and is killed by the OS with SIGSEGV before any
 * diagnostic can be printed.  We cap the nesting and raise a normal Myon
 * runtime error instead (same graceful path as the Step12 integer-overflow
 * checks).
 *
 * Known-issues #9: the cap used to be the fixed constant 4000, sized for the
 * 8 MiB default stack of Linux/macOS.  That is *not* what programs actually
 * run with:
 *
 *   * `ulimit -s 1024` / `2048` (containers, CI images, some distros' service
 *     managers) give 1-2 MiB, where the real capacity measured 1710 / 3421
 *     frames for a trivial recursion -- below 4000, so the OS killed the
 *     process with SIGSEGV *before* the guard could fire.
 *   * A sanitizer / debug build spends far more stack per frame (measured
 *     ~4.1 KiB vs ~0.6-0.8 KiB at -O2), so even an 8 MiB stack overflows at
 *     ~2050 frames.
 *
 * So the cap is now derived from the stack limit the process actually has
 * (getrlimit(RLIMIT_STACK) on POSIX), using a conservative per-frame budget
 * and leaving a quarter of the stack unused as headroom.  MYON_MAX_CALL_DEPTH
 * remains the upper bound (so a huge/unlimited stack does not allow unbounded
 * nesting), and MYON_MIN_CALL_DEPTH keeps a usable floor on tiny stacks.
 * Tail-call optimisation is intentionally out of scope.
 */
#define MYON_MAX_CALL_DEPTH 4000
#define MYON_MIN_CALL_DEPTH 64

/* Conservative stack budget per Myon call frame (call_function + exec_block +
 * a few eval_expr levels).  Measured worst cases: ~0.8 KiB at -O2, ~4.1 KiB
 * for an ASan/-O0 build; the estimates below keep a ~2x margin on top. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define MYON_CALL_FRAME_BYTES (8 * 1024)
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define MYON_CALL_FRAME_BYTES (8 * 1024)
#  else
#    define MYON_CALL_FRAME_BYTES (2 * 1024)
#  endif
#else
#  define MYON_CALL_FRAME_BYTES (2 * 1024)
#endif

/* Assumed stack size when the OS will not tell us (Windows, or an unlimited
 * RLIMIT_STACK).  8 MiB is the Linux/macOS default; Windows links a 1 MiB
 * main-thread stack by default, so assume that there. */
#if defined(_WIN32)
#  define MYON_ASSUMED_STACK_BYTES (1024u * 1024u)
#else
#  define MYON_ASSUMED_STACK_BYTES (8u * 1024u * 1024u)
#endif

/*
 * Effective call-depth cap for this process, computed once on first use.
 *
 * This describes the main C stack. call_function uses the coroutine stack
 * size and a per-task nesting counter when executing in the event loop.
 */
static int myon_max_call_depth(void) {
    static int cached = 0;
    if (cached) return cached;

    unsigned long long stack_bytes = MYON_ASSUMED_STACK_BYTES;
#if defined(MYON_OS_POSIX)
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 &&
        rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0)
        stack_bytes = (unsigned long long)rl.rlim_cur;
#endif

    /* Leave a quarter of the stack unused: the guard has to fire while there
     * is still room for the error path (diag snippet printing, longjmp). */
    unsigned long long usable = stack_bytes / 4 * 3;
    unsigned long long depth  = usable / MYON_CALL_FRAME_BYTES;

    if (depth > (unsigned long long)MYON_MAX_CALL_DEPTH) depth = MYON_MAX_CALL_DEPTH;
    if (depth < (unsigned long long)MYON_MIN_CALL_DEPTH) depth = MYON_MIN_CALL_DEPTH;
    cached = (int)depth;
    return cached;
}

typedef enum { FLOW_NORMAL, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN } Flow;

/* registry of declared structs (for constructor / method lookup) */
typedef struct {
    StructDecl **items;
    int          count;
} StructReg;

/* modules that have been loaded (for cycle detection + namespaces) */
typedef struct ModuleEntry {
    char               *path;    /* dotted path */
    char               *alias;   /* alias or NULL */
    int                 loading; /* in-progress flag for cycle detection */
    /* Per-module namespace scope.  When a module is imported with an alias
     * (`module external.util.math as m`), its top-level functions/variables are
     * executed into `ns` (a child of global) instead of leaking into global, so
     * they are reached qualified as `m.<name>`.  NULL for builtin modules and
     * for external modules imported without an alias (which keep the historical
     * flat "dump into global" namespace model). */
    Env                *ns;
    struct ModuleEntry *next;
} ModuleEntry;

typedef struct {
    int count;
    Value values[];
} OwnedValues;

typedef struct {
    void *resource;
    void (*dispose)(void *);
    Task *owner;
    uint64_t sequence;
} CleanupEntry;

struct Interp {
    Env         *global;
    StructReg    structs;
    ModuleEntry *modules;
    uint32_t     enabled_modules;
    jmp_buf      on_error;
    /* return-value transport */
    Value       *ret_values;
    OwnedValues *ret_owner;
    int          ret_count;
    /* generic type-parameter bindings active in the current call */
    Env         *type_env;   /* name -> not used as value; tracked separately */
    /* retained parsed programs (REPL keeps ASTs alive across inputs) */
    Program    **programs;
    int          program_count;
    /* Phase3 C FFI subsystem (lazily created on first myon.ffi.* use) */
    FFIState    *ffi;
    /* Phase4 myon.random: 1 once the PRNG has been seeded (either explicitly
     * via myon.random.seed() or auto-seeded on first int()/float() use). */
    int          random_seeded;
    /* Cooperative event loop, created lazily. Query its current task at use
     * sites: a cached interpreter pointer becomes stale across suspension. */
    EventLoop   *loop;
    /* Phase5 myon.net: socket table (lazily created on first myon.net use). */
    NetState    *net;
    /* Phase5.2: current interpreter-level call-nesting depth, used to guard
     * against C-stack overflow from unbounded Myon recursion.  Incremented on
     * entry to call_function and decremented on every (normal or error) exit.
     * Because a runtime_error longjmps straight back to the top-level barrier,
     * the deep frames never run their decrement, so interp_run/interpret reset
     * this to 0 after catching an error (see those functions). */
    int          call_depth;
    /* Live call scopes carry task ownership and monotonic error-barrier marks.
     * Errors unwind only their own coroutine; shutdown disposes all scopes. */
    CleanupEntry *cleanup_stack;
    uint64_t     cleanup_sequence;
    int          cleanup_count;
    int          cleanup_cap;
    /*
     * Step 6 (MVM bridge): when non-NULL, argument expressions are ignored and
     * the stdlib / method dispatchers take their arguments from this vector of
     * already-evaluated Values instead.  This lets the bytecode VM reuse the
     * exact same C stdlib implementations without owning any AST: the VM has
     * Values on its operand stack, not Exprs.  It is NULL for every tree-walk
     * (`.myon`) call, so the tree-walking execution path is byte-for-byte
     * unchanged.  `bridge_args` are borrowed (owned by the VM); eval_arg()
     * returns value_copy() of the requested slot, matching the ownership the
     * dispatchers expect from a normally-evaluated argument.
     */
    MyonVMCaller vm_caller;
    void        *vm_context;
    Value       *bridge_args;
    int          bridge_argc;
    /*
     * Directory of the top-level script being run, used to resolve external
     * module import paths (`module external.util.math`) relative to the script
     * itself rather than the process's current working directory.  This makes
     * `myon /some/where/main.myon` work no matter where it is invoked from.
     * NULL means "resolve relative to the current working directory" (REPL /
     * stdin, where there is no owning file).
     */
    char        *script_dir;
    /*
     * Script command-line arguments (spec §10.1.1): everything the CLI
     * passed after the script path (or after a `--` separator).  Exposed to
     * Myon programs via the myon.argv() builtin.  Borrowed pointers into the
     * process argv vector (owned by main.c / the module global below), so this
     * interpreter never frees the strings.  script_argc == 0 / script_argv ==
     * NULL when no script args were supplied (REPL, stdin, bare `myon file`).
     */
    char       **script_argv;
    int          script_argc;
};
typedef struct Interp Interp;

/* Look up an aliased external module by its alias name; defined later. */
static ModuleEntry *find_module_by_alias(Interp *it, const char *alias);

/* ------------------------------------------------------------------ */
/* Error barriers and call-scope cleanup must follow the currently running
 * coroutine, because independent tasks interleave on the same interpreter. */
static Task *interp_current_task(Interp *it) {
    return it->loop ? event_loop_current(it->loop) : NULL;
}

static jmp_buf *interp_error_buf(Interp *it) {
    Task *task = interp_current_task(it);
    return task ? (jmp_buf *)event_loop_task_error_buf(task) : &it->on_error;
}

static void cleanup_register(Interp *it, void *resource, void (*dispose)(void *)) {
    if (it->cleanup_count == it->cleanup_cap) {
        int ncap = it->cleanup_cap ? it->cleanup_cap * 2 : 16;
        it->cleanup_stack = myon_xrealloc(it->cleanup_stack,
                                         (size_t)ncap * sizeof(CleanupEntry));
        it->cleanup_cap = ncap;
    }
    CleanupEntry entry = {resource, dispose, interp_current_task(it), ++it->cleanup_sequence};
    it->cleanup_stack[it->cleanup_count++] = entry;
}

static void cleanup_remove(Interp *it, int index) {
    memmove(&it->cleanup_stack[index], &it->cleanup_stack[index + 1],
            (size_t)(it->cleanup_count - index - 1) * sizeof(CleanupEntry));
    it->cleanup_count--;
}

static void cleanup_forget(Interp *it, void *resource) {
    for (int i = it->cleanup_count - 1; i >= 0; i--) {
        if (it->cleanup_stack[i].resource == resource) { cleanup_remove(it, i); return; }
    }
}

static void dispose_env(void *resource) { env_free(resource); }
static void cleanup_push(Interp *it, Env *env) { cleanup_register(it, env, dispose_env); }
static void cleanup_pop(Interp *it, Env *env) { cleanup_forget(it, env); }
static void dispose_values(void *resource) {
    OwnedValues *owned = resource;
    for (int i = 0; i < owned->count; i++) value_free(&owned->values[i]);
    free(owned);
}
static OwnedValues *owned_values_new(Interp *it, int capacity) {
    size_t bytes, total;
    if (capacity < 0 || !checked_mul_size((size_t)capacity, sizeof(Value), &bytes) ||
        !checked_add_size(bytes, sizeof(OwnedValues), &total)) exit(70);
    OwnedValues *owned = myon_xmalloc(total);
    owned->count = 0;
    cleanup_register(it, owned, dispose_values);
    return owned;
}
static void owned_values_release(Interp *it, OwnedValues *owned) {
    if (!owned) return;
    cleanup_forget(it, owned);
    dispose_values(owned);
}

/* Sequence marks stay valid when other tasks remove interleaved frames. */
static uint64_t cleanup_mark(Interp *it) { return it->cleanup_sequence; }

static void cleanup_unwind(Interp *it, uint64_t mark) {
    Task *owner = interp_current_task(it);
    for (int i = it->cleanup_count - 1; i >= 0; i--) {
        CleanupEntry entry = it->cleanup_stack[i];
        if (entry.owner != owner || entry.sequence <= mark) continue;
        cleanup_remove(it, i);
        if (entry.resource == it->ret_owner) {
            it->ret_owner = NULL; it->ret_values = NULL; it->ret_count = 0;
        }
        entry.dispose(entry.resource);
    }
}

/* At shutdown no task can resume; dispose frames belonging to every task. */
static void cleanup_destroy(Interp *it) {
    while (it->cleanup_count) {
        CleanupEntry entry = it->cleanup_stack[--it->cleanup_count];
        entry.dispose(entry.resource);
    }
}

static void runtime_error(Interp *it, int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "myon: runtime error at line %d: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    /* P5: show the offending source line.  The interpreter only tracks the
     * line (not the column) for a running expression, so the caret points at
     * the start of the line as a best-effort locator. */
    diag_print_snippet(line, 1);
    longjmp(*interp_error_buf(it), 1);
}

/* Error messages copied from values must be freed before the nonlocal jump. */
static void runtime_error_owned(Interp *it, int line, char *message) {
    fprintf(stderr, "myon: runtime error at line %d: %s\n", line, message);
    free(message);
    diag_print_snippet(line, 1);
    longjmp(*interp_error_buf(it), 1);
}

static void runtime_error_free(Interp *it, int line, void *owned, const char *format, ...) {
    va_list ap;
    fprintf(stderr, "myon: runtime error at line %d: ", line);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    free(owned);
    fputc('\n', stderr);
    diag_print_snippet(line, 1);
    longjmp(*interp_error_buf(it), 1);
}

static void require_module(Interp *it, int line, const char *module) {
    uint32_t bit = myon_builtin_module_bit(module);
    if (!bit || !(it->enabled_modules & bit))
        runtime_error(it, line, "module '%s' is not declared; add 'module %s'", module, module);
}
static void require_native_module(Interp *it, int line, const char *name) {
    const char *module = myon_call_module(name);
    if (module) require_module(it, line, module);
}

static Value eval_expr(Interp *it, Env *env, Expr *e);
static Value eval_arg(Interp *it, Env *env, Expr *call, int i);
static Flow  exec_block(Interp *it, Env *env, StmtList *body);
static Flow  exec_stmt(Interp *it, Env *env, Stmt *s);
static Value call_function(Interp *it, int line, Value fn, Value *args, int argc,
                           char **arg_names);

/* Phase5: ensure the event loop exists (lazy). */
static void task_result_free(void *payload) {
    Value *v = (Value *)payload;
    value_free(v);
    free(v);
}

static EventLoop *ensure_loop(Interp *it) {
    if (!it->loop) {
        it->loop = event_loop_create();
        event_loop_set_result_cleanup(it->loop, task_result_free);
    }
    return it->loop;
}

/* ------------------------------------------------------------------ */
/* Struct registry                                                     */
/* ------------------------------------------------------------------ */

static void register_struct(Interp *it, StructDecl *sd) {
    it->structs.items = (StructDecl **)myon_xrealloc(
        it->structs.items, sizeof(StructDecl *) * (it->structs.count + 1));
    it->structs.items[it->structs.count++] = sd;
}

static StructDecl *find_struct(Interp *it, const char *name) {
    for (int i = 0; i < it->structs.count; i++)
        if (strcmp(it->structs.items[i]->name, name) == 0)
            return it->structs.items[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Casts / value constructors (spec 2.3)                               */
/* ------------------------------------------------------------------ */

static Value cast_to_str(Interp *it, int line, Value v) {
    (void)it; (void)line;
    char *s = value_to_cstr(&v);
    value_free(&v);
    return value_str(s);
}

/* double(LLONG_MAX) rounds up to 2^63, so the upper bound must be
 * exclusive. Compare before the C cast: nonfinite/out-of-range casts are UB. */
static Value checked_float_to_int(Interp *it, int line, double n, const char *operation) {
    if (!isfinite(n) || n < (double)LLONG_MIN || n >= -(double)LLONG_MIN)
        runtime_error(it, line, "%s: numeric value out of range for int", operation);
    return value_int((long long)n);
}

static Value cast_to_int(Interp *it, int line, Value v) {
    switch (v.type) {
        case TYPE_INT: return v;
        case TYPE_FLOAT: return checked_float_to_int(it, line, v.as.f, "int()");
        case TYPE_BOOL: { long long r = v.as.b; return value_int(r); }
        case TYPE_STR: {
            char *end = NULL;
            errno = 0;
            long long r = strtoll(v.as.obj->as.str, &end, 10);
            int range_error = errno == ERANGE;
            if (end == v.as.obj->as.str || *end != '\0') {
                char *bad = myon_strdup(v.as.obj->as.str);
                value_free(&v);
                runtime_error_free(it, line, bad, "int(): cannot parse '%s' as integer", bad);
            }
            value_free(&v);
            if (range_error) runtime_error(it, line, "int(): integer string out of range");
            return value_int(r);
        }
        case TYPE_CHAR: {
            long long r = (unsigned char)v.as.obj->as.str[0];
            value_free(&v);
            return value_int(r);
        }
        default:
            runtime_error(it, line, "int(): unsupported source type %s", value_type_name(&v));
            return value_nil();
    }
}

static Value cast_to_char(Interp *it, int line, Value v) {
    switch (v.type) {
        case TYPE_CHAR: return v;
        case TYPE_INT: {
            char buf[2] = { (char)(v.as.i & 0xFF), '\0' };
            return value_char(myon_strdup(buf));
        }
        case TYPE_STR: {
            if (strlen(v.as.obj->as.str) == 0) {
                value_free(&v);
                runtime_error(it, line, "char(): empty string");
            }
            char buf[2] = { v.as.obj->as.str[0], '\0' };
            value_free(&v);
            return value_char(myon_strdup(buf));
        }
        default:
            runtime_error(it, line, "char(): unsupported source type %s", value_type_name(&v));
            return value_nil();
    }
}

/* ------------------------------------------------------------------ */
/* Arithmetic / comparison with strict typing (spec 2.2)               */
/* ------------------------------------------------------------------ */

static long long int_arith(Interp *it, int line, OpKind op, long long a, long long b) {
    long long r;
    switch (op) {
        case OP_ADD:
            if (__builtin_add_overflow(a, b, &r))
                runtime_error(it, line, "integer overflow in addition");
            return r;
        case OP_SUB:
            if (__builtin_sub_overflow(a, b, &r))
                runtime_error(it, line, "integer overflow in subtraction");
            return r;
        case OP_MUL:
            if (__builtin_mul_overflow(a, b, &r))
                runtime_error(it, line, "integer overflow in multiplication");
            return r;
        case OP_DIV:
            if (b == 0) runtime_error(it, line, "division by zero");
            if (a == LLONG_MIN && b == -1)
                runtime_error(it, line, "integer overflow in division");
            return a / b;
        default:
            runtime_error(it, line, "unsupported integer operator");
            return 0;
    }
}

static Value negate_value(Interp *it, int line, Value v) {
    if (v.type == TYPE_INT) {
        if (v.as.i == LLONG_MIN) runtime_error(it, line, "integer overflow in negation");
        return value_int(-v.as.i);
    }
    if (v.type == TYPE_FLOAT) return value_float(-v.as.f);
    Type t = v.type;
    value_free(&v);
    runtime_error(it, line, "unary '-' requires int or float, got %s", type_name(t));
    return value_nil();
}

static int compare_ints(long long a, long long b, OpKind op) {
    switch (op) {
        case OP_LT: return a < b;
        case OP_GT: return a > b;
        case OP_LE: return a <= b;
        case OP_GE: return a >= b;
        case OP_EQ: return a == b;
        case OP_NEQ: return a != b;
        default: return 0;
    }
}

static int compare_ordered(double a, double b, OpKind op) {
    switch (op) {
        case OP_LT: return a < b;
        case OP_GT: return a > b;
        case OP_LE: return a <= b;
        case OP_GE: return a >= b;
        case OP_EQ: return a == b;
        case OP_NEQ:return a != b;
        default:    return 0;
    }
}

static Value eval_binary(Interp *it, int line, OpKind op, Value l, Value r) {
    if (op == OP_EQ || op == OP_NEQ) {
        if (l.type == TYPE_NIL || r.type == TYPE_NIL) {
            int both_nil = (l.type == TYPE_NIL && r.type == TYPE_NIL);
            int one_err_nil =
                (l.type == TYPE_ERROR && r.type == TYPE_NIL) ||
                (r.type == TYPE_ERROR && l.type == TYPE_NIL);
            if (!both_nil && !one_err_nil) {
                Type lt = l.type, rt = r.type;
                value_free(&l); value_free(&r);
                runtime_error(it, line,
                    "myon.nil may only be compared with error values (spec 2.4); got %s and %s",
                    type_name(lt), type_name(rt));
            }
            int equal = both_nil;
            value_free(&l); value_free(&r);
            return value_bool(op == OP_EQ ? equal : !equal);
        }
    }

    if (!type_equal(l.type, r.type)) {
        Type lt = l.type, rt = r.type;
        value_free(&l); value_free(&r);
        runtime_error(it, line,
            "type mismatch: cannot apply operator to %s and %s (strict typing, spec 2.2)",
            type_name(lt), type_name(rt));
    }

    switch (l.type) {
        case TYPE_INT: {
            long long a = l.as.i, b = r.as.i;
            switch (op) {
                case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
                    return value_int(int_arith(it, line, op, a, b));
                default:
                    return value_bool(compare_ints(a, b, op));
            }
        }
        case TYPE_FLOAT: {
            double a = l.as.f, b = r.as.f;
            switch (op) {
                case OP_ADD: return value_float(a + b);
                case OP_SUB: return value_float(a - b);
                case OP_MUL: return value_float(a * b);
                case OP_DIV:
                    if (b == 0.0) runtime_error(it, line, "division by zero");
                    return value_float(a / b);
                default: return value_bool(compare_ordered(a, b, op));
            }
        }
        case TYPE_STR: {
            const char *ls = l.as.obj->as.str, *rs = r.as.obj->as.str;
            if (op == OP_ADD) {
                size_t la = strlen(ls), lb = strlen(rs);
                char *buf = (char *)myon_xmalloc(la + lb + 1);
                memcpy(buf, ls, la);
                memcpy(buf + la, rs, lb);
                buf[la + lb] = '\0';
                value_free(&l); value_free(&r);
                return value_str(buf);
            }
            if (op == OP_EQ || op == OP_NEQ) {
                int eq = strcmp(ls, rs) == 0;
                value_free(&l); value_free(&r);
                return value_bool(op == OP_EQ ? eq : !eq);
            }
            value_free(&l); value_free(&r);
            runtime_error(it, line, "unsupported operator on str");
            break;
        }
        case TYPE_CHAR: {
            int eq = strcmp(l.as.obj->as.str, r.as.obj->as.str) == 0;
            if (op == OP_EQ || op == OP_NEQ) {
                value_free(&l); value_free(&r);
                return value_bool(op == OP_EQ ? eq : !eq);
            }
            value_free(&l); value_free(&r);
            runtime_error(it, line, "unsupported operator on char");
            break;
        }
        case TYPE_BOOL: {
            int eq = (l.as.b == r.as.b);
            if (op == OP_EQ || op == OP_NEQ)
                return value_bool(op == OP_EQ ? eq : !eq);
            runtime_error(it, line, "unsupported operator on bool");
            break;
        }
        case TYPE_ERROR: {
            int eq = strcmp(l.as.obj->as.str, r.as.obj->as.str) == 0;
            if (op == OP_EQ || op == OP_NEQ) {
                value_free(&l); value_free(&r);
                return value_bool(op == OP_EQ ? eq : !eq);
            }
            value_free(&l); value_free(&r);
            runtime_error(it, line, "unsupported operator on error");
            break;
        }
        default: {
            Type type = l.type;
            value_free(&l); value_free(&r);
            runtime_error(it, line, "unsupported operand type %s", type_name(type));
        }
    }
    return value_nil();
}

/* ------------------------------------------------------------------ */
/* String interpolation (spec section 4, Step 10)                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *out, *source, *piece;
    Program *program;
    TokenList tokens;
} StringEval;

static void dispose_string_eval(void *resource) {
    StringEval *ctx = resource;
    free(ctx->out); free(ctx->source); free(ctx->piece);
    program_free(ctx->program);
    token_list_free(&ctx->tokens);
    free(ctx);
}
static void string_reserve(Interp *it, int line, StringEval *ctx, size_t *cap,
                           size_t length, size_t extra) {
    size_t needed;
    if (!checked_add_size(length, extra, &needed) || !checked_add_size(needed, 1, &needed))
        runtime_error(it, line, "interpolated string is too large");
    if (needed <= *cap) return;
    size_t grown;
    if (!checked_mul_size(*cap, 2, &grown) || grown < needed) grown = needed;
    ctx->out = myon_xrealloc(ctx->out, grown);
    *cap = grown;
}

/* The context owns partial lexing/parsing/output across nested runtime errors. */
static Value interpolate_string(Interp *it, Env *env, int line, const char *raw) {
    size_t cap = strlen(raw) + 1, len = 0;
    StringEval *ctx = myon_xmalloc(sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));
    cleanup_register(it, ctx, dispose_string_eval);
    ctx->out = myon_xmalloc(cap);
    ctx->out[0] = '\0';
    const char *s = raw;
    while (*s) {
        char escaped;
        if (myon_string_escape(&s, &escaped)) {
            string_reserve(it, line, ctx, &cap, len, 1);
            ctx->out[len++] = escaped; ctx->out[len] = '\0'; continue;
        }
        if ((s[0] == '{' && s[1] == '{') || (s[0] == '}' && s[1] == '}')) {
            string_reserve(it, line, ctx, &cap, len, 1);
            ctx->out[len++] = s[0]; ctx->out[len] = '\0'; s += 2; continue;
        }
        if (s[0] == '{') {
            const char *end = strchr(s + 1, '}');
            if (!end) runtime_error(it, line, "unterminated '{' in string interpolation");
            ctx->source = myon_strndup(s + 1, (size_t)(end - s - 1));
            if (!lexer_tokenize(ctx->source, &ctx->tokens))
                runtime_error(it, line, "lexical error in interpolation");
            ctx->program = parser_parse(&ctx->tokens);
            if (!ctx->program || ctx->program->stmts.count != 1 ||
                ctx->program->stmts.items[0]->kind != STMT_EXPR)
                runtime_error(it, line, "invalid expression in string interpolation");
            Value v = eval_expr(it, env, ctx->program->stmts.items[0]->as.expr);
            ctx->piece = value_to_cstr(&v);
            value_free(&v);
            size_t n = strlen(ctx->piece);
            string_reserve(it, line, ctx, &cap, len, n);
            memcpy(ctx->out + len, ctx->piece, n + 1); len += n;
            free(ctx->piece); ctx->piece = NULL;
            free(ctx->source); ctx->source = NULL;
            program_free(ctx->program); ctx->program = NULL;
            token_list_free(&ctx->tokens);
            s = end + 1;
            continue;
        }
        string_reserve(it, line, ctx, &cap, len, 1);
        ctx->out[len++] = *s++; ctx->out[len] = '\0';
    }
    char *out = ctx->out;
    ctx->out = NULL;
    cleanup_forget(it, ctx);
    dispose_string_eval(ctx);
    return value_str(out);
}

/* ------------------------------------------------------------------ */
/* Builtins: myon.print / myon.input                                   */
/* ------------------------------------------------------------------ */

/* Return non-zero when the given C stream is connected to a terminal (TTY),
 * abstracting the POSIX/Win32 spelling difference (see the <io.h>/<unistd.h>
 * include block above).  Used by myon.is_tty(). */
static int myon_stream_is_tty(FILE *stream) {
#if defined(_WIN32)
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

/*
 * Shared core for the stdout writers.  Concatenates every argument's string
 * form to `stream`, honouring the same named-argument rejection as before.
 * `newline` controls whether a trailing '\n' is appended:
 *   - myon.print    -> newline = 1 (auto newline, the historical behaviour)
 *   - myon.println  -> newline = 1 (explicit auto-newline alias)
 *   - myon.write    -> newline = 0 (no trailing newline; enables \r overwrites,
 *                                   progress bars and spinners — spec §10.1)
 */
static Value builtin_print_impl(Interp *it, Env *env, Expr *call,
                                FILE *stream, int newline,
                                const char *who) {
    /* argc comes from the injected bridge vector when the VM calls us,
     * otherwise from the AST — see eval_arg() for the same convention. */
    int argc = it->bridge_args ? it->bridge_argc : call->as.call.arg_count;
    for (int i = 0; i < argc; i++) {
        /* Named args (`x=...`) only exist in an AST call; the VM never
         * produces them, so this check is skipped in bridge mode. */
        if (!it->bridge_args && call->as.call.arg_names[i])
            runtime_error(it, call->line, "%s does not accept named arguments", who);
        Value v = eval_arg(it, env, call, i);
        char *s = value_to_cstr(&v);
        fputs(s, stream);
        free(s);
        value_free(&v);
    }
    if (newline) fputc('\n', stream);
    return value_void();
}

static Value builtin_print(Interp *it, Env *env, Expr *call) {
    /* myon.print: auto newline (unchanged historical behaviour). */
    return builtin_print_impl(it, env, call, stdout, 1, "myon.print");
}

static Value builtin_println(Interp *it, Env *env, Expr *call) {
    /* myon.println: explicit auto-newline writer (alias of print). */
    return builtin_print_impl(it, env, call, stdout, 1, "myon.println");
}

static Value builtin_write(Interp *it, Env *env, Expr *call) {
    /* myon.write: NO trailing newline.  This is the output path that makes
     * carriage-return overwrites (progress bars, spinners) possible. */
    return builtin_print_impl(it, env, call, stdout, 0, "myon.write");
}

static Value builtin_eprint(Interp *it, Env *env, Expr *call) {
    /* myon.eprint: write to stderr, WITH a trailing newline, so diagnostics
     * and progress output stay off stdout (which may be piped/processed). */
    return builtin_print_impl(it, env, call, stderr, 1, "myon.eprint");
}

static Value builtin_flush(Interp *it, Env *env, Expr *call) {
    /* myon.flush() ret void — force buffered stdout to the terminal now.
     * Essential when using myon.write for a live-updating single line. */
    int argc = it->bridge_args ? it->bridge_argc : call->as.call.arg_count;
    if (argc != 0)
        runtime_error(it, call->line, "myon.flush expects no arguments");
    (void)env;
    fflush(stdout);
    return value_void();
}

static Value builtin_is_tty(Interp *it, Env *env, Expr *call) {
    /* myon.is_tty() ret bool — true when stdout is an interactive terminal.
     * Lets scripts suppress ANSI control codes when piped/redirected. */
    int argc = it->bridge_args ? it->bridge_argc : call->as.call.arg_count;
    if (argc != 0)
        runtime_error(it, call->line, "myon.is_tty expects no arguments");
    (void)env;
    return value_bool(myon_stream_is_tty(stdout));
}

static Value builtin_argv(Interp *it, Env *env, Expr *call) {
    /* myon.argv() ret myon.array(str) — the script arguments passed after the
     * script path on the command line (spec §10.1.1).  Always returns an
     * array (possibly empty), never nil. */
    int argc = it->bridge_args ? it->bridge_argc : call->as.call.arg_count;
    if (argc != 0)
        runtime_error(it, call->line, "myon.argv expects no arguments");
    (void)env;
    Value arr = value_array(typespec_prim(TYPE_STR));
    for (int i = 0; i < it->script_argc; i++)
        array_push(&arr, value_str(myon_strdup(it->script_argv[i])));
    return arr;
}

static Value builtin_input(Interp *it, Env *env, Expr *call) {
    int argc = it->bridge_args ? it->bridge_argc : call->as.call.arg_count;
    if (argc > 0) {
        Value prompt = eval_arg(it, env, call, 0);
        char *s = value_to_cstr(&prompt);
        fputs(s, stdout);
        fflush(stdout);
        free(s);
        value_free(&prompt);
    }
    size_t cap = 128, len = 0;
    char *buf = (char *)myon_xmalloc(cap);
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (len + 1 >= cap) { cap *= 2; buf = myon_xrealloc(buf, cap); }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return value_str(buf);
}

/* ------------------------------------------------------------------ */
/* Standard library: myon.math / myon.string (Step 16)                 */
/* ------------------------------------------------------------------ */

static Value eval_arg(Interp *it, Env *env, Expr *call, int i) {
    /*
     * Step 6 (MVM bridge): when the VM has injected pre-evaluated arguments,
     * hand back a copy of the requested slot rather than walking an AST node.
     * `env`/`call` are unused in that mode.  For the tree-walking interpreter
     * bridge_args is always NULL, so this is exactly eval_expr(args[i]).
     */
    if (it->bridge_args) {
        if (i < 0 || i >= it->bridge_argc) return value_nil();
        return value_copy(&it->bridge_args[i]);
    }
    return eval_expr(it, env, call->as.call.args[i]);
}

static double as_number(Interp *it, int line, Value v) {
    if (v.type == TYPE_INT) { double d = (double)v.as.i; return d; }
    if (v.type == TYPE_FLOAT) return v.as.f;
    runtime_error(it, line, "expected a numeric argument, got %s", type_name(v.type));
    return 0;
}

/* Numeric type-preservation helper (Phase3.5, Step1).
 * Returns 1 when *both* arguments are TYPE_INT (so the caller can compute the
 * result in int precision without a lossy round-trip through double), and 0
 * when at least one operand is a float.  Non-numeric arguments raise a
 * runtime_error, exactly like as_number(). */
static int both_int(Interp *it, int line, Value a, Value b) {
    (void)as_number(it, line, a); /* reuse as_number purely for its type check */
    (void)as_number(it, line, b);
    return a.type == TYPE_INT && b.type == TYPE_INT;
}

/* UTF-8 helpers (Phase3.5, Step2).
 * Count the number of Unicode code points in a NUL-terminated UTF-8 byte
 * string.  Robust against malformed sequences: an invalid lead/continuation
 * byte is counted as a single character rather than crashing or looping
 * forever (Myon str values may carry arbitrary bytes via FFI). */
static long long utf8_char_count(const char *s) {
    long long count = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        int extra;
        if      ((c & 0x80) == 0x00) extra = 0; /* 0xxxxxxx */
        else if ((c & 0xE0) == 0xC0) extra = 1; /* 110xxxxx */
        else if ((c & 0xF0) == 0xE0) extra = 2; /* 1110xxxx */
        else if ((c & 0xF8) == 0xF0) extra = 3; /* 11110xxx */
        else { p++; count++; continue; }        /* invalid lead byte */
        p++;
        int ok = 1;
        for (int k = 0; k < extra; k++) {
            if ((*p & 0xC0) != 0x80) { ok = 0; break; }
            p++;
        }
        (void)ok; /* robustness first: count one code point regardless */
        count++;
    }
    return count;
}

/* Return the byte offset at which code point `char_idx` (0-indexed) begins in
 * the NUL-terminated UTF-8 string `s`.  When `char_idx` reaches or exceeds the
 * number of code points, the byte length (offset of the NUL terminator) is
 * returned, so callers can address the one-past-the-end position for slicing.
 * Malformed sequences use the same robust fallback as utf8_char_count(). */
static long long utf8_byte_offset(const char *s, long long char_idx) {
    const unsigned char *base = (const unsigned char *)s;
    const unsigned char *p = base;
    long long seen = 0;
    while (*p && seen < char_idx) {
        unsigned char c = *p;
        int extra;
        if      ((c & 0x80) == 0x00) extra = 0; /* 0xxxxxxx */
        else if ((c & 0xE0) == 0xC0) extra = 1; /* 110xxxxx */
        else if ((c & 0xF0) == 0xE0) extra = 2; /* 1110xxxx */
        else if ((c & 0xF8) == 0xF0) extra = 3; /* 11110xxx */
        else { p++; seen++; continue; }         /* invalid lead byte */
        p++;
        for (int k = 0; k < extra && *p; k++) {
            if ((*p & 0xC0) != 0x80) break;      /* invalid continuation */
            p++;
        }
        seen++;
    }
    return (long long)(p - base);
}

/* Build a 2-value tuple (value, error) for the Rust/Go-style return
 * convention (spec 6.2).  Uses the same untyped-array tuple representation as
 * multi-value `ret`, so multiple-target assignment can unpack it. */
static Value make_result_pair(Value ok, Value err) {
    Value tup = value_array(NULL);
    array_push(&tup, ok);
    array_push(&tup, err);
    return tup;
}

/* ------------------------------------------------------------------ */
/* Standard library: myon.file.* (Step P4 — file I/O)                  */
/*                                                                     */
/* Failures do not raise a Myon runtime_error; instead they are        */
/* surfaced as an `error(...)` value in the second tuple slot so the   */
/* caller can inspect it with `myon.if err != myon.nil` (spec 6.2).    */
/* ------------------------------------------------------------------ */

/*
 * Portable temp-path resolution for myon.file.*.
 *
 * Cross-platform Myon programs (and the test-suite fixtures) naturally reach
 * for the POSIX convention "/tmp/<file>" for scratch files.  That works on
 * Linux/macOS/BSD, but a natively-built myon.exe on Windows links the Windows
 * C runtime, whose fopen() does *not* understand the MSYS2 "/tmp" mount -- so
 * fopen("/tmp/...") fails with ENOENT and every file op reports an error.  To
 * keep "/tmp/<file>" meaningful everywhere, on Windows we transparently rewrite
 * a leading "/tmp/" to the process's real temp directory (%TEMP%/%TMP%, falling
 * back to the current directory).  The rewrite is written into `buf` and, if it
 * happened, `buf` is returned; otherwise the original path is returned
 * untouched.  On POSIX this is a no-op, so "/tmp" stays literally "/tmp".
 */
static const char *resolve_fs_path(const char *path, char *buf, size_t bufsz) {
#if defined(MYON_OS_WINDOWS)
    if (path && strncmp(path, "/tmp/", 5) == 0) {
        const char *tmp = getenv("TEMP");
        if (!tmp || !*tmp) tmp = getenv("TMP");
        if (!tmp || !*tmp) tmp = ".";
        snprintf(buf, bufsz, "%s\\%s", tmp, path + 5);
        return buf;
    }
#else
    (void)buf; (void)bufsz;
#endif
    return path;
}

/* Dispatch a "myon.file.<fn>" call. Returns 1 and sets *out if handled. */
static int call_file_io(Interp *it, Env *env, const char *name, Expr *call, Value *out) {
    int line = call->line;

    /* myon.file.read(path) ret str, error */
    if (strcmp(name, "myon.file.read") == 0) {
        Value p = eval_arg(it, env, call, 0);
        if (p.type != TYPE_STR) { value_free(&p); runtime_error(it, line, "myon.file.read expects a str path"); }
        const char *path = p.as.obj->as.str;
        if (!path) { value_free(&p); *out = make_result_pair(value_str(myon_strdup("")), value_error(myon_strdup("null path"))); return 1; }
        char pathbuf[1024];
        const char *ospath = resolve_fs_path(path, pathbuf, sizeof(pathbuf));
        FILE *f = fopen(ospath, "rb");
        if (!f) {
            char msg[512];
            snprintf(msg, sizeof(msg), "cannot open '%s' for reading", path);
            value_free(&p);
            *out = make_result_pair(value_str(myon_strdup("")), value_error(myon_strdup(msg)));
            return 1;
        }
        size_t cap = 4096, len = 0;
        char *buf = (char *)myon_xmalloc(cap);
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (len + 1 >= cap) { cap *= 2; buf = (char *)myon_xrealloc(buf, cap); }
            buf[len++] = (char)c;
        }
        buf[len] = '\0';
        fclose(f);
        value_free(&p);
        *out = make_result_pair(value_str(buf), value_nil());
        return 1;
    }

    /* myon.file.write(path, content) / myon.file.append(path, content)
     *   ret bool, error */
    if (strcmp(name, "myon.file.write") == 0 || strcmp(name, "myon.file.append") == 0) {
        int append = (strcmp(name, "myon.file.append") == 0);
        Value p = eval_arg(it, env, call, 0);
        Value cnt = eval_arg(it, env, call, 1);
        if (p.type != TYPE_STR || cnt.type != TYPE_STR) {
            value_free(&p); value_free(&cnt);
            runtime_error(it, line, "%s expects (str path, str content)", name);
        }
        const char *path = p.as.obj->as.str;
        const char *content = cnt.as.obj->as.str;
        if (!path) {
            value_free(&p); value_free(&cnt);
            *out = make_result_pair(value_bool(0), value_error(myon_strdup("null path")));
            return 1;
        }
        char pathbuf[1024];
        const char *ospath = resolve_fs_path(path, pathbuf, sizeof(pathbuf));
        FILE *f = fopen(ospath, append ? "ab" : "wb");
        if (!f) {
            char msg[512];
            snprintf(msg, sizeof(msg), "cannot open '%s' for %s", path, append ? "appending" : "writing");
            value_free(&p); value_free(&cnt);
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(msg)));
            return 1;
        }
        size_t n = strlen(content);
        size_t w = fwrite(content, 1, n, f);
        int close_err = fclose(f);
        if (w != n || close_err != 0) {
            char msg[512];
            snprintf(msg, sizeof(msg), "write to '%s' failed", path);
            value_free(&p); value_free(&cnt);
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(msg)));
            return 1;
        }
        value_free(&p); value_free(&cnt);
        *out = make_result_pair(value_bool(1), value_nil());
        return 1;
    }

    /* myon.file.exists(path) ret bool */
    if (strcmp(name, "myon.file.exists") == 0) {
        Value p = eval_arg(it, env, call, 0);
        if (p.type != TYPE_STR) { value_free(&p); runtime_error(it, line, "myon.file.exists expects a str path"); }
        const char *path = p.as.obj->as.str;
        int exists = 0;
        if (path) {
            char pathbuf[1024];
            const char *ospath = resolve_fs_path(path, pathbuf, sizeof(pathbuf));
            FILE *f = fopen(ospath, "rb");
            if (f) { exists = 1; fclose(f); }
        }
        value_free(&p);
        *out = value_bool(exists);
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Standard library: myon.ffi.* (Phase3 — C FFI)                       */
/*                                                                     */
/* Like myon.file.*, failures surface as an `error(...)` value in the  */
/* second tuple slot rather than aborting with a runtime_error, so the */
/* caller can inspect them via `myon.if err != myon.nil` (spec 6.2).   */
/* ------------------------------------------------------------------ */

/* Lazily create (once) the interpreter's FFI subsystem. */
static FFIState *ffi_get_state(Interp *it) {
    if (!it->ffi) it->ffi = ffi_state_create();
    return it->ffi;
}

/* Convert one already-evaluated Myon Value into an FFIArgValue according to the
 * signature character `sig`.  Returns 1 on success, 0 on a type mismatch (and
 * writes a heap error message into *err).  `st` is used to resolve 'b'
 * (memory-block) arguments to their real address. */
static int ffi_convert_arg(FFIState *st, char sig, const Value *v,
                           FFIArgValue *av, FFIArgKind *ak, char **err) {
    switch (sig) {
        case 'i':
            if (v->type == TYPE_INT)  { av->i = v->as.i; }
            else if (v->type == TYPE_BOOL) { av->i = v->as.b; }
            else { *err = myon_strdup("ffi: 'i' argument expects int/bool"); return 0; }
            *ak = FFI_ARG_I64;
            return 1;
        case 'p':
            /* pointer handles are carried as Myon int */
            if (v->type == TYPE_INT) { av->p = (void *)(size_t)v->as.i; }
            else { *err = myon_strdup("ffi: 'p' argument expects int (pointer handle)"); return 0; }
            *ak = FFI_ARG_PTR;
            return 1;
        case 'b': {
            /* Phase3.1: 'b' takes a Myon.ffi.alloc block ID and passes the
             * block's real memory address to the C function (as a pointer).
             * Guards against invalid IDs so a bogus value can never crash. */
            if (v->type != TYPE_INT) { *err = myon_strdup("ffi: 'b' argument expects int (block id)"); return 0; }
            void *p = ffi_mem_ptr(st, v->as.i);
            if (!p) { *err = myon_strdup("ffi: 'b' argument is not a valid memory block id"); return 0; }
            av->p = p;
            *ak = FFI_ARG_PTR;
            return 1;
        }
        case 'd':
            if (v->type == TYPE_FLOAT) { av->d = v->as.f; }
            else if (v->type == TYPE_INT) { av->d = (double)v->as.i; }
            else { *err = myon_strdup("ffi: 'd' argument expects float/int"); return 0; }
            *ak = FFI_ARG_F64;
            return 1;
        case 's':
            if (v->type == TYPE_STR) { av->s = v->as.obj->as.str; }
            else { *err = myon_strdup("ffi: 's' argument expects str"); return 0; }
            *ak = FFI_ARG_STR;
            return 1;
        default: {
            char buf[64];
            snprintf(buf, sizeof(buf), "ffi: unknown signature char '%c'", sig);
            *err = myon_strdup(buf);
            return 0;
        }
    }
}

/* Shared implementation for myon.ffi.call_{i,d,p,v}. */
static Value ffi_do_call(Interp *it, Env *env, Expr *call, FFIRetKind ret_kind) {
    int argc = call->as.call.arg_count;

    Value fail_default = (ret_kind == FFI_RET_F64) ? value_float(0.0)
                       : (ret_kind == FFI_RET_VOID) ? value_bool(0)
                       : value_int(0);

    if (argc < 3) {
        return make_result_pair(fail_default,
            value_error(myon_strdup("ffi.call_* expects (handle, name, sig, ...)")));
    }

    Value hv   = eval_arg(it, env, call, 0);
    Value name = eval_arg(it, env, call, 1);
    Value sigv = eval_arg(it, env, call, 2);

    if (hv.type != TYPE_INT || name.type != TYPE_STR || sigv.type != TYPE_STR) {
        value_free(&hv); value_free(&name); value_free(&sigv);
        return make_result_pair(fail_default,
            value_error(myon_strdup("ffi.call_* expects (int handle, str name, str sig)")));
    }

    long long handle = hv.as.i;
    const char *sym  = name.as.obj->as.str;
    const char *sig  = sigv.as.obj->as.str;
    int nsig = (int)strlen(sig);

    /* number of provided variadic args must match sig length */
    int provided = argc - 3;
    if (nsig != provided) {
        value_free(&hv); value_free(&name); value_free(&sigv);
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "ffi.call_*: signature has %d args but %d were provided",
                 nsig, provided);
        return make_result_pair(fail_default, value_error(myon_strdup(buf)));
    }
    if (nsig > 6) {
        value_free(&hv); value_free(&name); value_free(&sigv);
        return make_result_pair(fail_default,
            value_error(myon_strdup("ffi.call_*: at most 6 arguments are supported")));
    }

    /* resolve the symbol */
    void *fn = ffi_lookup_symbol(ffi_get_state(it), handle, sym);
    if (!fn) {
        char buf[256];
        snprintf(buf, sizeof(buf), "ffi.call_*: symbol '%s' not found in handle %lld",
                 sym, handle);
        value_free(&hv); value_free(&name); value_free(&sigv);
        return make_result_pair(fail_default, value_error(myon_strdup(buf)));
    }

    /* evaluate and convert the variadic arguments in signature order */
    FFIArgValue args[6];
    FFIArgKind  kinds[6];
    Value       argvals[6];
    int         nargvals = 0;
    char       *cerr = NULL;
    int         ok = 1;

    for (int i = 0; i < nsig; i++) {
        Value av = eval_arg(it, env, call, 3 + i);
        argvals[nargvals++] = av;
        if (!ffi_convert_arg(ffi_get_state(it), sig[i], &av, &args[i], &kinds[i], &cerr)) {
            ok = 0;
            break;
        }
    }

    Value result;
    if (!ok) {
        result = make_result_pair(fail_default, value_error(cerr));
    } else {
        FFIRetValue rv;
        if (!ffi_call_dispatch(fn, args, kinds, nsig, ret_kind, &rv)) {
            value_free(&fail_default);
            result = make_result_pair(
                (ret_kind == FFI_RET_F64) ? value_float(0.0)
                : (ret_kind == FFI_RET_VOID) ? value_bool(0)
                : value_int(0),
                value_error(myon_strdup(
                    "ffi.call_*: unsupported argument arity/ordering "
                    "(integer-class args must precede double args; max 6)")));
        } else {
            value_free(&fail_default);
            Value ok_val;
            switch (ret_kind) {
                case FFI_RET_F64:  ok_val = value_float(rv.d); break;
                case FFI_RET_PTR:  ok_val = value_int((long long)(size_t)rv.p); break;
                case FFI_RET_VOID: ok_val = value_bool(1); break;
                case FFI_RET_I64:
                default:           ok_val = value_int(rv.i); break;
            }
            result = make_result_pair(ok_val, value_nil());
        }
    }

    for (int i = 0; i < nargvals; i++) value_free(&argvals[i]);
    value_free(&hv); value_free(&name); value_free(&sigv);
    return result;
}

/* Dispatch a "myon.ffi.<fn>" call. Returns 1 and sets *out if handled. */
static int call_ffi(Interp *it, Env *env, const char *name, Expr *call, Value *out) {
    int line = call->line;

    /* myon.ffi.load(path) ret int, error */
    if (strcmp(name, "myon.ffi.load") == 0) {
        Value p = eval_arg(it, env, call, 0);
        if (p.type != TYPE_STR) { value_free(&p); runtime_error(it, line, "myon.ffi.load expects a str path"); }
        if (!ffi_platform_supported()) {
            value_free(&p);
            *out = make_result_pair(value_int(-1),
                value_error(myon_strdup("FFI is not supported on this platform yet (Phase3 stub)")));
            return 1;
        }
        const char *path = p.as.obj->as.str;
        char *err = NULL;
        long long h = ffi_load(ffi_get_state(it), path, &err);
        if (h < 0) {
            char buf[512];
            snprintf(buf, sizeof(buf), "ffi.load: %s", err ? err : "load failed");
            free(err);
            value_free(&p);
            *out = make_result_pair(value_int(-1), value_error(myon_strdup(buf)));
            return 1;
        }
        value_free(&p);
        *out = make_result_pair(value_int(h), value_nil());
        return 1;
    }

    /* myon.ffi.close(handle) ret bool, error */
    if (strcmp(name, "myon.ffi.close") == 0) {
        Value hv = eval_arg(it, env, call, 0);
        if (hv.type != TYPE_INT) { value_free(&hv); runtime_error(it, line, "myon.ffi.close expects an int handle"); }
        int ok = ffi_close(ffi_get_state(it), hv.as.i);
        value_free(&hv);
        if (!ok) {
            *out = make_result_pair(value_bool(0),
                value_error(myon_strdup("ffi.close: invalid or already-closed handle")));
        } else {
            *out = make_result_pair(value_bool(1), value_nil());
        }
        return 1;
    }

    if (strcmp(name, "myon.ffi.call_i") == 0) { *out = ffi_do_call(it, env, call, FFI_RET_I64);  return 1; }
    if (strcmp(name, "myon.ffi.call_d") == 0) { *out = ffi_do_call(it, env, call, FFI_RET_F64);  return 1; }
    if (strcmp(name, "myon.ffi.call_p") == 0) { *out = ffi_do_call(it, env, call, FFI_RET_PTR);  return 1; }
    if (strcmp(name, "myon.ffi.call_v") == 0) { *out = ffi_do_call(it, env, call, FFI_RET_VOID); return 1; }

    /* myon.ffi.alloc(size) ret int, error  (Phase3.1) */
    if (strcmp(name, "myon.ffi.alloc") == 0) {
        Value sv = eval_arg(it, env, call, 0);
        if (sv.type != TYPE_INT) { value_free(&sv); runtime_error(it, line, "myon.ffi.alloc expects an int size"); }
        long long size = sv.as.i;
        value_free(&sv);
        if (size <= 0) {
            *out = make_result_pair(value_int(-1),
                value_error(myon_strdup("ffi.alloc: size must be positive")));
            return 1;
        }
        long long id = ffi_mem_alloc(ffi_get_state(it), size);
        if (id < 0) {
            *out = make_result_pair(value_int(-1),
                value_error(myon_strdup("ffi.alloc: out of memory")));
        } else {
            *out = make_result_pair(value_int(id), value_nil());
        }
        return 1;
    }

    /* myon.ffi.free(block_id) ret bool, error  (Phase3.1) */
    if (strcmp(name, "myon.ffi.free") == 0) {
        Value bv = eval_arg(it, env, call, 0);
        if (bv.type != TYPE_INT) { value_free(&bv); runtime_error(it, line, "myon.ffi.free expects an int block id"); }
        int ok = ffi_mem_free(ffi_get_state(it), bv.as.i);
        value_free(&bv);
        if (!ok) {
            *out = make_result_pair(value_bool(0),
                value_error(myon_strdup("ffi.free: invalid or already-freed block id")));
        } else {
            *out = make_result_pair(value_bool(1), value_nil());
        }
        return 1;
    }

    /* myon.ffi.read_cstr(addr, max_len) ret str, error  (Phase3.1) */
    if (strcmp(name, "myon.ffi.read_cstr") == 0) {
        Value av = eval_arg(it, env, call, 0);
        Value mv = eval_arg(it, env, call, 1);
        if (av.type != TYPE_INT || mv.type != TYPE_INT) {
            value_free(&av); value_free(&mv);
            *out = make_result_pair(value_str(myon_strdup("")),
                value_error(myon_strdup("ffi.read_cstr expects (int addr, int max_len)")));
            return 1;
        }
        long long addr = av.as.i;
        long long max_len = mv.as.i;
        value_free(&av); value_free(&mv);

        /* upper bound on max_len to avoid runaway reads (16 MB) */
        const long long FFI_READ_CSTR_LIMIT = 16LL * 1024 * 1024;
        if (max_len <= 0 || max_len > FFI_READ_CSTR_LIMIT) {
            *out = make_result_pair(value_str(myon_strdup("")),
                value_error(myon_strdup("ffi.read_cstr: max_len must be in 1..16777216")));
            return 1;
        }
        if (addr == 0) {
            *out = make_result_pair(value_str(myon_strdup("")),
                value_error(myon_strdup("ffi.read_cstr: NULL address")));
            return 1;
        }
        char *s = ffi_read_cstring(addr, max_len);
        if (!s) {
            *out = make_result_pair(value_str(myon_strdup("")),
                value_error(myon_strdup("ffi.read_cstr: could not read string")));
        } else {
            *out = make_result_pair(value_str(s), value_nil());
        }
        return 1;
    }

    /* myon.ffi.write_bytes(block_id, offset, data) ret bool, error  (Phase3.1)
     *
     * Writes the bytes of the Myon str `data` into a previously allocated
     * memory block.  Myon's str is a NUL-terminated char* (no length field),
     * so the write covers strlen(data) bytes — embedded NUL bytes truncate.
     * This limitation is documented in docs/myon_spec.md (10.3.1). */
    if (strcmp(name, "myon.ffi.write_bytes") == 0) {
        Value bv = eval_arg(it, env, call, 0);
        Value ov = eval_arg(it, env, call, 1);
        Value dv = eval_arg(it, env, call, 2);
        if (bv.type != TYPE_INT || ov.type != TYPE_INT || dv.type != TYPE_STR) {
            value_free(&bv); value_free(&ov); value_free(&dv);
            *out = make_result_pair(value_bool(0),
                value_error(myon_strdup(
                    "ffi.write_bytes expects (int block_id, int offset, str data)")));
            return 1;
        }
        long long block_id = bv.as.i;
        long long offset   = ov.as.i;
        const char *data   = dv.as.obj->as.str;
        long long len      = (long long)strlen(data);
        value_free(&bv); value_free(&ov);
        int ok = ffi_mem_write(ffi_get_state(it), block_id, offset,
                               (const unsigned char *)data, len);
        value_free(&dv);
        if (!ok) {
            *out = make_result_pair(value_bool(0),
                value_error(myon_strdup(
                    "ffi.write_bytes: invalid block id or out-of-range write")));
        } else {
            *out = make_result_pair(value_bool(1), value_nil());
        }
        return 1;
    }

    /* myon.ffi.read_bytes(block_id, offset, len) ret str, error  (Phase3.1)
     *
     * Reads `len` bytes from a block into a Myon str.  Because str is
     * NUL-terminated, the result is best-effort for binary data (an embedded
     * NUL byte truncates the visible string). */
    if (strcmp(name, "myon.ffi.read_bytes") == 0) {
        Value bv = eval_arg(it, env, call, 0);
        Value ov = eval_arg(it, env, call, 1);
        Value lv = eval_arg(it, env, call, 2);
        if (bv.type != TYPE_INT || ov.type != TYPE_INT || lv.type != TYPE_INT) {
            value_free(&bv); value_free(&ov); value_free(&lv);
            *out = make_result_pair(value_str(myon_strdup("")),
                value_error(myon_strdup(
                    "ffi.read_bytes expects (int block_id, int offset, int len)")));
            return 1;
        }
        long long block_id = bv.as.i;
        long long offset   = ov.as.i;
        long long len      = lv.as.i;
        value_free(&bv); value_free(&ov); value_free(&lv);
        if (len < 0) {
            *out = make_result_pair(value_str(myon_strdup("")),
                value_error(myon_strdup("ffi.read_bytes: len must be non-negative")));
            return 1;
        }
        unsigned char *buf = ffi_mem_read(ffi_get_state(it), block_id, offset, len);
        if (!buf) {
            *out = make_result_pair(value_str(myon_strdup("")),
                value_error(myon_strdup(
                    "ffi.read_bytes: invalid block id or out-of-range read")));
        } else {
            /* buf is NUL-terminated by ffi_mem_read; hand it to value_str. */
            *out = make_result_pair(value_str((char *)buf), value_nil());
        }
        return 1;
    }

    /* myon.ffi.read_i64(block_id, offset) ret int, error  (Phase3.1)
     *
     * Reads 8 bytes as a little-endian int64.  Convenience for pulling an
     * out-parameter pointer (e.g. sqlite3* from sqlite3_open) out of a block
     * as a plain address value. */
    /* myon.ffi.read_i32(block_id, offset) ret int, error  (Phase5.1, Step7)
     *
     * Reads 4 bytes as a little-endian int32, sign-extended to int.  The
     * read-side counterpart of ffi.write_i32; used to pull 4-byte out-parameter
     * fields (e.g. the `Uint32 type` at offset 0, or the `sym` keycode at
     * offset 20, of an SDL_Event) back out of a block. */
    if (strcmp(name, "myon.ffi.read_i64") == 0 ||
        strcmp(name, "myon.ffi.read_i32") == 0) {
        int is64 = (strstr(name, "64") != NULL); /* "read_i64" vs "read_i32" */
        Value bv = eval_arg(it, env, call, 0);
        Value ov = eval_arg(it, env, call, 1);
        if (bv.type != TYPE_INT || ov.type != TYPE_INT) {
            value_free(&bv); value_free(&ov);
            *out = make_result_pair(value_int(0),
                value_error(myon_strdup(
                    is64 ? "ffi.read_i64 expects (int block_id, int offset)"
                         : "ffi.read_i32 expects (int block_id, int offset)")));
            return 1;
        }
        long long block_id = bv.as.i;
        long long offset   = ov.as.i;
        value_free(&bv); value_free(&ov);
        long long v = 0;
        int ok = is64 ? ffi_mem_read_i64(ffi_get_state(it), block_id, offset, &v)
                      : ffi_mem_read_i32(ffi_get_state(it), block_id, offset, &v);
        if (!ok) {
            *out = make_result_pair(value_int(0),
                value_error(myon_strdup(
                    is64 ? "ffi.read_i64: invalid block id or out-of-range read"
                         : "ffi.read_i32: invalid block id or out-of-range read")));
        } else {
            *out = make_result_pair(value_int(v), value_nil());
        }
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* Phase4.1, Step1: typed memory writes                           */
    /* -------------------------------------------------------------- */
    /* Shared helper macro would obscure the (value, error) style, so each
     * variant is spelled out.  int variants require an int `v`, float
     * variants accept float or int (widened to double). */
    if (strcmp(name, "myon.ffi.write_i64") == 0 ||
        strcmp(name, "myon.ffi.write_i32") == 0) {
        int is64 = (strstr(name, "64") != NULL); /* "write_i64" vs "write_i32" */
        Value bv = eval_arg(it, env, call, 0);
        Value ov = eval_arg(it, env, call, 1);
        Value vv = eval_arg(it, env, call, 2);
        if (bv.type != TYPE_INT || ov.type != TYPE_INT || vv.type != TYPE_INT) {
            value_free(&bv); value_free(&ov); value_free(&vv);
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                is64 ? "ffi.write_i64 expects (int block_id, int offset, int v)"
                     : "ffi.write_i32 expects (int block_id, int offset, int v)")));
            return 1;
        }
        long long b = bv.as.i, o = ov.as.i, v = vv.as.i;
        value_free(&bv); value_free(&ov); value_free(&vv);
        int ok = is64 ? ffi_mem_write_i64(ffi_get_state(it), b, o, v)
                      : ffi_mem_write_i32(ffi_get_state(it), b, o, v);
        if (!ok)
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                "ffi.write_i*: invalid block id or out-of-range write")));
        else
            *out = make_result_pair(value_bool(1), value_nil());
        return 1;
    }

    if (strcmp(name, "myon.ffi.write_f64") == 0 ||
        strcmp(name, "myon.ffi.write_f32") == 0) {
        int is64 = (strstr(name, "64") != NULL); /* "write_f64" vs "write_f32" */
        Value bv = eval_arg(it, env, call, 0);
        Value ov = eval_arg(it, env, call, 1);
        Value vv = eval_arg(it, env, call, 2);
        int nums_ok = (bv.type == TYPE_INT && ov.type == TYPE_INT &&
                       (vv.type == TYPE_FLOAT || vv.type == TYPE_INT));
        if (!nums_ok) {
            value_free(&bv); value_free(&ov); value_free(&vv);
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                is64 ? "ffi.write_f64 expects (int block_id, int offset, float v)"
                     : "ffi.write_f32 expects (int block_id, int offset, float v)")));
            return 1;
        }
        long long b = bv.as.i, o = ov.as.i;
        double v = (vv.type == TYPE_FLOAT) ? vv.as.f : (double)vv.as.i;
        value_free(&bv); value_free(&ov); value_free(&vv);
        int ok = is64 ? ffi_mem_write_f64(ffi_get_state(it), b, o, v)
                      : ffi_mem_write_f32(ffi_get_state(it), b, o, v);
        if (!ok)
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                "ffi.write_f*: invalid block id or out-of-range write")));
        else
            *out = make_result_pair(value_bool(1), value_nil());
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* Phase4.1, Step2: struct layout DSL                             */
    /* -------------------------------------------------------------- */

    /* myon.ffi.struct_def(name, field_kinds) ret int, error */
    if (strcmp(name, "myon.ffi.struct_def") == 0) {
        Value nv = eval_arg(it, env, call, 0);
        Value fv = eval_arg(it, env, call, 1);
        if (nv.type != TYPE_STR || fv.type != TYPE_ARRAY) {
            value_free(&nv); value_free(&fv);
            *out = make_result_pair(value_int(-1), value_error(myon_strdup(
                "ffi.struct_def expects (str name, array of str field_kinds)")));
            return 1;
        }
        ArrayData *a = &fv.as.obj->as.arr;
        if (a->count <= 0) {
            value_free(&nv); value_free(&fv);
            *out = make_result_pair(value_int(-1), value_error(myon_strdup(
                "ffi.struct_def: field_kinds must be non-empty")));
            return 1;
        }
        FFIFieldKind *kinds =
            (FFIFieldKind *)myon_xmalloc(sizeof(FFIFieldKind) * (size_t)a->count);
        int bad = 0;
        for (int i = 0; i < a->count; i++) {
            Value *e = &a->items[i];
            if (e->type != TYPE_STR) { bad = 1; break; }
            const char *s = e->as.obj->as.str;
            if      (strcmp(s, "i32") == 0) kinds[i] = FFI_FIELD_I32;
            else if (strcmp(s, "i64") == 0) kinds[i] = FFI_FIELD_I64;
            else if (strcmp(s, "f32") == 0) kinds[i] = FFI_FIELD_F32;
            else if (strcmp(s, "f64") == 0) kinds[i] = FFI_FIELD_F64;
            else { bad = 1; break; }
        }
        if (bad) {
            free(kinds);
            value_free(&nv); value_free(&fv);
            *out = make_result_pair(value_int(-1), value_error(myon_strdup(
                "ffi.struct_def: field kinds must each be one of "
                "\"i32\",\"i64\",\"f32\",\"f64\"")));
            return 1;
        }
        long long sz = ffi_struct_define(ffi_get_state(it),
                                         nv.as.obj->as.str, kinds, a->count);
        free(kinds);
        value_free(&nv); value_free(&fv);
        if (sz < 0)
            *out = make_result_pair(value_int(-1), value_error(myon_strdup(
                "ffi.struct_def: invalid struct definition")));
        else
            *out = make_result_pair(value_int(sz), value_nil());
        return 1;
    }

    /* myon.ffi.struct_alloc(name) ret int, error */
    if (strcmp(name, "myon.ffi.struct_alloc") == 0) {
        Value nv = eval_arg(it, env, call, 0);
        if (nv.type != TYPE_STR) {
            value_free(&nv);
            *out = make_result_pair(value_int(-1), value_error(myon_strdup(
                "ffi.struct_alloc expects (str name)")));
            return 1;
        }
        long long sz = ffi_struct_size(ffi_get_state(it), nv.as.obj->as.str);
        value_free(&nv);
        if (sz < 0) {
            *out = make_result_pair(value_int(-1), value_error(myon_strdup(
                "ffi.struct_alloc: undefined struct name")));
            return 1;
        }
        long long id = ffi_mem_alloc(ffi_get_state(it), sz);
        if (id < 0)
            *out = make_result_pair(value_int(-1), value_error(myon_strdup(
                "ffi.struct_alloc: out of memory")));
        else
            *out = make_result_pair(value_int(id), value_nil());
        return 1;
    }

    /* myon.ffi.struct_write(block_id, name, field_index, v) ret bool, error */
    if (strcmp(name, "myon.ffi.struct_write") == 0) {
        Value bv = eval_arg(it, env, call, 0);
        Value nv = eval_arg(it, env, call, 1);
        Value iv = eval_arg(it, env, call, 2);
        Value vv = eval_arg(it, env, call, 3);
        if (bv.type != TYPE_INT || nv.type != TYPE_STR || iv.type != TYPE_INT) {
            value_free(&bv); value_free(&nv); value_free(&iv); value_free(&vv);
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                "ffi.struct_write expects (int block_id, str name, int field_index, v)")));
            return 1;
        }
        FFIState *st = ffi_get_state(it);
        long long b = bv.as.i;
        int idx = (int)iv.as.i;
        FFIFieldKind kind;
        if (!ffi_struct_field_kind(st, nv.as.obj->as.str, idx, &kind)) {
            value_free(&bv); value_free(&nv); value_free(&iv); value_free(&vv);
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                "ffi.struct_write: undefined struct name or field index out of range")));
            return 1;
        }
        long long off = ffi_struct_field_offset(st, nv.as.obj->as.str, idx);
        value_free(&nv); value_free(&iv); value_free(&bv);

        int is_float_field = (kind == FFI_FIELD_F32 || kind == FFI_FIELD_F64);
        int ok = 0;
        if (is_float_field) {
            if (vv.type != TYPE_FLOAT && vv.type != TYPE_INT) {
                value_free(&vv);
                *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                    "ffi.struct_write: float field expects a float value")));
                return 1;
            }
            double v = (vv.type == TYPE_FLOAT) ? vv.as.f : (double)vv.as.i;
            ok = (kind == FFI_FIELD_F32) ? ffi_mem_write_f32(st, b, off, v)
                                         : ffi_mem_write_f64(st, b, off, v);
        } else {
            if (vv.type != TYPE_INT) {
                value_free(&vv);
                *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                    "ffi.struct_write: int field expects an int value")));
                return 1;
            }
            long long v = vv.as.i;
            ok = (kind == FFI_FIELD_I32) ? ffi_mem_write_i32(st, b, off, v)
                                         : ffi_mem_write_i64(st, b, off, v);
        }
        value_free(&vv);
        if (!ok)
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                "ffi.struct_write: invalid block id or out-of-range write")));
        else
            *out = make_result_pair(value_bool(1), value_nil());
        return 1;
    }

    /* myon.ffi.struct_read(block_id, name, field_index)
     *   ret int|float, error
     * The returned value's type is chosen at run time from the field kind
     * (int for i32/i64, float for f32/f64).  Myon's tuple return is untyped
     * (make_result_pair packs a plain array), so a run-time-varying scalar
     * type unpacks naturally; this is documented in docs/myon_spec.md. */
    if (strcmp(name, "myon.ffi.struct_read") == 0) {
        Value bv = eval_arg(it, env, call, 0);
        Value nv = eval_arg(it, env, call, 1);
        Value iv = eval_arg(it, env, call, 2);
        if (bv.type != TYPE_INT || nv.type != TYPE_STR || iv.type != TYPE_INT) {
            value_free(&bv); value_free(&nv); value_free(&iv);
            *out = make_result_pair(value_int(0), value_error(myon_strdup(
                "ffi.struct_read expects (int block_id, str name, int field_index)")));
            return 1;
        }
        FFIState *st = ffi_get_state(it);
        long long b = bv.as.i;
        int idx = (int)iv.as.i;
        FFIFieldKind kind;
        if (!ffi_struct_field_kind(st, nv.as.obj->as.str, idx, &kind)) {
            value_free(&bv); value_free(&nv); value_free(&iv);
            *out = make_result_pair(value_int(0), value_error(myon_strdup(
                "ffi.struct_read: undefined struct name or field index out of range")));
            return 1;
        }
        long long off = ffi_struct_field_offset(st, nv.as.obj->as.str, idx);
        value_free(&bv); value_free(&nv); value_free(&iv);

        if (kind == FFI_FIELD_F32 || kind == FFI_FIELD_F64) {
            double vals[1];
            int ok = (kind == FFI_FIELD_F32)
                        ? ffi_mem_read_array_f32(st, b, off, vals, 1)
                        : ffi_mem_read_array_f64(st, b, off, vals, 1);
            if (!ok)
                *out = make_result_pair(value_float(0.0), value_error(myon_strdup(
                    "ffi.struct_read: invalid block id or out-of-range read")));
            else
                *out = make_result_pair(value_float(vals[0]), value_nil());
        } else {
            long long vals[1];
            int ok = (kind == FFI_FIELD_I32)
                        ? ffi_mem_read_array_i32(st, b, off, vals, 1)
                        : ffi_mem_read_array_i64(st, b, off, vals, 1);
            if (!ok)
                *out = make_result_pair(value_int(0), value_error(myon_strdup(
                    "ffi.struct_read: invalid block id or out-of-range read")));
            else
                *out = make_result_pair(value_int(vals[0]), value_nil());
        }
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* Phase4.1, Step3: bulk array read/write                         */
    /* -------------------------------------------------------------- */
    if (strncmp(name, "myon.ffi.write_array_", 21) == 0) {
        const char *tag = name + 21; /* "i32"/"i64"/"f32"/"f64" */
        int is_float = (tag[0] == 'f');
        int elem_size = (tag[1] == '6') ? 8 : 4;
        Value bv = eval_arg(it, env, call, 0);
        Value ov = eval_arg(it, env, call, 1);
        Value av = eval_arg(it, env, call, 2);
        if (bv.type != TYPE_INT || ov.type != TYPE_INT || av.type != TYPE_ARRAY) {
            value_free(&bv); value_free(&ov); value_free(&av);
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                "ffi.write_array_* expects (int block_id, int offset, array)")));
            return 1;
        }
        long long b = bv.as.i, o = ov.as.i;
        ArrayData *a = &av.as.obj->as.arr;
        long long count = a->count;
        value_free(&bv); value_free(&ov);

        FFIState *st = ffi_get_state(it);
        int ok = 1;
        if (is_float) {
            double *vals = count ? (double *)myon_xmalloc(sizeof(double) * (size_t)count) : NULL;
            for (long long i = 0; i < count && ok; i++) {
                Value *e = &a->items[i];
                if (e->type == TYPE_FLOAT) vals[i] = e->as.f;
                else if (e->type == TYPE_INT) vals[i] = (double)e->as.i;
                else ok = 0;
            }
            if (ok)
                ok = (elem_size == 4)
                        ? ffi_mem_write_array_f32(st, b, o, vals, count)
                        : ffi_mem_write_array_f64(st, b, o, vals, count);
            free(vals);
            if (!ok) {
                value_free(&av);
                *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                    "ffi.write_array_f*: type mismatch, invalid block, or out-of-range")));
                return 1;
            }
        } else {
            long long *vals = count ? (long long *)myon_xmalloc(sizeof(long long) * (size_t)count) : NULL;
            for (long long i = 0; i < count && ok; i++) {
                Value *e = &a->items[i];
                if (e->type == TYPE_INT) vals[i] = e->as.i;
                else if (e->type == TYPE_BOOL) vals[i] = e->as.b;
                else ok = 0;
            }
            if (ok)
                ok = (elem_size == 4)
                        ? ffi_mem_write_array_i32(st, b, o, vals, count)
                        : ffi_mem_write_array_i64(st, b, o, vals, count);
            free(vals);
            if (!ok) {
                value_free(&av);
                *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                    "ffi.write_array_i*: type mismatch, invalid block, or out-of-range")));
                return 1;
            }
        }
        value_free(&av);
        *out = make_result_pair(value_bool(1), value_nil());
        return 1;
    }

    if (strncmp(name, "myon.ffi.read_array_", 20) == 0) {
        const char *tag = name + 20; /* "i32"/"i64"/"f32"/"f64" */
        int is_float = (tag[0] == 'f');
        int elem_size = (tag[1] == '6') ? 8 : 4;
        Value bv = eval_arg(it, env, call, 0);
        Value ov = eval_arg(it, env, call, 1);
        Value cv = eval_arg(it, env, call, 2);
        if (bv.type != TYPE_INT || ov.type != TYPE_INT || cv.type != TYPE_INT) {
            value_free(&bv); value_free(&ov); value_free(&cv);
            *out = make_result_pair(value_array(NULL), value_error(myon_strdup(
                "ffi.read_array_* expects (int block_id, int offset, int count)")));
            return 1;
        }
        long long b = bv.as.i, o = ov.as.i, count = cv.as.i;
        value_free(&bv); value_free(&ov); value_free(&cv);
        if (count < 0) {
            *out = make_result_pair(value_array(NULL), value_error(myon_strdup(
                "ffi.read_array_*: count must be non-negative")));
            return 1;
        }
        FFIState *st = ffi_get_state(it);
        if (is_float) {
            double *vals = count ? (double *)myon_xmalloc(sizeof(double) * (size_t)count) : NULL;
            int ok = (elem_size == 4)
                        ? ffi_mem_read_array_f32(st, b, o, vals, count)
                        : ffi_mem_read_array_f64(st, b, o, vals, count);
            if (!ok) {
                free(vals);
                *out = make_result_pair(value_array(NULL), value_error(myon_strdup(
                    "ffi.read_array_f*: invalid block id or out-of-range read")));
                return 1;
            }
            Value res = value_array(typespec_prim(TYPE_FLOAT));
            for (long long i = 0; i < count; i++)
                array_push(&res, value_float(vals[i]));
            free(vals);
            *out = make_result_pair(res, value_nil());
        } else {
            long long *vals = count ? (long long *)myon_xmalloc(sizeof(long long) * (size_t)count) : NULL;
            int ok = (elem_size == 4)
                        ? ffi_mem_read_array_i32(st, b, o, vals, count)
                        : ffi_mem_read_array_i64(st, b, o, vals, count);
            if (!ok) {
                free(vals);
                *out = make_result_pair(value_array(NULL), value_error(myon_strdup(
                    "ffi.read_array_i*: invalid block id or out-of-range read")));
                return 1;
            }
            Value res = value_array(typespec_prim(TYPE_INT));
            for (long long i = 0; i < count; i++)
                array_push(&res, value_int(vals[i]));
            free(vals);
            *out = make_result_pair(res, value_nil());
        }
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* Phase4.1, Step4: C-to-Myon callbacks                           */
    /* -------------------------------------------------------------- */

    /* myon.ffi.make_callback(fn, arg_count) ret int, error */
    if (strcmp(name, "myon.ffi.make_callback") == 0) {
        Value fnv = eval_arg(it, env, call, 0);
        Value acv = eval_arg(it, env, call, 1);
        if (fnv.type != TYPE_FUNC || acv.type != TYPE_INT) {
            value_free(&fnv); value_free(&acv);
            *out = make_result_pair(value_int(0), value_error(myon_strdup(
                "ffi.make_callback expects (func fn, int arg_count)")));
            return 1;
        }
        int arg_count = (int)acv.as.i;
        value_free(&acv);
        if (arg_count < 0 || arg_count > MYON_FFI_CB_MAX_ARGS) {
            value_free(&fnv);
            *out = make_result_pair(value_int(0), value_error(myon_strdup(
                "ffi.make_callback: arg_count must be 0..4")));
            return 1;
        }
        void *ptr = ffi_callback_register(it, fnv, arg_count);
        value_free(&fnv);
        if (!ptr)
            *out = make_result_pair(value_int(0), value_error(myon_strdup(
                "ffi.make_callback: no free callback slot (max 16)")));
        else
            *out = make_result_pair(value_int((long long)(size_t)ptr), value_nil());
        return 1;
    }

    /* myon.ffi.free_callback(ptr) ret bool, error */
    if (strcmp(name, "myon.ffi.free_callback") == 0) {
        Value pv = eval_arg(it, env, call, 0);
        if (pv.type != TYPE_INT) {
            value_free(&pv);
            *out = make_result_pair(value_bool(0), value_error(myon_strdup(
                "ffi.free_callback expects (int ptr)")));
            return 1;
        }
        void *ptr = (void *)(size_t)pv.as.i;
        value_free(&pv);
        ffi_callback_unregister(ptr);
        *out = make_result_pair(value_bool(1), value_nil());
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Standard library: myon.time.* (Phase4, Step5)                       */
/*                                                                     */
/* Minimal wall-clock helpers built on the C standard library / POSIX  */
/* clocks.  now()/now_ms() return int64 epoch seconds/milliseconds;    */
/* sleep_ms() suspends the calling thread (a negative argument is a    */
/* no-op rather than an error — a simple safe-side convention).        */
/* ------------------------------------------------------------------ */
static int call_time(Interp *it, Env *env, const char *name, Expr *call, Value *out) {
    int line = call->line;

    /* myon.time.now() ret int — UNIX epoch seconds. */
    if (strcmp(name, "myon.time.now") == 0) {
        *out = value_int((long long)time(NULL));
        return 1;
    }

    /* myon.time.now_ms() ret int — UNIX epoch milliseconds. */
    if (strcmp(name, "myon.time.now_ms") == 0) {
        *out = value_int(myon_wallclock_ms());
        return 1;
    }

    /* myon.time.sleep_ms(ms) ret void — sleep, negative ms is a no-op. */
    if (strcmp(name, "myon.time.sleep_ms") == 0) {
        Value a = eval_arg(it, env, call, 0);
        if (a.type != TYPE_INT) {
            value_free(&a);
            runtime_error(it, line, "myon.time.sleep_ms expects (int)");
        }
        long long ms = a.as.i;
        value_free(&a);
        if (ms > 0) {
            /* Phase5: inside a coroutine driven by the event loop, yield to
             * other tasks instead of blocking the whole process.  Outside any
             * coroutine (plain synchronous scripts) fall back to the classic
             * blocking nanosleep so pre-Phase5 semantics are preserved. */
            if (it->loop && event_loop_current(it->loop)) {
                event_loop_sleep_ms(it->loop, ms);
            } else {
                /* Synchronous (non-coroutine) fallback: block the process.
                 * Windows has neither nanosleep nor CLOCK_MONOTONIC; its native
                 * millisecond blocking primitive is Sleep() (Win32). */
#if defined(_WIN32)
                Sleep((unsigned long)ms);
#else
                struct timespec req;
                req.tv_sec  = (time_t)(ms / 1000);
                req.tv_nsec = (long)((ms % 1000) * 1000000L);
                nanosleep(&req, NULL);
#endif
            }
        }
        *out = value_void();
        return 1;
    }

    /* myon.time.frame_start() ret int — Phase5.1 Step10 game-loop helper.
     * Semantic alias of now_ms(): records the wall-clock start of a frame
     * in milliseconds so that frame_wait() can pace the loop to a target
     * FPS.  Kept as a distinct name purely to make game loops read clearly. */
    if (strcmp(name, "myon.time.frame_start") == 0) {
        *out = value_int(myon_wallclock_ms());
        return 1;
    }

    /* myon.time.frame_wait(frame_start_ms, target_fps) ret int
     *   Phase5.1 Step10 game-loop helper.  Given the timestamp recorded by
     *   frame_start() and a target FPS, sleep just long enough for the frame
     *   to occupy 1000/target_fps milliseconds, then return the total elapsed
     *   time (dt) for the frame in milliseconds (sleep included).  A
     *   target_fps of 0 or below disables the cap: no sleep, elapsed time is
     *   returned as-is.  Uses the same sleep path as sleep_ms so it yields to
     *   the event loop when running inside a coroutine. */
    if (strcmp(name, "myon.time.frame_wait") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        if (a.type != TYPE_INT || b.type != TYPE_INT) {
            value_free(&a); value_free(&b);
            runtime_error(it, line, "myon.time.frame_wait expects (int, int)");
        }
        long long frame_start_ms = a.as.i;
        long long target_fps = b.as.i;
        value_free(&a); value_free(&b);

        long long now_ms = myon_wallclock_ms();
        long long elapsed = now_ms - frame_start_ms;
        if (elapsed < 0) elapsed = 0; /* guard against clock going backwards */

        if (target_fps > 0) {
            long long target_ms = 1000 / target_fps;
            long long remaining = target_ms - elapsed;
            if (remaining > 0) {
                if (it->loop && event_loop_current(it->loop)) {
                    event_loop_sleep_ms(it->loop, remaining);
                } else {
#if defined(_WIN32)
                    Sleep((unsigned long)remaining);
#else
                    struct timespec req;
                    req.tv_sec  = (time_t)(remaining / 1000);
                    req.tv_nsec = (long)((remaining % 1000) * 1000000L);
                    nanosleep(&req, NULL);
#endif
                }
                elapsed = target_ms;
            }
        }
        *out = value_int(elapsed);
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Standard library: myon.random.* (Phase4, Step6)                     */
/*                                                                     */
/* Thin wrapper over the C standard PRNG (srand/rand).  NOT crypto-safe.*/
/* The PRNG is auto-seeded from the wall clock on first use if the      */
/* script never calls myon.random.seed() explicitly.                   */
/* ------------------------------------------------------------------ */
static void random_ensure_seeded(Interp *it) {
    if (!it->random_seeded) {
        srand((unsigned)time(NULL));
        it->random_seeded = 1;
    }
}

/* Fill `buf` with `n` cryptographically-secure random bytes from the OS CSPRNG.
 * Returns 0 on success, -1 on failure.  This backs myon.random.secure_int,
 * which (unlike the srand/rand-based helpers above) is safe for tokens/keys
 * (addresses the historical known-issue "myon.random is not cryptographically safe").
 * macOS/BSD: arc4random_buf() (the OS CSPRNG; cannot fail, no fd needed).
 * Linux and other POSIX targets: read from /dev/urandom (portable across
 * kernels without pulling in <sys/random.h>/getrandom feature-test macros).
 * Platforms with no OS CSPRNG report failure so callers surface a clear error
 * rather than a weak value.  See platform.h (MYON_HAVE_ARC4RANDOM /
 * MYON_HAVE_DEV_URANDOM). */
static int random_secure_bytes(unsigned char *buf, size_t n) {
#if defined(MYON_HAVE_ARC4RANDOM)
    /* macOS/BSD: arc4random_buf() is the OS CSPRNG.  It cannot fail and needs
     * no file descriptor, so it is preferred over /dev/urandom there. */
    arc4random_buf(buf, n);
    return 0;
#elif defined(MYON_HAVE_DEV_URANDOM)
    /* Linux (and any other POSIX target without arc4random): read the OS
     * CSPRNG from /dev/urandom.  Portable across kernels without pulling in
     * <sys/random.h>/getrandom feature-test macros. */
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return (got == n) ? 0 : -1;
#else
    /* No OS CSPRNG on this platform: report failure so callers surface a clear
     * error rather than handing back a weak (srand/rand) value. */
    (void)buf; (void)n;
    return -1;
#endif
}

static int call_random(Interp *it, Env *env, const char *name, Expr *call, Value *out) {
    int line = call->line;

    /* myon.random.seed(n) ret void */
    if (strcmp(name, "myon.random.seed") == 0) {
        Value a = eval_arg(it, env, call, 0);
        if (a.type != TYPE_INT) {
            value_free(&a);
            runtime_error(it, line, "myon.random.seed expects (int)");
        }
        srand((unsigned)a.as.i);
        it->random_seeded = 1;
        value_free(&a);
        *out = value_void();
        return 1;
    }

    /* myon.random.int(lo, hi) ret int, error — inclusive on both ends. */
    if (strcmp(name, "myon.random.int") == 0) {
        Value lo = eval_arg(it, env, call, 0), hi = eval_arg(it, env, call, 1);
        if (lo.type != TYPE_INT || hi.type != TYPE_INT) {
            value_free(&lo); value_free(&hi);
            runtime_error(it, line, "myon.random.int expects (int, int)");
        }
        long long lov = lo.as.i, hiv = hi.as.i;
        value_free(&lo); value_free(&hi);
        if (lov > hiv) {
            *out = make_result_pair(value_nil(),
                value_error(myon_strdup("myon.random.int: lo must be <= hi")));
            return 1;
        }
        random_ensure_seeded(it);
        unsigned long long span = (unsigned long long)(hiv - lov) + 1ULL;
        long long v = lov + (long long)((unsigned long long)rand() % span);
        *out = make_result_pair(value_int(v), value_nil());
        return 1;
    }

    /* myon.random.float() ret float — 0.0 <= x < 1.0 */
    if (strcmp(name, "myon.random.float") == 0) {
        random_ensure_seeded(it);
        double v = (double)rand() / ((double)RAND_MAX + 1.0);
        *out = value_float(v);
        return 1;
    }

    /* myon.random.secure_int(lo, hi) ret int, error — inclusive on both ends.
     * Crypto-safe (OS CSPRNG) counterpart to myon.random.int; uses unbiased
     * rejection sampling so every value in [lo, hi] is equiprobable.  Suitable
     * for tokens/keys/nonces (OS CSPRNG). */
    if (strcmp(name, "myon.random.secure_int") == 0) {
        Value lo = eval_arg(it, env, call, 0), hi = eval_arg(it, env, call, 1);
        if (lo.type != TYPE_INT || hi.type != TYPE_INT) {
            value_free(&lo); value_free(&hi);
            runtime_error(it, line, "myon.random.secure_int expects (int, int)");
        }
        long long lov = lo.as.i, hiv = hi.as.i;
        value_free(&lo); value_free(&hi);
        if (lov > hiv) {
            *out = make_result_pair(value_nil(),
                value_error(myon_strdup("myon.random.secure_int: lo must be <= hi")));
            return 1;
        }
        unsigned long long span = (unsigned long long)(hiv - lov) + 1ULL;
        unsigned long long v;
        if (span == 0ULL) {
            /* Full 64-bit range (lo=INT64_MIN, hi=INT64_MAX): any draw is fine. */
            if (random_secure_bytes((unsigned char *)&v, sizeof(v)) != 0) {
                *out = make_result_pair(value_nil(),
                    value_error(myon_strdup("myon.random.secure_int: OS CSPRNG unavailable")));
                return 1;
            }
        } else {
            /* Reject the top partial bucket to remove modulo bias. */
            unsigned long long limit = (~0ULL) - ((~0ULL) % span);
            do {
                if (random_secure_bytes((unsigned char *)&v, sizeof(v)) != 0) {
                    *out = make_result_pair(value_nil(),
                        value_error(myon_strdup("myon.random.secure_int: OS CSPRNG unavailable")));
                    return 1;
                }
            } while (v >= limit);
            v %= span;
        }
        *out = make_result_pair(value_int(lov + (long long)v), value_nil());
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Standard library: myon.net.* (Phase5)                               */
/*                                                                     */
/* Non-blocking sockets (src/net.c).  When called from inside an        */
/* event-loop coroutine we yield to other tasks on EWOULDBLOCK; outside */
/* one we block on a single fd via the loop's wait helpers driven       */
/* directly (a synchronous fallback for plain scripts).                 */
/* ------------------------------------------------------------------ */

static NetState *ensure_net(Interp *it) {
    if (!it->net) it->net = net_state_create();
    return it->net;
}

/* Wait until `fd` is readable/writable.  Inside a coroutine this suspends
 * cooperatively; outside one it drives the loop / blocks on the single fd. */
static void net_wait_fd(Interp *it, myon_fd_t fd, int for_write) {
    if (it->loop && event_loop_current(it->loop)) {
        if (for_write) event_loop_wait_writable(it->loop, fd);
        else           event_loop_wait_readable(it->loop, fd);
        return;
    }
    /* synchronous fallback: block on just this fd */
#if defined(MYON_OS_POSIX)
    /* fd width: fd is myon_fd_t; a POSIX fd fits back into int for
     * FD_SET/select() (widening was lossless, so this narrowing is safe). */
    int ifd = (int)fd;
    fd_set fds; FD_ZERO(&fds); FD_SET(ifd, &fds);
    if (for_write) select(ifd + 1, NULL, &fds, NULL, NULL);
    else           select(ifd + 1, &fds, NULL, NULL, NULL);
#elif defined(_WIN32)
    /* Delegate to net.c, which owns the Winsock select()/fd_set surface (see
     * the header-collision note at the top of this file). */
    net_sync_wait_fd(fd, for_write);
#else
    (void)fd; (void)for_write;
#endif
}

static Value err_pair_str(const char *msg) {
    return make_result_pair(value_str(myon_strdup("")), value_error(myon_strdup(msg)));
}

static int call_net(Interp *it, Env *env, const char *name, Expr *call, Value *out) {
    int line = call->line;
    NetState *st = ensure_net(it);
    if (!net_supported()) {
        *out = make_result_pair(value_int(-1),
            value_error(myon_strdup("myon.net unsupported on this platform")));
        return 1;
    }

    if (strcmp(name, "myon.net.tcp_socket") == 0 ||
        strcmp(name, "myon.net.udp_socket") == 0) {
        int kind = (name[9] == 'u') ? 1 : 0;
        char *err = NULL;
        int id = net_socket_create(st, kind, &err);
        if (id < 0) { *out = make_result_pair(value_int(-1), value_error(err ? err : myon_strdup("socket failed"))); }
        else        { *out = make_result_pair(value_int(id), value_nil()); }
        return 1;
    }

    if (strcmp(name, "myon.net.bind") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1), c = eval_arg(it, env, call, 2);
        char *err = NULL;
        int rc = net_bind(st, (int)a.as.i, b.as.obj ? b.as.obj->as.str : "", (int)c.as.i, &err);
        value_free(&a); value_free(&b); value_free(&c);
        *out = make_result_pair(value_bool(rc == 0), rc == 0 ? value_nil() : value_error(err ? err : myon_strdup("bind failed")));
        return 1;
    }

    if (strcmp(name, "myon.net.listen") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        char *err = NULL;
        int rc = net_listen(st, (int)a.as.i, (int)b.as.i, &err);
        value_free(&a); value_free(&b);
        *out = make_result_pair(value_bool(rc == 0), rc == 0 ? value_nil() : value_error(err ? err : myon_strdup("listen failed")));
        return 1;
    }

    if (strcmp(name, "myon.net.local_port") == 0) {
        Value a = eval_arg(it, env, call, 0);
        char *err = NULL;
        int p = net_local_port(st, (int)a.as.i, &err);
        value_free(&a);
        if (p < 0) *out = make_result_pair(value_int(-1), value_error(err ? err : myon_strdup("local_port failed")));
        else       *out = make_result_pair(value_int(p), value_nil());
        return 1;
    }

    if (strcmp(name, "myon.net.accept") == 0) {
        Value a = eval_arg(it, env, call, 0);
        int lid = (int)a.as.i; value_free(&a);
        myon_fd_t lfd = net_raw_fd(st, lid); /* native-width fd */
        for (;;) {
            char *peer = NULL, *err = NULL;
            int cid = net_try_accept(st, lid, &peer, &err);
            if (cid == -2) { free(peer); net_wait_fd(it, lfd, 0); continue; }
            if (cid < 0)   { *out = make_result_pair(value_int(-1), value_error(err ? err : myon_strdup("accept failed"))); free(peer); return 1; }
            free(peer);
            *out = make_result_pair(value_int(cid), value_nil());
            return 1;
        }
    }

    if (strcmp(name, "myon.net.connect") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1), c = eval_arg(it, env, call, 2);
        int sid = (int)a.as.i; const char *host = b.as.obj ? b.as.obj->as.str : ""; int port = (int)c.as.i;
        myon_fd_t fd = net_raw_fd(st, sid); /* native-width fd */
        char *err = NULL;
        int rc = net_connect(st, sid, host, port, &err);
        while (rc == -2) {
            net_wait_fd(it, fd, 1);
            free(err); err = NULL;
            rc = net_connect_check(st, sid, &err);
        }
        value_free(&a); value_free(&b); value_free(&c);
        *out = make_result_pair(value_bool(rc == 0), rc == 0 ? value_nil() : value_error(err ? err : myon_strdup("connect failed")));
        return 1;
    }

    if (strcmp(name, "myon.net.send") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        int sid = (int)a.as.i; const char *data = b.as.obj ? b.as.obj->as.str : "";
        long long len = (long long)strlen(data); myon_fd_t fd = net_raw_fd(st, sid); /* native-width fd */
        char *err = NULL; long long n;
        for (;;) {
            n = net_send(st, sid, data, len, &err);
            if (n == -2) { net_wait_fd(it, fd, 1); continue; }
            break;
        }
        value_free(&a); value_free(&b);
        if (n < 0) *out = make_result_pair(value_int(-1), value_error(err ? err : myon_strdup("send failed")));
        else       *out = make_result_pair(value_int((long long)n), value_nil());
        return 1;
    }

    if (strcmp(name, "myon.net.recv") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        int sid = (int)a.as.i; long long maxlen = b.as.i; myon_fd_t fd = net_raw_fd(st, sid); /* native-width fd */
        value_free(&a); value_free(&b);
        if (maxlen <= 0) maxlen = 4096;
        char *buf = (char *)myon_xmalloc((size_t)maxlen + 1);
        char *err = NULL; long long n;
        for (;;) {
            n = net_recv(st, sid, buf, maxlen, &err);
            if (n == -2) { net_wait_fd(it, fd, 0); continue; }
            break;
        }
        if (n < 0) { free(buf); *out = err_pair_str(err ? err : "recv failed"); if (err) free(err); return 1; }
        buf[n] = '\0';
        char *dup = (char *)myon_xmalloc((size_t)n + 1);
        memcpy(dup, buf, (size_t)n); dup[n] = '\0'; free(buf);
        *out = make_result_pair(value_str(dup), value_nil());
        return 1;
    }

    if (strcmp(name, "myon.net.send_to") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1),
              c = eval_arg(it, env, call, 2), d = eval_arg(it, env, call, 3);
        int sid = (int)a.as.i; const char *data = b.as.obj ? b.as.obj->as.str : "";
        long long len = (long long)strlen(data);
        const char *host = c.as.obj ? c.as.obj->as.str : ""; int port = (int)d.as.i;
        myon_fd_t fd = net_raw_fd(st, sid); /* native-width fd */
        char *err = NULL; long long n;
        for (;;) { n = net_sendto(st, sid, data, len, host, port, &err); if (n == -2) { net_wait_fd(it, fd, 1); continue; } break; }
        value_free(&a); value_free(&b); value_free(&c); value_free(&d);
        if (n < 0) *out = make_result_pair(value_int(-1), value_error(err ? err : myon_strdup("send_to failed")));
        else       *out = make_result_pair(value_int((long long)n), value_nil());
        return 1;
    }

    if (strcmp(name, "myon.net.recv_from") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        int sid = (int)a.as.i; long long maxlen = b.as.i; myon_fd_t fd = net_raw_fd(st, sid); /* native-width fd */
        value_free(&a); value_free(&b);
        if (maxlen <= 0) maxlen = 4096;
        char *buf = (char *)myon_xmalloc((size_t)maxlen + 1);
        char *from = NULL, *err = NULL; long long n;
        for (;;) { n = net_recvfrom(st, sid, buf, maxlen, &from, &err); if (n == -2) { net_wait_fd(it, fd, 0); continue; } break; }
        Value tup = value_array(NULL);
        if (n < 0) {
            array_push(&tup, value_str(myon_strdup("")));
            array_push(&tup, value_str(myon_strdup("")));
            array_push(&tup, value_error(err ? err : myon_strdup("recv_from failed")));
        } else {
            buf[n] = '\0';
            array_push(&tup, value_str(myon_strdup(buf)));
            /* `from` is a malloc'd "host:port" from net.c; hand it to a str
             * value (which owns the heap pointer) or an empty string. */
            array_push(&tup, value_str(from ? from : myon_strdup("")));
            array_push(&tup, value_nil());
        }
        free(buf);
        *out = tup;
        (void)line;
        return 1;
    }

    if (strcmp(name, "myon.net.close") == 0) {
        Value a = eval_arg(it, env, call, 0);
        net_close(st, (int)a.as.i);
        value_free(&a);
        *out = make_result_pair(value_bool(1), value_nil());
        return 1;
    }

    return 0;
}

/* Phase5 myon.http dispatch (implemented after the async-task machinery,
 * which the server loop depends on). */
static int call_http(Interp *it, Env *env, const char *name, Expr *call, Value *out);

/* dispatch a "myon.math.<fn>" or "myon.string.<fn>" call.
 * Returns 1 and sets *out if handled. */
static int call_stdlib(Interp *it, Env *env, const char *name, Expr *call, Value *out) {
    int argc = call->as.call.arg_count;
    int line = call->line;

    /* ---- myon.file (file I/O, P4) ---- */
    if (strncmp(name, "myon.file.", 10) == 0) {
        if (call_file_io(it, env, name, call, out)) return 1;
    }

    /* ---- myon.ffi (C FFI, Phase3) ---- */
    if (strncmp(name, "myon.ffi.", 9) == 0) {
        if (call_ffi(it, env, name, call, out)) return 1;
    }

    /* ---- myon.time (Phase4, Step5) ---- */
    if (strncmp(name, "myon.time.", 10) == 0) {
        if (call_time(it, env, name, call, out)) return 1;
    }

    /* ---- myon.random (Phase4, Step6) ---- */
    if (strncmp(name, "myon.random.", 12) == 0) {
        if (call_random(it, env, name, call, out)) return 1;
    }

    /* ---- myon.net (Phase5, Step4) ---- */
    if (strncmp(name, "myon.net.", 9) == 0) {
        if (call_net(it, env, name, call, out)) return 1;
    }

    /* ---- myon.http (Phase5, Step5) ---- */
    if (strncmp(name, "myon.http.", 10) == 0) {
        if (call_http(it, env, name, call, out)) return 1;
    }

    /* ---- myon.math ---- */
    if (strcmp(name, "myon.math.sqrt") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(sqrt(as_number(it, line, a))); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.pow") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        *out = value_float(pow(as_number(it, line, a), as_number(it, line, b)));
        value_free(&a); value_free(&b); return 1;
    }
    if (strcmp(name, "myon.math.abs") == 0) {
        Value a = eval_arg(it, env, call, 0);
        if (a.type == TYPE_INT) {
            if (a.as.i == LLONG_MIN) runtime_error(it, line, "integer overflow in myon.math.abs");
            *out = value_int(a.as.i < 0 ? -a.as.i : a.as.i);
        }
        else *out = value_float(fabs(as_number(it, line, a)));
        value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.floor") == 0) {
        Value a = eval_arg(it, env, call, 0);
        /* floor(int) == int: skip the lossy double round-trip (Step1). */
        if (a.type == TYPE_INT) { *out = value_int(a.as.i); value_free(&a); return 1; }
        *out = checked_float_to_int(it, line, floor(as_number(it, line, a)), "myon.math.floor"); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.ceil") == 0) {
        Value a = eval_arg(it, env, call, 0);
        /* ceil(int) == int: skip the lossy double round-trip (Step1). */
        if (a.type == TYPE_INT) { *out = value_int(a.as.i); value_free(&a); return 1; }
        *out = checked_float_to_int(it, line, ceil(as_number(it, line, a)), "myon.math.ceil"); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.max") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        if (both_int(it, line, a, b)) {
            /* Compare in int64 precision — no double round-trip, so values
             * above 2^53 are handled exactly (Step1). */
            *out = value_int(a.as.i > b.as.i ? a.as.i : b.as.i);
        } else {
            double x = as_number(it, line, a), y = as_number(it, line, b);
            *out = value_float(x > y ? x : y);
        }
        value_free(&a); value_free(&b); return 1;
    }
    if (strcmp(name, "myon.math.min") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        if (both_int(it, line, a, b)) {
            *out = value_int(a.as.i < b.as.i ? a.as.i : b.as.i);
        } else {
            double x = as_number(it, line, a), y = as_number(it, line, b);
            *out = value_float(x < y ? x : y);
        }
        value_free(&a); value_free(&b); return 1;
    }

    /* ---- myon.math (Phase3.5 extension, Step3) ---- */

    /* Trigonometric / inverse-trig (radians, always float). */
    if (strcmp(name, "myon.math.sin") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(sin(as_number(it, line, a))); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.cos") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(cos(as_number(it, line, a))); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.tan") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(tan(as_number(it, line, a))); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.asin") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(asin(as_number(it, line, a))); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.acos") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(acos(as_number(it, line, a))); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.atan") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(atan(as_number(it, line, a))); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.atan2") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        *out = value_float(atan2(as_number(it, line, a), as_number(it, line, b)));
        value_free(&a); value_free(&b); return 1;
    }

    /* Logarithms / exponential (always float). */
    if (strcmp(name, "myon.math.log") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(log(as_number(it, line, a))); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.log2") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(log2(as_number(it, line, a))); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.log10") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(log10(as_number(it, line, a))); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.exp") == 0) {
        Value a = eval_arg(it, env, call, 0);
        *out = value_float(exp(as_number(it, line, a))); value_free(&a); return 1;
    }

    /* round / trunc: always return int; int input passes through unchanged. */
    if (strcmp(name, "myon.math.round") == 0) {
        Value a = eval_arg(it, env, call, 0);
        if (a.type == TYPE_INT) { *out = value_int(a.as.i); value_free(&a); return 1; }
        *out = checked_float_to_int(it, line, round(as_number(it, line, a)), "myon.math.round"); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.math.trunc") == 0) {
        Value a = eval_arg(it, env, call, 0);
        if (a.type == TYPE_INT) { *out = value_int(a.as.i); value_free(&a); return 1; }
        *out = checked_float_to_int(it, line, trunc(as_number(it, line, a)), "myon.math.trunc"); value_free(&a); return 1;
    }

    /* mod: int% for two ints, fmod otherwise; division by zero returns a
     * (value, error) pair rather than raising a runtime_error. */
    if (strcmp(name, "myon.math.mod") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        if (both_int(it, line, a, b)) {
            if (b.as.i == 0) {
                *out = make_result_pair(value_nil(), value_error(myon_strdup("division by zero")));
            } else {
                *out = make_result_pair(value_int(a.as.i % b.as.i), value_nil());
            }
        } else {
            double x = as_number(it, line, a), y = as_number(it, line, b);
            if (y == 0.0) {
                *out = make_result_pair(value_nil(), value_error(myon_strdup("division by zero")));
            } else {
                *out = make_result_pair(value_float(fmod(x, y)), value_nil());
            }
        }
        value_free(&a); value_free(&b); return 1;
    }

    /* sign: +1 / -1 / 0, always int; int input uses int comparison only. */
    if (strcmp(name, "myon.math.sign") == 0) {
        Value a = eval_arg(it, env, call, 0);
        long long s;
        if (a.type == TYPE_INT) {
            s = (a.as.i > 0) - (a.as.i < 0);
        } else {
            double x = as_number(it, line, a);
            s = (x > 0.0) - (x < 0.0);
        }
        *out = value_int(s); value_free(&a); return 1;
    }

    /* clamp(x, lo, hi): int math when all three are int, else float.
     * hi < lo is defined to always return hi. */
    if (strcmp(name, "myon.math.clamp") == 0) {
        Value x = eval_arg(it, env, call, 0);
        Value lo = eval_arg(it, env, call, 1);
        Value hi = eval_arg(it, env, call, 2);
        int all_int = (x.type == TYPE_INT && lo.type == TYPE_INT && hi.type == TYPE_INT);
        /* Type-check every operand even in the float path. */
        (void)as_number(it, line, x);
        (void)as_number(it, line, lo);
        (void)as_number(it, line, hi);
        if (all_int) {
            long long xv = x.as.i, lv = lo.as.i, hv = hi.as.i;
            long long r = xv < lv ? lv : (xv > hv ? hv : xv);
            if (hv < lv) r = hv;
            *out = value_int(r);
        } else {
            double xv = as_number(it, line, x);
            double lv = as_number(it, line, lo);
            double hv = as_number(it, line, hi);
            double r = xv < lv ? lv : (xv > hv ? hv : xv);
            if (hv < lv) r = hv;
            *out = value_float(r);
        }
        value_free(&x); value_free(&lo); value_free(&hi); return 1;
    }

    /* Constants exposed as 0-argument functions. */
    if (strcmp(name, "myon.math.pi") == 0) {
        *out = value_float(M_PI); return 1;
    }
    if (strcmp(name, "myon.math.e") == 0) {
        *out = value_float(M_E); return 1;
    }

    /* ---- myon.math (Phase4, Step4 — integer number theory) ---- */

    /* gcd(a, b): Euclidean algorithm on absolute values; always >= 0.
     * gcd(0, 0) is defined here as 0 (mathematically undefined, but a
     * simple edge-case convention). */
    if (strcmp(name, "myon.math.gcd") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        if (a.type != TYPE_INT || b.type != TYPE_INT) {
            value_free(&a); value_free(&b);
            runtime_error(it, line, "myon.math.gcd expects (int, int)");
        }
        /* Work in unsigned to avoid llabs(INT64_MIN) undefined behaviour. */
        unsigned long long x = (a.as.i < 0) ? -(unsigned long long)a.as.i
                                            :  (unsigned long long)a.as.i;
        unsigned long long y = (b.as.i < 0) ? -(unsigned long long)b.as.i
                                            :  (unsigned long long)b.as.i;
        while (y != 0) { unsigned long long t = x % y; x = y; y = t; }
        value_free(&a); value_free(&b);
        if (x > (unsigned long long)LLONG_MAX)
            runtime_error(it, line, "integer overflow in myon.math.gcd");
        *out = value_int((long long)x);
        return 1;
    }

    /* lcm(a, b) ret int, error: abs(a / gcd(a,b) * b), dividing first to
     * limit overflow.  lcm(0, *) == 0.  Overflow -> error. */
    if (strcmp(name, "myon.math.lcm") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        if (a.type != TYPE_INT || b.type != TYPE_INT) {
            value_free(&a); value_free(&b);
            runtime_error(it, line, "myon.math.lcm expects (int, int)");
        }
        long long av = a.as.i, bv = b.as.i;
        value_free(&a); value_free(&b);
        if (av == 0 || bv == 0) {
            *out = make_result_pair(value_int(0), value_nil());
            return 1;
        }
        unsigned long long x = (av < 0) ? -(unsigned long long)av : (unsigned long long)av;
        unsigned long long y = (bv < 0) ? -(unsigned long long)bv : (unsigned long long)bv;
        unsigned long long g = x, gy = y;
        while (gy != 0) { unsigned long long t = g % gy; g = gy; gy = t; }
        /* result = (x / g) * y, computed in signed int64 with overflow check */
        long long q = (long long)(x / g);
        long long r;
        if (__builtin_mul_overflow(q, (long long)y, &r) || r < 0) {
            *out = make_result_pair(value_nil(),
                value_error(myon_strdup("myon.math.lcm: result overflows int64")));
        } else {
            *out = make_result_pair(value_int(r), value_nil());
        }
        return 1;
    }

    /* pow_int(base, exp) ret int, error: exact int64 binary exponentiation.
     * Negative exp -> error (use myon.math.pow for fractional results).
     * Overflow at any multiplication -> error. */
    if (strcmp(name, "myon.math.pow_int") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        if (a.type != TYPE_INT || b.type != TYPE_INT) {
            value_free(&a); value_free(&b);
            runtime_error(it, line, "myon.math.pow_int expects (int, int)");
        }
        long long base = a.as.i, exp = b.as.i;
        value_free(&a); value_free(&b);
        if (exp < 0) {
            *out = make_result_pair(value_nil(),
                value_error(myon_strdup("myon.math.pow_int: negative exponent (use myon.math.pow for fractional results)")));
            return 1;
        }
        long long result = 1;
        long long acc = base;
        long long e = exp;
        int overflow = 0;
        while (e > 0) {
            if (e & 1) {
                if (__builtin_mul_overflow(result, acc, &result)) { overflow = 1; break; }
            }
            e >>= 1;
            if (e > 0) {
                if (__builtin_mul_overflow(acc, acc, &acc)) { overflow = 1; break; }
            }
        }
        if (overflow) {
            *out = make_result_pair(value_nil(),
                value_error(myon_strdup("myon.math.pow_int: result overflows int64")));
        } else {
            *out = make_result_pair(value_int(result), value_nil());
        }
        return 1;
    }

    /* ---- myon.string ---- */
    if (strcmp(name, "myon.string.length") == 0) {
        /* Raw byte length (strlen). Kept as the original byte-count
         * behavior for backward compatibility. */
        Value a = eval_arg(it, env, call, 0);
        if (a.type != TYPE_STR) { value_free(&a); runtime_error(it, line, "myon.string.length expects str"); }
        *out = value_int((long long)strlen(a.as.obj->as.str));
        value_free(&a); return 1;
    }
    if (strcmp(name, "myon.string.length_chars") == 0) {
        /* Unicode code-point count, not byte count. */
        Value a = eval_arg(it, env, call, 0);
        if (a.type != TYPE_STR) { value_free(&a); runtime_error(it, line, "myon.string.length_chars expects str"); }
        *out = value_int(utf8_char_count(a.as.obj->as.str));
        value_free(&a); return 1;
    }
    if (strcmp(name, "myon.string.concat") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        if (a.type != TYPE_STR || b.type != TYPE_STR) {
            value_free(&a); value_free(&b);
            runtime_error(it, line, "myon.string.concat expects two str values");
        }
        *out = eval_binary(it, line, OP_ADD, a, b); return 1;
    }
    if (strcmp(name, "myon.string.contains") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        if (a.type != TYPE_STR || b.type != TYPE_STR) {
            value_free(&a); value_free(&b);
            runtime_error(it, line, "myon.string.contains expects two str values");
        }
        int found = strstr(a.as.obj->as.str, b.as.obj->as.str) != NULL;
        value_free(&a); value_free(&b);
        *out = value_bool(found); return 1;
    }
    if (strcmp(name, "myon.string.upper") == 0 || strcmp(name, "myon.string.lower") == 0) {
        Value a = eval_arg(it, env, call, 0);
        if (a.type != TYPE_STR) { value_free(&a); runtime_error(it, line, "expects str"); }
        bool up = strcmp(name, "myon.string.upper") == 0;
        char *s = myon_unicode_case(a.as.obj->as.str, up);
        value_free(&a);
        if (!s) runtime_error(it, line, "Unicode case conversion is too large");
        *out = value_str(s); return 1;
    }

    /* ---- myon.string (Phase3.5 extension, Step4) ----
     * Index-based helpers operate on *character* (code-point) positions, not
     * byte offsets, so they stay correct for multi-byte UTF-8 text. */

    /* substring(s, start, len) ret str, error — character-based slice. */
    if (strcmp(name, "myon.string.substring") == 0) {
        Value s = eval_arg(it, env, call, 0);
        Value vs = eval_arg(it, env, call, 1);
        Value vl = eval_arg(it, env, call, 2);
        if (s.type != TYPE_STR || vs.type != TYPE_INT || vl.type != TYPE_INT) {
            value_free(&s); value_free(&vs); value_free(&vl);
            runtime_error(it, line, "myon.string.substring expects (str, int, int)");
        }
        long long start = vs.as.i, len = vl.as.i;
        const char *cs = s.as.obj->as.str;
        long long nchars = utf8_char_count(cs);
        /* Avoid `start + len` (signed overflow / UB) by checking each bound
         * separately.  With `start <= nchars` established, `nchars - start`
         * cannot underflow. */
        if (start < 0 || len < 0 || start > nchars || len > nchars - start) {
            *out = make_result_pair(value_str(myon_strdup("")),
                value_error(myon_strdup("myon.string.substring: range out of bounds")));
            value_free(&s); value_free(&vs); value_free(&vl); return 1;
        }
        long long boff = utf8_byte_offset(cs, start);
        long long eoff = utf8_byte_offset(cs, start + len);
        long long nbytes = eoff - boff;
        char *buf = (char *)myon_xmalloc((size_t)nbytes + 1);
        memcpy(buf, cs + boff, (size_t)nbytes);
        buf[nbytes] = '\0';
        *out = make_result_pair(value_str(buf), value_nil());
        value_free(&s); value_free(&vs); value_free(&vl); return 1;
    }

    /* split(s, sep) ret myon.array(str). Empty sep splits into single chars. */
    if (strcmp(name, "myon.string.split") == 0) {
        Value s = eval_arg(it, env, call, 0), sep = eval_arg(it, env, call, 1);
        if (s.type != TYPE_STR || sep.type != TYPE_STR) {
            value_free(&s); value_free(&sep);
            runtime_error(it, line, "myon.string.split expects two str values");
        }
        const char *cs = s.as.obj->as.str;
        const char *csep = sep.as.obj->as.str;
        Value arr = value_array(typespec_prim(TYPE_STR));
        if (cs[0] == '\0') {
            /* Empty input yields an empty array. */
        } else if (csep[0] == '\0') {
            /* Empty separator: one element per code point. */
            const char *p = cs;
            while (*p) {
                long long adv = utf8_byte_offset(p, 1);
                if (adv <= 0) adv = 1; /* defensive: always make progress */
                char *piece = (char *)myon_xmalloc((size_t)adv + 1);
                memcpy(piece, p, (size_t)adv);
                piece[adv] = '\0';
                array_push(&arr, value_str(piece));
                p += adv;
            }
        } else {
            size_t seplen = strlen(csep);
            const char *p = cs;
            const char *hit;
            while ((hit = strstr(p, csep)) != NULL) {
                size_t n = (size_t)(hit - p);
                char *piece = (char *)myon_xmalloc(n + 1);
                memcpy(piece, p, n);
                piece[n] = '\0';
                array_push(&arr, value_str(piece));
                p = hit + seplen;
            }
            array_push(&arr, value_str(myon_strdup(p)));
        }
        value_free(&s); value_free(&sep);
        *out = arr; return 1;
    }

    /* join(parts, sep) ret str. */
    if (strcmp(name, "myon.string.join") == 0) {
        Value parts = eval_arg(it, env, call, 0), sep = eval_arg(it, env, call, 1);
        if (parts.type != TYPE_ARRAY || sep.type != TYPE_STR) {
            value_free(&parts); value_free(&sep);
            runtime_error(it, line, "myon.string.join expects (myon.array(str), str)");
        }
        const char *csep = sep.as.obj->as.str;
        size_t seplen = strlen(csep);
        ArrayData *a = &parts.as.obj->as.arr;
        /* Accumulate the buffer size through the checked helpers (B-3) so a huge
         * array of strings cannot wrap `total` and under-allocate `buf`. */
        size_t total = 1; /* trailing NUL */
        int overflow = 0;
        for (int i = 0; i < a->count; i++) {
            if (a->items[i].type != TYPE_STR) {
                value_free(&parts); value_free(&sep);
                runtime_error(it, line, "myon.string.join: array elements must be str");
            }
            if (!checked_add_size(total, strlen(a->items[i].as.obj->as.str), &total))
                { overflow = 1; break; }
            if (i && !checked_add_size(total, seplen, &total))
                { overflow = 1; break; }
        }
        if (overflow) {
            value_free(&parts); value_free(&sep);
            runtime_error(it, line, "myon.string.join: result too large");
        }
        char *buf = (char *)myon_xmalloc(total);
        buf[0] = '\0';
        size_t off = 0;
        for (int i = 0; i < a->count; i++) {
            if (i) { memcpy(buf + off, csep, seplen); off += seplen; }
            const char *piece = a->items[i].as.obj->as.str;
            size_t plen = strlen(piece);
            memcpy(buf + off, piece, plen); off += plen;
        }
        buf[off] = '\0';
        value_free(&parts); value_free(&sep);
        *out = value_str(buf); return 1;
    }

    /* trim(s) ret str — strip leading/trailing ASCII whitespace. */
    if (strcmp(name, "myon.string.trim") == 0) {
        Value s = eval_arg(it, env, call, 0);
        if (s.type != TYPE_STR) { value_free(&s); runtime_error(it, line, "myon.string.trim expects str"); }
        const char *cs = s.as.obj->as.str;
        const char *b = cs;
        while (*b && isspace((unsigned char)*b)) b++;
        const char *e = cs + strlen(cs);
        while (e > b && isspace((unsigned char)e[-1])) e--;
        size_t n = (size_t)(e - b);
        char *buf = (char *)myon_xmalloc(n + 1);
        memcpy(buf, b, n); buf[n] = '\0';
        value_free(&s);
        *out = value_str(buf); return 1;
    }

    /* replace(s, from, to) ret str — replace every occurrence. */
    if (strcmp(name, "myon.string.replace") == 0) {
        Value s = eval_arg(it, env, call, 0);
        Value from = eval_arg(it, env, call, 1);
        Value to = eval_arg(it, env, call, 2);
        if (s.type != TYPE_STR || from.type != TYPE_STR || to.type != TYPE_STR) {
            value_free(&s); value_free(&from); value_free(&to);
            runtime_error(it, line, "myon.string.replace expects three str values");
        }
        const char *cs = s.as.obj->as.str;
        const char *cf = from.as.obj->as.str;
        const char *ct = to.as.obj->as.str;
        if (cf[0] == '\0') {
            /* Empty needle: return the input unchanged (avoid infinite loop). */
            value_free(&from); value_free(&to);
            *out = s; return 1;
        }
        size_t flen = strlen(cf), tlen = strlen(ct);
        size_t cap = strlen(cs) + 1, len = 0;
        char *buf = (char *)myon_xmalloc(cap);
        const char *p = cs;
        const char *hit;
        while ((hit = strstr(p, cf)) != NULL) {
            size_t chunk = (size_t)(hit - p);
            while (len + chunk + tlen + 1 > cap) { cap *= 2; buf = (char *)myon_xrealloc(buf, cap); }
            memcpy(buf + len, p, chunk); len += chunk;
            memcpy(buf + len, ct, tlen); len += tlen;
            p = hit + flen;
        }
        size_t rest = strlen(p);
        while (len + rest + 1 > cap) { cap *= 2; buf = (char *)myon_xrealloc(buf, cap); }
        memcpy(buf + len, p, rest); len += rest;
        buf[len] = '\0';
        value_free(&s); value_free(&from); value_free(&to);
        *out = value_str(buf); return 1;
    }

    /* index_of(s, sub) ret int — character index of first match, or -1. */
    if (strcmp(name, "myon.string.index_of") == 0) {
        Value s = eval_arg(it, env, call, 0), sub = eval_arg(it, env, call, 1);
        if (s.type != TYPE_STR || sub.type != TYPE_STR) {
            value_free(&s); value_free(&sub);
            runtime_error(it, line, "myon.string.index_of expects two str values");
        }
        const char *cs = s.as.obj->as.str;
        const char *hit = strstr(cs, sub.as.obj->as.str);
        long long idx;
        if (!hit) {
            idx = -1;
        } else {
            /* Convert the byte offset of the match to a character index. */
            size_t boff = (size_t)(hit - cs);
            char saved = cs[boff];
            ((char *)cs)[boff] = '\0';
            idx = utf8_char_count(cs);
            ((char *)cs)[boff] = saved;
        }
        value_free(&s); value_free(&sub);
        *out = value_int(idx); return 1;
    }

    /* starts_with / ends_with ret bool. */
    if (strcmp(name, "myon.string.starts_with") == 0) {
        Value s = eval_arg(it, env, call, 0), pre = eval_arg(it, env, call, 1);
        if (s.type != TYPE_STR || pre.type != TYPE_STR) {
            value_free(&s); value_free(&pre);
            runtime_error(it, line, "myon.string.starts_with expects two str values");
        }
        const char *cs = s.as.obj->as.str;
        const char *cp = pre.as.obj->as.str;
        size_t plen = strlen(cp);
        int r = strncmp(cs, cp, plen) == 0;
        value_free(&s); value_free(&pre);
        *out = value_bool(r); return 1;
    }
    if (strcmp(name, "myon.string.ends_with") == 0) {
        Value s = eval_arg(it, env, call, 0), suf = eval_arg(it, env, call, 1);
        if (s.type != TYPE_STR || suf.type != TYPE_STR) {
            value_free(&s); value_free(&suf);
            runtime_error(it, line, "myon.string.ends_with expects two str values");
        }
        const char *cs = s.as.obj->as.str;
        const char *cx = suf.as.obj->as.str;
        size_t slen = strlen(cs), xlen = strlen(cx);
        int r = xlen <= slen && memcmp(cs + slen - xlen, cx, xlen) == 0;
        value_free(&s); value_free(&suf);
        *out = value_bool(r); return 1;
    }

    /* repeat(s, n) ret str, error — n<0 errors, n==0 yields "". */
    if (strcmp(name, "myon.string.repeat") == 0) {
        Value s = eval_arg(it, env, call, 0), vn = eval_arg(it, env, call, 1);
        if (s.type != TYPE_STR || vn.type != TYPE_INT) {
            value_free(&s); value_free(&vn);
            runtime_error(it, line, "myon.string.repeat expects (str, int)");
        }
        long long n = vn.as.i;
        if (n < 0) {
            *out = make_result_pair(value_str(myon_strdup("")),
                value_error(myon_strdup("myon.string.repeat: count must be non-negative")));
            value_free(&s); value_free(&vn); return 1;
        }
        const char *cs = s.as.obj->as.str;
        size_t unit = strlen(cs);
        size_t total, alloc;
        /* Guard the size computation against multiplication overflow: a wrapped
         * `unit * n` would under-allocate and let the memcpy loop write past the
         * buffer (heap-buffer-overflow).  Route it through the shared checked
         * helpers (B-3) so the `* n` and the `+ 1` NUL byte are both verified. */
        if (!checked_mul_size(unit, (size_t)n, &total) ||
            !checked_add_size(total, 1, &alloc)) {
            *out = make_result_pair(value_str(myon_strdup("")),
                value_error(myon_strdup("myon.string.repeat: result too large")));
            value_free(&s); value_free(&vn); return 1;
        }
        char *buf = (char *)myon_xmalloc(alloc);
        for (long long i = 0; i < n; i++) memcpy(buf + (size_t)i * unit, cs, unit);
        buf[total] = '\0';
        *out = make_result_pair(value_str(buf), value_nil());
        value_free(&s); value_free(&vn); return 1;
    }

    /* to_int(s) ret int, error. */
    if (strcmp(name, "myon.string.to_int") == 0) {
        Value s = eval_arg(it, env, call, 0);
        if (s.type != TYPE_STR) { value_free(&s); runtime_error(it, line, "myon.string.to_int expects str"); }
        const char *cs = s.as.obj->as.str;
        char *end = NULL;
        errno = 0;
        long long v = strtoll(cs, &end, 10);
        if (cs[0] == '\0' || end == cs || *end != '\0') {
            *out = make_result_pair(value_int(0),
                value_error(myon_strdup("myon.string.to_int: not an integer")));
        } else if (errno == ERANGE) {
            /* strtoll saturated to LLONG_MIN/MAX; report instead of silently
             * returning a wrong value. */
            *out = make_result_pair(value_int(0),
                value_error(myon_strdup("myon.string.to_int: out of range")));
        } else {
            *out = make_result_pair(value_int(v), value_nil());
        }
        value_free(&s); return 1;
    }

    /* to_float(s) ret float, error. */
    if (strcmp(name, "myon.string.to_float") == 0) {
        Value s = eval_arg(it, env, call, 0);
        if (s.type != TYPE_STR) { value_free(&s); runtime_error(it, line, "myon.string.to_float expects str"); }
        const char *cs = s.as.obj->as.str;
        char *end = NULL;
        errno = 0;
        double v = strtod(cs, &end);
        if (cs[0] == '\0' || end == cs || *end != '\0') {
            *out = make_result_pair(value_float(0.0),
                value_error(myon_strdup("myon.string.to_float: not a number")));
        } else if (errno == ERANGE) {
            /* strtod overflowed to +/-HUGE_VAL or underflowed to a subnormal;
             * report instead of silently returning a wrong value. */
            *out = make_result_pair(value_float(0.0),
                value_error(myon_strdup("myon.string.to_float: out of range")));
        } else {
            *out = make_result_pair(value_float(v), value_nil());
        }
        value_free(&s); return 1;
    }

    /* from_int / from_float ret str — reuse value_to_cstr()'s rendering. */
    if (strcmp(name, "myon.string.from_int") == 0) {
        Value a = eval_arg(it, env, call, 0);
        if (a.type != TYPE_INT) { value_free(&a); runtime_error(it, line, "myon.string.from_int expects int"); }
        *out = value_str(value_to_cstr(&a)); value_free(&a); return 1;
    }
    if (strcmp(name, "myon.string.from_float") == 0) {
        Value a = eval_arg(it, env, call, 0);
        if (a.type != TYPE_FLOAT) { value_free(&a); runtime_error(it, line, "myon.string.from_float expects float"); }
        *out = value_str(value_to_cstr(&a)); value_free(&a); return 1;
    }

    (void)argc;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Method dispatch on arrays / maps / structs                          */
/* ------------------------------------------------------------------ */

static int typespec_matches_value(Interp *it, TypeSpec *ts, const Value *v);

/* Ascending comparison for myon.array.sort() (Phase4, Step1).
 * qsort's compar callback receives only void* pointers with no user-data
 * channel (C11 qsort has no context argument), so the element type is
 * decided per comparison by inspecting each Value's .type field directly.
 *   - int   vs int   : compare as long long
 *   - float vs float : compare as double
 *   - int/float mix  : promote both to double (same rule as as_number)
 *   - str   vs str   : strcmp() byte-wise lexicographic order
 * Any other combination is not comparable; callers must reject such arrays
 * before invoking qsort (see the sort() dispatch below), so this function
 * only ever sees the comparable cases and falls back to 0 defensively. */
static int array_value_compare_asc(const void *pa, const void *pb) {
    const Value *a = (const Value *)pa;
    const Value *b = (const Value *)pb;

    if (a->type == TYPE_STR && b->type == TYPE_STR)
        return strcmp(a->as.obj->as.str, b->as.obj->as.str);

    int a_num = (a->type == TYPE_INT || a->type == TYPE_FLOAT);
    int b_num = (b->type == TYPE_INT || b->type == TYPE_FLOAT);
    if (a_num && b_num) {
        if (a->type == TYPE_INT && b->type == TYPE_INT) {
            long long x = a->as.i, y = b->as.i;
            return (x < y) ? -1 : (x > y) ? 1 : 0;
        }
        double x = (a->type == TYPE_INT) ? (double)a->as.i : a->as.f;
        double y = (b->type == TYPE_INT) ? (double)b->as.i : b->as.f;
        return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
    return 0; /* unreachable: callers pre-validate element comparability */
}

/* Reverse an array's elements in place (shared by reverse() and sort_desc()). */
static void array_reverse_inplace(ArrayData *a) {
    for (int i = 0, j = a->count - 1; i < j; i++, j--) {
        Value tmp = a->items[i];
        a->items[i] = a->items[j];
        a->items[j] = tmp;
    }
}

/* Return 1 when every element of `a` is a comparable type (int/float/str);
 * mixing numeric with str is still rejected because strcmp vs numeric order
 * is undefined. Returns 0 for empty arrays only vacuously (treated as OK by
 * caller). Sets *has_str / *has_num so the caller can reject numeric/str mix. */
static int array_all_sortable(const ArrayData *a) {
    int has_str = 0, has_num = 0;
    for (int i = 0; i < a->count; i++) {
        Type t = a->items[i].type;
        if (t == TYPE_STR) has_str = 1;
        else if (t == TYPE_INT || t == TYPE_FLOAT) has_num = 1;
        else return 0; /* array/map/struct/func/bool/nil -> not sortable */
    }
    if (has_str && has_num) return 0; /* cannot compare str against numbers */
    return 1;
}

static Value call_method(Interp *it, Env *env, Expr *call, Value recv,
                         const char *method) {
    int line = call->line;
    int argc = call->as.call.arg_count;

    if (recv.type == TYPE_ARRAY) {
        ArrayData *a = &recv.as.obj->as.arr;
        if (strcmp(method, "push") == 0) {
            if (argc != 1) runtime_error(it, line, "push expects 1 argument");
            Value v = eval_arg(it, env, call, 0);
            if (a->elem_type && !typespec_matches_value(it, a->elem_type, &v)) {
                char *want = typespec_to_cstr(a->elem_type);
                const char *got = value_type_name(&v);
                OwnedValues *bad = owned_values_new(it, 1);
                bad->values[bad->count++] = v;
                runtime_error_free(it, line, want,
                    "type mismatch: cannot push %s into myon.array(%s)", got, want);
            }
            array_push(&recv, v);
            return value_void();
        }
        if (strcmp(method, "pop") == 0) {
            Value out;
            if (!array_pop(&recv, &out))
                runtime_error(it, line, "pop from empty array");
            return out;
        }
        if (strcmp(method, "length") == 0) {
            return value_int(a->count);
        }
        /* ---- Phase4, Step1: sort / sort_desc / reverse / contains /
         *      index_of / slice ---- */
        if (strcmp(method, "sort") == 0) {
            if (argc != 0) runtime_error(it, line, "sort expects 0 arguments");
            if (!array_all_sortable(a))
                runtime_error(it, line,
                    "sort: array elements must all be int, float or str "
                    "(and numbers cannot be mixed with strings)");
            if (a->count > 1)
                qsort(a->items, (size_t)a->count, sizeof(Value),
                      array_value_compare_asc);
            return value_void();
        }
        if (strcmp(method, "sort_desc") == 0) {
            if (argc != 0) runtime_error(it, line, "sort_desc expects 0 arguments");
            if (!array_all_sortable(a))
                runtime_error(it, line,
                    "sort_desc: array elements must all be int, float or str "
                    "(and numbers cannot be mixed with strings)");
            if (a->count > 1) {
                qsort(a->items, (size_t)a->count, sizeof(Value),
                      array_value_compare_asc);
                array_reverse_inplace(a);
            }
            return value_void();
        }
        if (strcmp(method, "reverse") == 0) {
            if (argc != 0) runtime_error(it, line, "reverse expects 0 arguments");
            array_reverse_inplace(a);
            return value_void();
        }
        if (strcmp(method, "contains") == 0) {
            if (argc != 1) runtime_error(it, line, "contains expects 1 argument");
            Value v = eval_arg(it, env, call, 0);
            int found = 0;
            for (int i = 0; i < a->count; i++) {
                if (value_equal(&a->items[i], &v)) { found = 1; break; }
            }
            value_free(&v);
            return value_bool(found);
        }
        if (strcmp(method, "index_of") == 0) {
            if (argc != 1) runtime_error(it, line, "index_of expects 1 argument");
            Value v = eval_arg(it, env, call, 0);
            long long idx = -1;
            for (int i = 0; i < a->count; i++) {
                if (value_equal(&a->items[i], &v)) { idx = i; break; }
            }
            value_free(&v);
            return value_int(idx);
        }
        if (strcmp(method, "slice") == 0) {
            if (argc != 2) runtime_error(it, line, "slice expects 2 arguments");
            Value vs = eval_arg(it, env, call, 0);
            Value vl = eval_arg(it, env, call, 1);
            if (vs.type != TYPE_INT || vl.type != TYPE_INT) {
                value_free(&vs); value_free(&vl);
                runtime_error(it, line, "slice expects (int, int)");
            }
            long long start = vs.as.i, len = vl.as.i;
            value_free(&vs); value_free(&vl);
            long long count = (long long)a->count;
            /* Avoid `start + len` (signed overflow / UB) by checking each bound
             * separately, mirroring myon.string.substring. */
            if (start < 0 || len < 0 || start > count || len > count - start) {
                return make_result_pair(
                    value_array(a->elem_type ? typespec_clone(a->elem_type) : NULL),
                    value_error(myon_strdup("myon.array.slice: range out of bounds")));
            }
            Value res = value_array(a->elem_type ? typespec_clone(a->elem_type) : NULL);
            long long end = start + len; /* safe: len <= count - start */
            for (long long i = start; i < end; i++)
                array_push(&res, value_copy(&a->items[i]));
            return make_result_pair(res, value_nil());
        }
        if (strcmp(method, "map") == 0 || strcmp(method, "filter") == 0 ||
            strcmp(method, "reduce") == 0) {
            int reduce = strcmp(method, "reduce") == 0;
            int filter = strcmp(method, "filter") == 0;
            if (argc != (reduce ? 2 : 1))
                runtime_error(it, line, "%s expects %d argument(s)", method, reduce ? 2 : 1);
            OwnedValues *owned = owned_values_new(it, 3);
            for (int i = 0; i < 3; i++) owned->values[i] = value_nil();
            owned->count = 3;
            Value *v = owned->values; /* function, accumulator/result, element */
            v[0] = eval_arg(it, env, call, 0);
            if (reduce) v[1] = eval_arg(it, env, call, 1);
            else v[1] = value_array(filter && a->elem_type ? typespec_clone(a->elem_type) : NULL);
            for (int i = 0; i < a->count; i++) {
                v[2] = value_copy(&a->items[i]);
                Value result;
                if (reduce) {
                    result = call_function(it, line, v[0], v + 1, 2, NULL);
                    value_free(&v[1]);
                    v[1] = result;
                } else {
                    result = call_function(it, line, v[0], v + 2, 1, NULL);
                    if (filter) {
                        if (value_truthy(&result)) array_push(&v[1], value_copy(&v[2]));
                        value_free(&result);
                    } else {
                        if (i == 0) v[1].as.obj->as.arr.elem_type = typespec_prim(result.type);
                        array_push(&v[1], result);
                    }
                }
                value_free(&v[2]);
            }
            Value result = v[1];
            v[1] = value_nil();
            owned_values_release(it, owned);
            return result;
        }
        runtime_error(it, line, "unknown array method '%s'", method);
    }

    if (recv.type == TYPE_MAP) {
        if (strcmp(method, "set") == 0) {
            Value k = eval_arg(it, env, call, 0);
            Value v = eval_arg(it, env, call, 1);
            map_set(&recv, k, v);
            return value_void();
        }
        if (strcmp(method, "get") == 0) {
            Value k = eval_arg(it, env, call, 0);
            Value out;
            if (!map_get(&recv, &k, &out)) { value_free(&k); return value_nil(); }
            value_free(&k);
            return out;
        }
        if (strcmp(method, "has") == 0) {
            Value k = eval_arg(it, env, call, 0);
            int has = map_has(&recv, &k);
            value_free(&k);
            return value_bool(has);
        }
        if (strcmp(method, "delete") == 0) {
            Value k = eval_arg(it, env, call, 0);
            int ok = map_delete(&recv, &k);
            value_free(&k);
            return value_bool(ok);
        }
        /* ---- Phase4, Step2: keys / values / length ---- */
        MapData *m = &recv.as.obj->as.map;
        if (strcmp(method, "keys") == 0) {
            if (argc != 0) runtime_error(it, line, "keys expects 0 arguments");
            Value arr = value_array(m->key_type ? typespec_clone(m->key_type) : NULL);
            for (MapEntry *e = m->head; e; e = e->next)
                array_push(&arr, value_copy(&e->key));
            return arr;
        }
        if (strcmp(method, "values") == 0) {
            if (argc != 0) runtime_error(it, line, "values expects 0 arguments");
            Value arr = value_array(m->val_type ? typespec_clone(m->val_type) : NULL);
            for (MapEntry *e = m->head; e; e = e->next)
                array_push(&arr, value_copy(&e->val));
            return arr;
        }
        if (strcmp(method, "length") == 0) {
            if (argc != 0) runtime_error(it, line, "length expects 0 arguments");
            return value_int((long long)m->count);
        }
        runtime_error(it, line, "unknown map method '%s'", method);
    }

    if (recv.type == TYPE_STRUCT) {
        StructDecl *sd = recv.as.obj->as.st.decl;
        /* search up the inheritance chain for the method */
        FuncDecl *m = NULL;
        for (StructDecl *cur = sd; cur && !m; cur = cur->parent) {
            for (int i = 0; i < cur->method_count; i++)
                if (strcmp(cur->methods[i]->name, method) == 0) { m = cur->methods[i]; break; }
        }
        if (!m)
            runtime_error(it, line, "struct '%s' has no method '%s'",
                          recv.as.obj->as.st.type_name, method);

        OwnedValues *owned = owned_values_new(it, argc + 1);
        owned->values[owned->count++] = value_nil();
        for (int i = 0; i < argc; i++) {
            owned->values[owned->count] = eval_expr(it, env, call->as.call.args[i]);
            owned->count++;
        }
        Value fnv = value_func(m, it->global);
        owned->values[0] = fnv;
        fnv.as.obj->as.fn.is_bound = 1;
        fnv.as.obj->as.fn.bound_self = myon_xmalloc(sizeof(Value));
        *fnv.as.obj->as.fn.bound_self = value_copy(&recv);
        Value r = call_function(it, line, fnv, owned->values + 1, argc, NULL);
        owned_values_release(it, owned);
        return r;
    }

    runtime_error(it, line, "type %s has no methods", value_type_name(&recv));
    return value_nil();
}

/* ------------------------------------------------------------------ */
/* Struct construction (StructName(field=..., ...))                    */
/* ------------------------------------------------------------------ */

/* collect fields from the whole inheritance chain (parent first) */
static void collect_fields(StructDecl *sd, StructField **out, int *count) {
    if (!sd) return;
    collect_fields(sd->parent, out, count);
    for (int i = 0; i < sd->field_count; i++) {
        *out = (StructField *)myon_xrealloc(*out, sizeof(StructField) * (*count + 1));
        (*out)[(*count)++] = sd->fields[i];
    }
}

static Value construct_struct(Interp *it, Env *env, Expr *call, StructDecl *sd,
                              TypeSpec **type_args, int type_arg_count) {
    int line = call->line;
    Value sv = value_struct(sd->name, sd);
    OwnedValues *owned = owned_values_new(it, 1);
    owned->values[owned->count++] = sv;

    /* record generic bindings on the instance (Step 15) */
    if (type_arg_count > 0) {
        StructData *st = &sv.as.obj->as.st;
        st->tparam_count = type_arg_count;
        st->tparam_names = (char **)myon_xmalloc(sizeof(char *) * type_arg_count);
        st->tparam_types = (TypeSpec **)myon_xmalloc(sizeof(TypeSpec *) * type_arg_count);
        for (int i = 0; i < type_arg_count && i < sd->tparam_count; i++) {
            st->tparam_names[i] = myon_strdup(sd->tparams[i]);
            st->tparam_types[i] = typespec_clone(type_args[i]);
        }
        /* fill remaining names if fewer args than params */
        for (int i = sd->tparam_count; i < type_arg_count; i++) {
            st->tparam_names[i] = myon_strdup("?");
            st->tparam_types[i] = typespec_clone(type_args[i]);
        }
    }

    StructField *fields = NULL;
    int fcount = 0;
    collect_fields(sd, &fields, &fcount);
    if (fields) cleanup_register(it, fields, free);

    /* initialise every field, matching named arguments */
    for (int i = 0; i < fcount; i++) {
        Value fv = value_nil();
        int found = 0;
        for (int j = 0; j < call->as.call.arg_count; j++) {
            if (call->as.call.arg_names[j] &&
                strcmp(call->as.call.arg_names[j], fields[i].name) == 0) {
                fv = eval_expr(it, env, call->as.call.args[j]);
                found = 1;
                break;
            }
        }
        if (!found) {
            runtime_error(it, line, "missing field '%s' in %s(...) construction",
                          fields[i].name, sd->name);
        }
        struct_add_field(&sv, fields[i].name, fv);
    }
    if (fields) { cleanup_forget(it, fields); free(fields); }
    owned->count = 0;
    owned_values_release(it, owned);
    return sv;
}

/* ------------------------------------------------------------------ */
/* typespec / value compatibility                                      */
/* ------------------------------------------------------------------ */

static int typespec_matches_value(Interp *it, TypeSpec *ts, const Value *v) {
    (void)it;
    if (!ts) return 1;
    switch (ts->base) {
        case TYPE_INT:    return v->type == TYPE_INT;
        case TYPE_FLOAT:  return v->type == TYPE_FLOAT;
        case TYPE_STR:    return v->type == TYPE_STR;
        case TYPE_CHAR:   return v->type == TYPE_CHAR;
        case TYPE_BOOL:   return v->type == TYPE_BOOL;
        case TYPE_VOID:   return v->type == TYPE_VOID;
        case TYPE_ERROR:  return v->type == TYPE_ERROR || v->type == TYPE_NIL;
        case TYPE_ARRAY:  return v->type == TYPE_ARRAY;
        case TYPE_MAP:    return v->type == TYPE_MAP;
        case TYPE_STRUCT:
            /* a bare identifier annotation: struct name OR a generic
             * type parameter — accept structurally by name, else accept any
             * (type parameters are erased at runtime). */
            if (v->type == TYPE_STRUCT)
                return strcmp(ts->name, v->as.obj->as.st.type_name) == 0;
            /* unknown struct name -> treat as generic param, accept anything */
            return 1;
        default:          return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Function invocation (Step 6 + closures)                             */
/* ------------------------------------------------------------------ */

static Value call_function(Interp *it, int line, Value fn, Value *args, int argc,
                           char **arg_names) {
    (void)arg_names;
    if (fn.type != TYPE_FUNC)
        runtime_error(it, line, "attempt to call a non-function value (%s)", type_name(fn.type));

    FuncDecl *fd = fn.as.obj->as.fn.decl;
    if (!fn.as.obj->as.fn.mvm_chunk && argc != fd->param_count)
        runtime_error(it, line, "function '%s' expects %d argument(s), got %d",
                      fd->name ? fd->name : "<lambda>", fd->param_count, argc);

    /* Phase5.2: guard the native C stack against unbounded Myon recursion.
     * We check *before* entering another frame so the limit reflects the depth
     * we are about to reach.  On overflow we raise a normal runtime error
     * (which longjmps to the top-level barrier where call_depth is reset) rather
     * than letting the OS kill us with SIGSEGV.  Note: it->call_depth has not
     * yet been incremented for this frame, so comparing >= against the cap makes
     * the (depth+1)-th nested call the one that fails. */
    Task *running = it->loop ? event_loop_current(it->loop) : NULL;
    int *call_depth = running ? event_loop_task_call_depth(running) : &it->call_depth;
    int max_depth = myon_max_call_depth();
    if (running) {
        /* The coroutine has its own 256 KiB stack, regardless of RLIMIT_STACK.
         * Do not apply the main-stack minimum (64) to sanitizer stacks. */
        max_depth = (int)(MYON_TASK_STACK_SIZE / 4 * 3 / MYON_CALL_FRAME_BYTES);
    }
    /* Native -> VM -> native reentry adds a dispatch loop to each C frame. */
    if (fn.as.obj->as.fn.mvm_chunk) max_depth /= 4;
    if (*call_depth >= max_depth)
        runtime_error(it, line,
            "call stack too deep: recursion exceeded %d nested calls "
            "(possible infinite recursion in '%s')",
            max_depth, fd && fd->name ? fd->name : "<lambda>");
    (*call_depth)++;

    if (fn.as.obj->as.fn.mvm_chunk) {
        if (!it->vm_caller) runtime_error(it, line, "bytecode callback has no active VM");
        Value result = it->vm_caller(it->vm_context, fn, args, argc, line);
        (*call_depth)--;
        return result;
    }

    Env *call_env = env_new(fn.as.obj->as.fn.closure);
    /* Known-issue #1: register this frame so a runtime_error() longjmp from any
     * nested call still frees it (via cleanup_unwind at the setjmp barrier). */
    cleanup_push(it, call_env);

    /* bind self for methods */
    if (fn.as.obj->as.fn.is_bound && fn.as.obj->as.fn.bound_self) {
        env_define(call_env, "self", value_copy(fn.as.obj->as.fn.bound_self));
        /* A struct method's closure is the global scope (see call_method), so
         * without a boundary here an assignment inside the method body to a
         * throwaway name that happens to match a global variable would write
         * through and clobber that global (e.g. turning the caller's `bar`
         * struct into a float, so the next `bar.method()` fails with
         * "type <T> has no methods").  Mark this frame as a function boundary
         * so assign_or_define() defines such names locally instead of
         * mutating globals the method never meant to touch.  Reads still see
         * globals (top-level funcs/structs) via env_get, which is unaffected. */
        call_env->is_fn_boundary = 1;
    }

    /* bind parameters with type-checking (spec 6.1: annotations required) */
    for (int i = 0; i < argc; i++) {
        if (!typespec_matches_value(it, fd->params[i].type, &args[i])) {
            char *want = typespec_to_cstr(fd->params[i].type);
            const char *got = value_type_name(&args[i]);
            cleanup_pop(it, call_env);   /* remove before freeing, avoids double-free at unwind */
            env_free(call_env);
            runtime_error_free(it, line, want,
                "argument %d of '%s' expects %s, got %s",
                i + 1, fd->name ? fd->name : "<lambda>", want, got);
        }
        env_define(call_env, fd->params[i].name, value_copy(&args[i]));
    }

    /* run body */
    Value *saved_ret = it->ret_values;
    OwnedValues *saved_owner = it->ret_owner;
    int    saved_cnt = it->ret_count;
    it->ret_values = NULL;
    it->ret_owner = NULL;
    it->ret_count = 0;

    Flow f = exec_block(it, call_env, fd->body);

    Value result;
    if (f == FLOW_RETURN) {
        if (it->ret_count == 0) {
            result = value_void();
        } else if (it->ret_count == 1) {
            result = it->ret_values[0];
            it->ret_values[0] = value_nil();
        } else {
            /* multiple returns -> pack into an array-like tuple.
             * We represent the tuple with a special array so multi-assignment
             * can unpack it. */
            Value tup = value_array(NULL);
            for (int i = 0; i < it->ret_count; i++) {
                array_push(&tup, it->ret_values[i]);
                it->ret_values[i] = value_nil();
            }
            result = tup;
        }
        owned_values_release(it, it->ret_owner);
    } else {
        result = value_void();
    }

    it->ret_values = saved_ret;
    it->ret_owner = saved_owner;
    it->ret_count = saved_cnt;

    cleanup_pop(it, call_env);   /* normal exit: unregister before freeing */
    env_free(call_env);
    /* Phase5.2: pop this frame's depth on the normal (return/void) exit path.
     * The runtime_error paths above intentionally skip this because they
     * longjmp past here to the top-level barrier, which resets call_depth. */
    (*call_depth)--;
    return result;
}

/* ------------------------------------------------------------------ */
/* Phase5: async tasks (coroutines on the event loop)                  */
/*                                                                     */
/* Calling a `myon.async myon.func` no longer runs its body inline;    */
/* instead it spawns an event-loop Task that runs the body on its own  */
/* C stack.  The caller immediately gets back a TYPE_TASK value.        */
/* `myon.await` suspends the current coroutine until the target task    */
/* finishes, then yields the target's return value (or re-raises its    */
/* runtime error).                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    Interp *it;
    Value   fn;      /* owned copy of the async function value        */
    Value  *args;    /* owned copies of the arguments                 */
    int     argc;
    Value  *result;  /* heap Value produced by the body (owned by task) */
    Task   *task;    /* set after spawn so the entry can record result */
} AsyncCtx;

static void async_task_cleanup(void *ud) {
    AsyncCtx *ctx = ud;
    value_free(&ctx->fn);
    for (int i = 0; i < ctx->argc; i++) value_free(&ctx->args[i]);
    free(ctx->args);
    if (ctx->result) { value_free(ctx->result); free(ctx->result); }
    free(ctx);
}

/* Entry point run on the task's own C stack. */
static void async_task_entry(void *ud) {
    AsyncCtx *ctx = (AsyncCtx *)ud;
    Interp *it = ctx->it;

    /* Install a local error barrier: a runtime_error inside the body must not
     * longjmp across the coroutine boundary.  On error we finish the task with
     * has_error=1 and store the error value so await can re-raise it. */
    jmp_buf saved;
    memcpy(&saved, interp_error_buf(it), sizeof(jmp_buf));

    Value *res = (Value *)myon_xmalloc(sizeof(Value));
    *res = value_nil();
    ctx->result = res;
    int has_error = 0;

    /* Phase5.2: the coroutine runs on its own fresh C stack, so its recursion
     * budget is independent of the caller's.  Save the caller's depth, start
     * the body at 0, and restore afterwards (the error path longjmps to the
     * barrier below without unwinding the coroutine's frames). */
    int saved_depth = it->call_depth;
    it->call_depth = 0;
    /* Known-issue #1: mark the cleanup stack so an error inside the body frees
     * the call frames it longjmps over (see cleanup_unwind). */
    uint64_t cleanup_at = cleanup_mark(it);

    if (setjmp(*interp_error_buf(it)) == 0) {
        *res = call_function(it, 0, ctx->fn, ctx->args, ctx->argc, NULL);
    } else {
        /* body raised: free the frames the longjmp skipped, then represent the
         * failure as an error value. */
        cleanup_unwind(it, cleanup_at);
        *res = value_error(myon_strdup("async task failed"));
        has_error = 1;
    }

    it->call_depth = saved_depth;
    memcpy(interp_error_buf(it), &saved, sizeof(jmp_buf));

    event_loop_task_set_result(ctx->task, res, has_error);
    ctx->result = NULL; /* the task now owns the result */
}

/* Spawn an async task for `fn(args...)` and return a TYPE_TASK value. */
static Value spawn_async_task(Interp *it, Value fn, Value *args, int argc) {
    EventLoop *loop = ensure_loop(it);

    AsyncCtx *ctx = (AsyncCtx *)myon_xmalloc(sizeof(AsyncCtx));
    ctx->it = it;
    ctx->fn = value_copy(&fn);
    ctx->args = argc ? (Value *)myon_xmalloc(sizeof(Value) * argc) : NULL;
    for (int i = 0; i < argc; i++) ctx->args[i] = value_copy(&args[i]);
    ctx->argc = argc;
    ctx->result = NULL;
    ctx->task = NULL;

    Task *task = event_loop_spawn(loop, async_task_entry, ctx);
    event_loop_task_set_cleanup(task, async_task_cleanup);
    ctx->task = task;
    return value_task(task);
}

/* Await a TYPE_TASK: drive the loop until it completes, then hand back its
 * result value (moved out) or re-raise its error. */
static Value await_task(Interp *it, int line, Value taskv) {
    Task *target = (Task *)taskv.as.obj->as.task.task;
    if (!target) {
        runtime_error(it, line, "myon.await: invalid task handle");
    }

    if (!event_loop_task_done(target)) {
        if (event_loop_current(it->loop)) {
            /* cooperative: suspend this coroutine until target finishes */
            event_loop_wait_task(it->loop, target);
        } else {
            /* no active coroutine context: drive the loop to completion */
            while (!event_loop_task_done(target)) {
                if (event_loop_run_once(it->loop) == 0) break;
            }
        }
    }

    Value *res = (Value *)event_loop_task_result(target);
    int has_error = event_loop_task_has_error(target);

    Value out;
    if (res) {
        out = *res;          /* move out */
        free(res);
        /* prevent double-free: clear the loop's pointer to this payload */
        event_loop_task_set_result(target, NULL, has_error);
    } else {
        out = value_void();
    }

    value_free(&taskv); /* await consumes its evaluated task handle */
    if (has_error) {
        char *msg = out.type == TYPE_ERROR ? myon_strdup(out.as.obj->as.str)
                                           : myon_strdup("async task failed");
        value_free(&out);
        runtime_error_owned(it, line, msg);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Standard library: myon.http.* (Phase5, Step5)                       */
/*                                                                     */
/* Server side (serve_static / serve) is a native implementation on    */
/* top of net.c + the event loop: an accept loop runs as a coroutine   */
/* and each accepted connection is handled in its own spawned task.    */
/* HTTP/1.0 only, one request per connection, no Keep-Alive.           */
/*                                                                     */
/* Client side (get / post) is a minimal self-contained TCP            */
/* implementation (no libcurl dependency): plain http:// only, no       */
/* redirects, no TLS.  https:// returns a "TLS not supported" error.    */
/* See docs/myon_spec.md 10.6 for the rationale.                       */
/* ------------------------------------------------------------------ */

/* Read one whole HTTP request (headers + Content-Length body) from a socket.
 * Returns 0 and fills *req on success; -1 on error/EOF (req untouched). */
static int http_read_request(Interp *it, NetState *st, int sock_id,
                             HttpRequest *req) {
    myon_fd_t fd = net_raw_fd(st, sock_id); /* native-width fd */
    size_t cap = 4096, len = 0;
    char *buf = (char *)myon_xmalloc(cap);
    size_t header_end = 0;

    /* 1. read until we have the full header block */
    for (;;) {
        if (len == cap) { cap *= 2; buf = (char *)myon_xrealloc(buf, cap); }
        char *err = NULL;
        long long n = net_recv(st, sock_id, buf + len, (long long)(cap - len), &err);
        if (n == -2) { free(err); net_wait_fd(it, fd, 0); continue; }
        if (n < 0)   { free(err); free(buf); return -1; }
        if (n == 0)  { free(buf); return -1; } /* EOF before full request */
        len += (size_t)n;
        header_end = http_find_header_end(buf, len);
        if (header_end) break;
        if (len > (1u << 20)) { free(buf); return -1; } /* 1MB header cap */
    }

    /* 2. figure out how much body we already have; read the rest if needed */
    size_t have_body = len - header_end;
    /* peek Content-Length without a full parse yet */
    HttpRequest tmp;
    if (http_parse_request(buf, header_end, buf + header_end, have_body, &tmp) != 0) {
        free(buf);
        return -1;
    }
    long want = tmp.content_length;
    /* Phase5.2 audit fix: Content-Length is attacker-controlled, so cap the
     * body we are willing to buffer.  Without this a request advertising e.g.
     * "Content-Length: 999999999999" would drive an unbounded myon_xrealloc and
     * exhaust memory (DoS).  16 MiB mirrors the spirit of the 1 MiB header cap
     * above while comfortably covering realistic form/JSON payloads. */
    if (want > (16L << 20)) { http_request_free(&tmp); free(buf); return -1; }
    if (want > 0 && (size_t)want > have_body) {
        size_t need = (size_t)want - have_body;
        size_t need_cap = len + need;
        if (need_cap > cap) { cap = need_cap; buf = (char *)myon_xrealloc(buf, cap); }
        while (have_body < (size_t)want) {
            char *err = NULL;
            long long n = net_recv(st, sock_id, buf + len,
                                   (long long)(cap - len), &err);
            if (n == -2) { free(err); net_wait_fd(it, fd, 0); continue; }
            if (n <= 0)  { free(err); break; } /* short body: use what we have */
            len += (size_t)n;
            have_body += (size_t)n;
        }
        /* re-parse with the complete body */
        http_request_free(&tmp);
        if (http_parse_request(buf, header_end, buf + header_end,
                               have_body, &tmp) != 0) {
            free(buf);
            return -1;
        }
    }

    *req = tmp;
    free(buf);
    return 0;
}

/* Send all `len` bytes on a socket, yielding on EWOULDBLOCK. */
static void http_send_all(Interp *it, NetState *st, int sock_id,
                          const char *data, size_t len) {
    myon_fd_t fd = net_raw_fd(st, sock_id); /* native-width fd */
    size_t off = 0;
    while (off < len) {
        char *err = NULL;
        long long n = net_send(st, sock_id, data + off,
                               (long long)(len - off), &err);
        if (n == -2) { free(err); net_wait_fd(it, fd, 1); continue; }
        if (n < 0)   { free(err); break; }
        off += (size_t)n;
    }
}

/* Like http_send_all, but over a TLS session.  `fd` is the underlying raw
 * socket fd, used only to wait for writability when the TLS layer would
 * block. */
static void tls_send_all(Interp *it, TlsConn *tls, myon_fd_t fd, /* native-width fd */
                         const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        char *err = NULL;
        long long n = tls_write(tls, data + off, (long long)(len - off), &err);
        if (n == -2) { free(err); net_wait_fd(it, fd, 1); continue; }
        if (n < 0)   { free(err); break; }
        off += (size_t)n;
    }
}

/* Serve a single request from an already-accepted static-file connection. */
static void http_serve_static_conn(Interp *it, NetState *st, int conn_id,
                                   const char *root_dir) {
    HttpRequest req;
    if (http_read_request(it, st, conn_id, &req) != 0) { net_close(st, conn_id); return; }

    char *fspath = http_resolve_static_path(root_dir, req.path);
    char *resp = NULL; size_t resp_len = 0;

    if (!fspath) {
        const char *body = "403 Forbidden";
        resp = http_build_response(403, "Forbidden", "text/plain",
                                   body, strlen(body), &resp_len);
    } else {
        int is_regular = 1;
#if defined(MYON_OS_POSIX)
        /* Only serve regular files: fopen("rb") on a directory or special
         * file makes ftell() indeterminate (historical bug).  stat() +
         * S_ISREG() rejects directories/FIFOs/devices up front. */
        {
            struct stat sb;
            if (stat(fspath, &sb) != 0 || !S_ISREG(sb.st_mode))
                is_regular = 0;
        }
#endif
        FILE *f = is_regular ? fopen(fspath, "rb") : NULL;
        if (!f) {
            const char *body = "404 Not Found";
            resp = http_build_response(404, "Not Found", "text/plain",
                                       body, strlen(body), &resp_len);
        } else {
            fseek(f, 0, SEEK_END);
            long fsz = ftell(f);
            if (fsz < 0) fsz = 0;
            fseek(f, 0, SEEK_SET);
            char *fbuf = (char *)myon_xmalloc((size_t)fsz + 1);
            size_t rd = fread(fbuf, 1, (size_t)fsz, f);
            fclose(f);
            resp = http_build_response(200, "OK",
                                       http_content_type_for_path(fspath),
                                       fbuf, rd, &resp_len);
            free(fbuf);
        }
        free(fspath);
    }

    if (resp) { http_send_all(it, st, conn_id, resp, resp_len); free(resp); }
    http_request_free(&req);
    net_close(st, conn_id);
}

/* --- context/entry for a single connection handled by a Myon handler --- */
typedef struct {
    Interp  *it;
    NetState *st;
    int      conn_id;
    Value    handler;   /* owned TYPE_FUNC copy */
    Task    *task;
} HttpConnCtx;

static void http_conn_cleanup(void *ud) {
    HttpConnCtx *c = ud;
    net_close(c->st, c->conn_id);
    value_free(&c->handler);
    free(c);
}

static void http_conn_task_entry(void *ud) {
    HttpConnCtx *c = (HttpConnCtx *)ud;
    Interp *it = c->it;

    jmp_buf saved;
    memcpy(&saved, interp_error_buf(it), sizeof(jmp_buf));

    HttpRequest req;
    if (http_read_request(it, c->st, c->conn_id, &req) == 0) {
        char *body_out = NULL;
        /* Response status/content-type are controllable by the handler
         * (bounded handler output).  A plain str return keeps the historical
         * 200 OK / text/plain default; returning an array lets the handler
         * override them:
         *   [body]                        -> 200, text/plain
         *   [status, body]                -> status, text/plain
         *   [status, content_type, body]  -> status, content_type
         * The trailing element is always the body. */
        /* volatile: written between setjmp and a possible longjmp (-Wclobbered). */
        volatile int status_out = 200;
        char * volatile ctype_out = NULL;   /* NULL => "text/plain" default */
        /* Phase5.2: this handler runs on the connection coroutine's own C
         * stack; give it an independent recursion budget (see async_task_entry). */
        int saved_depth = it->call_depth;
        it->call_depth = 0;
        /* Known-issue #1: free any call frames a handler error longjmps over. */
        uint64_t cleanup_at = cleanup_mark(it);
        if (setjmp(*interp_error_buf(it)) == 0) {
            Value args[3];
            args[0] = value_str(myon_strdup(req.method));
            args[1] = value_str(myon_strdup(req.path));
            args[2] = value_str(myon_strdup(req.body));
            Value r = call_function(it, 0, c->handler, args, 3, NULL);
            if (r.type == TYPE_STR && r.as.obj) {
                body_out = myon_strdup(r.as.obj->as.str);
            } else if (r.type == TYPE_ARRAY && r.as.obj) {
                ArrayData *ad = &r.as.obj->as.arr;
                int n = ad->count;
                /* body is the last element */
                if (n >= 1) {
                    Value *bv = &ad->items[n - 1];
                    body_out = myon_strdup((bv->type == TYPE_STR && bv->as.obj)
                                           ? bv->as.obj->as.str : "");
                } else {
                    body_out = myon_strdup("");
                }
                if (n >= 2) {
                    /* status may be an int, or a str (typed arrays are
                     * homogeneous, so myon.array(str) handlers pass "404"). */
                    long long s = -1;
                    if (ad->items[0].type == TYPE_INT) {
                        s = ad->items[0].as.i;
                    } else if (ad->items[0].type == TYPE_STR && ad->items[0].as.obj) {
                        char *end = NULL;
                        long parsed = strtol(ad->items[0].as.obj->as.str, &end, 10);
                        if (end && *end == '\0') s = parsed;
                    }
                    if (s >= 100 && s <= 599) status_out = (int)s;
                }
                if (n >= 3 && ad->items[1].type == TYPE_STR && ad->items[1].as.obj)
                    ctype_out = myon_strdup(ad->items[1].as.obj->as.str);
            } else {
                body_out = myon_strdup("");
            }
            value_free(&r);
            for (int i = 0; i < 3; i++) value_free(&args[i]);
        } else {
            cleanup_unwind(it, cleanup_at);
            status_out = 500;
            body_out = myon_strdup("500 Internal Server Error");
        }
        it->call_depth = saved_depth;
        size_t rlen = 0;
        char *resp = http_build_response(status_out,
                                         http_status_text(status_out),
                                         ctype_out ? ctype_out : "text/plain",
                                         body_out, strlen(body_out), &rlen);
        if (resp) { http_send_all(it, c->st, c->conn_id, resp, rlen); free(resp); }
        free(body_out);
        free(ctype_out);
        http_request_free(&req);
    }

    memcpy(interp_error_buf(it), &saved, sizeof(jmp_buf));

    event_loop_task_set_result(c->task, NULL, 0);
}

/* --- context/entry for the accept loop, run as its own coroutine --- */
typedef struct {
    Interp  *it;
    NetState *st;
    int      lsock;
    int      has_handler;
    Value    handler;    /* owned TYPE_FUNC copy (nil for serve_static) */
    char    *root_dir;   /* owned copy (for serve_static)               */
    Task    *task;
} HttpServerCtx;

static void http_server_cleanup(void *ud) {
    HttpServerCtx *sc = ud;
    net_close(sc->st, sc->lsock);
    value_free(&sc->handler);
    free(sc->root_dir);
    free(sc);
}

/* The accept loop itself.  Runs as a coroutine so it cooperatively yields to
 * the event loop (and to per-connection handler tasks) between accepts. */
static void http_accept_loop_entry(void *ud) {
    HttpServerCtx *sc = (HttpServerCtx *)ud;
    Interp *it = sc->it;
    NetState *st = sc->st;

    myon_fd_t lfd = net_raw_fd(st, sc->lsock); /* native-width fd */
    for (;;) {
        char *peer = NULL, *aerr = NULL;
        int cid = net_try_accept(st, sc->lsock, &peer, &aerr);
        free(peer);
        if (cid == -2) { free(aerr); net_wait_fd(it, lfd, 0); continue; }
        if (cid < 0)   { free(aerr); break; } /* fatal accept error */

        if (sc->has_handler) {
            HttpConnCtx *c = (HttpConnCtx *)myon_xmalloc(sizeof(HttpConnCtx));
            c->it = it; c->st = st; c->conn_id = cid;
            c->handler = value_copy(&sc->handler);
            c->task = NULL;
            Task *t = event_loop_spawn(ensure_loop(it), http_conn_task_entry, c);
            event_loop_task_set_cleanup(t, http_conn_cleanup);
            c->task = t;
        } else {
            /* static file serving is quick; handle inline on this coroutine */
            http_serve_static_conn(it, st, cid, sc->root_dir);
        }
    }

    event_loop_task_set_result(sc->task, NULL, 0);
}

/* Set up the listening socket then run the accept loop as a coroutine.
 * Blocks the script forever, like `python -m http.server` (Ctrl+C to stop).
 * `handler` is nil for serve_static. */
static Value http_run_server(Interp *it, int line, int port,
                             const char *root_dir, Value handler) {
    NetState *st = ensure_net(it);
    if (!net_supported())
        return make_result_pair(value_bool(0),
            value_error(myon_strdup("myon.http unsupported on this platform")));

    char *err = NULL;
    int lsock = net_socket_create(st, 0, &err);
    if (lsock < 0)
        return make_result_pair(value_bool(0),
            value_error(err ? err : myon_strdup("socket failed")));
    if (net_bind(st, lsock, "0.0.0.0", port, &err) != 0) {
        net_close(st, lsock);
        return make_result_pair(value_bool(0),
            value_error(err ? err : myon_strdup("bind failed")));
    }
    if (net_listen(st, lsock, 64, &err) != 0) {
        net_close(st, lsock);
        return make_result_pair(value_bool(0),
            value_error(err ? err : myon_strdup("listen failed")));
    }

    /* Spawn the accept loop as a coroutine so per-connection handler tasks
     * get scheduled fairly alongside it (regardless of whether serve* was
     * called from the top level or inside another async task). */
    HttpServerCtx *sc = (HttpServerCtx *)myon_xmalloc(sizeof(HttpServerCtx));
    sc->it = it; sc->st = st; sc->lsock = lsock;
    sc->has_handler = (handler.type == TYPE_FUNC);
    sc->handler = sc->has_handler ? value_copy(&handler) : value_nil();
    sc->root_dir = myon_strdup(root_dir ? root_dir : ".");
    sc->task = NULL;
    Task *server_task = event_loop_spawn(ensure_loop(it),
                                         http_accept_loop_entry, sc);
    event_loop_task_set_cleanup(server_task, http_server_cleanup);
    sc->task = server_task;

    if (event_loop_current(it->loop)) {
        /* Inside another coroutine (e.g. a test that also runs a client task):
         * the server is background work.  Mark it a daemon so it does not keep
         * the program alive once the foreground tasks finish, and yield to the
         * loop so it and its connection tasks make progress while we are
         * "blocked" here.  Control returns if the accept loop ever ends. */
        event_loop_set_daemon(server_task, 1);
        /* The task that is blocked serving (this coroutine) is likewise
         * background work — mark it a daemon too so awaiting the endless
         * accept loop does not count as foreground and hang program exit. */
        event_loop_set_daemon(event_loop_current(it->loop), 1);
        event_loop_wait_task(it->loop, server_task);
    } else {
        /* Top level: this is the whole program's purpose (like
         * `python -m http.server`).  Drive the loop forever; Ctrl+C exits. */
        event_loop_run_until_all_done(it->loop);
    }

    (void)line;
    return make_result_pair(value_bool(1), value_nil());
}

/* --- minimal self-contained HTTP client over raw TCP (no TLS) --- */

/* Parse "http(s)://host[:port]/path" -> host/port/path (all malloc'd out).
 * `*is_https_out` is set to 1 for an https:// URL, 0 for http:// (or a
 * scheme-less URL, which defaults to http).  The default port is 443 for
 * https and 80 for http when no explicit ":port" is present.
 * Returns 0 on success, -1 on malformed input. */
static int http_parse_url(const char *url, char **host_out, int *port_out,
                          char **path_out, int *is_https_out, char **err_out) {
    const char *p = url;
    int is_https = 0;
    int default_port = 80;
    if (strncmp(p, "https://", 8) == 0) {
        p += 8; is_https = 1; default_port = 443;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    /* host[:port] up to '/' or end */
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    int port = default_port;
    size_t hlen;
    const char *host_start = p;
    /* Parse a port substring strictly into [1, 65535].  Empty / non-numeric /
     * out-of-range / trailing-junk ports are a malformed URL and reported as
     * an error rather than silently falling back to the default. */
    const char *port_str = NULL; size_t port_len = 0;

    if (p < hostend && *p == '[') {
        /* IPv6 literal: "[::1]" or "[::1]:8080" (historical bug).  The host
         * is everything between the brackets; a ':' inside must not be treated
         * as the port separator. */
        const char *rb = memchr(p, ']', (size_t)(hostend - p));
        if (!rb) {
            if (err_out) *err_out = myon_strdup("invalid URL: unterminated IPv6 literal");
            return -1;
        }
        host_start = p + 1;
        hlen = (size_t)(rb - host_start);
        const char *after = rb + 1;
        if (after < hostend && *after == ':') {
            port_str = after + 1;
            port_len = (size_t)(hostend - port_str);
        } else if (after < hostend) {
            if (err_out) *err_out = myon_strdup("invalid URL: malformed IPv6 authority");
            return -1;
        }
    } else {
        const char *colon = memchr(p, ':', (size_t)(hostend - p));
        if (colon) {
            hlen = (size_t)(colon - p);
            port_str = colon + 1;
            port_len = (size_t)(hostend - port_str);
        } else {
            hlen = (size_t)(hostend - p);
        }
    }

    if (port_str) {
        if (port_len == 0 || port_len > 5) {
            if (err_out) *err_out = myon_strdup("invalid URL: bad port");
            return -1;
        }
        long pv = 0;
        for (size_t i = 0; i < port_len; i++) {
            char c = port_str[i];
            if (c < '0' || c > '9') {
                if (err_out) *err_out = myon_strdup("invalid URL: non-numeric port");
                return -1;
            }
            pv = pv * 10 + (c - '0');
        }
        if (pv < 1 || pv > 65535) {
            if (err_out) *err_out = myon_strdup("invalid URL: port out of range (1-65535)");
            return -1;
        }
        port = (int)pv;
    }

    if (hlen == 0) { if (err_out) *err_out = myon_strdup("invalid URL: missing host"); return -1; }

    /* Reject control/whitespace bytes in the host: it is interpolated verbatim
     * into the "Host:" header, so CR/LF/NUL/space would enable HTTP header /
     * request injection (CRLF injection).  A real host/IP never contains them. */
    for (size_t i = 0; i < hlen; i++) {
        unsigned char c = (unsigned char)host_start[i];
        if (c <= 0x20 || c == 0x7f) {
            if (err_out) *err_out = myon_strdup("invalid URL: illegal character in host");
            return -1;
        }
    }
    char *host = (char *)myon_xmalloc(hlen + 1);
    memcpy(host, host_start, hlen); host[hlen] = '\0';

    /* The request-target is interpolated into the request line; reject any
     * control byte so a crafted URL cannot inject extra request lines/headers. */
    const char *raw_path = slash ? slash : "/";
    for (const char *q2 = raw_path; *q2; q2++) {
        unsigned char c = (unsigned char)*q2;
        if (c < 0x20 || c == 0x7f) {
            free(host);
            if (err_out) *err_out = myon_strdup("invalid URL: illegal character in path");
            return -1;
        }
    }
    char *path = myon_strdup(raw_path);
    *host_out = host; *port_out = port; *path_out = path;
    if (is_https_out) *is_https_out = is_https;
    return 0;
}

/* Perform one HTTP/1.0 request; returns (body, status, error) as a 3-tuple.
 * `method` is "GET" or "POST"; body/content_type may be NULL for GET. */
static Value http_client_request(Interp *it, int line, const char *url,
                                 const char *method, const char *body,
                                 const char *content_type) {
    (void)line;
    Value tup = value_array(NULL);
    char *host = NULL, *path = NULL, *perr = NULL; int port = 80; int is_https = 0;
    if (http_parse_url(url, &host, &port, &path, &is_https, &perr) != 0) {
        array_push(&tup, value_str(myon_strdup("")));
        array_push(&tup, value_int(0));
        array_push(&tup, value_error(perr ? perr : myon_strdup("bad URL")));
        return tup;
    }

    /* Build the request header up-front, before any socket is created.
     *
     * C-2 fix (known-issues #5): snprintf() returns the length it *would* have
     * written, not the length that actually fit.  Passing that return value to
     * send() as the payload length meant a request header longer than the
     * 1024-byte stack buffer (a long URL path, host or content type) made us
     * hand `hn > sizeof(head)` to send(), which read past the buffer and
     * streamed adjacent stack memory to the peer -- an out-of-bounds read *and*
     * an information-disclosure channel (the same pattern already guarded in
     * src/http.c and src/pkg_fetch.c).
     *
     * Instead of truncating (which would silently send a corrupt request) or
     * rejecting every long-but-legitimate URL, keep a stack fast path and fall
     * back to an exactly-sized heap buffer when the header does not fit.  The
     * total is still capped so an absurd URL cannot drive a huge allocation,
     * and the check happens before connect() so it costs no network traffic. */
    size_t blen = body ? strlen(body) : 0;
    #define MYON_HTTP_CLIENT_MAX_HEAD (64UL << 10) /* 64 KiB */
    #define MYON_BUILD_HEAD(dst, cap)                                          \
        (blen ? snprintf((dst), (cap),                                         \
                    "%s %s HTTP/1.0\r\nHost: %s\r\nContent-Type: %s\r\n"       \
                    "Content-Length: %zu\r\nConnection: close\r\n\r\n",        \
                    method, path, host,                                        \
                    content_type ? content_type : "application/octet-stream",  \
                    blen)                                                      \
              : snprintf((dst), (cap),                                         \
                    "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", \
                    method, path, host))
    char   headbuf[1024];
    char  *head = headbuf;
    char  *head_heap = NULL;
    int    hn = MYON_BUILD_HEAD(headbuf, sizeof(headbuf));
    if (hn < 0 || (size_t)hn >= MYON_HTTP_CLIENT_MAX_HEAD) {
        /* encoding error, or a header too large to be a plausible request */
        free(host); free(path);
        array_push(&tup, value_str(myon_strdup("")));
        array_push(&tup, value_int(0));
        array_push(&tup, value_error(myon_strdup(
            "HTTP request header exceeds 64 KiB limit")));
        return tup;
    }
    if ((size_t)hn >= sizeof(headbuf)) {
        /* truncated in the stack buffer: rebuild at full size on the heap */
        size_t headcap = (size_t)hn + 1;
        head_heap = (char *)myon_xmalloc(headcap);
        head = head_heap;
        int hn2 = MYON_BUILD_HEAD(head, headcap);
        /* Defensive: never send more bytes than the buffer actually holds. */
        if (hn2 < 0 || (size_t)hn2 >= headcap) {
            free(head_heap); free(host); free(path);
            array_push(&tup, value_str(myon_strdup("")));
            array_push(&tup, value_int(0));
            array_push(&tup, value_error(myon_strdup("HTTP request build failed")));
            return tup;
        }
        hn = hn2;
    }
    #undef MYON_BUILD_HEAD

    NetState *st = ensure_net(it);
    char *err = NULL;
    int sock = net_socket_create(st, 0, &err);
    if (sock < 0) {
        array_push(&tup, value_str(myon_strdup("")));
        array_push(&tup, value_int(0));
        array_push(&tup, value_error(err ? err : myon_strdup("socket failed")));
        free(head_heap); free(host); free(path);
        return tup;
    }
    myon_fd_t fd = net_raw_fd(st, sock); /* native-width fd */

    /* connect (non-blocking, yield/loop until complete) */
    int rc = net_connect(st, sock, host, port, &err);
    while (rc == -2) { net_wait_fd(it, fd, 1); free(err); err = NULL; rc = net_connect_check(st, sock, &err); }
    if (rc != 0) {
        array_push(&tup, value_str(myon_strdup("")));
        array_push(&tup, value_int(0));
        array_push(&tup, value_error(err ? err : myon_strdup("connect failed")));
        net_close(st, sock); free(head_heap); free(host); free(path);
        return tup;
    }

    /* For https:// establish a TLS session on top of the connected socket. */
    TlsConn *tls = NULL;
    if (is_https) {
        char *terr = NULL;
        tls = tls_connect(fd, host, &terr);
        if (!tls) {
            array_push(&tup, value_str(myon_strdup("")));
            array_push(&tup, value_int(0));
            array_push(&tup, value_error(terr ? terr : myon_strdup("TLS handshake failed")));
            net_close(st, sock); free(head_heap); free(host); free(path);
            return tup;
        }
    }

    /* send the pre-built request (header length is now guaranteed in-bounds) */
    if (tls) {
        tls_send_all(it, tls, fd, head, (size_t)hn);
        if (blen) tls_send_all(it, tls, fd, body, blen);
    } else {
        http_send_all(it, st, sock, head, (size_t)hn);
        if (blen) http_send_all(it, st, sock, body, blen);
    }
    free(head_heap);
    head_heap = NULL; head = NULL;

    /* read the whole response.
     *
     * Security (manual review): the response is attacker-controlled -- a
     * malicious or MITM'd server (TLS verification is only best-effort, see
     * Phase5.1) can stream data forever.  Cap the buffer we are willing to
     * hold so an endless response cannot drive myon_xrealloc until
     * myon_xmalloc aborts the whole interpreter (out-of-memory, exit 70).
     * This mirrors the server-side Content-Length cap added in Phase 5.2. */
    #define MYON_HTTP_CLIENT_MAX_RESP (64UL << 20) /* 64 MiB */
    size_t cap = 8192, len = 0;
    int truncated = 0;
    char *rbuf = (char *)myon_xmalloc(cap);
    for (;;) {
        if (len == cap) {
            if (cap >= MYON_HTTP_CLIENT_MAX_RESP) { truncated = 1; break; }
            cap *= 2;
            if (cap > MYON_HTTP_CLIENT_MAX_RESP) cap = MYON_HTTP_CLIENT_MAX_RESP;
            rbuf = (char *)myon_xrealloc(rbuf, cap);
        }
        char *rerr = NULL;
        long long n;
        if (tls) n = tls_read(tls, rbuf + len, (long long)(cap - len), &rerr);
        else     n = net_recv(st, sock, rbuf + len, (long long)(cap - len), &rerr);
        if (n == -2) { free(rerr); net_wait_fd(it, fd, 0); continue; }
        if (n <= 0)  { free(rerr); break; } /* 0 == EOF/close */
        len += (size_t)n;
    }
    if (truncated) {
        if (tls) tls_close(tls);
        net_close(st, sock);
        free(host); free(path);
        array_push(&tup, value_str(myon_strdup("")));
        array_push(&tup, value_int(0));
        array_push(&tup, value_error(myon_strdup(
            "HTTP response exceeded 64 MiB limit (possible malicious server)")));
        free(rbuf);
        return tup;
    }
    if (tls) tls_close(tls);
    net_close(st, sock);
    free(host); free(path);

    /* split status line / headers / body */
    int status = 0;
    if (len >= 12 && strncmp(rbuf, "HTTP/", 5) == 0) {
        const char *sp = memchr(rbuf, ' ', len);
        if (sp) status = (int)strtol(sp + 1, NULL, 10);
    }
    size_t hend = http_find_header_end(rbuf, len);
    const char *bodyp = rbuf + hend;
    size_t bodylen = hend <= len ? len - hend : 0;

    array_push(&tup, value_str(myon_strndup(bodyp, bodylen)));
    array_push(&tup, value_int(status));
    array_push(&tup, value_nil());
    free(rbuf);
    return tup;
}

static int call_http(Interp *it, Env *env, const char *name, Expr *call, Value *out) {
    int line = call->line;

    if (strcmp(name, "myon.http.serve_static") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        int port = (int)a.as.i;
        char *root = (b.type == TYPE_STR && b.as.obj) ? myon_strdup(b.as.obj->as.str)
                                                      : myon_strdup(".");
        value_free(&a); value_free(&b);
        *out = http_run_server(it, line, port, root, value_nil());
        free(root);
        return 1;
    }

    if (strcmp(name, "myon.http.serve") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1);
        int port = (int)a.as.i;
        if (b.type != TYPE_FUNC) {
            value_free(&a); value_free(&b);
            runtime_error(it, line, "myon.http.serve: second argument must be a function");
        }
        *out = http_run_server(it, line, port, ".", b);
        value_free(&a); value_free(&b);
        return 1;
    }

    if (strcmp(name, "myon.http.get") == 0) {
        Value a = eval_arg(it, env, call, 0);
        const char *url = (a.type == TYPE_STR && a.as.obj) ? a.as.obj->as.str : "";
        *out = http_client_request(it, line, url, "GET", NULL, NULL);
        value_free(&a);
        return 1;
    }

    if (strcmp(name, "myon.http.post") == 0) {
        Value a = eval_arg(it, env, call, 0), b = eval_arg(it, env, call, 1),
              c = eval_arg(it, env, call, 2);
        const char *url  = (a.type == TYPE_STR && a.as.obj) ? a.as.obj->as.str : "";
        const char *body = (b.type == TYPE_STR && b.as.obj) ? b.as.obj->as.str : "";
        const char *ct   = (c.type == TYPE_STR && c.as.obj) ? c.as.obj->as.str : "application/octet-stream";
        *out = http_client_request(it, line, url, "POST", body, ct);
        value_free(&a); value_free(&b); value_free(&c);
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Phase4.1, Step4: C-to-Myon callback slots                           */
/*                                                                     */
/* ffi_callback.c hands out static trampoline function pointers keyed  */
/* by (slot, arg_count); this side owns the Myon function value and    */
/* interpreter context for each slot and performs the actual call when */
/* a trampoline fires.  A callback fires from *outside* the normal      */
/* eval loop (a C library calls back into us), so a runtime_error       */
/* raised inside the callback body would longjmp past the C frame.  To  */
/* keep that safe we install a local setjmp barrier around the call:    */
/* on error we print to stderr and return 0 rather than unwinding into  */
/* the C library.                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    int    active;
    Interp *it;
    Value   fn;         /* owned copy (TYPE_FUNC) */
    int    arg_count;
} MyonCBSlot;

static MyonCBSlot g_myon_cb_slots[MYON_FFI_CB_SLOTS];

void myon_ffi_callback_bind_slot(int slot, Interp *it, Value fn, int arg_count) {
    if (slot < 0 || slot >= MYON_FFI_CB_SLOTS) return;
    if (g_myon_cb_slots[slot].active) value_free(&g_myon_cb_slots[slot].fn);
    g_myon_cb_slots[slot].active = 1;
    g_myon_cb_slots[slot].it = it;
    g_myon_cb_slots[slot].fn = value_copy(&fn);
    g_myon_cb_slots[slot].arg_count = arg_count;
}

void myon_ffi_callback_clear_slot(int slot) {
    if (slot < 0 || slot >= MYON_FFI_CB_SLOTS) return;
    if (g_myon_cb_slots[slot].active) {
        value_free(&g_myon_cb_slots[slot].fn);
        g_myon_cb_slots[slot].active = 0;
        g_myon_cb_slots[slot].it = NULL;
        g_myon_cb_slots[slot].arg_count = 0;
    }
}

long long myon_ffi_callback_dispatch(int slot, int argc, const long long *args) {
    if (slot < 0 || slot >= MYON_FFI_CB_SLOTS) return 0;
    MyonCBSlot *cb = &g_myon_cb_slots[slot];
    if (!cb->active || !cb->it) return 0;

    Interp *it = cb->it;

    /* Marshal int64 arguments into Myon int values. */
    Value argv[MYON_FFI_CB_MAX_ARGS];
    for (int i = 0; i < argc; i++) argv[i] = value_int(args[i]);

    /* Install a temporary error barrier so a runtime_error in the callback
     * body does not longjmp across the foreign C frame. */
    jmp_buf saved;
    memcpy(&saved, interp_error_buf(it), sizeof(jmp_buf));

    /* Phase5.2: the callback fires synchronously from within a foreign C frame
     * that is itself nested under our own call stack, so depth keeps
     * accumulating here (no reset).  But if the body raises, the longjmp to the
     * barrier below skips the per-frame decrements, so snapshot the depth and
     * restore it on the way out to keep the counter balanced. */
    Task *running = interp_current_task(it);
    int *depth = running ? event_loop_task_call_depth(running) : &it->call_depth;
    int saved_depth = *depth;
    Value *saved_args = it->bridge_args;
    int saved_argc = it->bridge_argc;
    /* Known-issue #1: free the call frames a callback-body error longjmps over. */
    uint64_t cleanup_at = cleanup_mark(it);

    long long ret = 0;
    if (setjmp(*interp_error_buf(it)) == 0) {
        Value r = call_function(it, 0, cb->fn, argv, argc, NULL);
        if (r.type == TYPE_INT)       ret = r.as.i;
        else if (r.type == TYPE_BOOL) ret = r.as.b;
        else                          ret = 0; /* non-int return -> 0 */
        value_free(&r);
    } else {
        cleanup_unwind(it, cleanup_at);
        fprintf(stderr,
                "myon: runtime error inside FFI callback (slot %d); "
                "returning 0\n", slot);
        ret = 0;
    }

    *depth = saved_depth;
    it->bridge_args = saved_args;
    it->bridge_argc = saved_argc;
    /* restore the outer barrier */
    memcpy(interp_error_buf(it), &saved, sizeof(jmp_buf));

    for (int i = 0; i < argc; i++) value_free(&argv[i]);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Expression evaluation                                               */
/* ------------------------------------------------------------------ */

static Value invoke_owned_function(Interp *it, Env *env, Expr *call, Value fn) {
    int argc = call->as.call.arg_count;
    OwnedValues *owned = owned_values_new(it, argc + 1);
    owned->values[owned->count++] = fn;
    for (int i = 0; i < argc; i++) {
        owned->values[owned->count] = eval_expr(it, env, call->as.call.args[i]);
        owned->count++;
    }
    Value out;
    if (fn.as.obj->as.fn.decl && fn.as.obj->as.fn.decl->is_async)
        out = spawn_async_task(it, fn, owned->values + 1, argc);
    else
        out = call_function(it, call->line, fn, owned->values + 1, argc, call->as.call.arg_names);
    owned_values_release(it, owned);
    return out;
}

static Value eval_call(Interp *it, Env *env, Expr *e) {
    Expr *callee = e->as.call.callee;
    int line = e->line;

    /* builtin ident callees */
    if (callee->kind == EXPR_IDENT) {
        const char *name = callee->as.ident;
        require_native_module(it, line, name);
        if (strcmp(name, "myon.print") == 0) return builtin_print(it, env, e);
        if (strcmp(name, "myon.println") == 0) return builtin_println(it, env, e);
        if (strcmp(name, "myon.write") == 0) return builtin_write(it, env, e);
        if (strcmp(name, "myon.eprint") == 0) return builtin_eprint(it, env, e);
        if (strcmp(name, "myon.flush") == 0) return builtin_flush(it, env, e);
        if (strcmp(name, "myon.is_tty") == 0) return builtin_is_tty(it, env, e);
        if (strcmp(name, "myon.argv") == 0) return builtin_argv(it, env, e);
        if (strcmp(name, "myon.input") == 0) return builtin_input(it, env, e);
        /* stdlib namespaced calls (myon.math.* / myon.string.*) */
        if (strncmp(name, "myon.", 5) == 0) {
            Value out;
            if (call_stdlib(it, env, name, e, &out)) return out;
        }
        /* struct constructor: Name(field=...) */
        StructDecl *sd = find_struct(it, name);
        if (sd)
            return construct_struct(it, env, e, sd, NULL, 0);
    }

    /* generic constructor: Name<T>(...) */
    if (callee->kind == EXPR_GENERIC) {
        StructDecl *sd = find_struct(it, callee->as.generic.name);
        if (sd)
            return construct_struct(it, env, e, sd,
                                    callee->as.generic.args,
                                    callee->as.generic.arg_count);
        runtime_error(it, line, "unknown generic type '%s'", callee->as.generic.name);
    }

    /* module-qualified call via alias: `m.square(args)` where `m` is an
     * aliased external module.  Only taken when `m` is not shadowed by a real
     * variable of the same name, so ordinary method calls on values are
     * unaffected. */
    if (callee->kind == EXPR_MEMBER &&
        callee->as.member.target->kind == EXPR_IDENT) {
        const char *base = callee->as.member.target->as.ident;
        Value dummy;
        if (!env_get(env, base, &dummy)) {
            ModuleEntry *mod = find_module_by_alias(it, base);
            if (mod) {
                Value fn;
                if (!env_get(mod->ns, callee->as.member.name, &fn))
                    runtime_error(it, line,
                        "module '%s' (alias '%s') has no member '%s'",
                        mod->path, base, callee->as.member.name);
                if (fn.type != TYPE_FUNC) {
                    value_free(&fn);
                    runtime_error(it, line,
                        "module member '%s.%s' is not callable",
                        base, callee->as.member.name);
                }
                return invoke_owned_function(it, env, e, fn);
            }
        } else {
            value_free(&dummy);
        }
    }

    /* method call: obj.method(args) */
    if (callee->kind == EXPR_MEMBER) {
        OwnedValues *owned = owned_values_new(it, 1);
        owned->values[0] = eval_expr(it, env, callee->as.member.target);
        owned->count = 1;
        Value r = call_method(it, env, e, owned->values[0], callee->as.member.name);
        owned_values_release(it, owned);
        return r;
    }

    /* first-class function value (variable holding a func / lambda) */
    Value fn = eval_expr(it, env, callee);
    if (fn.type == TYPE_FUNC) return invoke_owned_function(it, env, e, fn);

    value_free(&fn);
    runtime_error(it, line, "call of unsupported callee");
    return value_nil();
}

static Value eval_expr(Interp *it, Env *env, Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LIT:   return value_int(e->as.int_val);
        case EXPR_FLOAT_LIT: return value_float(e->as.float_val);
        case EXPR_BOOL_LIT:  return value_bool(e->as.bool_val);
        case EXPR_STRING:    return interpolate_string(it, env, e->line, e->as.str_val);
        case EXPR_NIL:       return value_nil();

        case EXPR_IDENT: {
            Value v;
            if (!env_get(env, e->as.ident, &v))
                runtime_error(it, e->line, "undefined variable '%s'", e->as.ident);
            return v;
        }

        case EXPR_STR_CTOR:
            return cast_to_str(it, e->line, eval_expr(it, env, e->as.operand));
        case EXPR_INT_CTOR:
            return cast_to_int(it, e->line, eval_expr(it, env, e->as.operand));
        case EXPR_CHAR_CTOR:
            return cast_to_char(it, e->line, eval_expr(it, env, e->as.operand));
        case EXPR_ERROR_CTOR: {
            Value v = eval_expr(it, env, e->as.operand);
            if (v.type != TYPE_STR) {
                value_free(&v);
                runtime_error(it, e->line, "error() expects a str argument");
            }
            char *msg = myon_strdup(v.as.obj->as.str);
            value_free(&v);
            return value_error(msg);
        }

        case EXPR_ARRAY_CTOR:
            require_module(it, e->line, "myon.array");
            return value_array(typespec_clone(e->as.array_elem));
        case EXPR_MAP_CTOR:
            require_module(it, e->line, "myon.map");
            return value_map(typespec_clone(e->as.map_types.key),
                             typespec_clone(e->as.map_types.val));

        case EXPR_LAMBDA:
            /* capture the current environment as the closure */
            return value_func(e->as.lambda, env);

        case EXPR_AWAIT: {
            /* Phase5: if the operand is (or evaluates to) an async task, wait
             * for it cooperatively and yield its result.  Otherwise this is a
             * plain value (e.g. `myon.await` on a non-async call) — keep the
             * historical pseudo-async behaviour of returning it as-is so
             * pre-Phase5 code still works. */
            Value v = eval_expr(it, env, e->as.operand);
            if (v.type == TYPE_TASK) {
                return await_task(it, e->line, v);
            }
            return v;
        }

        case EXPR_GENERIC: {
            /* a generic name used without a call — treat as a struct ctor ref
             * is invalid here; only meaningful inside a call. */
            runtime_error(it, e->line, "generic type '%s' must be instantiated with (...)",
                          e->as.generic.name);
            return value_nil();
        }

        case EXPR_UNARY: {
            Value v = eval_expr(it, env, e->as.unary.operand);
            if (e->as.unary.op == OP_NEG) {
                return negate_value(it, e->line, v);
            } else {
                if (v.type != TYPE_BOOL) {
                    Type t = v.type; value_free(&v);
                    runtime_error(it, e->line, "myon.not requires bool, got %s", type_name(t));
                }
                return value_bool(!v.as.b);
            }
            return value_nil();
        }

        case EXPR_LOGICAL: {
            Value l = eval_expr(it, env, e->as.binary.left);
            if (l.type != TYPE_BOOL) {
                Type t = l.type; value_free(&l);
                runtime_error(it, e->line, "logical operator requires bool, got %s", type_name(t));
            }
            if (e->as.binary.op == OP_AND && !l.as.b) return value_bool(0);
            if (e->as.binary.op == OP_OR  &&  l.as.b) return value_bool(1);
            Value r = eval_expr(it, env, e->as.binary.right);
            if (r.type != TYPE_BOOL) {
                Type t = r.type; value_free(&r);
                runtime_error(it, e->line, "logical operator requires bool, got %s", type_name(t));
            }
            return value_bool(r.as.b);
        }

        case EXPR_BINARY: {
            OwnedValues *owned = owned_values_new(it, 2);
            owned->values[0] = eval_expr(it, env, e->as.binary.left);
            owned->count = 1;
            owned->values[1] = eval_expr(it, env, e->as.binary.right);
            owned->count = 2;
            Value l = owned->values[0], r = owned->values[1];
            owned->count = 0; /* eval_binary consumes both values */
            owned_values_release(it, owned);
            return eval_binary(it, e->line, e->as.binary.op, l, r);
        }

        case EXPR_CALL:
            return eval_call(it, env, e);

        case EXPR_INDEX: {
            Value target = eval_expr(it, env, e->as.index.target);
            Value idx = eval_expr(it, env, e->as.index.index);
            if (target.type == TYPE_ARRAY) {
                if (idx.type != TYPE_INT) {
                    value_free(&target); value_free(&idx);
                    runtime_error(it, e->line, "array index must be int");
                }
                ArrayData *a = &target.as.obj->as.arr;
                long long i = idx.as.i;
                if (i < 0 || i >= a->count) {
                    value_free(&target); value_free(&idx);
                    runtime_error(it, e->line, "array index %lld out of bounds (length %d)", i, a->count);
                }
                Value r = value_copy(&a->items[i]);
                value_free(&target); value_free(&idx);
                return r;
            }
            if (target.type == TYPE_MAP) {
                Value out;
                int ok = map_get(&target, &idx, &out);
                value_free(&target); value_free(&idx);
                if (!ok) return value_nil();
                return out;
            }
            value_free(&target); value_free(&idx);
            runtime_error(it, e->line, "cannot index type");
            return value_nil();
        }

        case EXPR_MEMBER: {
            /* module-qualified value access via alias: `m.square` yields the
             * function value, so it can be stored / passed as a first-class
             * value.  Only when `m` is not shadowed by a real variable. */
            if (e->as.member.target->kind == EXPR_IDENT) {
                const char *base = e->as.member.target->as.ident;
                Value dummy;
                if (!env_get(env, base, &dummy)) {
                    ModuleEntry *mod = find_module_by_alias(it, base);
                    if (mod) {
                        Value v;
                        if (!env_get(mod->ns, e->as.member.name, &v))
                            runtime_error(it, e->line,
                                "module '%s' (alias '%s') has no member '%s'",
                                mod->path, base, e->as.member.name);
                        return v;
                    }
                } else {
                    value_free(&dummy);
                }
            }
            Value target = eval_expr(it, env, e->as.member.target);
            if (target.type == TYPE_STRUCT) {
                Value *fp = struct_field_ptr(&target, e->as.member.name);
                if (!fp) {
                    const char *tn = target.as.obj->as.st.type_name;
                    value_free(&target);
                    runtime_error(it, e->line, "struct '%s' has no field '%s'",
                                  tn, e->as.member.name);
                }
                Value r = value_copy(fp);
                value_free(&target);
                return r;
            }
            value_free(&target);
            runtime_error(it, e->line, "member access on non-struct value");
            return value_nil();
        }
    }
    runtime_error(it, e->line, "unknown expression kind");
    return value_nil();
}

/* ------------------------------------------------------------------ */
/* Assignment to member / index targets                                */
/* ------------------------------------------------------------------ */

static void assign_target(Interp *it, Env *env, Expr *target, Value v) {
    int line = target->line;
    if (target->kind == EXPR_MEMBER) {
        Value obj = eval_expr(it, env, target->as.member.target);
        if (obj.type != TYPE_STRUCT) {
            value_free(&obj); value_free(&v);
            runtime_error(it, line, "cannot set field on non-struct");
        }
        Value *fp = struct_field_ptr(&obj, target->as.member.name);
        if (!fp) {
            const char *tn = obj.as.obj->as.st.type_name;
            value_free(&obj); value_free(&v);
            runtime_error(it, line, "struct '%s' has no field '%s'", tn, target->as.member.name);
        }
        value_free(fp);
        *fp = v;               /* obj shares the same Obj, mutation is visible */
        value_free(&obj);
        return;
    }
    if (target->kind == EXPR_INDEX) {
        Value obj = eval_expr(it, env, target->as.index.target);
        Value idx = eval_expr(it, env, target->as.index.index);
        if (obj.type == TYPE_ARRAY) {
            if (idx.type != TYPE_INT) { value_free(&obj); value_free(&idx); value_free(&v); runtime_error(it, line, "array index must be int"); }
            ArrayData *a = &obj.as.obj->as.arr;
            long long i = idx.as.i;
            if (i < 0 || i >= a->count) { value_free(&obj); value_free(&idx); value_free(&v); runtime_error(it, line, "array index out of bounds"); }
            value_free(&a->items[i]);
            a->items[i] = v;
            value_free(&obj); value_free(&idx);
            return;
        }
        if (obj.type == TYPE_MAP) {
            map_set(&obj, idx, v);   /* takes ownership of idx and v */
            value_free(&obj);
            return;
        }
        value_free(&obj); value_free(&idx); value_free(&v);
        runtime_error(it, line, "cannot index-assign to this type");
    }
    value_free(&v);
    runtime_error(it, line, "invalid assignment target");
}

/* ------------------------------------------------------------------ */
/* Statement execution                                                 */
/* ------------------------------------------------------------------ */

/*
 * Assignment / shadowing resolution shared by every plain (name-target)
 * assignment site: single assignment (`x = ...`) and multiple assignment
 * (`x, y = ...`).  Implements the spec 9.2 rule in one place so the two
 * sites cannot drift apart again (see the historical multi-assign bug
 * where the enclosing-scope lookup was missing).
 *
 * Rule (spec 9.2):
 *   1. If `name` is bound in the *current* scope, update it (env_set).
 *   2. Else if `name` is bound in some *enclosing* scope:
 *        a. inside an explicit `{ }` block (env->is_block) this is an
 *           illegal attempt to shadow an outer variable -> runtime_error.
 *        b. otherwise (function body etc.) assign through to the existing
 *           outer binding (env_set walks the chain).
 *   3. If `name` is bound nowhere, define it in the current scope.
 *
 * Takes ownership of `v` (either stored via env_set/env_define, or freed
 * on the error path).
 */
/* Is `name` bound in some ENCLOSING scope that is reachable for
 * assignment write-through?  Walk the parent chain but STOP crossing a
 * struct-method function boundary (Env.is_fn_boundary): a method body must
 * not silently write through into an outer scope (e.g. a global that merely
 * shares a throwaway variable's name), which used to turn `bar` into a
 * non-struct value and make a second `bar.render()` fail with
 * "type <T> has no methods".  The frame carrying is_fn_boundary is the
 * method's own call_env; scopes strictly outside it are not assignable
 * targets, so a name found only there is treated as "not an outer binding"
 * and a fresh local is defined instead. */
static int assignable_outer(Env *env, const char *name) {
    /* The boundary flag lives on the method's own call_env.  Everything
     * strictly outside that frame (its parent chain) is off-limits for
     * write-through.  Walk from the starting scope outward: at each frame,
     * first honour a same-name binding (assignable), and if the frame is a
     * boundary stop before crossing into its parent.  The starting scope's
     * own locals are handled by the caller, so a boundary on `env` itself
     * means no enclosing binding is assignable. */
    for (Env *e = env; e; e = e->parent) {
        if (e != env && env_defined_local(e, name)) return 1;
        if (e->is_fn_boundary) return 0;
    }
    return 0;
}

static void assign_or_define(Interp *it, Env *env, int line,
                             const char *name, Value v) {
    if (env_defined_local(env, name)) {
        env_set(env, name, v);
        return;
    }
    int outer = assignable_outer(env, name);
    if (outer && env->is_block) {
        value_free(&v);
        runtime_error(it, line,
            "redefinition of '%s' shadows an outer variable (forbidden, spec 9.2)",
            name);
    }
    if (outer) {
        /* assign through to the existing outer binding */
        env_set(env, name, v);
    } else {
        env_define(env, name, v);
    }
}

static Flow do_multi_assign(Interp *it, Env *env, Stmt *s, Value v) {
    /* v must be a tuple (array with NULL elem_type) from a multi-return */
    OwnedValues *owned = owned_values_new(it, 1);
    owned->values[owned->count++] = v;
    int total = 1 + s->as.assign.extra_count;
    if (v.type != TYPE_ARRAY || v.as.obj->as.arr.elem_type != NULL) {
        runtime_error(it, s->line,
            "multiple-target assignment requires a multi-value return (spec 6.2)");
    }
    ArrayData *a = &v.as.obj->as.arr;
    if (a->count != total) {
        runtime_error(it, s->line,
            "assignment target count (%d) does not match return count (%d)",
            total, a->count);
    }
    /* names[0] = name, names[1..] = extra_names */
    for (int i = 0; i < total; i++) {
        const char *nm = (i == 0) ? s->as.assign.name : s->as.assign.extra_names[i - 1];
        Value elem = value_copy(&a->items[i]);
        /* spec 9.2: same resolution as single assignment — update an
         * existing (local or enclosing) binding, or define a new one,
         * rejecting illegal shadowing inside `{ }` blocks. */
        assign_or_define(it, env, s->line, nm, elem);
    }
    owned_values_release(it, owned);
    return FLOW_NORMAL;
}

static Flow exec_stmt(Interp *it, Env *env, Stmt *s) {
    switch (s->kind) {
        case STMT_SYSTEM:
            return FLOW_NORMAL;
        case STMT_MODULE:
            runtime_error(it, s->line, "module declarations are only allowed at top level");
            return FLOW_NORMAL;

        case STMT_FUNC: {
            const char *nm = s->as.func->name;
            /* Top-level functions are hoisted during prescan; when we reach
             * the declaration statement again it is already bound to exactly
             * this decl, so treat it as a no-op. A nested function declaration
             * (inside a block/loop/function) is defined here on first sight. */
            if (env_defined_local(env, nm)) {
                Value existing;
                env_get(env, nm, &existing);
                int same = (existing.type == TYPE_FUNC &&
                            existing.as.obj->as.fn.decl == s->as.func);
                value_free(&existing);
                if (same) return FLOW_NORMAL;
                runtime_error(it, s->line,
                    "redefinition of '%s' (shadowing is forbidden, spec 9.2)", nm);
            }
            env_define(env, nm, value_func(s->as.func, env));
            return FLOW_NORMAL;
        }

        case STMT_STRUCT:
            /* registration is done in the preamble; ignore here */
            return FLOW_NORMAL;

        case STMT_RETURN: {
            int n = s->as.ret.count;
            OwnedValues *owned = owned_values_new(it, n);
            for (int i = 0; i < n; i++) {
                owned->values[i] = eval_expr(it, env, s->as.ret.values[i]);
                owned->count++;
            }
            it->ret_values = owned->values;
            it->ret_owner = owned;
            it->ret_count = n;
            return FLOW_RETURN;
        }

        case STMT_ASSIGN: {
            /* target assignment (a.b = v / a[i] = v) */
            if (s->as.assign.target) {
                if (s->as.assign.is_compound) {
                    Value cur = eval_expr(it, env, s->as.assign.target);
                    Value rhs = eval_expr(it, env, s->as.assign.value);
                    Value res = eval_binary(it, s->line, s->as.assign.compound, cur, rhs);
                    assign_target(it, env, s->as.assign.target, res);
                } else {
                    Value v = eval_expr(it, env, s->as.assign.value);
                    assign_target(it, env, s->as.assign.target, v);
                }
                return FLOW_NORMAL;
            }

            Value v = eval_expr(it, env, s->as.assign.value);

            if (s->as.assign.is_compound) {
                Value cur;
                if (!env_get(env, s->as.assign.name, &cur)) {
                    value_free(&v);
                    runtime_error(it, s->line, "compound assignment to undefined variable '%s'",
                                  s->as.assign.name);
                }
                Value res = eval_binary(it, s->line, s->as.assign.compound, cur, v);
                if (!env_set(env, s->as.assign.name, res))
                    runtime_error(it, s->line, "failed to assign '%s'", s->as.assign.name);
                return FLOW_NORMAL;
            }

            /* multiple-target assignment */
            if (s->as.assign.extra_count > 0)
                return do_multi_assign(it, env, s, v);

            if (v.type == TYPE_NIL) {
                value_free(&v);
                runtime_error(it, s->line,
                    "cannot assign myon.nil to a normal variable (spec 2.4)");
            }

            if (s->as.assign.has_type &&
                !typespec_matches_value(it, s->as.assign.annotated, &v)) {
                char *want = typespec_to_cstr(s->as.assign.annotated);
                const char *got = value_type_name(&v);
                OwnedValues *bad = owned_values_new(it, 1);
                bad->values[bad->count++] = v;
                runtime_error_free(it, s->line, want,
                    "type annotation mismatch: '%s' declared %s but value is %s",
                    s->as.assign.name, want, got);
            }

            /* Assignment / shadowing rules (spec 9.2) — shared with
             * multiple assignment via assign_or_define():
             *  - re-assignment to a name already bound (locally or in an
             *    enclosing scope) updates that binding.
             *  - inside an explicit `{ }` block, introducing a name that
             *    already exists in an outer scope is forbidden (shadowing).
             */
            assign_or_define(it, env, s->line, s->as.assign.name, v);
            return FLOW_NORMAL;
        }

        case STMT_EXPR: {
            Value v = eval_expr(it, env, s->as.expr);
            value_free(&v);
            return FLOW_NORMAL;
        }

        case STMT_IF: {
            Value c = eval_expr(it, env, s->as.if_stmt.cond);
            if (c.type != TYPE_BOOL) {
                Type t = c.type; value_free(&c);
                runtime_error(it, s->line, "if condition must be bool, got %s", type_name(t));
            }
            int taken = c.as.b;
            if (taken) {
                Env *sc = env_new(env);
                cleanup_push(it, sc);
                Flow f = exec_block(it, sc, &s->as.if_stmt.then_body);
                cleanup_pop(it, sc);
                env_free(sc);
                return f;
            }
            for (int i = 0; i < s->as.if_stmt.elif_count; i++) {
                Value ec = eval_expr(it, env, s->as.if_stmt.elifs[i].cond);
                if (ec.type != TYPE_BOOL) {
                    Type t = ec.type; value_free(&ec);
                    runtime_error(it, s->line, "elif condition must be bool, got %s", type_name(t));
                }
                if (ec.as.b) {
                    Env *sc = env_new(env);
                    cleanup_push(it, sc);
                    Flow f = exec_block(it, sc, &s->as.if_stmt.elifs[i].body);
                    cleanup_pop(it, sc);
                    env_free(sc);
                    return f;
                }
            }
            if (s->as.if_stmt.has_else) {
                Env *sc = env_new(env);
                cleanup_push(it, sc);
                Flow f = exec_block(it, sc, &s->as.if_stmt.else_body);
                cleanup_pop(it, sc);
                env_free(sc);
                return f;
            }
            return FLOW_NORMAL;
        }

        case STMT_WHILE: {
            for (;;) {
                Value c = eval_expr(it, env, s->as.while_stmt.cond);
                if (c.type != TYPE_BOOL) {
                    Type t = c.type; value_free(&c);
                    runtime_error(it, s->line, "while condition must be bool, got %s", type_name(t));
                }
                if (!c.as.b) break;
                Env *sc = env_new(env);
                cleanup_push(it, sc);
                Flow f = exec_block(it, sc, &s->as.while_stmt.body);
                cleanup_pop(it, sc);
                env_free(sc);
                if (f == FLOW_BREAK) break;
                if (f == FLOW_RETURN) return f;
            }
            return FLOW_NORMAL;
        }

        case STMT_FOR: {
            if (s->as.for_stmt.is_range) {
                Value start = eval_expr(it, env, s->as.for_stmt.range_start);
                Value end   = eval_expr(it, env, s->as.for_stmt.range_end);
                if (start.type != TYPE_INT || end.type != TYPE_INT) {
                    Type st = start.type, et = end.type;
                    value_free(&start); value_free(&end);
                    runtime_error(it, s->line, "range(...) requires int bounds, got %s and %s",
                                  type_name(st), type_name(et));
                }
                long long lo = start.as.i, hi = end.as.i;
                for (long long i = lo; i < hi; i++) {
                    Env *loop = env_new(env);
                    cleanup_push(it, loop);
                    env_define(loop, s->as.for_stmt.var, value_int(i));
                    Flow f = exec_block(it, loop, &s->as.for_stmt.body);
                    cleanup_pop(it, loop);
                    env_free(loop);
                    if (f == FLOW_BREAK) break;
                    if (f == FLOW_RETURN) return f;
                }
                return FLOW_NORMAL;
            }
            /* for-in over an array (spec 5.2 / 7) */
            OwnedValues *owned = owned_values_new(it, 1);
            Value iter = eval_expr(it, env, s->as.for_stmt.iterable);
            owned->values[owned->count++] = iter;
            if (iter.type != TYPE_ARRAY) {
                Type t = iter.type;
                runtime_error(it, s->line, "myon.for ... myon.in requires an array, got %s", type_name(t));
            }
            ArrayData *a = &iter.as.obj->as.arr;
            for (int i = 0; i < a->count; i++) {
                Env *loop = env_new(env);
                cleanup_push(it, loop);
                env_define(loop, s->as.for_stmt.var, value_copy(&a->items[i]));
                Flow f = exec_block(it, loop, &s->as.for_stmt.body);
                cleanup_pop(it, loop);
                env_free(loop);
                if (f == FLOW_BREAK) break;
                if (f == FLOW_RETURN) { owned_values_release(it, owned); return f; }
            }
            owned_values_release(it, owned);
            return FLOW_NORMAL;
        }

        case STMT_BREAK:    return FLOW_BREAK;
        case STMT_CONTINUE: return FLOW_CONTINUE;

        case STMT_BLOCK: {
            Env *scope = env_new(env);
            cleanup_push(it, scope);
            scope->is_block = 1;
            Flow f = exec_block(it, scope, &s->as.block);
            cleanup_pop(it, scope);
            env_free(scope);
            return f;
        }

        case STMT_EXPOSE: {
            /* copy the named binding from this scope into the parent (spec 9.1).
             *
             * NOTE: this deliberately does NOT go through assign_or_define().
             * Per spec 9.1, `myon.expose` publishes a binding exactly one
             * level outward (into env->parent), not recursively up the whole
             * chain, so the single-level env_defined_local(env->parent, ...)
             * check below is the intended behaviour and is left unchanged. */
            Value v;
            if (!env_get(env, s->as.expose_name, &v))
                runtime_error(it, s->line, "myon.expose: '%s' is not defined here",
                              s->as.expose_name);
            if (env->parent) {
                if (env_defined_local(env->parent, s->as.expose_name))
                    env_set(env->parent, s->as.expose_name, v);
                else
                    env_define(env->parent, s->as.expose_name, v);
            } else {
                value_free(&v);
            }
            return FLOW_NORMAL;
        }
    }
    return FLOW_NORMAL;
}

static Flow exec_block(Interp *it, Env *env, StmtList *body) {
    for (int i = 0; i < body->count; i++) {
        Flow f = exec_stmt(it, env, body->items[i]);
        if (f != FLOW_NORMAL) return f;
    }
    return FLOW_NORMAL;
}

/* ------------------------------------------------------------------ */
/* Module loading (Step 11)                                            */
/* ------------------------------------------------------------------ */

static int is_builtin_module(const char *path) {
    return myon_builtin_module_bit(path) != 0;
}

static ModuleEntry *find_module(Interp *it, const char *path) {
    for (ModuleEntry *m = it->modules; m; m = m->next)
        if (strcmp(m->path, path) == 0) return m;
    return NULL;
}

/* Find an aliased module by its alias name (e.g. the `m` in
 * `module external.util.math as m`).  Only modules that carry a namespace
 * scope (aliased external modules) are considered. */
static ModuleEntry *find_module_by_alias(Interp *it, const char *alias) {
    if (!alias) return NULL;
    for (ModuleEntry *m = it->modules; m; m = m->next)
        if (m->ns && m->alias && strcmp(m->alias, alias) == 0) return m;
    return NULL;
}

/* register + pre-scan a program's top-level struct/function declarations */
static void prescan(Interp *it, Env *env, Program *prog);

static int load_external_module(Interp *it, Stmt *decl);
static int load_package_module(Interp *it, Stmt *decl);
static void interp_retain_program(Interp *it, Program *prog);

/*
 * Spec §6.1 prefix dispatch.  A dotted module path resolves in exactly one of
 * three disjoint ways, and the branch is chosen up front so the three resolver
 * responsibilities stay separated (legacy `external.*`, installed package, and
 * builtin) rather than being tangled into a single function:
 *
 *   myon.*        -> builtin module (enabled and recorded)
 *   external.*    -> legacy external resolver, relative to the script directory
 *   <anything>    -> installed package module resolver (validated against the
 *                    project's myon.lock and the on-disk .myon/packages tree)
 */
static void handle_module_decl(Interp *it, Stmt *s) {
    const char *path = s->as.module_decl.path;
    if (is_builtin_module(path)) {
        it->enabled_modules |= myon_builtin_module_bit(path);
        if (find_module(it, path)) return;
        ModuleEntry *m = (ModuleEntry *)myon_xmalloc(sizeof(ModuleEntry));
        m->path = myon_strdup(path);
        m->alias = s->as.module_decl.alias ? myon_strdup(s->as.module_decl.alias) : NULL;
        m->loading = 0;
        m->ns = NULL;
        m->next = it->modules;
        it->modules = m;
        return;
    }
    if (strncmp(path, "external.", 9) == 0 || strcmp(path, "external") == 0) {
        /* legacy external module: external.util.math -> ./util/math.myon */
        load_external_module(it, s);
        return;
    }
    /* installed package module: resolved from myon.lock + .myon/packages/. */
    load_package_module(it, s);
}

/* Enable all builtin declarations before executing any imported module body. */
static void load_program_modules(Interp *it, Program *program) {
    for (int builtin = 1; builtin >= 0; builtin--)
        for (int i = 0; i < program->stmts.count; i++) {
            Stmt *stmt = program->stmts.items[i];
            if (stmt->kind == STMT_MODULE &&
                is_builtin_module(stmt->as.module_decl.path) == builtin)
                handle_module_decl(it, stmt);
        }
}

/*
 * Shared module-body loader used by both the legacy external resolver and the
 * installed-package resolver.  `file` is an already-resolved filesystem path;
 * `m` is the freshly-created ModuleEntry (its `loading` flag set, its `ns`
 * chosen).  Reads the file, lexes/parses it, executes its top level into the
 * module's target scope, and retains the AST for the interpreter's lifetime.
 * On any I/O / lex / parse failure this runtime_error()s (never returns).
 */
static void exec_module_file(Interp *it, Stmt *decl, ModuleEntry *m,
                             const char *file) {
    const char *path = decl->as.module_decl.path;
    Env *target = m->ns ? m->ns : it->global;

    FILE *f = fopen(file, "rb");
    if (!f) {
        char *fcopy = myon_strdup(file);
        runtime_error_free(it, decl->line, fcopy,
                      "cannot open module file '%s' for '%s'", fcopy, path);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); runtime_error(it, decl->line, "cannot read module '%s'", path); }
    char *src = (char *)myon_xmalloc((size_t)sz + 1);
    size_t n = fread(src, 1, (size_t)sz, f);
    src[n] = '\0';
    fclose(f);

    TokenList tl;
    if (!lexer_tokenize(src, &tl)) { free(src); runtime_error(it, decl->line, "lex error in module '%s'", path); }
    Program *mp = parser_parse(&tl);
    if (!mp) { token_list_free(&tl); free(src); runtime_error(it, decl->line, "parse error in module '%s'", path); }

    /* Retain before executing: recursive imports and module bodies can fail. */
    interp_retain_program(it, mp);
    token_list_free(&tl);
    free(src);

    /* Execute module top-level into the target scope.  Aliased modules use
     * their own namespace env (`m.<name>` access); unaliased ones flatten into
     * global (historical simple namespace model). */
    prescan(it, target, mp);
    load_program_modules(it, mp);
    for (int i = 0; i < mp->stmts.count; i++)
        if (mp->stmts.items[i]->kind != STMT_MODULE)
            exec_stmt(it, target, mp->stmts.items[i]);

    m->loading = 0;
}

/* Retain a parsed Program for the interpreter's lifetime so its AST stays
 * valid (function/struct values reference its nodes) and is freed once, at
 * interp_free() time.  Used for both the top-level program and every loaded
 * external module. */
static void interp_retain_program(Interp *it, Program *prog) {
    it->programs = (Program **)myon_xrealloc(
        it->programs, sizeof(Program *) * (it->program_count + 1));
    it->programs[it->program_count++] = prog;
}

/*
 * Resolve an external module's dotted path to a filesystem path.
 *
 *   external.util.math  ->  <base>/util/math.myon
 *
 * <base> is the directory of the importing script (it->script_dir) when known,
 * so a script runs identically no matter what the process's current working
 * directory is (fixes: `myon /elsewhere/main.myon` failing to find its own
 * sibling modules).  When no script directory is known (REPL / stdin) we fall
 * back to the current working directory ("./"), matching the old behaviour.
 *
 * Returns a freshly heap-allocated path string (caller frees), or NULL on
 * allocation failure.  The buffer is sized exactly for the input, so there is
 * no fixed length cap on how deep/long a dotted module path may be
 * (known-issues.md: "Module path length capped at 1024 bytes").
 */
static char *resolve_module_file(Interp *it, const char *path) {
    const char *p = path;
    if (strncmp(p, "external.", 9) == 0) p += 9;

    const char *base = (it->script_dir && it->script_dir[0]) ? it->script_dir : ".";

    /* Result is: "<base>" + "/" + path-with-dots-as-slashes + ".myon" + NUL. */
    size_t base_len = strlen(base);
    size_t path_len = strlen(p);
    size_t out_sz = base_len + 1 /* '/' */ + path_len + 5 /* ".myon" */ + 1;
    char *out = (char *)malloc(out_sz);
    if (!out) return NULL;

    size_t len = 0;
    memcpy(out + len, base, base_len); len += base_len;
    out[len++] = '/';
    for (const char *c = p; *c; c++)
        out[len++] = (*c == '.') ? '/' : *c;
    memcpy(out + len, ".myon", 5); len += 5;
    out[len] = '\0';
    return out;
}

static int load_external_module(Interp *it, Stmt *decl) {
    const char *path = decl->as.module_decl.path;

    ModuleEntry *existing = find_module(it, path);
    if (existing) {
        if (existing->loading)
            runtime_error(it, decl->line, "circular module import detected: '%s' (spec 14.5)", path);
        return 1; /* already loaded */
    }

    ModuleEntry *m = (ModuleEntry *)myon_xmalloc(sizeof(ModuleEntry));
    m->path = myon_strdup(path);
    m->alias = decl->as.module_decl.alias ? myon_strdup(decl->as.module_decl.alias) : NULL;
    m->loading = 1;
    /* Aliased imports get their own namespace scope (a child of global so the
     * module body can still see builtins); unaliased imports keep the flat
     * "everything lands in global" model for backward compatibility. */
    m->ns = m->alias ? env_new(it->global) : NULL;
    m->next = it->modules;
    it->modules = m;

    /* build file path relative to the importing script's directory */
    char *file = resolve_module_file(it, path);
    if (!file)
        runtime_error(it, decl->line, "out of memory resolving module path: '%s'", path);

    cleanup_register(it, file, free);
    exec_module_file(it, decl, m, file);
    cleanup_forget(it, file);
    free(file);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Installed package module resolver (spec §6.2)                        */
/* ------------------------------------------------------------------ */
/*
 * Resolve `module <pkg-module-path> as <alias>` against the project's
 * myon.lock and the on-disk .myon/packages tree.  The package that owns the
 * import is the locked package whose declared `module` namespace is the
 * LONGEST dotted-prefix of the requested path (spec §6.2: "package manifest が
 * 宣言した import path の最長一致で package module namespace を決める").  The
 * file read is always:
 *
 *     <project-root>/.myon/packages/<package-name>/modules/<path→slashes>.myon
 *
 * where <path→slashes> is the *full* requested dotted path with '.' turned
 * into '/'.  Every path segment is validated with pkg_fs_safe_component so a
 * crafted import can never escape the package directory (spec §6.2: reject
 * "..", absolute paths, separator injection; never resolve outside
 * .myon/packages/).
 *
 * Constraints enforced here (spec §6.2):
 *   - a package import MUST carry an alias;
 *   - the package must exist in myon.lock (unlocked packages are rejected);
 *   - cycles are caught via the shared ModuleEntry `loading` flag.
 */

/* True if dotted namespace `ns` is `path` itself or a dotted prefix of it,
 * i.e. the next char in `path` after `ns` is '.' (component boundary). */
static int module_ns_is_prefix(const char *ns, const char *path) {
    size_t n = strlen(ns);
    if (strncmp(path, ns, n) != 0) return 0;
    return path[n] == '\0' || path[n] == '.';
}

static int load_package_module(Interp *it, Stmt *decl) {
    const char *path  = decl->as.module_decl.path;
    const char *alias = decl->as.module_decl.alias;

    /* Already loaded (or loading -> cycle)? */
    ModuleEntry *existing = find_module(it, path);
    if (existing) {
        if (existing->loading)
            runtime_error(it, decl->line,
                          "circular package module import detected: '%s' (spec §6.2)", path);
        return 1;
    }

    /* Spec §6.2: package imports require an alias. */
    if (!alias)
        runtime_error(it, decl->line,
                      "package module '%s' must be imported with an alias "
                      "(module %s as <name>) (spec §6.2)", path, path);

    /* Discover the project root relative to the running script (not the
     * process CWD), then read the lockfile. */
    char *root_err = NULL;
    char *root = pkg_fs_find_project_root_from(it->script_dir, &root_err);
    if (!root) {
        char *msg = root_err ? root_err : myon_strdup("no project root");
        runtime_error_free(it, decl->line, msg,
                      "cannot import package module '%s': %s "
                      "(run `myon pkg install` in your project first)", path, msg);
    }

    char *lock_path = pkg_fs_join(root, "myon.lock");
    if (!pkg_fs_is_file(lock_path)) {
        char rp[4096]; snprintf(rp, sizeof(rp), "%s", root);
        free(lock_path); free(root);
        runtime_error(it, decl->line,
                      "cannot import package module '%s': no myon.lock in project '%s' "
                      "(run `myon pkg install` first)", path, rp);
    }

    PkgError perr; pkg_error_init(&perr);
    PkgLock *lock = pkg_lock_parse_file(lock_path, &perr);
    free(lock_path);
    if (!lock) {
        char ebuf[512];
        snprintf(ebuf, sizeof(ebuf), "%s",
                 (perr.message && perr.message[0]) ? perr.message : "invalid myon.lock");
        pkg_error_reset(&perr);
        free(root);
        runtime_error(it, decl->line,
                      "cannot import package module '%s': %s", path, ebuf);
    }

    /* Longest-prefix match of a locked package's declared module namespace. */
    const PkgLockEntry *best = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < lock->count; i++) {
        const PkgLockEntry *e = &lock->entries[i];
        if (e->module && module_ns_is_prefix(e->module, path)) {
            size_t l = strlen(e->module);
            if (!best || l > best_len) { best = e; best_len = l; }
        }
    }
    if (!best) {
        pkg_lock_free(lock);
        free(root);
        runtime_error(it, decl->line,
                      "package module '%s' is not provided by any locked package "
                      "(is it installed and declared in myon.toml?) (spec §6.2)", path);
    }

    char *pkg_name = myon_strdup(best->name);
    pkg_lock_free(lock);

    /* Build .../.myon/packages/<pkg>/modules and validate each requested
     * segment so nothing can escape the package directory. */
    char *dotmyon  = pkg_fs_join(root, PKG_DIR_DOTMYON);
    char *packages = pkg_fs_join(dotmyon, PKG_DIR_PACKAGES);
    char *pkgroot  = pkg_fs_join(packages, pkg_name);
    char *modules  = pkg_fs_join(pkgroot, "modules");
    free(dotmyon); free(packages); free(root);

    if (!pkg_fs_is_dir(pkgroot)) {
        char pn[256]; snprintf(pn, sizeof(pn), "%s", pkg_name);
        free(pkg_name); free(pkgroot); free(modules);
        runtime_error(it, decl->line,
                      "package '%s' is locked but not installed on disk "
                      "(run `myon pkg install`) (spec §6.2)", pn);
    }

    /* Turn the full dotted path into modules/<a>/<b>/.../<z>.myon, validating
     * every segment.  A copy of `path` is tokenised on '.'. */
    char *dup = myon_strdup(path);
    char *cur = modules; /* accumulates the file path; freed as we go */
    char *save = NULL;
    for (char *seg = strtok_r(dup, ".", &save); seg; seg = strtok_r(NULL, ".", &save)) {
        if (!pkg_fs_safe_component(seg)) {
            char pbad[256]; snprintf(pbad, sizeof(pbad), "%s", path);
            free(dup); free(cur); free(pkg_name); free(pkgroot);
            runtime_error(it, decl->line,
                          "illegal package module path component in '%s' (spec §6.2)", pbad);
        }
        char *next = pkg_fs_join(cur, seg);
        free(cur);
        cur = next;
    }
    free(dup);

    /* Append ".myon". */
    size_t clen = strlen(cur);
    char *file = (char *)myon_xmalloc(clen + 6);
    memcpy(file, cur, clen);
    memcpy(file + clen, ".myon", 6);
    free(cur);
    free(pkgroot);
    free(pkg_name);

    if (!pkg_fs_is_file(file)) {
        char fp[4096]; snprintf(fp, sizeof(fp), "%s", file);
        free(file);
        runtime_error(it, decl->line,
                      "package module '%s' has no source file '%s' (spec §6.2)", path, fp);
    }

    /* Register the module (aliased namespace, cycle flag) and load it. */
    ModuleEntry *m = (ModuleEntry *)myon_xmalloc(sizeof(ModuleEntry));
    m->path = myon_strdup(path);
    m->alias = myon_strdup(alias);
    m->loading = 1;
    m->ns = env_new(it->global);   /* package imports are always aliased */
    m->next = it->modules;
    it->modules = m;

    cleanup_register(it, file, free);
    exec_module_file(it, decl, m, file);
    cleanup_forget(it, file);
    free(file);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Pre-scan: hoist struct + function declarations                      */
/* ------------------------------------------------------------------ */

static void prescan(Interp *it, Env *env, Program *prog) {
    /* First pass: register structs so constructors/type refs resolve. */
    for (int i = 0; i < prog->stmts.count; i++) {
        Stmt *s = prog->stmts.items[i];
        if (s->kind == STMT_STRUCT) {
            StructDecl *sd = s->as.struct_decl;
            if (find_struct(it, sd->name))
                runtime_error(it, s->line, "redefinition of struct '%s'", sd->name);
            register_struct(it, sd);
        }
    }
    /* resolve parents + check field-name collisions (spec 14.6) */
    for (int i = 0; i < it->structs.count; i++) {
        StructDecl *sd = it->structs.items[i];
        if (sd->parent_name && !sd->parent) {
            sd->parent = find_struct(it, sd->parent_name);
            if (!sd->parent)
                runtime_error(it, 0, "struct '%s' extends unknown struct '%s'",
                              sd->name, sd->parent_name);
            /* field collision check */
            for (int a = 0; a < sd->field_count; a++)
                for (StructDecl *pc = sd->parent; pc; pc = pc->parent)
                    for (int b = 0; b < pc->field_count; b++)
                        if (strcmp(sd->fields[a].name, pc->fields[b].name) == 0)
                            runtime_error(it, 0,
                                "field '%s' in struct '%s' collides with parent (spec 14.6)",
                                sd->fields[a].name, sd->name);
        }
    }
    /* Second pass: hoist top-level functions so they can be mutually recursive. */
    for (int i = 0; i < prog->stmts.count; i++) {
        Stmt *s = prog->stmts.items[i];
        if (s->kind == STMT_FUNC) {
            const char *nm = s->as.func->name;
            if (!env_defined_local(env, nm))
                env_define(env, nm, value_func(s->as.func, env));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

/*
 * Script directory used to resolve external module imports.  Set once by
 * interpret_set_script_path() from main.c before a run; consumed when an
 * interpreter is created (interp_create / one-shot interpret()).  A module
 * global keeps the public API tiny and works for both the one-shot and
 * persistent (REPL) entry points, neither of which changes the running script
 * mid-execution.
 */
static char *g_script_dir = NULL;

/*
 * Script argument vector staged by main.c before a run and consumed when an
 * interpreter is created (interp_create / one-shot interpret()).  These are
 * borrowed pointers into the process argv (never freed here); the module
 * global mirrors g_script_dir's lifetime discipline.  Exposed to programs via
 * myon.argv().
 */
static char **g_script_argv = NULL;
static int    g_script_argc = 0;

/* Return a heap copy of the directory portion of `path`, or NULL when `path`
 * has no directory component (bare filename) or is NULL/empty. */
static char *dirname_dup(const char *path) {
    if (!path || !path[0]) return NULL;
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (!slash) return NULL;              /* no directory part */
    if (slash == path) return myon_strdup("/"); /* root: "/foo.myon" */
    size_t n = (size_t)(slash - path);
    char *d = (char *)myon_xmalloc(n + 1);
    memcpy(d, path, n);
    d[n] = '\0';
    return d;
}

void interpret_set_script_path(const char *script_path) {
    free(g_script_dir);
    g_script_dir = dirname_dup(script_path);
}

void interpret_set_script_args(char **argv, int argc) {
    /* Borrow the caller's vector (main.c keeps it alive for the whole run). */
    g_script_argv = argv;
    g_script_argc = (argc > 0) ? argc : 0;
}

Interp *interp_create(void) {
    Interp *it = (Interp *)myon_xmalloc(sizeof(Interp));
    memset(it, 0, sizeof(*it));
    it->global = env_new(NULL);
    it->script_dir = g_script_dir ? myon_strdup(g_script_dir) : NULL;
    it->script_argv = g_script_argv;   /* borrowed (see g_script_argv note) */
    it->script_argc = g_script_argc;
    return it;
}

void interp_free(Interp *it) {
    if (!it) return;
    ffi_callback_reset_all(); /* drop any live FFI callback slots (Phase4.1) */
    if (it->loop) event_loop_destroy(it->loop);
    cleanup_destroy(it);
    if (it->net) net_state_destroy(it->net);
    env_free(it->global);
    free(it->structs.items);
    if (it->ffi) ffi_state_free(it->ffi);
    ModuleEntry *m = it->modules;
    while (m) {
        ModuleEntry *n = m->next;
        if (m->ns) env_free(m->ns); /* aliased-module namespace scope */
        free(m->path); free(m->alias); free(m);
        m = n;
    }
    /* free retained program ASTs (structs/functions may reference their nodes,
     * so this only happens once the interpreter itself is torn down). */
    for (int i = 0; i < it->program_count; i++)
        program_free(it->programs[i]);
    free(it->programs);
    /* All completed and suspended call scopes have now been disposed. */
    free(it->cleanup_stack);
    free(it->script_dir);
    free(it);
    value_collect_cycles();
}

/*
 * Execute a program's top-level statements on `it`'s global scope.  Assumes a
 * error barrier for the current execution context is installed by the caller.
 * Returns 0 normally, 1 on the break/continue-outside-loop error.
 */
static int run_toplevel(Interp *it, Program *program) {
    /* load modules first (so external decls hoist) */
    load_program_modules(it, program);

    /* hoist struct/function declarations */
    prescan(it, it->global, program);

    for (int i = 0; i < program->stmts.count; i++) {
        Stmt *s = program->stmts.items[i];
        if (s->kind == STMT_MODULE) continue; /* already handled */
        Flow f = exec_stmt(it, it->global, s);
        if (f == FLOW_BREAK || f == FLOW_CONTINUE) {
            fprintf(stderr, "myon: runtime error: break/continue outside of a loop\n");
            return 1;
        }
        if (f == FLOW_RETURN) {
            /* top-level ret: ignore returned values */
            owned_values_release(it, it->ret_owner);
            it->ret_owner = NULL;
            it->ret_values = NULL; it->ret_count = 0;
        }
    }

    /* Phase5: drain any async tasks that were spawned but never awaited so
     * their side effects (prints, I/O) run to completion before we exit —
     * matching asyncio's "run until complete" at program end.  Daemon tasks
     * (e.g. an HTTP server accept loop) are ignored here so a script whose
     * foreground work is done can exit even though a server is still pending.
     * Any pending task result values are reclaimed by event_loop_destroy(). */
    if (it->loop)
        event_loop_run_until_foreground_done(it->loop);
    return 0;
}

int interp_run(Interp *it, Program *program) {
    /* retain the AST for the interpreter's lifetime */
    interp_retain_program(it, program);

    if (setjmp(*interp_error_buf(it))) {
        /* a runtime error aborted this program; interpreter stays alive.
         * Phase5.2: the erroring frame longjmped straight here without running
         * the per-frame call_depth decrements, so reset the counter for the
         * next program the (REPL) interpreter runs. */
        it->call_depth = 0;
        /* Known-issue #1: free every call frame the longjmp unwound past. */
        cleanup_unwind(it, 0);
        return 1;
    }
    /* Keep this frame alive until run_toplevel returns. MinGW's setjmp uses
     * its frame address for Windows unwinding; a sibling tail call would leave
     * a runtime error with a jump target in a frame that no longer exists. */
    volatile int rc = run_toplevel(it, program);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Step 6: myon_bridge_* — shared runtime seam for the MVM bytecode VM. */
/*                                                                       */
/* Every wrapper below simply forwards to the very same internal helper  */
/* the tree-walker uses (eval_binary, cast_to_*, call_stdlib,            */
/* call_method, ...), so `.myc` execution reuses the standard library    */
/* verbatim.  Nothing here is reachable from the tree-walking path, and  */
/* none of these functions alter interpreter state that the tree-walker  */
/* observes, so `.myon` behaviour is unchanged.                          */
/* ------------------------------------------------------------------ */

Interp *myon_bridge_interp_new(void)  { return interp_create(); }
void    myon_bridge_interp_free(Interp *it) { interp_free(it); }
void   *myon_bridge_error_buf(Interp *it) { return (void *)interp_error_buf(it); }
void myon_bridge_set_vm_caller(Interp *it, MyonVMCaller caller, void *context) {
    it->vm_caller = caller; it->vm_context = context;
}
void myon_bridge_own_resource(Interp *it, void *resource, void (*dispose)(void *)) {
    cleanup_register(it, resource, dispose);
}
void myon_bridge_release_resource(Interp *it, void *resource) {
    cleanup_forget(it, resource);
}
_Noreturn void myon_bridge_propagate_error(Interp *it) {
    longjmp(*interp_error_buf(it), 1);
}


Value myon_bridge_binary(Interp *it, int line, int op, Value l, Value r) {
    /* eval_binary consumes l and r (spec §2.2 strict typing). */
    return eval_binary(it, line, (OpKind)op, l, r);
}

Value myon_bridge_neg(Interp *it, int line, Value v) {
    return negate_value(it, line, v);
}

Value myon_bridge_not(Interp *it, int line, Value v) {
    if (v.type != TYPE_BOOL) {
        Type t = v.type; value_free(&v);
        runtime_error(it, line, "myon.not requires bool, got %s", type_name(t));
    }
    int b = v.as.b; value_free(&v);
    return value_bool(!b);
}

Value myon_bridge_cast_str(Interp *it, int line, Value v)  { return cast_to_str(it, line, v); }
Value myon_bridge_cast_int(Interp *it, int line, Value v)  { return cast_to_int(it, line, v); }
Value myon_bridge_cast_char(Interp *it, int line, Value v) { return cast_to_char(it, line, v); }

Value myon_bridge_make_error(Interp *it, int line, Value v) {
    if (v.type != TYPE_STR) {
        value_free(&v);
        runtime_error(it, line, "error() expects a str argument");
    }
    char *msg = myon_strdup(v.as.obj->as.str);
    value_free(&v);
    return value_error(msg);
}

/*
 * Build the minimal synthetic call Expr that call_stdlib / call_method read:
 * they only look at ->line and ->as.call.arg_count (argument *values* come
 * from the injected bridge vector via eval_arg()).  Everything else is left
 * zeroed so any stray access is an obvious NULL, not silent misbehaviour.
 */
static Expr bridge_make_call_expr(int line, int argc) {
    Expr e;
    memset(&e, 0, sizeof(e));
    e.kind = EXPR_CALL;
    e.line = line;
    e.as.call.arg_count = argc;
    e.as.call.args = NULL;       /* never dereferenced in bridge mode */
    e.as.call.arg_names = NULL;
    return e;
}

Value myon_bridge_call_function(Interp *it, Value fn, Value *args, int argc, int line) {
    Value *saved = it->bridge_args;
    int saved_argc = it->bridge_argc;
    it->bridge_args = NULL; it->bridge_argc = 0;
    Value result;
    if (fn.type == TYPE_FUNC && fn.as.obj->as.fn.decl && fn.as.obj->as.fn.decl->is_async)
        result = spawn_async_task(it, fn, args, argc);
    else result = call_function(it, line, fn, args, argc, NULL);
    it->bridge_args = saved; it->bridge_argc = saved_argc;
    return result;
}

int myon_bridge_call_native(Interp *it, const char *name,
                            Value *args, int argc, int line, Value *out) {
    if (strcmp(name, "myon.__package_import") == 0 || strcmp(name, "myon.__package_member") == 0) {
        if (argc != 2 || args[0].type != TYPE_STR || args[1].type != TYPE_STR)
            runtime_error(it, line, "invalid package instruction");
        if (!strcmp(name, "myon.__package_import")) {
            Stmt decl; memset(&decl, 0, sizeof(decl));
            decl.kind = STMT_MODULE; decl.line = line;
            decl.as.module_decl.path = args[0].as.obj->as.str;
            decl.as.module_decl.alias = args[1].as.obj->as.str;
            Value *saved = it->bridge_args; int count = it->bridge_argc;
            it->bridge_args = NULL; it->bridge_argc = 0;
            load_package_module(it, &decl);
            it->bridge_args = saved; it->bridge_argc = count;
            *out = value_void();
        } else {
            ModuleEntry *m = find_module_by_alias(it, args[0].as.obj->as.str);
            if (!m || !env_get(m->ns, args[1].as.obj->as.str, out))
                runtime_error(it, line, "package member '%s.%s' not found",
                    args[0].as.obj->as.str, args[1].as.obj->as.str);
        }
        return 1;
    }
    if (strcmp(name, "myon.__module_nested") == 0)
        runtime_error(it, line, "module declarations are only allowed at top level");
    if (strcmp(name, "myon.__module_enable") == 0 || strcmp(name, "myon.__module_require") == 0) {
        if (argc != 1 || args[0].type != TYPE_STR)
            runtime_error(it, line, "invalid builtin module instruction");
        const char *module = args[0].as.obj->as.str;
        uint32_t bit = myon_builtin_module_bit(module);
        if (!bit) runtime_error(it, line, "unknown builtin module '%s'", module);
        if (strcmp(name, "myon.__module_enable") == 0) it->enabled_modules |= bit;
        else require_module(it, line, module);
        *out = value_void();
        return 1;
    }
    require_native_module(it, line, name);
    Expr call = bridge_make_call_expr(line, argc);
    Value *saved_args = it->bridge_args;
    int    saved_argc = it->bridge_argc;
    it->bridge_args = args;      /* borrowed; eval_arg() hands back copies */
    it->bridge_argc = argc;

    int handled = 0;
    /* builtins that are not under call_stdlib (spec §4.14 general native op) */
    if (strcmp(name, "myon.len") == 0) {
        /* Internal helper synthesized by the MVM compiler for the
         * `for x in iterable` lowering (mvm_compiler.c): it needs the element
         * count of the iterable to bound the hidden index.  Mirror the
         * tree-walker's `for ... in` semantics exactly (interpreter STMT_FOR):
         * only arrays are iterable, and a non-array is the same runtime error.
         * This keeps `.myon` and `.myc` iterable-for behaviour identical. */
        Value a = eval_arg(it, NULL, &call, 0);
        if (a.type != TYPE_ARRAY) {
            Type t = a.type; value_free(&a);
            runtime_error(it, line,
                "myon.for ... myon.in requires an array, got %s", type_name(t));
        }
        *out = value_int((long long)a.as.obj->as.arr.count);
        value_free(&a);
        handled = 1;
    } else if (strcmp(name, "myon.print") == 0) {
        *out = builtin_print(it, NULL, &call);
        handled = 1;
    } else if (strcmp(name, "myon.println") == 0) {
        *out = builtin_println(it, NULL, &call);
        handled = 1;
    } else if (strcmp(name, "myon.write") == 0) {
        *out = builtin_write(it, NULL, &call);
        handled = 1;
    } else if (strcmp(name, "myon.eprint") == 0) {
        *out = builtin_eprint(it, NULL, &call);
        handled = 1;
    } else if (strcmp(name, "myon.flush") == 0) {
        *out = builtin_flush(it, NULL, &call);
        handled = 1;
    } else if (strcmp(name, "myon.is_tty") == 0) {
        *out = builtin_is_tty(it, NULL, &call);
        handled = 1;
    } else if (strcmp(name, "myon.argv") == 0) {
        *out = builtin_argv(it, NULL, &call);
        handled = 1;
    } else if (strcmp(name, "myon.input") == 0) {
        *out = builtin_input(it, NULL, &call);
        handled = 1;
    } else if (strncmp(name, "myon.", 5) == 0) {
        handled = call_stdlib(it, NULL, name, &call, out);
    }

    it->bridge_args = saved_args;
    it->bridge_argc = saved_argc;
    return handled;
}

void myon_bridge_call_method(Interp *it, Value recv, const char *method,
                             Value *args, int argc, int line, Value *out) {
    Expr call = bridge_make_call_expr(line, argc);
    Value *saved_args = it->bridge_args;
    int    saved_argc = it->bridge_argc;
    it->bridge_args = args;
    it->bridge_argc = argc;

    /* call_method borrows recv (does not free it); it reads args via eval_arg. */
    OwnedValues *owned = owned_values_new(it, 1);
    owned->values[owned->count++] = recv;
    *out = call_method(it, NULL, &call, recv, method);
    owned_values_release(it, owned);

    it->bridge_args = saved_args;
    it->bridge_argc = saved_argc;
}

/* ---- async/await seam for the MVM (spec §14.9) --------------------------- */
/*
 * Bridge-side async, structurally identical to the tree-walker's
 * async_task_entry / spawn_async_task / await_task above, but the task body is
 * a caller-supplied C callback (the VM's nested dispatch loop) rather than
 * call_function.  Running on the task's own ucontext stack means the whole VM
 * C call stack is parked at a suspend point and resumed intact.
 */
typedef struct {
    Interp       *it;
    MyonTaskBody  body;
    void         *ud;
    void        (*cleanup)(void *);
    Value        *result;
    Task         *task;      /* set after spawn */
} BridgeTaskCtx;

static void bridge_task_cleanup(void *ud) {
    BridgeTaskCtx *ctx = ud;
    if (ctx->cleanup) ctx->cleanup(ctx->ud);
    if (ctx->result) { value_free(ctx->result); free(ctx->result); }
    free(ctx);
}

/*
 * Coroutine entry for an MVM async task.  This whole function runs on the
 * task's own C stack, so every local here (`ctx`, `res`, `has_error`) is
 * private to this task and survives suspend/resume intact.  Crucially, the
 * body reports its result through *stack-local* out-params rather than a
 * shared global: with several tasks suspended and resumed out of order, a
 * global "current task" pointer is stale by the time a resumed body finishes,
 * which previously caused one task to record another task's return value.
 */
static void bridge_task_entry(void *ud) {
    BridgeTaskCtx *ctx = (BridgeTaskCtx *)ud;
    Interp *it = ctx->it;

    Value *res = (Value *)myon_xmalloc(sizeof(Value));
    *res = value_nil();
    ctx->result = res;
    int has_error = 0;

    /* Local error barrier: a runtime_error inside the body must not longjmp
     * across the coroutine boundary (mirrors async_task_entry). */
    jmp_buf saved;
    memcpy(&saved, interp_error_buf(it), sizeof(jmp_buf));
    int saved_depth = it->call_depth;
    it->call_depth = 0;
    uint64_t cleanup_at = cleanup_mark(it);

    if (setjmp(*interp_error_buf(it)) == 0) {
        ctx->body(ctx->ud, res, &has_error);   /* runs the nested VM loop */
    } else {
        /* body raised: free skipped frames, finish with an error result */
        cleanup_unwind(it, cleanup_at);
        value_free(res);
        *res = value_error(myon_strdup("async task failed"));
        has_error = 1;
    }

    it->call_depth = saved_depth;
    memcpy(interp_error_buf(it), &saved, sizeof(jmp_buf));

    event_loop_task_set_result(ctx->task, res, has_error);
    ctx->result = NULL;
}

Value myon_bridge_spawn_task(Interp *it, MyonTaskBody body, void *ud,
                             void (*cleanup)(void *)) {
    EventLoop *loop = ensure_loop(it);
    BridgeTaskCtx *ctx = (BridgeTaskCtx *)myon_xmalloc(sizeof(BridgeTaskCtx));
    ctx->it = it; ctx->body = body; ctx->ud = ud; ctx->task = NULL;
    ctx->cleanup = cleanup; ctx->result = NULL;
    Task *task = event_loop_spawn(loop, bridge_task_entry, ctx);
    event_loop_task_set_cleanup(task, bridge_task_cleanup);
    ctx->task = task;
    return value_task(task);
}

Value myon_bridge_await(Interp *it, int line, Value task_value) {
    /* `myon.await <plain value>` is a no-op that yields the value (spec §14.9
     * allows awaiting a non-task; matches the tree-walker). */
    if (task_value.type != TYPE_TASK) return task_value;
    return await_task(it, line, task_value);
}

void myon_bridge_drain_tasks(Interp *it) {
    /* Program-exit drain (spec §14.9): run every not-yet-awaited foreground
     * task to completion so its side effects happen, ignoring daemon tasks
     * (e.g. an HTTP accept loop).  Identical to interp_run's exit path. */
    if (it->loop)
        event_loop_run_until_foreground_done(it->loop);
}

int interpret(Program *program) {
    Interp it;
    memset(&it, 0, sizeof(it));
    it.global = env_new(NULL);
    it.script_dir = g_script_dir ? myon_strdup(g_script_dir) : NULL;
    it.script_argv = g_script_argv;   /* borrowed (see g_script_argv note) */
    it.script_argc = g_script_argc;

    int rc = 0;
    if (setjmp(it.on_error)) {
        rc = 1;
        it.call_depth = 0; /* Phase5.2: see interp_run() */
        cleanup_unwind(&it, 0); /* Known-issue #1: free unwound call frames */
    } else {
        rc = run_toplevel(&it, program);
    }

    ffi_callback_reset_all(); /* drop any live FFI callback slots (Phase4.1) */
    if (it.loop) event_loop_destroy(it.loop);
    cleanup_destroy(&it);
    if (it.net) net_state_destroy(it.net);
    env_free(it.global);
    free(it.structs.items);
    if (it.ffi) ffi_state_free(it.ffi);
    ModuleEntry *m = it.modules;
    while (m) {
        ModuleEntry *n = m->next;
        if (m->ns) env_free(m->ns); /* aliased-module namespace scope */
        free(m->path); free(m->alias); free(m);
        m = n;
    }
    /* NOTE: for the one-shot path the caller (main.c) owns and frees the
     * top-level `program`.  Any *external module* programs, however, were
     * retained via interp_retain_program() and must be freed here (their
     * function/struct values in it.global have already been released above). */
    for (int i = 0; i < it.program_count; i++)
        program_free(it.programs[i]);
    free(it.programs);
    free(it.cleanup_stack); /* Known-issue #1: backing array (see interp_free) */
    free(it.script_dir);
    value_collect_cycles();
    return rc;
}
