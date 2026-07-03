#ifndef ocean_c_ast
#define ocean_c_ast

#include "token.h"
#include <stddef.h>

typedef struct typeTokens {
    size_t nTokens;
    token *tokens;
} typeTokens;

enum binaryExprType {
    EXPR_NULL = 0,
    // operators
    EXPR_ADD, EXPR_SUBTRACT, EXPR_MULTIPLY, EXPR_DIVIDE, EXPR_MODULO,
    EXPR_XOR, EXPR_AMPERSAND, EXPR_EQUALITY, EXPR_NOT_EQUALS, EXPR_LESS_THAN,
    EXPR_GREATER_THAN, EXPR_LESS_EQUALS, EXPR_GREATER_EQUALS, EXPR_BITSHIFT_LEFT,
    EXPR_BITSHIFT_RIGHT, EXPR_LOGICAL_OR, EXPR_BITWISE_OR, EXPR_BITWISE_AND,
    EXPR_LOGICAL_AND,

    // compound assignment operators
    EXPR_ADD_EQUALS, EXPR_SUBTRACT_EQUALS, EXPR_MULTIPLY_EQUALS,
    EXPR_DIVIDE_EQUALS, EXPR_MODULO_EQUALS, EXPR_XOR_EQUALS,
    EXPR_BITSHIFT_LEFT_EQUALS, EXPR_BITSHIFT_RIGHT_EQUALS,

    // other
    EXPR_COMMA, EXPR_ASSIGNMENT,
};

enum unaryExprType {
    UNARY_NULL = 0,
    UNARY_NEGATE, UNARY_BITWISE_NOT, UNARY_LOGICAL_NOT, UNARY_DEREFERENCE,
    UNARY_ADDRESS_OF, UNARY_SIZEOF, UNARY_POSTFIX_INCREMENT, UNARY_POSTFIX_DECREMENT,
    UNARY_PREFIX_INCREMENT, UNARY_PREFIX_DECREMENT,
};

enum stmtType {
    STMT_NULL = 0,
    STMT_PACKAGE, STMT_IMPORT, STMT_VAR, STMT_STRUCT, STMT_INTERFACE,
    STMT_FUNC, STMT_IF, STMT_WHILE, STMT_EXPR, STMT_BLOCK,
};

typedef struct stmt {
    enum stmtType type;
    void *value;
} stmt;

struct stmt_block {
    size_t nStmts;
    stmt *stmts;
};

typedef struct expr expr;

struct stmt_package {
    char *name;
};

struct stmt_import {
    char *path;
    char *importedName;
};

struct stmt_var {
    typeTokens type;
    token var;
    expr *initValue;
};

struct stmt_struct {
    size_t nTypeParams;
    token *typeParamInterfaces;
    token *typeParamNames;
    size_t nVarParams;
    char **varParamTypes;
    char **varParamNames;

    size_t nFields;
    typeTokens *fieldTypes;
    token *fieldNames;
};

struct stmt_interface {
    token name;
    struct methodSignature {
        token name;
        typeTokens returnType;
        size_t nParams;
        char **paramTypes;
    } *signatures;
    size_t nFields;
    token *fieldNames;
    typeTokens *fieldTypes;
};

struct stmt_func {
    typeTokens type;
    token name;

    size_t nParams;
    token *paramNames;
    typeTokens *paramTypeNames;

    struct stmt_block body;
};

struct stmt_if {
    stmt *preStmt;
    expr *condition;

    stmt *ifTrue;
    stmt *ifFalse;
};

struct stmt_while {
    expr *condition;
    stmt *body;
    char isDoWhile;
};

/* expr types:
    EXPR_BINARY, EXPR_LITERAL, EXPR_GROUPING, EXPR_ASSIGNMENT,
    EXPR_IDENTIFIER, EXPR_DOT, EXPR_CALL, EXPR_TERNARY,
*/

enum exprType {
    EXPR_TYPE_NULL = 0,
    EXPR_TYPE_IDENTIFIER,
    EXPR_TYPE_DOT,
    EXPR_TYPE_BINARY,
    EXPR_TYPE_LITERAL,
    EXPR_TYPE_CALL,
    EXPR_TYPE_TERNARY,
    EXPR_TYPE_UNARY,
    EXPR_TYPE_TYPECAST,
    EXPR_TYPE_INDEX,
};

struct expr {
    enum exprType type;
    void *underlying;
};

struct expr_identifier {
    token lexeme;
};

struct expr_call {
    expr func;

    size_t nParams;
    expr *params;
};

struct expr_dot {
    expr parent;
    expr field;
};

struct expr_binary {
    expr left, right;
    enum binaryExprType operation;
};

struct expr_ternary {
    expr condition;
    expr ifTrue;
    expr ifFalse;
};

enum literalType {
    LITERAL_TYPE_NULL = 0,
    LITERAL_TYPE_INT, LITERAL_TYPE_FLOAT, LITERAL_TYPE_STRING,
};

struct expr_literal {
    enum literalType type;
    void *value;
};

struct expr_unary {
    expr child;
    enum unaryExprType operation;
};

struct expr_typecast {
    expr child;
    typeTokens type;
};

struct expr_index {
    expr parent;
    expr index;
};

// print an AST to the terminal in (operator operands...) notation (eg. (PLUS 2 2) is 2 + 2)
// unsafe and unfinished, NEVER CALL IN PRODUCTION CODE
void printAst(expr parent);

#endif
