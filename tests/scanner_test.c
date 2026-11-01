#define _POSIX_C_SOURCE 200809L
#include "../scanner.h"
#include "test_support.h"
#include <math.h>
#include <limits.h>

static tokenlist *scan(char *source) { return scanTokens(source, "scanner-test"); }
static void release(tokenlist *list) { free_tokenlist(list); free(list); }
static void kinds(char *source, const enum tokenType *expected, size_t count) {
    tokenlist *list = scan(source);
    assert(list->top == (long)count + 1);
    for (size_t i = 0; i < count; i++) assert(list->arr[i].type == expected[i]);
    assert(list->arr[count].type == EOF_TOKEN);
    release(list);
}
static void unicode_escapes(void) {
    tokenlist *list = scan("\"\\u03A9\\U0001F30A\"");
    assert(!strcmp(list->arr[0].lexeme, "Ω🌊"));
    release(list);
}
static char *badSource;
static void bad_scan(void) { release(scan(badSource)); }
static void rejected(char *source, const char *diagnostic) {
    badSource = source;
    int status;
    char *output = capture_child(bad_scan, &status);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    assert(strstr(output, diagnostic));
    free(output);
    tokenlist *list = scan("ok"); // A later scan starts with clean state.
    assert(list->arr[0].type == IDENTIFIER);
    release(list);
}
int main(void) {
    enum tokenType keywords[] = {TYPE, STRUCT, FUNC, IMPORT, ANY, MIXED, INTERFACE,
        CONTINUE, BREAK, ENUM, IF, DEFER, SWITCH, FALLTHROUGH, EXTERN, FOR,
        GOTO, AS, PACKAGE, ELSE, RANGE, RETURN, CASE, DEFAULT, NULL_KEYWORD, PANIC};
    kinds("type struct func import any mixed interface continue break enum if defer "
          "switch fallthrough extern for goto as package else range return case default null panic",
          keywords, sizeof(keywords) / sizeof(*keywords));
    enum tokenType operators[] = {LEFT_CURLY_BRACE, RIGHT_CURLY_BRACE, LEFT_PAREN,
        RIGHT_PAREN, LEFT_BRACKET, RIGHT_BRACKET, UNDERSCORE, SEMICOLON, COMMA,
        DOT, COLON, QUESTION_MARK, ARROW, PLUS, MINUS, STAR, SLASH, MODULO,
        XOR, AMPERSAND, TILDE, EXCLAMATION, EQUALS_EQUALS, NOT_EQUALS,
        LESS_THAN, GREATER_THAN, LESS_EQUALS, GREATER_EQUALS, BITSHIFT_LEFT,
        BITSHIFT_RIGHT, BITWISE_OR, LOGICAL_OR, LOGICAL_AND, EQUALS,
        PLUS_EQUALS, MINUS_EQUALS, STAR_EQUALS, SLASH_EQUALS, MODULO_EQUALS,
        XOR_EQUALS, TILDE_EQUALS, AMPERSAND_EQUALS, BITWISE_OR_EQUALS,
        BITSHIFT_LEFT_EQUALS, BITSHIFT_RIGHT_EQUALS, PLUS_PLUS, MINUS_MINUS};
    kinds("{ } ( ) [ ] _ ; , . : ? -> + - * / % ^ & ~ ! == != < > <= >= << >> "
          "| || && = += -= *= /= %= ^= ~= &= |= <<= >>= ++ --",
          operators, sizeof(operators) / sizeof(*operators));
    tokenlist *list = scan("alpha\n beta2\tindex\r\nreturning");
    assert(list->top == 5 && !strcmp(list->arr[0].lexeme, "alpha") &&
           !strcmp(list->arr[1].lexeme, "beta2") && list->arr[2].type == IDENTIFIER &&
           !strcmp(list->arr[3].lexeme, "returning"));
    assert(list->arr[0].line == 1 && list->arr[0].column == 1 &&
           list->arr[1].line == 2 && list->arr[1].column == 2 &&
           list->arr[3].line == 3 && !strcmp(list->arr[3].filename, "scanner-test"));
    release(list);
    list = scan("\n\n\tarr[index]");
    assert(list->arr[0].line == 3 && list->arr[0].column == 2);
    assert(list->arr[1].type == LEFT_BRACKET && list->arr[1].line == 3 && list->arr[1].column == 5);
    release(list);
    enum tokenType names[] = {IDENTIFIER, IDENTIFIER};
    kinds("first /* outer /* nested */ still outer */ second // EOF comment", names, 2);
    kinds("first // comment\nsecond", names, 2);
    kinds(" \t\r\n", NULL, 0);
    kinds("", NULL, 0);
    list = scan("0 42 0xff 0XAF 0b101 0B11 0o17 0O10");
    long integers[] = {0, 42, 255, 175, 5, 3, 15, 8};
    for (size_t i = 0; i < 8; i++) {
        assert(list->arr[i].type == LITERAL_INT && list->arr[i].numValue.integer == integers[i]);
        assert(strlen(list->arr[i].lexeme) > 0);
    }
    assert(!strcmp(list->arr[2].lexeme, "0xff"));
    release(list);
    list = scan("12.5 0.25 1.0");
    double floats[] = {12.5, 0.25, 1.0};
    assert(list->top == 4);
    for (size_t i = 0; i < 3; i++)
        assert(list->arr[i].type == LITERAL_FLOAT && fabs(list->arr[i].numValue.floating - floats[i]) < 1e-9);
    assert(!strcmp(list->arr[0].lexeme, "12.5"));
    release(list);
    list = scan("\"\" `raw\ntext` \"a\\n\\t\\r\\\\\\\"\\'\\?\\a\\b\\f\\v\" \"\\x41\\101\"");
    assert(list->top == 5 && !strcmp(list->arr[0].lexeme, "") &&
           !strcmp(list->arr[1].lexeme, "raw\ntext") &&
           !strcmp(list->arr[2].lexeme, "a\n\t\r\\\"'?\a\b\f\v") &&
           !strcmp(list->arr[3].lexeme, "AA"));
    release(list);
    list = scan("'a' '\\n' '\\x41' '\\101'");
    long chars[] = {'a', '\n', 'A', 'A'};
    assert(list->top == 5);
    for (size_t i = 0; i < 4; i++)
        assert(list->arr[i].type == LITERAL_INT && list->arr[i].numValue.integer == chars[i]);
    assert(!strcmp(list->arr[0].lexeme, "'a'"));
    release(list);
    list = scan("\"\\08\" \"\\x00A\" \"\\e\"");
    assert(list->arr[0].stringLength == 2 && list->arr[1].stringLength == 2);
    assert(!memcmp(list->arr[0].lexeme, "\0" "8\0", 3));
    assert(!memcmp(list->arr[1].lexeme, "\0A\0", 3));
    assert((unsigned char)list->arr[2].lexeme[0] == 27 && !list->arr[2].lexeme[1]);
    release(list);
    list = scan("\"Ω\"");
    assert(list->arr[0].type == LITERAL_STRING && list->arr[0].stringLength == 2);
    assert(!memcmp(list->arr[0].lexeme, "\xCE\xA9", 2));
    release(list);
    int unicodeStatus;
    char *unicodeOutput = capture_child(unicode_escapes, &unicodeStatus);
    assert(WIFEXITED(unicodeStatus) && !WEXITSTATUS(unicodeStatus));
    free(unicodeOutput);
    list = scan("_name Ωmega café a\xcc\x81 Δelta");
    assert(list->top == 6);
    for (size_t i=0;i<5;i++) assert(list->arr[i].type == IDENTIFIER);
    assert(exportedName(list->arr[4].lexeme) && !exportedName(list->arr[1].lexeme + 2));
    release(list);
    list = scan("1e3 2.5E-2 3e+2 0e99999 18446744073709551615 0xffffffffffffffff 'Ω' '🌊'");
    assert(list->top == 9);
    assert(list->arr[0].numValue.floating == 1000);
    assert(fabs(list->arr[1].numValue.floating - .025) < 1e-12);
    assert(list->arr[2].numValue.floating == 300 && list->arr[3].numValue.floating == 0);
    for(size_t i=4;i<6;i++) assert(list->arr[i].unsignedLiteral && list->arr[i].numValue.unsignedInteger == UINT64_MAX);
    assert(list->arr[6].numValue.integer == 0x3a9 && list->arr[7].numValue.integer == 0x1f30a);
    release(list);
    rejected("1e", "exponent"); rejected("1e+", "exponent");
    rejected("18446744073709551616", "overflow");
    rejected("0x10000000000000000", "overflow");
    rejected("\"\\uD800\"", "Unicode"); rejected("\"\\U00110000\"", "Unicode");
    rejected("\xc0\xaf", "UTF-8"); rejected("\xf4\x90\x80\x80", "UTF-8");
    rejected("'\xed\xa0\x80'", "UTF-8");
    char boundary[80];
    snprintf(boundary, sizeof(boundary), "%ld", LONG_MAX);
    list = scan(boundary);
    assert(list->arr[0].numValue.integer == LONG_MAX);
    release(list);
    snprintf(boundary, sizeof(boundary), "%ld0", LONG_MAX);
    rejected(boundary, "overflow");
    rejected("0b1.2", "invalid");
    rejected("'\n'", "character");
    rejected("\"\\q\"", "escape");
    rejected("\"\\x4\"", "escape");
    rejected("0b2", "invalid"); rejected("0o8", "invalid");
    rejected("0x", "digit"); rejected("\"unfinished", "string");
    rejected("\"newline\n\"", "newline"); rejected("`unfinished", "raw string");
    rejected("/* unfinished", "comment"); rejected("'", "character");
    rejected("'ab'", "character"); rejected("'a", "character");
    rejected("\"\\xG0\"", "escape"); rejected("\"\\400\"", "escape");
    rejected("@", "invalid");
    puts("scanner token, value, position, comment, and error checks passed");
}
