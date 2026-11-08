#include "unicode.h"
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *s, const char *upper, const char *lower) {
    char *u = myon_unicode_case(s, true);
    char *l = myon_unicode_case(s, false);
    assert(u && l);
    assert(strcmp(u, upper) == 0);
    assert(strcmp(l, lower) == 0);
    free(u); free(l);
}
static void vectors(void) {
    check("", "", "");
    check("Hello 123", "HELLO 123", "hello 123");
    check("caf\u00E9", "CAF\u00C9", "caf\u00E9");
    check("Stra\u00DFe \uFB03", "STRASSE FFI", "stra\u00DFe \uFB03");
    check("\u01F0\u0390", "J\u030C\u0399\u0308\u0301", "\u01F0\u0390");
    check("\u0130 I \u0131 i", "\u0130 I I I", "i\u0307 i \u0131 i");
    check("\u01F2\u01F3", "\u01F1\u01F1", "\u01F3\u01F3");
    check("\u039F\u03A3 \u039F\u03A3\u0391 \u03A3", "\u039F\u03A3 \u039F\u03A3\u0391 \u03A3", "\u03BF\u03C2 \u03BF\u03C3\u03B1 \u03C3");
    check("A\u03A3\u0301", "A\u03A3\u0301", "a\u03C2\u0301");
    check("A\u03A3\u0301A", "A\u03A3\u0301A", "a\u03C3\u0301a");
    check("A\u03A3'A A\u03A3'", "A\u03A3'A A\u03A3'", "a\u03C3'a a\u03C2'");
    check("\u0345\u03A3 A\u0345\u03A3 A\u03A3\u0345", "\u0399\u03A3 A\u0399\u03A3 A\u03A3\u0399", "\u0345\u03C3 a\u0345\u03C2 a\u03C2\u0345");
    check("\u041F\u0420\u0418\u0412\u0415\u0422", "\u041F\u0420\u0418\u0412\u0415\u0422", "\u043F\u0440\u0438\u0432\u0435\u0442");
    check("\U00010400\U00010428", "\U00010400\U00010400", "\U00010428\U00010428");
    check("\u65E5\u672C\u8A9E \U0001F600", "\u65E5\u672C\u8A9E \U0001F600", "\u65E5\u672C\u8A9E \U0001F600");
    check("\u1C89\u1C8A", "\u1C89\u1C89", "\u1C8A\u1C8A");
    check("\u019B\uA7DC", "\uA7DC\uA7DC", "\u019B\u019B");
    check("\U00010D50\U00010D70", "\U00010D50\U00010D50", "\U00010D70\U00010D70");
}
static void invalid_utf8(void) {
    check("A\x80" "B", "A\x80" "B", "a\x80" "b");
    check("\xC0\xAF", "\xC0\xAF", "\xC0\xAF");
    check("\xED\xA0\x80", "\xED\xA0\x80", "\xED\xA0\x80");
    check("\xF0\x80\x80\xAF", "\xF0\x80\x80\xAF", "\xF0\x80\x80\xAF");
    check("\xF4\x90\x80\x80", "\xF4\x90\x80\x80", "\xF4\x90\x80\x80");
    check("A\xCE\xA3\xFF" "A", "A\xCE\xA3\xFF" "A", "a\xCF\x82\xFF" "a");
    for (unsigned int c = 1; c <= 255; c++) {
        char s[] = {(char)c, 0};
        char u[] = {(char)(c >= 'a' && c <= 'z' ? c - 32 : c), 0};
        char l[] = {(char)(c >= 'A' && c <= 'Z' ? c + 32 : c), 0};
        check(s, u, l); /* includes every truncated lead and stray continuation */
    }
    for (unsigned int c = 0xE0; c <= 0xF4; c++) {
        char s[] = {(char)c, (char)0x80, 0};
        check(s, s, s);
    }
}
static void long_context(void) {
    const size_t marks = 100000, n = 3 + 2 * marks;
    char *s = malloc(n + 1);
    assert(s);
    memcpy(s, "A\xCE\xA3", 3);
    for (size_t i = 3; i < n; i += 2) { s[i] = (char)0xCC; s[i+1] = (char)0x81; }
    s[n] = 0;
    char *l = myon_unicode_case(s, false);
    assert(l && strlen(l) == n && l[0] == 'a' && (unsigned char)l[2] == 0x82);
    assert(memcmp(l + 3, s + 3, n - 3) == 0);
    free(l); free(s);
}
int main(void) {
    assert(setlocale(LC_CTYPE, "C"));
    vectors(); invalid_utf8(); long_context();
    if (setlocale(LC_CTYPE, "")) vectors();
    puts("Unicode case mapping tests passed");
    return 0;
}
