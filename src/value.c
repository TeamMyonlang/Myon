/*
 * Copyright 2026 nyan<(nyan4)
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

static Obj *obj_new(ObjKind kind) {
    Obj *o = (Obj *)myon_xmalloc(sizeof(Obj));
    memset(o, 0, sizeof(Obj));
    o->kind = kind;
    o->refcount = 1;
    return o;
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
    o->as.fn.closure = closure;
    o->as.fn.is_bound = 0;
    o->as.fn.bound_self = NULL;
    return obj_value(TYPE_FUNC, o);
}

Value value_task(void *task) {
    Obj *o = obj_new(OBJ_TASK);
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

static void obj_free(Obj *o) {
    switch (o->kind) {
        case OBJ_STR:
            free(o->as.str);
            break;
        case OBJ_ARRAY:
            for (int i = 0; i < o->as.arr.count; i++)
                value_free(&o->as.arr.items[i]);
            free(o->as.arr.items);
            typespec_free(o->as.arr.elem_type);
            break;
        case OBJ_MAP: {
            MapEntry *e = o->as.map.head;
            while (e) {
                MapEntry *n = e->next;
                value_free(&e->key);
                value_free(&e->val);
                free(e);
                e = n;
            }
            typespec_free(o->as.map.key_type);
            typespec_free(o->as.map.val_type);
            break;
        }
        case OBJ_STRUCT:
            free(o->as.st.type_name);
            for (int i = 0; i < o->as.st.field_count; i++) {
                free(o->as.st.field_names[i]);
                value_free(&o->as.st.field_vals[i]);
            }
            free(o->as.st.field_names);
            free(o->as.st.field_vals);
            for (int i = 0; i < o->as.st.tparam_count; i++) {
                free(o->as.st.tparam_names[i]);
                typespec_free(o->as.st.tparam_types[i]);
            }
            free(o->as.st.tparam_names);
            free(o->as.st.tparam_types);
            break;
        case OBJ_FUNC:
            if (o->as.fn.bound_self) {
                value_free(o->as.fn.bound_self);
                free(o->as.fn.bound_self);
            }
            /* decl and closure are not owned by the function value */
            break;
        case OBJ_TASK:
            /* the event-loop Task is owned by the loop, not by this handle */
            break;
    }
    free(o);
}

void value_free(Value *v) {
    if (!v) return;
    if (is_obj(v) && v->as.obj) {
        if (--v->as.obj->refcount == 0)
            obj_free(v->as.obj);
        v->as.obj = NULL;
    }
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

char *value_to_cstr(const Value *v) {
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
            char *s = myon_strdup("[");
            ArrayData *a = &v->as.obj->as.arr;
            for (int i = 0; i < a->count; i++) {
                if (i) s = append(s, ", ");
                char *e = value_to_cstr(&a->items[i]);
                s = append(s, e);
                free(e);
            }
            s = append(s, "]");
            return s;
        }
        case TYPE_MAP: {
            char *s = myon_strdup("{");
            int first = 1;
            for (MapEntry *e = v->as.obj->as.map.head; e; e = e->next) {
                if (!first) s = append(s, ", ");
                first = 0;
                char *k = value_to_cstr(&e->key);
                char *val = value_to_cstr(&e->val);
                s = append(s, k); s = append(s, ": "); s = append(s, val);
                free(k); free(val);
            }
            s = append(s, "}");
            return s;
        }
        case TYPE_STRUCT: {
            StructData *st = &v->as.obj->as.st;
            char *s = myon_strdup(st->type_name);
            s = append(s, "(");
            for (int i = 0; i < st->field_count; i++) {
                if (i) s = append(s, ", ");
                s = append(s, st->field_names[i]);
                s = append(s, "=");
                char *fv = value_to_cstr(&st->field_vals[i]);
                s = append(s, fv);
                free(fv);
            }
            s = append(s, ")");
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

static MapEntry *map_find(MapData *m, const Value *key) {
    for (MapEntry *e = m->head; e; e = e->next)
        if (value_equal(&e->key, key)) return e;
    return NULL;
}

void map_set(Value *mv, Value key, Value val) {
    MapData *m = &mv->as.obj->as.map;
    MapEntry *e = map_find(m, &key);
    if (e) {
        value_free(&e->val);
        e->val = val;
        value_free(&key);
        return;
    }
    e = (MapEntry *)myon_xmalloc(sizeof(MapEntry));
    e->key = key;
    e->val = val;
    e->next = m->head;
    m->head = e;
}

int map_get(Value *mv, const Value *key, Value *out) {
    MapEntry *e = map_find(&mv->as.obj->as.map, key);
    if (!e) return 0;
    *out = value_copy(&e->val);
    return 1;
}

int map_has(Value *mv, const Value *key) {
    return map_find(&mv->as.obj->as.map, key) != NULL;
}

int map_delete(Value *mv, const Value *key) {
    MapData *m = &mv->as.obj->as.map;
    MapEntry **pp = &m->head;
    while (*pp) {
        if (value_equal(&(*pp)->key, key)) {
            MapEntry *dead = *pp;
            *pp = dead->next;
            value_free(&dead->key);
            value_free(&dead->val);
            free(dead);
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}
