#ifndef ocean_c_ast
#define ocean_c_ast

#include "token.h"
#include <stddef.h>

typedef struct typeTokens {
    size_t nTokens;
    token *tokens;
} typeTokens;

struct asm_reference { size_t offset, length, operand; };
struct stmt_asm {
    token architecture, platform;
    char *assembly;
    size_t nOperands, nReferences;
    token *operands;
    struct asm_reference *references;
};

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
    STMT_FUNC, STMT_IF, STMT_EXPR, STMT_BLOCK, STMT_FOR, STMT_RETURN, STMT_PANIC, STMT_RETURN_PANIC,
    STMT_TYPE, STMT_ENUM, STMT_BREAK, STMT_CONTINUE, STMT_GOTO,
    STMT_LABEL, STMT_DEFER, STMT_SWITCH, STMT_FALLTHROUGH, STMT_ASM,
};

typedef struct stmt {
    enum stmtType type;
    void *value;
    token location; // First token of the statement.
    char forwardPanic; // Forward returned panics from calls in this statement.
} stmt;

struct stmt_block {
    token open, close;
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
    char enumPayload; // Private payload layout; explicit any arguments remain concrete.
    char enumStorage; // Private enum representation, excluded from struct reflection.
    token indexField; // Field delegated to by []; EOF_TOKEN if none is designated.
    size_t nTypeParams;
    typeTokens *typeParamConstraints;
    token *typeParamNames;
    size_t nVarParams;
    typeTokens *varParamTypes; // Non-owning view of the first nVarParams fieldTypes.
    token *varParamNames; // Non-owning view of the first nVarParams fieldNames.

    size_t nFields; // Includes the immutable value-parameter prefix, in declaration order.
    typeTokens *fieldTypes;
    token *fieldNames;
};

struct stmt_type {
    token name;
    const char *displayName;
    typeTokens underlying;
    char isAlias;
};

struct stmt_enum {
    token name;
    size_t nTypeParams;
    token *typeParamNames;
    typeTokens *typeParamConstraints;
    size_t nMembers;
    token *names;
    expr **values; // Optional fixed payloads.
    typeTokens *valueTypes;
    unsigned char *forms; // 0: unit, 1: tuple, 2: record, 3: fixed value.
    struct stmt_struct *payloads;
    struct stmt_struct storage; // Private {variant tag, boxed payload} representation.
    char prepared;
    char isBuiltin;
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
    char isMatch;
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
    typeTokens *constraints; // Zero for method interfaces, otherwise one type-list span.
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

enum compilerIntrinsic { INTRINSIC_NONE, INTRINSIC_REFLECT_FIELDS, INTRINSIC_REFLECT_IS_STRUCT, INTRINSIC_THREAD_SPAWN, INTRINSIC_THREAD_RETURN };

struct stmt_func {
    enum compilerIntrinsic compilerIntrinsic; // Set only by the trusted standard-library loader.
    typeTokens type; // Complete result syntax, including parentheses when present.
    token name;
    token receiverName; // EOF_TOKEN when this is not a method.
    typeTokens receiverType;
    // Names bind positionally to the receiver struct's type parameters.
    size_t nReceiverTypeParams;
    token *receiverTypeParamNames;

    size_t nParams;
    token *paramNames;
    typeTokens *paramTypeNames;
    char isVariadic; // Only the last parameter may be variadic.
    char isExtern;
    char isBuiltin;
    const char *displayName; // Unqualified source name for diagnostics.
    const char *externalName; // Original C symbol when package names are qualified.

    size_t nResults;
    token *resultNames; // EOF_TOKEN marks an unnamed parameter/result.
    typeTokens *resultTypes;

    size_t nTypeParams;
    token *typeParamNames;
    typeTokens *typeParamConstraints;

    char isLiteral;
    struct stmt_func *lexicalParent; // Non-owning lexical function, for capture analysis.
    size_t nCaptures, nCapturedLocals;
    token *captures, *capturedLocals; // Shared mutable cells, including transitive captures.
    struct stmt_block body;
};

struct stmt_if {
    token open, close;
    stmt *preStmt;
    expr *condition;

    stmt *ifTrue;
    stmt *ifFalse;
};

struct stmt_for {
    token open, close;
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
    EXPR_TYPE_ARRAY_NEW,
    EXPR_TYPE_ZERO,
    EXPR_TYPE_TYPE_NAME,
    EXPR_TYPE_MAKE, EXPR_TYPE_NEW, EXPR_TYPE_ENUM,
};

struct expr {
    enum exprType type;
    void *underlying;
    token location; // Literal/name token or the operator introducing this expression.
};

struct expr_identifier {
    token lexeme;
    char externReference; // Explicit extern.name namespace access.
};

struct expr_call {
    char spread; // Expand the final array argument.
    char packsVariadic; // This call creates a caller-scoped ordinary array.
    expr func;

    size_t nParams;
    expr *params;
    token location; // Opening call parenthesis, for runtime call traces.
};

// Generic values and expr_call.func for f[T, U](...). An identifier argument
// can also denote an index; name resolution decides ambiguous cases like a[i].
struct expr_instantiation {
    expr parent;
    size_t nTypeArguments;
    typeTokens *typeArguments;
    token location; // Opening bracket, retained when an ambiguous index is resolved.
};

struct expr_dot {
    struct stmt_func *interfaceMethod; // Resolved signature for dynamic method calls.
    expr parent;
    expr field;
};

struct expr_binary {
    expr left, right;
    enum binaryExprType operation;
    typeTokens checkedResult; // Concrete result of a universally checked boxed operator.
    typeTokens operandTypes[2]; // Borrowed type-list constraints for boxed dispatch.
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
    char unsignedLiteral;
    size_t stringLength;
    void *value;
};

// zero(Type) and explicit mixed/interface literal prefixes retain type syntax.
struct expr_zero {
    typeTokens type;
};

struct expr_array_new {
    typeTokens elementType;
    expr count;
    char buffer; // Allocate raw buffer storage rather than a collection value.
};

struct expr_unary {
    typeTokens sizeofType; // Present for sizeof(T), rather than sizeof(value).
    typeTokens operandTypes; // Borrowed type-list constraint for boxed dispatch.
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
    expr *pattern; // Optional named record extraction pattern.
};

struct expr_index {
    expr parent;
    expr index; // EXPR_TYPE_NULL denotes access to the delegated collection.
    token location; // Opening subscript bracket.
    char tryIndex; // Two-binding indexing returns (value, success).
};

struct expr_composite {
    typeTokens literalType; // Resolved explicit prefix, for standalone construction.
    token location;
    char inferredArray; // An inferred declaration defaults to an array.
    expr type; // EXPR_TYPE_NULL for an inferred-type literal: {1, 2, 3}.
    size_t nEntries;
    expr *keys; // EXPR_TYPE_NULL entries for positional values.
    expr *values;
};

enum enumOperation { ENUM_CONSTRUCT, ENUM_VARIANT, ENUM_EXTRACT, ENUM_TAG };
struct expr_enum {
    enum enumOperation operation;
    struct stmt_enum *declaration;
    size_t member;
    size_t nTypeArguments;
    typeTokens *typeArguments; // Constructor specialization, resolved again by native monomorphization.
    expr child; // Extraction/tag source or constructor payload.
    size_t nFields;
    size_t *fields; // Selected record fields in binding order; NULL means full payload.
};

// print an AST to the terminal in (operator operands...) notation (eg. (PLUS 2 2) is 2 + 2)
// unsafe and unfinished, NEVER CALL IN PRODUCTION CODE
void printAst(expr parent);

#endif
