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

#include "value.h"
#include "ast.h"
#include "env.h"
#include "event_loop.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Float formatting                                                    */
/* ------------------------------------------------------------------ */

/* Render `d` into `buf` using the *shortest* decimal that round-trips back to
 * the same double.  We try %.15g / %.16g / %.17g in order (17 significant
 * digits is always enough for IEEE-754 binary64) and keep the first one that
 * reparses exactly.  A finite value with no '.', 'e' or 'n'/'i' gets a ".0"
 * suffix so it still reads back as a float rather than an int. */
static void format_float(char *buf, size_t cap, double d) {
    if (isnan(d)) { snprintf(buf, cap, "nan"); return; }
    if (isinf(d)) { snprintf(buf, cap, d < 0 ? "-inf" : "inf"); return; }
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, cap, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    if (!strpbrk(buf, ".eEnN")) {
        size_t len = strlen(buf);
        if (len + 2 < cap) { buf[len] = '.'; buf[len + 1] = '0'; buf[len + 2] = '\0'; }
    }
}

/* ------------------------------------------------------------------ */
/* Primitive constructors                                              */
/* ------------------------------------------------------------------ */

Value value_int(long long v)   { Value x; x.type = TYPE_INT;   x.as.i = v; return x; }
Value value_float(double v)    { Value x; x.type = TYPE_FLOAT; x.as.f = v; return x; }
Value value_bool(int v)        { Value x; x.type = TYPE_BOOL;  x.as.b = v ? 1 : 0; return x; }
Value value_nil(void)          { Value x; x.type = TYPE_NIL;   x.as.obj = NULL; return x; }
Value value_void(void)         { Value x; x.type = TYPE_VOID;  x.as.i = 0; return x; }

/* ------------------------------------------------------------------ */
/* Object allocation                                                   */
/* ------------------------------------------------------------------ */

static ValueGC *gc_head;
static size_t gc_live, gc_since_collect, gc_threshold = 256;
static int gc_collecting;

static void gc_register(ValueGC *g, void *owner, int *refs, ValueGCKind kind) {
    memset(g, 0, sizeof(*g));
    g->owner = owner; g->refs = refs; g->kind = kind;
    g->next = gc_head;
    if (gc_head) gc_head->prev = g;
    gc_head = g;
    gc_live++;
}
static void gc_unregister(ValueGC *g) {
    if (g->prev) g->prev->next = g->next;
    else gc_head = g->next;
    if (g->next) g->next->prev = g->prev;
    gc_live--;
}
static void gc_before_alloc(void) {
    /* Amortize full graph scans over allocation volume, including live heaps. */
    if (++gc_since_collect >= gc_threshold) {
        value_collect_cycles();
        gc_since_collect = 0;
        gc_threshold = gc_live + 256;
    }
}
size_t value_live_allocations(void) { return gc_live; }

static Obj *obj_new(ObjKind kind) {
    gc_before_alloc();
    Obj *o = (Obj *)myon_xmalloc(sizeof(Obj));
    memset(o, 0, sizeof(Obj));
    o->kind = kind;
    o->refcount = 1;
    gc_register(&o->gc, o, &o->refcount, GC_OBJECT);
    return o;
}

/* Environments are graph nodes: active scopes own a reference, closures own
 * their defining scope, and children own their parent. Keeping their lifecycle
 * here lets the collector reclaim Env <-> function/container cycles together. */
Env *env_ref(Env *env) {
    if (env) env->refcount++;
    return env;
}
Env *env_new(Env *parent) {
    gc_before_alloc();
    Env *env = myon_xmalloc(sizeof(*env));
    memset(env, 0, sizeof(*env));
    env->parent = env_ref(parent);
    env->refcount = 1;
    gc_register(&env->gc, env, &env->refcount, GC_ENV);
    return env;
}
static void env_destroy_payload(Env *env) {
    Binding *b = env->head;
    while (b) {
        Binding *next = b->next;
        myon_free(b->name);
        value_free(&b->value);
        myon_free(b);
        b = next;
    }
    env_free(env->parent);
}
void env_free(Env *env) {
    if (!env || (gc_collecting && !env->gc.marked)) return;
    if (--env->refcount == 0) {
        gc_unregister(&env->gc);
        env_destroy_payload(env);
        myon_free(env);
    }
}

