#include "ast.h"

#include <stdio.h>
#include <stdlib.h>

// this file contains quick-and-dirty code to print out an AST in the notation of (operator operands...)
// this is incomplete, unsafe, and not memory-efficient code that is prone to crash so NEVER CALL IN PRODUCTION

static char* stringOf(expr e) {
    if (e.type == EXPR_TYPE_LITERAL) {
        char *str = malloc(10240);
        switch ((*(struct expr_literal*)e.underlying).type) {
            case LITERAL_TYPE_INT:
                sprintf(str, "%ld", *(long*)((struct expr_literal*)e.underlying)->value);
                break;
            case LITERAL_TYPE_FLOAT:
                sprintf(str, "%lf", *(double*)((struct expr_literal*)e.underlying)->value);
                break;
            case LITERAL_TYPE_STRING:
                sprintf(str, "\"%s\"", (char*)((struct expr_literal*)e.underlying)->value);
                break;
        }
        return str;
    }
    if (e.type == EXPR_TYPE_IDENTIFIER) return (*(struct expr_identifier*)e.underlying).lexeme.lexeme;
    char* type = "[UNKNOWN TYPE]";
    if (e.type == EXPR_TYPE_UNARY) {
        struct expr_unary unary = *(struct expr_unary*)e.underlying;
        switch (unary.operation) {
            case UNARY_NEGATE:
                type = "NEGATE";
                break;
            case UNARY_BITWISE_NOT:
                type = "BITWISE_NOT";
                break;
            case UNARY_LOGICAL_NOT:
                type = "LOGICAL_NOT";
                break;
            case UNARY_DEREFERENCE:
                type = "DEREFERENCE";
                break;
            case UNARY_ADDRESS_OF:
                type = "ADDRESS_OF";
                break;
            case UNARY_SIZEOF:
                type = "SIZEOF";
                break;
            case UNARY_POSTFIX_INCREMENT:
                type = "POSTFIX_INCREMENT";
                break;
            case UNARY_POSTFIX_DECREMENT:
                type = "POSTFIX_DECREMENT";
                break;
            case UNARY_PREFIX_INCREMENT:
                type = "PREFIX_INCREMENT";
                break;
            case UNARY_PREFIX_DECREMENT:
                type = "PREFIX_DECREMENT";
                break;
        }
        char *str = malloc(10240);
        char *child = stringOf(unary.child);
        sprintf(str, "(%s %s)", type, child);
        free(child);
        return str;
    }
    if (e.type == EXPR_TYPE_TERNARY) {
        struct expr_ternary ternary = *(struct expr_ternary*)e.underlying;
        char *str = malloc(10240);
        char *condition = stringOf(ternary.condition);
        char *ifTrue = stringOf(ternary.ifTrue);
        char *ifFalse = stringOf(ternary.ifFalse);
        sprintf(str, "(TERNARY %s %s %s)", condition, ifTrue, ifFalse);
        free(condition);
        free(ifTrue);
        free(ifFalse);
        return str;
    }
    if (e.type == EXPR_TYPE_CALL) {
        struct expr_call call = *(struct expr_call*)e.underlying;
        char *str = malloc(10240);
        char *funName = stringOf(call.func);
        char **params = calloc(call.nParams, sizeof(char*));
        for (size_t i = 0; i < call.nParams; i++) {
            params[i] = stringOf(call.params[i]);
        }
        sprintf(str, "(FUNCTION_CALL %s %s)", funName, params[0]);
        free(funName);
        for (size_t i = 0; i < call.nParams; i++) {
            free(params[i]);
        }
        free(params);
        return str;
    }
    if (e.type != EXPR_TYPE_BINARY) {
        printf("Unknown expr type\n");
        abort();
    }
    struct expr_binary binary = *(struct expr_binary*)e.underlying;
    switch (binary.operation) {
        case EXPR_ADD:
            type = "ADD";
            break;
        case EXPR_SUBTRACT:
            type = "SUBTRACT";
            break;
        case EXPR_MULTIPLY:
            type = "MULTIPLY";
            break;
        case EXPR_DIVIDE:
            type = "DIVIDE";
            break;
        case EXPR_MODULO:
            type = "MODULO";
            break;
        case EXPR_XOR:
            type = "XOR";
            break;
        case EXPR_AMPERSAND:
            type = "AMPERSAND";
            break;
        case EXPR_EQUALITY:
            type = "EQUALITY";
            break;
        case EXPR_NOT_EQUALS:
            type = "NOT_EQUALS";
            break;
        case EXPR_LESS_THAN:
            type = "LESS_THAN";
            break;
        case EXPR_GREATER_THAN:
            type = "GREATER_THAN";
            break;
        case EXPR_LESS_EQUALS:
            type = "LESS_EQUALS";
            break;
        case EXPR_GREATER_EQUALS:
            type = "GREATER_EQUALS";
            break;
        case EXPR_BITSHIFT_LEFT:
            type = "BITSHIFT_LEFT";
            break;
        case EXPR_BITSHIFT_RIGHT:
            type = "BITSHIFT_RIGHT";
            break;
        case EXPR_LOGICAL_OR:
            type = "LOGICAL_OR";
            break;
        case EXPR_BITWISE_OR:
            type = "BITWISE_OR";
            break;
        case EXPR_BITWISE_AND:
            type = "BITWISE_AND";
            break;
        case EXPR_LOGICAL_AND:
            type = "LOGICAL_AND";
            break;
        case EXPR_ADD_EQUALS:
            type = "ADD_EQUALS";
            break;
        case EXPR_SUBTRACT_EQUALS:
            type = "SUBTRACT_EQUALS";
            break;
        case EXPR_MULTIPLY_EQUALS:
            type = "MULTIPLY_EQUALS";
            break;
        case EXPR_DIVIDE_EQUALS:
            type = "DIVIDE_EQUALS";
            break;
        case EXPR_MODULO_EQUALS:
            type = "MODULO_EQUALS";
            break;
        case EXPR_XOR_EQUALS:
            type = "XOR_EQUALS";
            break;
        case EXPR_BITSHIFT_LEFT_EQUALS:
            type = "BITSHIFT_LEFT_EQUALS";
            break;
        case EXPR_BITSHIFT_RIGHT_EQUALS:
            type = "BITSHIFT_RIGHT_EQUALS";
            break;
        case EXPR_COMMA:
            type = "COMMA";
            break;
        case EXPR_ASSIGNMENT:
            type = "ASSIGNMENT";
            break;
    }
    char *str = malloc(10240);
    char *left = stringOf(binary.left);
    char *right = stringOf(binary.right);
    sprintf(str, "(%s %s %s)", type, left, right);
    free(left);
    free(right);
    return str;
}

void printAst(expr parent) {
    printf("Warning: printAst() called. If you are a user and this is an official build of the\n"
           "Ocean compiler then this is a bug. Please report this to the Ocean developers.\n\n");
    char *str = stringOf(parent);
    printf("%s\n", str);
    free(str);
}