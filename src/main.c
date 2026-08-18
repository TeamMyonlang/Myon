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

#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "common.h"
#include "diag.h"
#include "mvm_compiler.h"
#include "mvm_chunk.h"
#include "mvm_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

/* Myon release version.  Bump on each release; --version prints it. */
#ifndef MYON_VERSION
#define MYON_VERSION "0.8.0"
#endif

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "myon: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)myon_xmalloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void dump_tokens(const TokenList *tl) {
    for (int i = 0; i < tl->count; i++) {
        const Token *t = &tl->items[i];
        printf("%3d  %-14s", i, token_type_name(t->type));
        if (t->lexeme && t->type != TOK_NEWLINE)
            printf("  '%s'", t->lexeme);
        printf("  (line %d)\n", t->line);
    }
}

static void version(void) {
    printf("myon %s\n", MYON_VERSION);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Myon interpreter\n"
        "\n"
        "usage:\n"
        "  %s <file.myon>                 run a source file (tree-walking interpreter)\n"
        "  %s <file.myc>                  run compiled MVM bytecode (bytecode VM)\n"
        "  %s --compile <src> [-o out]    compile a .myon to MVM bytecode (.myc); does not run\n"
        "  %s --dump-bytecode <src>       print the MVM disassembly of a .myon (or .myc) and exit\n"
        "  %s --tokens <file.myon>        print the token stream and exit\n"
        "  %s --tokens -                  read source from stdin and print the token stream\n"
        "  %s                             no argument: start the interactive REPL\n"
        "\n"
        "options:\n"
        "  -o <out>                       output path for --compile (default: <src> with .myc)\n"
        "  --strict-stale                 when running a .myc, error out (instead of warning)\n"
        "                                 if the source .myon is newer than the bytecode\n"
        "  -h, --help                     show this help and exit\n"
        "  -v, --version                  show the version and exit\n"
        "\n"
        "notes:\n"
        "  * A file argument is dispatched by content/extension: a MYC1 bytecode blob\n"
        "    (or a .myc name) runs on the MVM VM; anything else runs on the tree-walking\n"
        "    interpreter.  '-' always means stdin (treated as source).\n"
        "  * The REPL currently uses the tree-walking interpreter only; an MVM-backed\n"
        "    REPL is a future item.\n"
        "  * --run-mvm <src> compiles a .myon in memory and runs it on the MVM VM.\n"
        "    It is an internal cross-check used by the .myon/.myc equality test suite\n"
        "    (tests/run_mvm_tests.sh); normal execution does not need it.\n",
        prog, prog, prog, prog, prog, prog, prog);
}

/*
 * Step 5 helper: parse a .myon source file into a Program.  Shared by the
 * --compile and --dump-bytecode paths.  Returns a Program (caller frees with
 * program_free) plus the TokenList (out param, caller frees) and the source
 * buffer (out param, caller frees).  Returns NULL on error.
 *
 * NOTE: this is an additive path; it does not touch the tree-walking
 * interpreter used for `.myon` execution (docs/mvm_spec.md §9.2 defers the
 * final CLI wiring to Step 7).
 */
static Program *load_program(const char *path, char **out_source, TokenList *out_tokens) {
    char *source = read_file(path);
    if (!source) return NULL;
    diag_set_source(source);
    if (!lexer_tokenize(source, out_tokens)) {
        diag_clear_source();
        free(source);
        return NULL;
    }
    Program *program = parser_parse(out_tokens);
    if (!program) {
        token_list_free(out_tokens);
        diag_clear_source();
        free(source);
        return NULL;
    }
    *out_source = source;
    return program;
}

/* Derive a default output name: foo.myon -> foo.myc (Step 5 / spec §9.2). */
static char *default_myc_name(const char *src) {
    size_t n = strlen(src);
    const char *dot = strrchr(src, '.');
    char *out;
    if (dot && strcmp(dot, ".myon") == 0) {
        size_t base = (size_t)(dot - src);
        out = (char *)myon_xmalloc(base + 5);
        memcpy(out, src, base);
        memcpy(out + base, ".myc", 5);
    } else {
        out = (char *)myon_xmalloc(n + 5);
        memcpy(out, src, n);
        memcpy(out + n, ".myc", 5);
    }
    return out;
}