static Value obj_value(Type t, Obj *o) {
    Value x; x.type = t; x.as.obj = o; return x;
}

Value value_str(char *s) {
    Obj *o = obj_new(OBJ_STR);
    o->as.str = s;
    return obj_value(TYPE_STR, o);
}
Value value_char(char *s) {
    Obj *o = obj_new(OBJ_STR);
    o->as.str = s;
    return obj_value(TYPE_CHAR, o);
}
Value value_error(char *m) {
    Obj *o = obj_new(OBJ_STR);
    o->as.str = m;
    return obj_value(TYPE_ERROR, o);
}

Value value_array(TypeSpec *elem_type) {
    Obj *o = obj_new(OBJ_ARRAY);
    o->as.arr.items = NULL;
    o->as.arr.count = 0;
    o->as.arr.capacity = 0;
    o->as.arr.elem_type = elem_type;
    return obj_value(TYPE_ARRAY, o);
}

Value value_map(TypeSpec *key_type, TypeSpec *val_type) {
    Obj *o = obj_new(OBJ_MAP);
    o->as.map.head = NULL;
    o->as.map.key_type = key_type;
    o->as.map.val_type = val_type;
    return obj_value(TYPE_MAP, o);
}

Value value_struct(const char *type_name, StructDecl *decl) {
    Obj *o = obj_new(OBJ_STRUCT);
    o->as.st.type_name = myon_strdup(type_name);
    o->as.st.field_names = NULL;
    o->as.st.field_vals = NULL;
    o->as.st.field_count = 0;
    o->as.st.decl = decl;
    o->as.st.tparam_names = NULL;
    o->as.st.tparam_types = NULL;
    o->as.st.tparam_count = 0;
    return obj_value(TYPE_STRUCT, o);
}

Value value_func(FuncDecl *decl, Env *closure) {
    Obj *o = obj_new(OBJ_FUNC);
    o->as.fn.decl = decl;
    o->as.fn.closure = env_ref(closure);
    o->as.fn.is_bound = 0;
    o->as.fn.bound_self = NULL;
    o->as.fn.mvm_chunk = 0;
    o->as.fn.mvm_is_async = 0;
    o->as.fn.upvalues = NULL;
    o->as.fn.upvalue_count = 0;
    return obj_value(TYPE_FUNC, o);
}

/* ---- MVM upvalue cells (boxed captured variables, spec §7.3) ---- */
UpvalueCell *upvalue_cell_new(Value initial) {
    gc_before_alloc();
    UpvalueCell *c = (UpvalueCell *)myon_xmalloc(sizeof(UpvalueCell));
    c->value = initial;   /* takes ownership */
    c->refcount = 1;
    gc_register(&c->gc, c, &c->refcount, GC_UPVALUE);
    return c;
}

UpvalueCell *upvalue_cell_ref(UpvalueCell *c) {
    if (c) c->refcount++;
    return c;
}

void upvalue_cell_unref(UpvalueCell *c) {
    if (!c || (gc_collecting && !c->gc.marked)) return;
    if (--c->refcount <= 0) {
        gc_unregister(&c->gc);
        value_free(&c->value);
        myon_free(c);
    }
}

Value value_task(void *task) {
    Obj *o = obj_new(OBJ_TASK);
    event_loop_task_ref((Task *)task);
    o->as.task.task = task;
    return obj_value(TYPE_TASK, o);
}

/* ------------------------------------------------------------------ */
/* Reference counting                                                  */
/* ------------------------------------------------------------------ */

static int is_obj(const Value *v) {
    switch (v->type) {
        case TYPE_STR: case TYPE_CHAR: case TYPE_ERROR:
        case TYPE_ARRAY: case TYPE_MAP: case TYPE_STRUCT: case TYPE_FUNC:
        case TYPE_TASK:
            return 1;
        default: return 0;
    }
}

