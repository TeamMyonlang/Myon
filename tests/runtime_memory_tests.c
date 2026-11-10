/* Regression tests for known-issues 11/12. No timing/RSS thresholds: assert
 * ownership and reclamation directly, including shared live graph edges. */
#include "value.h"
#include "env.h"
#include "common.h"
#include "event_loop.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

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

static void map_index(void) {
    size_t base = value_live_allocations();
    Value m = value_map(NULL, NULL), got;
    for (int i = 0; i < 20000; i++) map_set(&m, value_int(i), value_int(i * 3));
    MapData *data = &m.as.obj->as.map;
    assert(data->count == 20000 && data->bucket_count >= 20000);
    int collisions = 0;
    for (size_t i = 0; i < data->bucket_count; i++)
        if (data->buckets[i] && data->buckets[i]->hash_next) collisions++;
    assert(collisions > 0);
    int order = 0;
    for (MapEntry *e = data->head; e; e = e->next) assert(e->key.as.i == order++);
    assert(order == 20000);
    for (int i = 0; i < 20000; i++) {
        Value key = value_int(i);
        assert(map_get(&m, &key, &got) && got.as.i == i * 3);
        value_free(&got);
        if (i % 2) assert(map_delete(&m, &key));
    }
    assert(data->count == 10000);
    order = 0;
    for (MapEntry *e = data->head; e; e = e->next) { assert(e->key.as.i == order); order += 2; }
    for (int i = 1; i < 20000; i += 2) map_set(&m, value_int(i), value_int(-i));
    map_set(&m, value_int(0), value_int(99));
    assert(data->head->key.as.i == 0 && data->tail->key.as.i == 19999);
    Value key = value_int(0); assert(map_delete(&m, &key));
    key = value_int(19999); assert(map_delete(&m, &key));
    key = value_int(1000); assert(map_delete(&m, &key));
    assert(!map_delete(&m, &key) && !map_has(&m, &key));
    assert(data->head->prev == NULL && data->tail->next == NULL && data->count == 19997);
    value_free(&m);

    m = value_map(NULL, NULL);
    for (int i = 0; i < 1000; i++) {
        char name[32]; snprintf(name, sizeof(name), "key-%d", i);
        map_set(&m, value_str(myon_strdup(name)), value_int(i));
    }
    for (int i = 999; i >= 0; i--) {
        char name[32]; snprintf(name, sizeof(name), "key-%d", i);
        Value k = value_str(myon_strdup(name));
        assert(map_get(&m, &k, &got) && got.as.i == i);
        value_free(&got);
        if (i % 3 == 0) assert(map_delete(&m, &k));
        value_free(&k);
    }
    assert(m.as.obj->as.map.count == 666);
    value_free(&m);

    /* Preserve the existing value_equal contract, including type distinctions,
     * float signed zero, and identity keys whose contents can change. */
    m = value_map(NULL, NULL);
    map_set(&m, value_float(0.0), value_int(1));
    map_set(&m, value_float(-0.0), value_int(2));
    key = value_float(0.0);
    assert(map_get(&m, &key, &got) && got.as.i == 2);
    value_free(&got);
    map_set(&m, value_int(0), value_int(3));
    map_set(&m, value_bool(0), value_int(4));
    map_set(&m, value_str(myon_strdup("same")), value_int(5));
    map_set(&m, value_char(myon_strdup("same")), value_int(6));
    map_set(&m, value_error(myon_strdup("same")), value_int(7));
    key = value_str(myon_strdup("same"));
    assert(map_get(&m, &key, &got) && got.as.i == 5);
    value_free(&got); value_free(&key);
    key = value_array(NULL);
    map_set(&m, value_copy(&key), value_int(8));
    array_push(&key, value_int(42));
    assert(map_get(&m, &key, &got) && got.as.i == 8);
    value_free(&got); value_free(&key);
    assert(m.as.obj->as.map.count == 7);
    value_free(&m);

    /* A dense independent oracle exercises replacement/deletion/lookup. */
    m = value_map(NULL, NULL);
    int present[257] = {0};
    long long expected[257] = {0};
    size_t count = 0;
    uint32_t rng = 7;
    for (int i = 0; i < 60000; i++) {
        rng = rng * UINT32_C(1664525) + UINT32_C(1013904223);
        unsigned int k = (rng >> 8) % 257, operation = rng % 3;
        key = value_int(k);
        if (operation == 0) {
            map_set(&m, key, value_int(i));
            if (!present[k]) count++;
            present[k] = 1; expected[k] = i;
        } else if (operation == 1) {
            assert(map_delete(&m, &key) == present[k]);
            if (present[k]) count--;
            present[k] = 0;
        } else {
            assert(map_get(&m, &key, &got) == present[k]);
            if (present[k]) { assert(got.as.i == expected[k]); value_free(&got); }
        }
        assert(m.as.obj->as.map.count == count);
    }
    value_free(&m);
    m = value_map(NULL, NULL);
    map_set(&m, value_copy(&m), value_copy(&m));
    value_free(&m);
    value_collect_cycles();
    assert(value_live_allocations() == base);
}

static void closure_environments(void) {
    size_t base = value_live_allocations();
    Env *parent = env_new(NULL);
    assert(env_define(parent, "label", value_str(myon_strdup("alive"))));
    Env *child = env_new(parent);
    Value first = value_func(NULL, child);
    Value second = value_func(NULL, child);
    assert(env_define(child, "self", value_copy(&first)));
    assert(env_define(child, "sibling", value_copy(&second)));
    env_free(child);
    env_free(parent);
    value_free(&first);
    value_collect_cycles(); /* second roots child, parent and both functions */
    Value label;
    assert(env_get(second.as.obj->as.fn.closure, "label", &label));
    assert(strcmp(label.as.obj->as.str, "alive") == 0);
    value_free(&label);
    assert(env_set(second.as.obj->as.fn.closure, "label", value_str(myon_strdup("changed"))));
    assert(value_live_allocations() == base + 5);
    value_free(&second);
    value_collect_cycles();
    assert(value_live_allocations() == base);

    /* A cycle's outgoing edge to an externally-owned scope must drop once. */
    parent = env_new(NULL);
    child = env_new(parent);
    first = value_func(NULL, child);
    assert(env_define(child, "self", first));
    env_free(child);
    value_collect_cycles();
    assert(value_live_allocations() == base + 1);
    assert(parent->refcount == 1);
    env_free(parent);
    assert(value_live_allocations() == base);

    /* Imported aliases participate in the same environment/cell cycle graph. */
    parent = env_new(NULL);
    first = value_func(NULL, NULL);
    UpvalueCell *alias = upvalue_cell_new(value_nil());
    alias->alias_env = env_ref(parent);
    alias->alias_name = myon_strdup("callback");
    first.as.obj->as.fn.upvalues = myon_xmalloc(sizeof(alias));
    first.as.obj->as.fn.upvalues[0] = alias;
    first.as.obj->as.fn.upvalue_count = 1;
    assert(env_define(parent, "callback", first));
    env_free(parent);
    value_collect_cycles();
    assert(value_live_allocations() == base);

    for (int i = 0; i < 10000; i++) {
        parent = env_new(NULL);
        child = env_new(parent);
        first = value_func(NULL, child);
        assert(env_define(parent, "callback", first));
        env_free(child);
        env_free(parent);
    }
    assert(value_live_allocations() < base + 1024);
    value_collect_cycles();
    assert(value_live_allocations() == base);
}

int main(void) {
    closure_environments();
    map_index();
    cycles();
    tasks();
    task_result_cycle();
    userdata_lifetime();
    puts("runtime memory tests passed");
    return 0;
}
