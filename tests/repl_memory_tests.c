/* Session rollback must release allocation transactions and retained VM roots. */
#include "lexer.h"
#include "parser.h"
#include "mvm_vm.h"
#include "value.h"
#include <assert.h>
#include <stdio.h>

static void input(MvmRepl *r, const char *s, int expected) {
    TokenList tokens;
    assert(lexer_tokenize(s, &tokens));
    Program *p = parser_parse(&tokens);
    assert(p);
    token_list_free(&tokens);
    int rc = mvm_repl_run(r, p, s);
    assert(rc == expected);
}
int main(void) {
    size_t base = value_live_allocations();
    for (int i = 0; i < 8; i++) {
        MvmRepl *r = mvm_repl_new();
        input(r, "module myon.array", 0);
        input(r, "x = 4", 0);
        input(r, "myon.func make() ret func { n = 1; ret myon.lambda() ret int { n += x; ret n } }", 0);
        input(r, "f = make()", 0);
        input(r, "xs = myon.array(str); xs.push(\"retained\")", 0);
        for (int j = 0; j < 6; j++) {
            input(r, "lost = \"partial compile\"; Bad<int>()", 65);
            input(r, "lost", 1);
            input(r, "f()", 0);
            input(r, "1 / 0", 1);
            input(r, "x += 1", 0);
        }
        input(r, "module myon.time", 0);
        input(r, "myon.async myon.func task() ret int { myon.time.sleep_ms(1); ret x }", 0);
        input(r, "myon.await task()", 0);
        mvm_repl_free(r);
        value_collect_cycles();
        assert(value_live_allocations() == base);
    }
    puts("MVM REPL ownership tests passed");
    return 0;
}