Value value_copy(const Value *v) {
    Value out = *v;
    if (is_obj(v) && v->as.obj)
        v->as.obj->refcount++;
    return out;
}

static void obj_destroy_payload(Obj *o) {
    switch (o->kind) {
        case OBJ_STR:
            myon_free(o->as.str);
            break;
        case OBJ_ARRAY:
            for (int i = 0; i < o->as.arr.count; i++)
                value_free(&o->as.arr.items[i]);
            myon_free(o->as.arr.items);
            typespec_free(o->as.arr.elem_type);
            break;
        case OBJ_MAP: {
            MapEntry *e = o->as.map.head;
            while (e) {
                MapEntry *n = e->next;
                value_free(&e->key);
                value_free(&e->val);
                myon_free(e);
                e = n;
            }
            myon_free(o->as.map.buckets);
            typespec_free(o->as.map.key_type);
            typespec_free(o->as.map.val_type);
            break;
        }
        case OBJ_STRUCT:
            myon_free(o->as.st.type_name);
            for (int i = 0; i < o->as.st.field_count; i++) {
                myon_free(o->as.st.field_names[i]);
                value_free(&o->as.st.field_vals[i]);
            }
            myon_free(o->as.st.field_names);
            myon_free(o->as.st.field_vals);
            for (int i = 0; i < o->as.st.tparam_count; i++) {
                myon_free(o->as.st.tparam_names[i]);
                typespec_free(o->as.st.tparam_types[i]);
            }
            myon_free(o->as.st.tparam_names);
            myon_free(o->as.st.tparam_types);
            break;
        case OBJ_FUNC:
            if (o->as.fn.bound_self) {
                value_free(o->as.fn.bound_self);
                myon_free(o->as.fn.bound_self);
            }
            /* MVM closures own a ref on each captured upvalue cell (spec §7.3). */
            if (o->as.fn.upvalues) {
                for (int i = 0; i < o->as.fn.upvalue_count; i++)
                    upvalue_cell_unref(o->as.fn.upvalues[i]);
                myon_free(o->as.fn.upvalues);
            }
            env_free(o->as.fn.closure);
            /* The declaration AST is retained by the interpreter. */
            break;
        case OBJ_TASK:
            event_loop_task_unref((Task *)o->as.task.task);
            break;
    }
}

static void obj_free(Obj *o) {
    gc_unregister(&o->gc);
    obj_destroy_payload(o);
    myon_free(o);
}

void value_free(Value *v) {
    if (!v) return;
    if (is_obj(v) && v->as.obj) {
        if (!(gc_collecting && !v->as.obj->gc.marked) &&
            --v->as.obj->refcount == 0)
            obj_free(v->as.obj);
        v->as.obj = NULL;
    }
}

/* Visit strong edges only. ASTs are borrowed; environments and upvalue cells
 * are distinct graph nodes so shared captures are counted once per owner. */
