#define _POSIX_C_SOURCE 200809L
#include "../ast.h"
#include "test_support.h"
#include <signal.h>

static expr value;
static void output_ast(void) { printAst(value); }
static void printed(expr tree, const char *expected) {
    value = tree;
    int status;
    char *output = capture_child(output_ast, &status);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    char *body = strstr(output, "\n\n");
    assert(body && !strcmp(body + 2, expected));
    free(output);
}
int main(void) {
    struct expr_identifier id = {.lexeme = {.type = IDENTIFIER, .lexeme = "x"}};
    expr x = {.type = EXPR_TYPE_IDENTIFIER, .underlying = &id};
    long integer = 42;
    double real = 1.25;
    struct expr_literal literal = {.type = LITERAL_TYPE_INT, .value = &integer};
    expr lit = {.type = EXPR_TYPE_LITERAL, .underlying = &literal};
    printed(lit, "42\n");
    literal.type = LITERAL_TYPE_FLOAT; literal.value = &real; printed(lit, "1.250000\n");
    literal.type = LITERAL_TYPE_STRING; literal.value = "hello"; printed(lit, "\"hello\"\n");
    literal.type = LITERAL_TYPE_INT; literal.value = &integer;
    printed(x, "x\n");
    const char *unaryNames[] = {"NEGATE", "BITWISE_NOT", "LOGICAL_NOT", "DEREFERENCE",
        "ADDRESS_OF", "SIZEOF", "POSTFIX_INCREMENT", "POSTFIX_DECREMENT",
        "PREFIX_INCREMENT", "PREFIX_DECREMENT"};
    for (size_t i = 0; i < sizeof(unaryNames) / sizeof(*unaryNames); i++) {
        struct expr_unary unary = {.child = x, .operation = (enum unaryExprType)(i + 1)};
        char expected[128]; snprintf(expected, sizeof(expected), "(%s x)\n", unaryNames[i]);
        printed((expr){.type = EXPR_TYPE_UNARY, .underlying = &unary}, expected);
    }
    const char *binaryNames[] = {"ADD", "SUBTRACT", "MULTIPLY", "DIVIDE", "MODULO", "XOR",
        "AMPERSAND", "EQUALITY", "NOT_EQUALS", "LESS_THAN", "GREATER_THAN", "LESS_EQUALS",
        "GREATER_EQUALS", "BITSHIFT_LEFT", "BITSHIFT_RIGHT", "LOGICAL_OR", "BITWISE_OR",
        "BITWISE_AND", "LOGICAL_AND", "ADD_EQUALS", "SUBTRACT_EQUALS", "MULTIPLY_EQUALS",
        "DIVIDE_EQUALS", "MODULO_EQUALS", "XOR_EQUALS", "BITSHIFT_LEFT_EQUALS",
        "BITSHIFT_RIGHT_EQUALS", "BITWISE_AND_EQUALS", "BITWISE_OR_EQUALS", "COMMA", "ASSIGNMENT"};
    for (size_t i = 0; i < sizeof(binaryNames) / sizeof(*binaryNames); i++) {
        struct expr_binary binary = {.left = x, .right = lit, .operation = (enum binaryExprType)(i + 1)};
        char expected[128]; snprintf(expected, sizeof(expected), "(%s x 42)\n", binaryNames[i]);
        printed((expr){.type = EXPR_TYPE_BINARY, .underlying = &binary}, expected);
    }
    struct expr_ternary ternary = {.condition = x, .ifTrue = lit, .ifFalse = x};
    printed((expr){.type = EXPR_TYPE_TERNARY, .underlying = &ternary}, "(TERNARY x 42 x)\n");
    expr args[] = {x, lit};
    struct expr_call call = {.func = x, .nParams = 0};
    expr callable = {.type = EXPR_TYPE_CALL, .underlying = &call};
    printed(callable, "(FUNCTION_CALL x)\n");
    call.nParams = 2; call.params = args;
    printed(callable, "(FUNCTION_CALL x x 42)\n");
    // Large strings must not overflow the printer's former fixed-size buffers.
    char *large = malloc(20001), *expected = malloc(20005);
    assert(large && expected); memset(large, 'a', 20000); large[20000] = 0;
    literal.type = LITERAL_TYPE_STRING; literal.value = large;
    snprintf(expected, 20005, "\"%s\"\n", large); printed(lit, expected);
    free(large); free(expected);
    value = (expr){0};
    int status;
    char *output = capture_child(output_ast, &status);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(strstr(output, "Unknown expr type"));
    free(output);
    puts("AST debug printer checks passed");
}
