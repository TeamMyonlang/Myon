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

#ifndef MYON_VALUE_H
#define MYON_VALUE_H

#include "types.h"

/*
 * Runtime value representation (spec.md section 2 and beyond).
 *
 * Primitives (int/float/bool) are stored inline.  Everything with heap
 * state (str/char/error strings, arrays, maps, structs, functions) is held
 * behind a reference-counted object so that value_copy() is cheap and the
 * tree-walking interpreter can freely duplicate values.
 */

typedef struct Value Value;
typedef struct Obj   Obj;

/* Forward decls for AST types referenced by function objects. */
struct Stmt;
struct Expr;
typedef struct StmtList StmtList;
struct Env;
typedef struct Env Env;

struct Value {
    Type type;
    union {
        long long i;   /* int */
        double    f;   /* float */
        int       b;   /* bool */
        Obj      *obj; /* str/char/error/array/map/struct/func */
    } as;
};

/* ---- heap object kinds ---- */
typedef enum {
    OBJ_STR,      /* str / char / error payload */
    OBJ_ARRAY,
    OBJ_MAP,
    OBJ_STRUCT,   /* struct instance */
    OBJ_FUNC,     /* function / lambda / bound method */
    OBJ_TASK      /* Phase5: async task handle (wraps event-loop Task*) */
} ObjKind;

/* dynamic array of values */
typedef struct {
    Value   *items;
    int      count;
    int      capacity;
    TypeSpec *elem_type;   /* declared element type (owned) */
} ArrayData;

/* map entry list (linear; keys are str or int) */
typedef struct MapEntry {
    Value            key;
    Value            val;
    struct MapEntry *next;
} MapEntry;

typedef struct {
    MapEntry *head;
    TypeSpec *key_type;
    TypeSpec *val_type;
} MapData;

/* struct instance: named fields */
typedef struct {
    char       *type_name;    /* struct type name (owned) */
    char      **field_names;  /* owned */
    Value      *field_vals;
    int         field_count;
    struct StructDecl *decl;  /* back-pointer to declaration (not owned) */
    /* generic bindings for this instance (owned), may be NULL */
    char      **tparam_names;
    TypeSpec  **tparam_types;
    int         tparam_count;
} StructData;

/* function object: closure over a definition environment */
typedef struct {
    struct FuncDecl *decl;  /* the declaration/lambda AST (not owned) */
    struct Env      *closure; /* captured environment (ref, not owned/freed) */
    /* If this is a bound method, `self` holds the receiver (owned copy). */
    int              is_bound;
    Value           *bound_self;
} FuncData;

/* async task handle (Phase5): opaque pointer into the event loop.  The
 * event-loop Task itself is owned by the loop (freed on loop destroy); this
 * object only holds a borrowed handle used by myon.await. */
typedef struct {
    void *task;   /* event-loop `Task *` (opaque here to keep layering clean) */
} TaskData;

struct Obj {
    ObjKind kind;
    int     refcount;
    union {
        char       *str;   /* OBJ_STR (str/char/error) */
        ArrayData   arr;
        MapData     map;
        StructData  st;
        FuncData    fn;
        TaskData    task;  /* OBJ_TASK */
    } as;
};

/* ---- constructors (primitives) ---- */
Value value_int(long long v);
Value value_float(double v);
Value value_bool(int v);
Value value_nil(void);
Value value_void(void);

/* string-family: take ownership of the heap string */
Value value_str(char *heap_str);
Value value_char(char *heap_str);
Value value_error(char *heap_msg);

/* compound object constructors */
Value value_array(TypeSpec *elem_type /*owned*/);
Value value_map(TypeSpec *key_type /*owned*/, TypeSpec *val_type /*owned*/);
Value value_struct(const char *type_name, struct StructDecl *decl);
Value value_func(struct FuncDecl *decl, struct Env *closure);
Value value_task(void *task /* borrowed event-loop Task* */);

/* ---- refcount management ---- */
Value value_copy(const Value *v);   /* shares objects, bumps refcount */
void  value_free(Value *v);         /* drops one reference */

/* ---- helpers ---- */
const char *value_type_name(const Value *v);
char *value_to_cstr(const Value *v);   /* heap string rendering */
int   value_truthy(const Value *v);
int   value_equal(const Value *a, const Value *b); /* structural == */

/* struct field access */
Value *struct_field_ptr(Value *sv, const char *name);
void   struct_add_field(Value *sv, const char *name, Value v);

/* array helpers */
void  array_push(Value *av, Value v);
int   array_pop(Value *av, Value *out);   /* returns 0 if empty */

/* map helpers */
void  map_set(Value *mv, Value key, Value val);
int   map_get(Value *mv, const Value *key, Value *out);   /* 0 if absent */
int   map_has(Value *mv, const Value *key);
int   map_delete(Value *mv, const Value *key);            /* 0 if absent */

#endif /* MYON_VALUE_H */
