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
    EXPR_BITWISE_AND_EQUALS, EXPR_BITWISE_OR_EQUALS,

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
    STMT_FUNC, STMT_IF, STMT_EXPR, STMT_BLOCK, STMT_FOR, STMT_RETURN,
    STMT_TYPE, STMT_ENUM, STMT_BREAK, STMT_CONTINUE, STMT_GOTO,
    STMT_LABEL, STMT_DEFER, STMT_SWITCH, STMT_FALLTHROUGH,
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

struct stmt_return {
    size_t nValues;
    expr *values;
};

struct stmt_package {
    char *name;
};

struct stmt_import {
    char *path;
    char *importedName;
};

struct stmt_var {
    typeTokens type;
    size_t nVars;
    token *vars;
    size_t nInitValues;
    expr *initValues;
};

struct stmt_struct {
    token name;
    token indexField; // Field delegated to by []; EOF_TOKEN if none is designated.
    size_t nTypeParams;
    typeTokens *typeParamConstraints;
    token *typeParamNames;
    size_t nVarParams;
    typeTokens *varParamTypes; // Complete type spans, one per value parameter.
    token *varParamNames;

    size_t nOperatorMethods;
    struct stmt_func *operatorMethods; // name holds the operator token; receiver is implicit self.
    size_t nFields;
    typeTokens *fieldTypes;
    token *fieldNames;
};

struct stmt_type {
    token name;
    typeTokens underlying;
    char isAlias;
};

struct stmt_enum {
    token name;
    size_t nMembers;
    token *names;
    expr **values; // NULL entries use the language's implicit enumeration value.
};

struct stmt_branch {
    token label; // EOF_TOKEN for an unlabeled break/continue.
};

struct stmt_label {
    token name;
    stmt *body;
};

struct stmt_defer {
    expr *call;
};

struct switch_case {
    size_t nValues; // Zero for default.
    expr *values;
    size_t nTypes; // Type-switch cases retain complete type spans.
    typeTokens *types;
    struct stmt_block body;
};

struct stmt_switch {
    stmt initializer; // STMT_NULL when absent.
    char isTypeSwitch;
    token binding; // EOF_TOKEN when the type guard has no binding.
    expr *value; // NULL for a condition-only switch.
    size_t nCases;
    struct switch_case *cases;
};

struct stmt_interface {
    token name;
    size_t nTypeParams;
    token *typeParamNames;
    typeTokens *typeParamConstraints;
    size_t nConstraints;
    typeTokens *constraints; // One span per embedded type/union, excluding ';'.
    size_t nMethods;
    struct stmt_func *methods;
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
    typeTokens type; // Complete result syntax, including parentheses when present.
    token name;
    token receiverName; // EOF_TOKEN when this is not a method.
    typeTokens receiverType;

    size_t nParams;
    token *paramNames;
    typeTokens *paramTypeNames;
    char isVariadic; // Only the last parameter may be variadic.
    char isExtern;

    size_t nResults;
    token *resultNames; // EOF_TOKEN marks an unnamed parameter/result.
    typeTokens *resultTypes;

    size_t nTypeParams;
    token *typeParamNames;
    typeTokens *typeParamConstraints;

    struct stmt_block body;
};

struct stmt_if {
    stmt *preStmt;
    expr *condition;

    stmt *ifTrue;
    stmt *ifFalse;
};

struct stmt_for {
    stmt *initializer;  // Executed once before the loop; optional.
    expr *condition;    // NULL means true for a three-clause loop.
    expr *update;       // Executed after each iteration; optional.
    stmt *body;
    char deferCondition; // Skip the first condition check only.

    // Range loops use bindings/range instead of initializer/condition/update.
    size_t nBindings;
    token bindings[2];
    expr *range;
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
    EXPR_TYPE_FUNCTION,
    EXPR_TYPE_COMPOSITE,
    EXPR_TYPE_INSTANTIATION,
    EXPR_TYPE_ASSERTION,
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

// Generic values and expr_call.func for f[T, U](...). An identifier argument
// can also denote an index; name resolution decides ambiguous cases like a[i].
struct expr_instantiation {
    expr parent;
    size_t nTypeArguments;
    typeTokens *typeArguments;
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
    typeTokens type; // Arrow target, excluding the outer delimiter parentheses.
};

struct expr_assertion {
    expr child;
    typeTokens type; // Empty for .(type), which is restricted to switch guards.
    char isTypeGuard;
};

struct expr_index {
    expr parent;
    expr index; // EXPR_TYPE_NULL denotes access to the delegated collection.
};

struct expr_composite {
    expr type; // EXPR_TYPE_NULL for an inferred-type literal: {1, 2, 3}.
    size_t nEntries;
    expr *keys; // EXPR_TYPE_NULL entries for positional values.
    expr *values;
};

// print an AST to the terminal in (operator operands...) notation (eg. (PLUS 2 2) is 2 + 2)
// unsafe and unfinished, NEVER CALL IN PRODUCTION CODE
void printAst(expr parent);

#endif
