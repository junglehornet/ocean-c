#define _POSIX_C_SOURCE 200809L
#include "../token.h"
#include "../scanner.h"
#include "test_support.h"

static void token_output(void) {
    printToken((token){.type = IDENTIFIER, .lexeme = "value"});
    printToken((token){.type = LITERAL_STRING, .lexeme = "hello"});
    printToken((token){.type = LITERAL_INT, .lexeme = "42", .numValue.integer = 42});
    printToken((token){.type = LITERAL_FLOAT, .lexeme = "1.25", .numValue.floating = 1.25});
    printToken((token){.type = LOGICAL_AND});
    token items[] = {{.type = PLUS}, {.type = EOF_TOKEN}};
    tokenlist list = {.arr = items, .top = 2, .len = 2};
    printTokensFromFile("sample.oc", &list);
}
#define NAME(t) {t, #t}
int main(void) {
    struct { enum tokenType type; const char *name; } names[] = {
        NAME(TYPE), NAME(STRUCT), NAME(FUNC), NAME(IMPORT), NAME(ANY), NAME(INTERFACE),
        NAME(CONTINUE), NAME(BREAK), NAME(ENUM), NAME(IF), NAME(DEFER), NAME(SWITCH),
        NAME(FALLTHROUGH), NAME(EXTERN), NAME(FOR), NAME(GOTO), NAME(AS), NAME(PACKAGE),
        NAME(ELSE), NAME(RANGE), NAME(RETURN), NAME(PANIC), NAME(CASE), NAME(DEFAULT), {NULL_KEYWORD, "NULL"},
        NAME(LEFT_CURLY_BRACE), NAME(RIGHT_CURLY_BRACE), NAME(LEFT_PAREN), NAME(RIGHT_PAREN),
        NAME(LEFT_BRACKET), NAME(RIGHT_BRACKET), NAME(UNDERSCORE), NAME(SEMICOLON),
        NAME(BACKTICK), NAME(COMMA), NAME(DOT), NAME(COLON), {QUESTION_MARK, "QUESTION"},
        NAME(ARROW), NAME(PLUS), NAME(MINUS), NAME(STAR), NAME(SLASH), NAME(MODULO),
        NAME(XOR), NAME(AMPERSAND), NAME(TILDE), NAME(EXCLAMATION), NAME(EQUALS_EQUALS),
        NAME(NOT_EQUALS), NAME(LESS_THAN), NAME(GREATER_THAN), NAME(LESS_EQUALS), NAME(GREATER_EQUALS),
        NAME(BITSHIFT_LEFT), NAME(BITSHIFT_RIGHT), NAME(BITWISE_OR), NAME(LOGICAL_OR), NAME(LOGICAL_AND),
        NAME(EQUALS), NAME(PLUS_EQUALS), NAME(MINUS_EQUALS), NAME(STAR_EQUALS), NAME(SLASH_EQUALS),
        NAME(MODULO_EQUALS), NAME(XOR_EQUALS), NAME(TILDE_EQUALS), NAME(AMPERSAND_EQUALS),
        NAME(BITWISE_OR_EQUALS), NAME(BITSHIFT_LEFT_EQUALS), NAME(BITSHIFT_RIGHT_EQUALS),
        NAME(PLUS_PLUS), NAME(MINUS_MINUS), NAME(LITERAL_INT), NAME(LITERAL_FLOAT),
        NAME(LITERAL_STRING), NAME(IDENTIFIER), {EOF_TOKEN, "EOF"}
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if (strcmp(getTokenTypeStr(names[i].type), names[i].name))
            fprintf(stderr, "Expected token name %s, got %s\n", names[i].name, getTokenTypeStr(names[i].type));
        assert(!strcmp(getTokenTypeStr(names[i].type), names[i].name));
    }
    assert(!strcmp(getTokenTypeStr((enum tokenType)-1), "???"));
    assert(!strcmp(getTokenTypeStr(TOKEN_SPECIAL_ASSIGN_START), "???"));
    int status;
    char *output = capture_child(token_output, &status);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    assert(strstr(output, "(IDENTIFIER value) (LITERAL_STRING hello) (LITERAL_INT \"42\" 42) "
                          "(LITERAL_FLOAT \"1.25\" 1.250000) LOGICAL_AND "));
    assert(strstr(output, "--==[sample.oc]==--\nPLUS EOF "));
    free(output);
    puts("token names and formatted output checks passed");
}