static void gc_value_edge(Value *v, void (*visit)(ValueGC *, void *), void *ud) {
    if (is_obj(v) && v->as.obj) visit(&v->as.obj->gc, ud);
}
static void gc_edges(ValueGC *g, void (*visit)(ValueGC *, void *), void *ud) {
    if (g->kind == GC_UPVALUE) {
        gc_value_edge(&((UpvalueCell *)g->owner)->value, visit, ud);
        return;
    }
    if (g->kind == GC_ENV) {
        Env *env = g->owner;
        if (env->parent) visit(&env->parent->gc, ud);
        for (Binding *b = env->head; b; b = b->next) gc_value_edge(&b->value, visit, ud);
        return;
    }
    Obj *o = (Obj *)g->owner;
    switch (o->kind) {
    case OBJ_ARRAY:
        for (int i = 0; i < o->as.arr.count; i++)
            gc_value_edge(&o->as.arr.items[i], visit, ud);
        break;
    case OBJ_MAP:
        for (MapEntry *e = o->as.map.head; e; e = e->next) {
            gc_value_edge(&e->key, visit, ud);
            gc_value_edge(&e->val, visit, ud);
        }
        break;
    case OBJ_STRUCT:
        for (int i = 0; i < o->as.st.field_count; i++)
            gc_value_edge(&o->as.st.field_vals[i], visit, ud);
        break;
    case OBJ_FUNC:
        if (o->as.fn.closure) visit(&o->as.fn.closure->gc, ud);
        if (o->as.fn.bound_self) gc_value_edge(o->as.fn.bound_self, visit, ud);
        for (int i = 0; i < o->as.fn.upvalue_count; i++)
            if (o->as.fn.upvalues[i]) visit(&o->as.fn.upvalues[i]->gc, ud);
        break;
    case OBJ_TASK: {
        Task *t = (Task *)o->as.task.task;
        /* Active tasks and C waiters root their result independently. Once
         * only this language handle owns it, its result is an internal edge. */
        if (event_loop_task_result_exclusive(t)) {
            Value *result = (Value *)event_loop_task_result(t);
            if (result) gc_value_edge(result, visit, ud);
        }
        break;
    }
    default: break;
    }
}
static void gc_subtract(ValueGC *g, void *ud) { (void)ud; g->trial--; }
static void gc_mark(ValueGC *g, void *ud) {
    ValueGC **work = (ValueGC **)ud;
    if (g->marked) return;
    g->marked = 1;
    g->work = *work;
    *work = g;
}
void value_collect_cycles(void) {
    if (gc_collecting) return;
    gc_collecting = 1;
    for (ValueGC *g = gc_head; g; g = g->next) {
        g->trial = *g->refs;
        g->marked = 0;
    }
    for (ValueGC *g = gc_head; g; g = g->next) gc_edges(g, gc_subtract, NULL);
    ValueGC *work = NULL;
    for (ValueGC *g = gc_head; g; g = g->next)
        if (g->trial > 0) gc_mark(g, &work);
    while (work) {
        ValueGC *g = work;
        work = g->work;
        gc_edges(g, gc_mark, &work);
    }
    /* Detach dead nodes from the registry while their storage remains valid. */
    ValueGC *dead = NULL;
    for (ValueGC *g = gc_head; g;) {
        ValueGC *next = g->next;
        if (!g->marked) {
            gc_unregister(g);
            g->work = dead; dead = g;
        }
        g = next;
    }
    /* Suppress decrements between dead nodes during payload destruction, and
     * delay freeing node storage until every edge has been released. */
    for (ValueGC *g = dead; g; g = g->work) {
        if (g->kind == GC_UPVALUE) value_free(&((UpvalueCell *)g->owner)->value);
        else if (g->kind == GC_ENV) env_destroy_payload((Env *)g->owner);
        else obj_destroy_payload((Obj *)g->owner);
    }
    while (dead) {
        ValueGC *next = dead->work;
        myon_free(dead->owner);
        dead = next;
    }
    gc_collecting = 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

const char *value_type_name(const Value *v) {
    if (v->type == TYPE_STRUCT && v->as.obj)
        return v->as.obj->as.st.type_name;
    return type_name(v->type);
}

static char *append(char *dst, const char *src) {
    size_t a = dst ? strlen(dst) : 0, b = strlen(src);
    dst = (char *)myon_xrealloc(dst, a + b + 1);
    memcpy(dst + a, src, b + 1);
    return dst;
}

/* ------------------------------------------------------------------ */
/* Rendering guard (known-issues #8)                                   */
/* ------------------------------------------------------------------ */

/*
 * value_to_cstr() recurses through arrays / maps / structs.  Reference
 * counting lets a container reach itself (`a.next = a`, a map holding the
 * array that holds it, ...), and without a guard that recursion never
 * terminates: the C stack is exhausted and the process dies with SIGSEGV
 * before anything is printed.  Both engines share this renderer (the MVM
 * reaches it through the bridge), so the guard fixes both at once.
 *
 * Two independent limits:
 *
 *   * A *path* set of the container objects currently being rendered.  A value
 *     that is already on the path is a genuine cycle and renders as
 *     MYON_TOSTR_CYCLE instead of recursing.  This is a path set, not a global
 *     "seen" set, so a DAG (the same array appearing twice side by side) still
 *     prints in full both times -- only real back-edges are cut.
 *
 *   * A depth cap, so a legitimately (but pathologically) deep acyclic nesting
 *     -- 100k nested arrays built in a loop -- cannot exhaust the C stack
 *     either.  Beyond the cap the subtree renders as MYON_TOSTR_DEPTH.
 */
#define MYON_TOSTR_MAX_DEPTH 256
#define MYON_TOSTR_CYCLE     "<cycle>"
#define MYON_TOSTR_DEPTH     "<...>"

typedef struct {
    const Obj **items;   /* containers on the current recursion path */
    int          count;
    int          cap;
    int          depth;
} ToStrPath;

static int tostr_path_contains(const ToStrPath *p, const Obj *o) {
    for (int i = 0; i < p->count; i++)
        if (p->items[i] == o) return 1;
    return 0;
}

static void tostr_path_push(ToStrPath *p, const Obj *o) {
    if (p->count == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 16;
        p->items = (const Obj **)myon_xrealloc((void *)p->items,
                                               sizeof(const Obj *) * (size_t)p->cap);
    }
    p->items[p->count++] = o;
}

static void tostr_path_pop(ToStrPath *p) {
    if (p->count > 0) p->count--;
}

static char *value_to_cstr_rec(const Value *v, ToStrPath *path) {
    char buf[64];
    switch (v->type) {
        case TYPE_INT:
            snprintf(buf, sizeof(buf), "%lld", v->as.i);
            return myon_strdup(buf);
        case TYPE_FLOAT:
            format_float(buf, sizeof(buf), v->as.f);
            return myon_strdup(buf);
        case TYPE_BOOL:
            return myon_strdup(v->as.b ? "true" : "false");
        case TYPE_STR:
        case TYPE_CHAR:
            return myon_strdup(v->as.obj && v->as.obj->as.str ? v->as.obj->as.str : "");
        case TYPE_ERROR:
            return myon_strdup(v->as.obj && v->as.obj->as.str ? v->as.obj->as.str : "error");
        case TYPE_NIL:
            return myon_strdup("myon.nil");
        case TYPE_VOID:
            return myon_strdup("void");
        case TYPE_ARRAY: {
            const Obj *o = v->as.obj;
            if (!o) return myon_strdup("[]");
            if (tostr_path_contains(path, o)) return myon_strdup(MYON_TOSTR_CYCLE);
            if (path->depth >= MYON_TOSTR_MAX_DEPTH) return myon_strdup(MYON_TOSTR_DEPTH);
            tostr_path_push(path, o);
            path->depth++;
            char *s = myon_strdup("[");
            const ArrayData *a = &o->as.arr;
            for (int i = 0; i < a->count; i++) {
                if (i) s = append(s, ", ");
                char *e = value_to_cstr_rec(&a->items[i], path);
                s = append(s, e);
                myon_free(e);
            }
            s = append(s, "]");
            path->depth--;
            tostr_path_pop(path);
            return s;
        }
        case TYPE_MAP: {
            const Obj *o = v->as.obj;
            if (!o) return myon_strdup("{}");
            if (tostr_path_contains(path, o)) return myon_strdup(MYON_TOSTR_CYCLE);
            if (path->depth >= MYON_TOSTR_MAX_DEPTH) return myon_strdup(MYON_TOSTR_DEPTH);
            tostr_path_push(path, o);
            path->depth++;
            char *s = myon_strdup("{");
            int first = 1;
            for (MapEntry *e = o->as.map.head; e; e = e->next) {
                if (!first) s = append(s, ", ");
                first = 0;
                char *k = value_to_cstr_rec(&e->key, path);
                char *val = value_to_cstr_rec(&e->val, path);
                s = append(s, k); s = append(s, ": "); s = append(s, val);
                myon_free(k); myon_free(val);
            }
            s = append(s, "}");
            path->depth--;
            tostr_path_pop(path);
            return s;
        }
        case TYPE_STRUCT: {
            const Obj *o = v->as.obj;
            if (!o) return myon_strdup("<struct>");
            const StructData *st = &o->as.st;
            if (tostr_path_contains(path, o)) {
                /* Keep the type name so the output still identifies the value
                 * that closed the cycle: `Node(next=Node<cycle>)`. */
                char *s = myon_strdup(st->type_name ? st->type_name : "struct");
                return append(s, MYON_TOSTR_CYCLE);
            }
            if (path->depth >= MYON_TOSTR_MAX_DEPTH) return myon_strdup(MYON_TOSTR_DEPTH);
            tostr_path_push(path, o);
            path->depth++;
            char *s = myon_strdup(st->type_name ? st->type_name : "struct");
            s = append(s, "(");
            for (int i = 0; i < st->field_count; i++) {
                if (i) s = append(s, ", ");
                s = append(s, st->field_names[i]);
                s = append(s, "=");
                char *fv = value_to_cstr_rec(&st->field_vals[i], path);
                s = append(s, fv);
                myon_free(fv);
            }
            s = append(s, ")");
            path->depth--;
            tostr_path_pop(path);
            return s;
        }
        case TYPE_FUNC:
            return myon_strdup("<func>");
        case TYPE_TASK:
            return myon_strdup("<task>");
        default:
            return myon_strdup("");
    }
}

char *value_to_cstr(const Value *v) {
    ToStrPath path;
    path.items = NULL;
    path.count = 0;
    path.cap   = 0;
    path.depth = 0;
    char *s = value_to_cstr_rec(v, &path);
    myon_free((void *)path.items);
    return s;
}

int value_truthy(const Value *v) {
    switch (v->type) {
        case TYPE_BOOL:  return v->as.b;
        case TYPE_INT:   return v->as.i != 0;
        case TYPE_FLOAT: return v->as.f != 0.0;
        case TYPE_NIL:   return 0;
        default:         return 1;
    }
}

int value_equal(const Value *a, const Value *b) {
    if (a->type != b->type) return 0;
    switch (a->type) {
        case TYPE_INT:   return a->as.i == b->as.i;
        case TYPE_FLOAT: return a->as.f == b->as.f;
        case TYPE_BOOL:  return a->as.b == b->as.b;
        case TYPE_STR: case TYPE_CHAR: case TYPE_ERROR:
            return strcmp(a->as.obj->as.str, b->as.obj->as.str) == 0;
        case TYPE_NIL:   return 1;
        default:         return a->as.obj == b->as.obj;
    }
}

/* ------------------------------------------------------------------ */
/* Struct field access                                                 */
/* ------------------------------------------------------------------ */

Value *struct_field_ptr(Value *sv, const char *name) {
    if (sv->type != TYPE_STRUCT) return NULL;
    StructData *st = &sv->as.obj->as.st;
    for (int i = 0; i < st->field_count; i++)
        if (strcmp(st->field_names[i], name) == 0)
            return &st->field_vals[i];
    return NULL;
}

void struct_add_field(Value *sv, const char *name, Value v) {
    StructData *st = &sv->as.obj->as.st;
    int n = st->field_count + 1;
    st->field_names = (char **)myon_xrealloc(st->field_names, sizeof(char *) * n);
    st->field_vals = (Value *)myon_xrealloc(st->field_vals, sizeof(Value) * n);
    st->field_names[st->field_count] = myon_strdup(name);
    st->field_vals[st->field_count] = v;
    st->field_count = n;
}

/* ------------------------------------------------------------------ */
/* Array helpers                                                       */
/* ------------------------------------------------------------------ */

void array_push(Value *av, Value v) {
    ArrayData *a = &av->as.obj->as.arr;
    if (a->count == a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 8;
        a->items = (Value *)myon_xrealloc(a->items, sizeof(Value) * a->capacity);
    }
    a->items[a->count++] = v;
}

int array_pop(Value *av, Value *out) {
    ArrayData *a = &av->as.obj->as.arr;
    if (a->count == 0) return 0;
    *out = a->items[--a->count];
    return 1;
}

/* ------------------------------------------------------------------ */
/* Map helpers                                                         */
/* ------------------------------------------------------------------ */

static uint64_t map_hash(const Value *key) {
    uint64_t h = 0;
    switch (key->type) {
        case TYPE_STR: case TYPE_CHAR: case TYPE_ERROR: {
            h = UINT64_C(14695981039346656037);
            const unsigned char *s = (const unsigned char *)key->as.obj->as.str;
            while (*s) { h ^= *s++; h *= UINT64_C(1099511628211); }
            break;
        }
        case TYPE_INT: h = (uint64_t)key->as.i; break;
        case TYPE_BOOL: h = (uint64_t)key->as.b; break;
        case TYPE_FLOAT:
            /* value_equal considers +0 and -0 equal; hash them identically. */
            if (key->as.f != 0.0) memcpy(&h, &key->as.f, sizeof(h));
            break;
        case TYPE_NIL: break;
        default: h = (uint64_t)(uintptr_t)key->as.obj; break;
    }
    h ^= (uint64_t)key->type * UINT64_C(0x9e3779b97f4a7c15);
    h = (h ^ (h >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    h = (h ^ (h >> 27)) * UINT64_C(0x94d049bb133111eb);
    return h ^ (h >> 31);
}
static MapEntry *map_find(MapData *m, const Value *key, uint64_t hash) {
    if (!m->bucket_count) return NULL;
    for (MapEntry *e = m->buckets[hash & (m->bucket_count - 1)]; e; e = e->hash_next)
        if (e->hash == hash && value_equal(&e->key, key)) return e;
    return NULL;
}
static void map_grow(MapData *m) {
    size_t count = 16, bytes;
    if ((m->bucket_count && !checked_mul_size(m->bucket_count, 2, &count)) ||
        !checked_mul_size(count, sizeof(MapEntry *), &bytes)) {
        fprintf(stderr, "myon: map index size overflow\n");
        exit(70);
    }
    MapEntry **buckets = myon_xmalloc(bytes);
    memset(buckets, 0, bytes);
    for (MapEntry *e = m->head; e; e = e->next) {
        size_t bucket = e->hash & (count - 1);
        e->hash_next = buckets[bucket];
        buckets[bucket] = e;
    }
    myon_free(m->buckets);
    m->buckets = buckets;
    m->bucket_count = count;
}

void map_set(Value *mv, Value key, Value val) {
    MapData *m = &mv->as.obj->as.map;
    uint64_t hash = map_hash(&key);
    MapEntry *e = map_find(m, &key, hash);
    if (e) {
        value_free(&e->val);
        e->val = val;
        value_free(&key);
        return;
    }
    if (!m->bucket_count || m->count >= m->bucket_count - m->bucket_count / 4)
        map_grow(m);
    e = myon_xmalloc(sizeof(MapEntry));
    e->key = key;
    e->val = val;
    e->hash = hash;
    e->next = NULL;
    e->prev = m->tail;
    if (m->tail) m->tail->next = e;
    else m->head = e;
    m->tail = e;
    size_t bucket = hash & (m->bucket_count - 1);
    e->hash_next = m->buckets[bucket];
    m->buckets[bucket] = e;
    m->count++;
}

int map_get(Value *mv, const Value *key, Value *out) {
    MapEntry *e = map_find(&mv->as.obj->as.map, key, map_hash(key));
    if (!e) return 0;
    *out = value_copy(&e->val);
    return 1;
}
int map_has(Value *mv, const Value *key) {
    return map_find(&mv->as.obj->as.map, key, map_hash(key)) != NULL;
}
int map_delete(Value *mv, const Value *key) {
    MapData *m = &mv->as.obj->as.map;
    if (!m->bucket_count) return 0;
    uint64_t hash = map_hash(key);
    MapEntry **link = &m->buckets[hash & (m->bucket_count - 1)];
    while (*link) {
        MapEntry *e = *link;
        if (e->hash == hash && value_equal(&e->key, key)) {
            *link = e->hash_next;
            if (e->prev) e->prev->next = e->next;
            else m->head = e->next;
            if (e->next) e->next->prev = e->prev;
            else m->tail = e->prev;
            m->count--;
            value_free(&e->key);
            value_free(&e->val);
            myon_free(e);
            return 1;
        }
        link = &e->hash_next;
    }
    return 0;
}
