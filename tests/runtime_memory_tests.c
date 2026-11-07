/* Regression tests for known-issues 11/12. No timing/RSS thresholds: assert
 * ownership and reclamation directly, including shared live graph edges. */
#include "value.h"
#include "common.h"
#include "event_loop.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void cycles(void) {
    size_t base = value_live_allocations();
    Value a = value_struct("Node", NULL);
    Value b = value_struct("Node", NULL);
    struct_add_field(&a, "next", value_copy(&b));
    struct_add_field(&b, "next", value_copy(&a));
    Value shared = value_str(myon_strdup("live"));
    struct_add_field(&b, "shared", value_copy(&shared));
    value_free(&a);
    value_collect_cycles(); /* b still roots the whole cycle */
    assert(value_live_allocations() == base + 3);
    assert(struct_field_ptr(&b, "next")->as.obj != NULL);
    value_free(&b);
    value_collect_cycles();
    assert(value_live_allocations() == base + 1);
    assert(shared.as.obj->refcount == 1);
    value_free(&shared);

    a = value_array(NULL);
    b = value_map(NULL, NULL);
    array_push(&a, value_copy(&b));
    map_set(&b, value_int(0), value_copy(&a));
    value_free(&a); value_free(&b);
    value_collect_cycles();
    assert(value_live_allocations() == base);

    /* Shared MVM upvalue: external cell ref keeps both functions alive. */
    a = value_func(NULL, NULL);
    b = value_func(NULL, NULL);
    UpvalueCell *cell = upvalue_cell_new(value_copy(&a));
    a.as.obj->as.fn.upvalues = myon_xmalloc(sizeof(cell));
    a.as.obj->as.fn.upvalues[0] = upvalue_cell_ref(cell);
    a.as.obj->as.fn.upvalue_count = 1;
    b.as.obj->as.fn.upvalues = myon_xmalloc(sizeof(cell));
    b.as.obj->as.fn.upvalues[0] = upvalue_cell_ref(cell);
    b.as.obj->as.fn.upvalue_count = 1;
    value_free(&a); value_free(&b);
    value_collect_cycles();
    assert(value_live_allocations() == base + 2); /* a + cell */
    upvalue_cell_unref(cell);
    value_collect_cycles();
    assert(value_live_allocations() == base);

    /* Bound receivers are owning edges too. */
    a = value_struct("Receiver", NULL);
    b = value_func(NULL, NULL);
    b.as.obj->as.fn.bound_self = myon_xmalloc(sizeof(Value));
    *b.as.obj->as.fn.bound_self = value_copy(&a);
    struct_add_field(&a, "method", value_copy(&b));
    value_free(&a); value_free(&b);
    value_collect_cycles();
    assert(value_live_allocations() == base);

    for (int i = 0; i < 20000; i++) {
        a = value_struct("Cycle", NULL);
        struct_add_field(&a, "self", value_copy(&a));
        value_free(&a);
    }
    assert(value_live_allocations() < base + 1024); /* automatic collection */
    value_collect_cycles();
    assert(value_live_allocations() == base);
}

static int disposed, entered;
static void dispose(void *p) { disposed++; free(p); }
static void entry(void *ud) {
    Task *self = event_loop_current(ud);
    int *result = malloc(sizeof(int));
    *result = 42;
    entered++;
    event_loop_task_set_result(self, result, 0);
}
static void tasks(void) {
    if (!event_loop_supported()) return;
    EventLoop *loop = event_loop_create();
    event_loop_set_result_cleanup(loop, dispose);
    Task *held = event_loop_spawn(loop, entry, loop);
    event_loop_task_ref(held);
    assert(event_loop_run_once(loop) == 0);
    assert(event_loop_task_done(held));
    assert(disposed == 0);
    for (int i = 0; i < 20000; i++) {
        event_loop_spawn(loop, entry, loop); /* no language handle */
        assert(event_loop_run_once(loop) == 0);
        assert(disposed == i + 1);
    }
    /* Finished metadata/result survives both reaping and loop destruction. */
    event_loop_destroy(loop);
    assert(*(int *)event_loop_task_result(held) == 42);
    event_loop_task_unref(held);
    assert(disposed == entered);
}
static void free_value_result(void *p) { value_free(p); free(p); }
static void result_cycle_entry(void *ud) {
    Value *handle = ud;
    Value *result = malloc(sizeof(Value));
    *result = value_copy(handle);
    event_loop_task_set_result(handle->as.obj->as.task.task, result, 0);
}
static void task_result_cycle(void) {
    if (!event_loop_supported()) return;
    size_t base = value_live_allocations();
    EventLoop *loop = event_loop_create();
    event_loop_set_result_cleanup(loop, free_value_result);
    Value handle = value_nil();
    Task *t = event_loop_spawn(loop, result_cycle_entry, &handle);
    handle = value_task(t);
    event_loop_run_until_all_done(loop);
    value_free(&handle); /* task result -> handle -> task result */
    value_collect_cycles();
    assert(value_live_allocations() == base);
    event_loop_destroy(loop);
}

static int userdata_disposed;
typedef struct {
    EventLoop *loop;
    int wait;
    Task *target;
    Value owned;
} OwnedTask;
static void owned_cleanup(void *ud) {
    OwnedTask *ctx = ud;
    value_free(&ctx->owned);
    userdata_disposed++;
    free(ctx);
}
static void owned_entry(void *ud) {
    OwnedTask *ctx = ud;
    if (ctx->target) event_loop_wait_task(ctx->loop, ctx->target);
    else if (ctx->wait) event_loop_sleep_ms(ctx->loop, 60000);
}
static Task *spawn_owned(EventLoop *loop, int wait, Task *target) {
    OwnedTask *ctx = malloc(sizeof(*ctx));
    ctx->loop = loop; ctx->wait = wait; ctx->target = target;
    ctx->owned = value_str(myon_strdup("task userdata"));
    Task *task = event_loop_spawn(loop, owned_entry, ctx);
    event_loop_task_set_cleanup(task, owned_cleanup);
    return task;
}
static void userdata_lifetime(void) {
    if (!event_loop_supported()) return;
    size_t base = value_live_allocations();
    EventLoop *loop = event_loop_create();
    spawn_owned(loop, 0, NULL);
    assert(event_loop_run_once(loop) == 0);
    assert(userdata_disposed == 1);
    Task *sleeping = spawn_owned(loop, 1, NULL);
    event_loop_task_ref(sleeping);
    assert(event_loop_run_once(loop) == 1);
    spawn_owned(loop, 0, sleeping);
    assert(event_loop_run_once(loop) == 2);
    spawn_owned(loop, 0, NULL); /* destroy before the entry ever runs */
    assert(userdata_disposed == 1);
    event_loop_destroy(loop);
    assert(userdata_disposed == 4);
    assert(event_loop_task_done(sleeping));
    event_loop_task_unref(sleeping); /* cleanup must not run a second time */
    assert(userdata_disposed == 4);
    value_collect_cycles();
    assert(value_live_allocations() == base);
}

int main(void) {
    cycles();
    tasks();
    task_result_cycle();
    userdata_lifetime();
    puts("runtime memory tests passed");
    return 0;
}