static int cmd_compile(const char *src, const char *out) {
    char *source = NULL;
    TokenList tokens;
    Program *program = load_program(src, &source, &tokens);
    if (!program) return 65;

    Module *m = mvm_compile_program(program, src);
    int rc = 0;
    if (!m) {
        rc = 65;
    } else {
        char *outname = out ? myon_strdup(out) : default_myc_name(src);
        if (mvm_module_write_file(m, outname) != 0) {
            fprintf(stderr, "myon: failed to write '%s'\n", outname);
            rc = 74;
        } else {
            fprintf(stderr, "myon: wrote %s\n", outname);
        }
        free(outname);
        module_free(m);
    }
    program_free(program);
    token_list_free(&tokens);
    diag_clear_source();
    free(source);
    return rc;
}

static int cmd_dump_bytecode(const char *src) {
    /* If it's already a .myc, load and dump it directly. */
    FILE *probe = fopen(src, "rb");
    if (probe) {
        unsigned char magic[4] = {0};
        size_t got = fread(magic, 1, 4, probe);
        fclose(probe);
        if (got == 4 && magic[0] == 'M' && magic[1] == 'Y' &&
            magic[2] == 'C' && magic[3] == '1') {
            Module *m = mvm_module_read_file(src);
            if (!m) { fprintf(stderr, "myon: cannot load '%s'\n", src); return 65; }
            mvm_module_disassemble(m, stdout);
            module_free(m);
            return 0;
        }
    }

    char *source = NULL;
    TokenList tokens;
    Program *program = load_program(src, &source, &tokens);
    if (!program) return 65;

    Module *m = mvm_compile_program(program, src);
    int rc = 0;
    if (!m) {
        rc = 65;
    } else {
        mvm_module_disassemble(m, stdout);
        module_free(m);
    }
    program_free(program);
    token_list_free(&tokens);
    diag_clear_source();
    free(source);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Step 6: run a compiled .myc through the MVM bytecode VM.            */
/* ------------------------------------------------------------------ */

/*
 * Detect a .myc file: either the name ends in ".myc" or the first four bytes
 * are the MYC1 magic (mvm_spec §6.2).  Checking the magic (not just the name)
 * lets `myon foo` run a compiled file even if it lacks the extension, and
 * avoids feeding a bytecode blob to the tokenizer.
 */
static int looks_like_myc(const char *path) {
    size_t n = strlen(path);
    if (n >= 4 && strcmp(path + n - 4, ".myc") == 0) return 1;
    FILE *probe = fopen(path, "rb");
    if (!probe) return 0;
    unsigned char magic[4] = {0};
    size_t got = fread(magic, 1, 4, probe);
    fclose(probe);
    return got == 4 && magic[0] == 'M' && magic[1] == 'Y' &&
           magic[2] == 'C' && magic[3] == '1';
}

/*
 * Load a .myc from disk and execute it on the VM (docs/mvm_spec.md §6, §8).
 * A reloaded .myc carries no struct/method declarations (those live in the
 * AST, not the bytecode), so `program` is NULL here and any struct opcode
 * raises a clear runtime error — matching the limitation documented in
 * mvm_vm.h / the Step 6 report.  Source snippets are unavailable for a
 * reloaded module, so runtime errors show the line number only.
 */
static int check_myc_stale(const Module *m, const char *myc_path, int strict);

static int cmd_run_myc(const char *path, int strict_stale) {
    Module *m = mvm_module_read_file(path);
    if (!m) {
        fprintf(stderr, "myon: cannot load bytecode '%s'\n", path);
        return 65;
    }
    /* mvm_spec.md §6.5: verify the .myc is not older than its source .myon.
     * Warns by default; aborts under --strict-stale. */
    int stale_rc = check_myc_stale(m, path, strict_stale);
    if (stale_rc != 0) {
        module_free(m);
        return stale_rc;
    }
    int rc = mvm_run_module(m, NULL, NULL);
    module_free(m);
    return rc;
}

/*
 * Step 7-b: compile a .myon in memory and immediately execute it on the MVM
 * bytecode VM, keeping the parsed Program (and thus struct/method
 * declarations, plus the source text for diagnostics) available to the VM.
 *
 * This is the "run compiled-in-memory" path referenced in mvm_vm.h: it is the
 * apples-to-apples counterpart to running the same .myon through the
 * tree-walking interpreter, and is what the .myon/.myc equality suite
 * (tests/run_mvm_tests.sh) uses to verify that both engines agree.  Unlike
 * `cmd_run_myc` (which reloads a serialized .myc and therefore has no struct
 * declarations), this path passes `program` to mvm_run_module so struct-using
 * programs execute identically to the tree-walker.
 *
 * This is strictly additive: the tree-walking path (interpret()) and .myon
 * behaviour are untouched.
 */
static int cmd_run_mvm(const char *src) {
    char *source = NULL;
    TokenList tokens;
    Program *program = load_program(src, &source, &tokens);
    if (!program) return 65;

    Module *m = mvm_compile_program(program, src);
    int rc;
    if (!m) {
        rc = 65;
    } else {
        rc = mvm_run_module(m, program, source);
        module_free(m);
    }
    program_free(program);
    token_list_free(&tokens);
    diag_clear_source();
    free(source);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Interactive REPL (spec 12)                                          */
/* ------------------------------------------------------------------ */

/*
 * Decide whether the accumulated REPL buffer is a syntactically *complete*
 * unit of input.  We use a lightweight scan that mirrors the lexer's own
 * string/comment skipping so that brackets inside string literals or comments
 * are not miscounted.  Input is considered incomplete while any of
 * ()/[]/{} remain open.  This is intentionally conservative: a genuinely
 * malformed line still gets handed to the parser, which reports the error.
 */
static int repl_input_complete(const char *src) {
    int depth = 0;      /* () [] {} nesting */
    for (const char *c = src; *c; c++) {
        if (*c == '"') {
            /* skip string literal, honoring backslash escapes */
            c++;
            while (*c && *c != '"') {
                if (*c == '\\' && c[1]) c++;
                c++;
            }
            if (!*c) return 0; /* unterminated string => keep reading */
            continue;
        }
        if (*c == '#') { /* '#' line comment */
            while (*c && *c != '\n') c++;
            if (!*c) break;
            continue;
        }
        if (*c == '/' && c[1] == '/') {
            while (*c && *c != '\n') c++;
            if (!*c) break;
            continue;
        }
        if (*c == '/' && c[1] == '*') {
            c += 2;
            while (*c && !(*c == '*' && c[1] == '/')) c++;
            if (!*c) return 0; /* unterminated block comment */
            c++; /* land on '/' (loop ++ consumes it) */
            continue;
        }
        if (*c == '(' || *c == '[' || *c == '{') depth++;
        else if (*c == ')' || *c == ']' || *c == '}') { if (depth > 0) depth--; }
    }
    return depth <= 0;
}

static int run_repl(void) {
    Interp *it = interp_create();
    printf("Myon REPL. Type 'exit' or 'quit' to leave (Ctrl+D also works).\n");

    size_t cap = 256, len = 0;
    char *buf = (char *)myon_xmalloc(cap);
    buf[0] = '\0';
    int continuing = 0;

    for (;;) {
        fputs(continuing ? "...> " : "myon> ", stdout);
        fflush(stdout);

        /* read a single physical line */
        char line[1024];
        if (!fgets(line, sizeof(line), stdin)) {
            fputc('\n', stdout);
            break; /* EOF (Ctrl+D) */
        }

        /* exit/quit only when not mid-continuation and buffer is empty */
        if (!continuing) {
            /* trim trailing newline for the command check */
            char trimmed[1024];
            size_t n = 0;
            for (const char *c = line; *c && *c != '\n' && n + 1 < sizeof(trimmed); c++)
                trimmed[n++] = *c;
            trimmed[n] = '\0';
            /* strip surrounding whitespace */
            char *s = trimmed;
            while (*s == ' ' || *s == '\t') s++;
            char *e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
            if (strcmp(s, "exit") == 0 || strcmp(s, "quit") == 0) break;
            if (*s == '\0') continue; /* blank line at top level: ignore */
        }

        /* append the physical line to the buffer */
        size_t ll = strlen(line);
        if (len + ll + 1 > cap) {
            while (len + ll + 1 > cap) cap *= 2;
            buf = (char *)myon_xrealloc(buf, cap);
        }
        memcpy(buf + len, line, ll + 1);
        len += ll;

        /* if input still has open brackets, prompt for continuation */
        if (!repl_input_complete(buf)) {
            continuing = 1;
            continue;
        }

        /* complete unit: tokenize + parse + run */
        TokenList tokens;
        /* P5: register the current input so diagnostics can show a snippet. */
        diag_set_source(buf);
        if (lexer_tokenize(buf, &tokens)) {
            Program *program = parser_parse(&tokens);
            if (program) {
                /* interp_run installs its own setjmp barrier: a runtime error
                 * aborts only this input, the REPL keeps going. */
                interp_run(it, program);
            }
            /* tokens are referenced by retained AST lexemes? No: the parser
             * copies what it needs into the AST, so freeing tokens is safe. */
            token_list_free(&tokens);
        }
        diag_clear_source();

        /* reset the buffer for the next statement */
        len = 0;
        buf[0] = '\0';
        continuing = 0;
    }

    free(buf);
    interp_free(it);
    return 0;
}

/* ------------------------------------------------------------------ */
/* .myc stale check against the adjacent .myon (mvm_spec.md §6.5).      */
/* ------------------------------------------------------------------ */

/*
 * Compute the FNV-1a 64-bit hash of a file's bytes.  This mirrors exactly the
 * algorithm the compiler uses in src/mvm_compiler.c fill_source_info(), so the
 * value stored in the .myc Source Info (first 8 bytes of src_hash, little
 * endian) can be reproduced and compared here.  Returns 0 on success and
 * writes the hash to *out; returns non-zero if the file cannot be read.
 */
static int fnv1a_file(const char *path, uint64_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t h = 1469598103934665603ULL;  /* FNV offset basis */
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        h ^= (uint8_t)ch;
        h *= 1099511628211ULL;             /* FNV prime */
    }
    fclose(f);
    *out = h;
    return 0;
}

/*
 * Verify that a loaded .myc is not older than the .myon it was compiled from
 * (mvm_spec.md §6.5, §12.1).  The check is best-effort: if the source .myon is
 * not present (a distributed .myc), verification is skipped and 0 is returned.
 *
 * Resolution order for the source: the path recorded in the .myc Source Info
 * (src_path) if it exists, otherwise a sibling "<myc-basename>.myon".
 *
 * If the .myon is present and differs (its mtime is newer, or its size/hash no
 * longer match the recorded Source Info), a warning is printed.  When `strict`
 * is set, this is treated as an error and a non-zero value is returned so the
 * caller aborts execution.
 */
static int check_myc_stale(const Module *m, const char *myc_path, int strict) {
    /* No source info recorded (older .myc) => nothing to compare. */
    if (m->src_size == 0 && m->src_mtime == 0) return 0;

    /* Resolve the source .myon path. */
    const char *src = NULL;
    char *derived = NULL;
    FILE *probe;
    if (m->src_path && (probe = fopen(m->src_path, "rb")) != NULL) {
        fclose(probe);
        src = m->src_path;
    } else {
        derived = default_myc_name(myc_path);  /* <base>.myc */
        size_t dn = strlen(derived);
        /* turn "<base>.myc" into "<base>.myon" */
        if (dn >= 4 && strcmp(derived + dn - 4, ".myc") == 0) {
            char *cand = (char *)myon_xmalloc(dn + 2);
            memcpy(cand, derived, dn - 4);
            memcpy(cand + dn - 4, ".myon", 6);
            if ((probe = fopen(cand, "rb")) != NULL) { fclose(probe); }
            else { free(cand); cand = NULL; }
            free(derived);
            derived = cand;
            src = derived;
        } else {
            free(derived);
            derived = NULL;
        }
    }

    if (!src) return 0;  /* no source available: skip (distributed .myc) */

    /* Gather the current source's mtime / size / hash. */
    int stale = 0;
    struct stat st;
    uint64_t cur_hash = 0;
    int have_hash = (fnv1a_file(src, &cur_hash) == 0);

    if (stat(src, &st) == 0) {
        if ((int64_t)st.st_mtime > m->src_mtime) stale = 1;
        if ((uint64_t)st.st_size != m->src_size) stale = 1;
    }
    if (have_hash) {
        uint64_t rec = 0;
        for (int i = 0; i < 8; i++)
            rec |= (uint64_t)m->src_hash[i] << (8 * i);
        if (rec != 0 && rec != cur_hash) stale = 1;
    }

    int rc = 0;
    if (stale) {
        if (strict) {
            fprintf(stderr,
                "myon: '%s' is stale: source '%s' is newer than the compiled "
                "bytecode. Recompile with --compile (--strict-stale).\n",
                myc_path, src);
            rc = 65;
        } else {
            fprintf(stderr,
                "myon: warning: '%s' may be older than '%s'; recompilation is "
                "recommended (--compile).\n",
                myc_path, src);
        }
    }

    if (derived) free(derived);
    return rc;
}

int main(int argc, char **argv) {
    int tokens_only = 0;
    int strict_stale = 0;
    const char *path = NULL;
    /* Step 5 (additive): MVM compile / disassemble subcommands. */
    const char *compile_src = NULL;
    const char *compile_out = NULL;
    const char *dump_src = NULL;
    const char *run_mvm_src = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) tokens_only = 1;
        else if (strcmp(argv[i], "--strict-stale") == 0) strict_stale = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            version(); return 0;
        } else if (strcmp(argv[i], "--compile") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 64; }
            compile_src = argv[++i];
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 64; }
            dump_src = argv[++i];
        } else if (strcmp(argv[i], "--run-mvm") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 64; }
            run_mvm_src = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 64; }
            compile_out = argv[++i];
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            /* Unknown option: fail loudly instead of silently treating it as a
             * file path.  '-' (stdin) is not an option and falls through. */
            fprintf(stderr, "myon: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 64;
        } else {
            path = argv[i];
        }
    }

    if (dump_src)    return cmd_dump_bytecode(dump_src);
    if (compile_src) return cmd_compile(compile_src, compile_out);
    if (run_mvm_src) return cmd_run_mvm(run_mvm_src);

    /* No file argument and not a token dump: start the interactive REPL. */
    if (!path && !tokens_only) {
        return run_repl();
    }

    if (!path) { usage(argv[0]); return 64; }

    /*
     * Step 6: a compiled .myc runs on the MVM bytecode VM; a .myon (or any
     * other source) keeps the unchanged tree-walking path below.  stdin ("-")
     * is always treated as source.
     */
    if (strcmp(path, "-") != 0 && looks_like_myc(path)) {
        return cmd_run_myc(path, strict_stale);
    }

    char *source = NULL;
    if (strcmp(path, "-") == 0) {
        /* read stdin */
        size_t cap = 4096, len = 0;
        source = (char *)myon_xmalloc(cap);
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (len + 1 >= cap) { cap *= 2; source = (char *)myon_xrealloc(source, cap); }
            source[len++] = (char)c;
        }
        source[len] = '\0';
    } else {
        source = read_file(path);
        if (!source) return 66;
    }

    /* Resolve external module imports relative to the script's own directory
     * (not the process CWD), so `myon /elsewhere/main.myon` finds its sibling
     * modules.  stdin ("-") has no directory, so imports fall back to CWD. */
    if (strcmp(path, "-") != 0)
        interpret_set_script_path(path);

    /* P5: register the source so lexer/parser/interpreter diagnostics can
     * print the offending line with a caret. */
    diag_set_source(source);

    TokenList tokens;
    if (!lexer_tokenize(source, &tokens)) {
        diag_clear_source();
        free(source);
        return 65;
    }

    if (tokens_only) {
        dump_tokens(&tokens);
        token_list_free(&tokens);
        free(source);
        return 0;
    }

    Program *program = parser_parse(&tokens);
    if (!program) {
        token_list_free(&tokens);
        free(source);
        return 65;
    }

    int rc = interpret(program);

    program_free(program);
    token_list_free(&tokens);
    diag_clear_source();
    free(source);
    return rc;
}
