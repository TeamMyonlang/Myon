/* Check semantic Value ownership as well as allocator reachability. */
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "mvm_compiler.h"
#include "mvm_vm.h"
#include "common.h"
#include "diag.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void allocation_scopes(void) {
    MyonAllocScope *outer = myon_alloc_scope_begin();
    char *keep = myon_xmalloc(16);
    memset(keep, 42, 16);
    MyonAllocScope *inner = myon_alloc_scope_begin();
    keep = myon_xrealloc(keep, 4096); /* retains ownership in outer */
    myon_xmalloc(123); /* rollback must reclaim this allocation */
    myon_alloc_scope_end(inner, true);
    assert(keep[0] == 42);
    keep[4095] = 7;
    TokenList tl;
    assert(lexer_tokenize("x = 1", &tl));
    Program *valid = parser_parse(&tl);
    assert(valid && valid->stmts.count == 1);
    token_list_free(&tl);
    Module *compiled = mvm_compile_program(valid, "allocation-scope");
    assert(compiled);
    module_free(compiled);
    assert(lexer_tokenize("module myon.stdio\nmyon.print(\"prefix-{missing}\")", &tl));
    Program *bad_compile = parser_parse(&tl);
    assert(bad_compile);
    token_list_free(&tl);
    assert(mvm_compile_program(bad_compile, "allocation-scope") == NULL);
    program_free(bad_compile);
    for (int i = 0; i < 20; i++) {
        assert(lexer_tokenize("myon.func bad<T>(x: myon.array(T)) ret str { ret ", &tl));
        assert(parser_parse(&tl) == NULL);
        token_list_free(&tl);
        assert(valid->stmts.count == 1);
    }
    myon_alloc_scope_end(outer, false);
    assert(keep[4095] == 7); /* successful commit keeps ordinary allocation layout */
    myon_free(keep);
    program_free(valid);
}

static void check_source(const char *source, const char *path, int mvm) {
    size_t before = value_live_allocations();
    interpret_set_script_path(path);
    diag_set_source(source);
    int rc = 65;
    TokenList tl;
    if (lexer_tokenize(source, &tl)) {
        Program *p = parser_parse(&tl);
        if (p) {
            if (!mvm) rc = interpret(p);
            else {
                Module *module = mvm_compile_program(p, path);
                if (module) { rc = mvm_run_module(module, p, source); module_free(module); }
            }
            program_free(p);
        }
        token_list_free(&tl);
    }
    diag_clear_source();
    if (rc != 1 && rc != 65) {
        fprintf(stderr, "FAIL expected error in %s (%s), got %d\n", path, mvm ? "MVM" : "tree", rc);
        exit(1);
    }
    value_collect_cycles();
    if (value_live_allocations() != before) {
        fprintf(stderr, "FAIL residual Values after %s (%s): %zu -> %zu\n", path,
                mvm ? "MVM" : "tree", before, value_live_allocations());
        exit(1);
    }
}
static const char *const probes[] = {
    "module myon.stdio\n\"left\" + missing",
    "myon.func f(a: str, b: int) ret int { ret b }\nf(\"argument\", missing)",
    "myon.func f() ret str, int { ret \"first\", missing }\nf()",
    "module myon.stdio\nmyon.print(\"{missing}\")",
    "module myon.stdio\nmyon.print(\"{1 +}\")",
    "module myon.array\nmyon.struct S { x: int }\na = myon.array(int)\na.push(S(x=1))",
    "myon.struct S { x: int }\ns = S(x=missing)",
    "myon.struct S { x: int }\ns = S()",
    "module myon.array\nxs = myon.array(str)\nxs.push(\"item\")\nmyon.for x myon.in xs { ret 1, missing }",
    "myon.struct S { x: int\nmyon.func f(a: str, b: int) ret str { ret a } }\ns = S(x=1)\ns.f(\"argument\", missing)",
    "myon.struct S { x: int\nmyon.func f(a: str) ret int { ret 1 / 0 } }\ns = S(x=1)\ns.f(\"argument\")",
    "myon.func incomplete<T>(x: myon.map(str, myon.array(T))) ret myon.array(T) {",
    "module myon.stdio\nmyon.print(\"unterminated",
};
int main(int argc, char **argv) {
    assert(argc >= 2);
    int mvm = strcmp(argv[1], "mvm") == 0;
    allocation_scopes();
    for (int i = 2; i < argc; i++) {
        size_t n = strlen(argv[i]);
        char *path = myon_xmalloc(n + 2);
        strcpy(path, argv[i]);
        if (n >= 4 && strcmp(path + n - 4, ".err") == 0) strcpy(path + n - 4, ".myon");
        FILE *file = fopen(path, "rb"); assert(file);
        assert(fseek(file, 0, SEEK_END) == 0);
        long size = ftell(file); assert(size >= 0); rewind(file);
        char *source = myon_xmalloc((size_t)size + 1);
        assert(fread(source, 1, (size_t)size, file) == (size_t)size);
        fclose(file); source[size] = 0;
        check_source(source, path, mvm);
        myon_free(source); myon_free(path);
    }
    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++)
        for (int repeat = 0; repeat < 3; repeat++) check_source(probes[i], "ownership-probe", mvm);
    interpret_set_script_path(NULL);
    puts("error cleanup ownership tests passed");
    return 0;
}
