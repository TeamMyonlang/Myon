/* Deliberately unreachable allocation: validates the sanitizer gate itself. */
#include <stdlib.h>
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void leak_for_probe(void) {
    volatile unsigned char *p = malloc(113);
    if (!p) exit(70);
    *p = 42;
}
int main(int argc, char **argv) {
    (void)argv;
    leak_for_probe();
    return argc > 1 ? 1 : 0;
}
