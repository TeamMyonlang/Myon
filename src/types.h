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

#ifndef MYON_TYPES_H
#define MYON_TYPES_H

/*
 * Myon's type system (spec.md section 2, plus 14.8 generics / 7 arrays /
 * 14.2 maps / 8 structs).
 *
 * `Type` is the primitive tag used everywhere runtime values are checked.
 * Compound and user-defined types (array/map/struct/generic parameter) are
 * described structurally by `TypeSpec`, which the parser builds from a type
 * annotation and the interpreter uses to validate elements/fields.
 */
typedef enum {
    TYPE_UNKNOWN = 0, /* not yet inferred / annotation omitted */
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STR,
    TYPE_CHAR,
    TYPE_BOOL,
    TYPE_VOID,
    TYPE_ERROR,       /* the error type; error(...) constructor */
    TYPE_NIL,         /* myon.nil : error-context sentinel only (2.4) */
    TYPE_ARRAY,       /* myon.array(T)     (section 7)  */
    TYPE_MAP,         /* myon.map(K, V)    (section 14.2) */
    TYPE_STRUCT,      /* user struct instance (section 8) */
    TYPE_FUNC,        /* function / lambda value (section 6) */
    TYPE_TYPEPARAM,   /* an unresolved generic type parameter T (14.8) */
    TYPE_TASK         /* Phase5: async task/future handle (internal only) */
} Type;

const char *type_name(Type t);

/* Type compatibility for binary arithmetic/comparison (strict, 2.2). */
int type_equal(Type a, Type b);

/* ------------------------------------------------------------------ */
/* Structural type descriptors (built by the parser from annotations)  */
/* ------------------------------------------------------------------ */

typedef struct TypeSpec TypeSpec;

struct TypeSpec {
    Type      base;      /* primitive tag or ARRAY/MAP/STRUCT/TYPEPARAM */
    char     *name;      /* struct name or type-parameter name (owned) */
    TypeSpec *elem;      /* array element type / map value type (owned) */
    TypeSpec *key;       /* map key type (owned) */
    /* Generic instantiation arguments, e.g. Box<int>: name="Box", args=[int] */
    TypeSpec **args;
    int        arg_count;
};

TypeSpec *typespec_new(Type base);
TypeSpec *typespec_prim(Type base);          /* primitive, no children */
TypeSpec *typespec_named(const char *name);  /* struct/param by name */
TypeSpec *typespec_clone(const TypeSpec *t);
void      typespec_free(TypeSpec *t);
char     *typespec_to_cstr(const TypeSpec *t); /* heap string for diagnostics */
int       typespec_equal(const TypeSpec *a, const TypeSpec *b);

#endif /* MYON_TYPES_H */
