#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "parser.h"

#include <math.h>

#include "c-hashmap/map.h"
#include "dynarray.h"
#include "token.h"
#include "ast.h"
#include "core.h"
#include "scanner.h"

typedef struct importedSymbol {
    char *name;
    char *type;
} importedSymbol;

typedef struct importResult {
    char *pkgName;
    char *importedName;

    size_t nImportedSymbols;
    importedSymbol *importedSymbols;
} importResult;

typedef struct parserState {
    struct scope globalScope;
    struct scope *localScope;

    char *packageName;

    token *tokens;
    size_t tokenIndex;
    size_t tokensLen;

    size_t nImports;
    importResult **importResults;
    
    int packageRootfd;

    char hasError;
    char isSynchronizing;
} parserState;

/*
static void newScope(parserState *state) {
    scope *new = (scope*)malloc(sizeof(scope));
    new->parent = state->localScope;
    new->variables = hashmap_create();
    state->localScope = new;
}

static void freeScope(parserState *state) {
    if (state->localScope->parent == NULL) {
        printf("Compiler Error (Parser): Tried to free the global scope");
        if (ABORT_ON_ERROR) abort();
        exit(-1);
    }
    scope *old = state->localScope;
    state->localScope = old->parent;
    hashmap_free(old->variables);
    free(old);
}

static void splitPath(parserState *state, strlist *list, char *path) {
    char workingPath[strlen(path) + 1];
    strcpy(workingPath, path);
    size_t idx;
    while ((idx = strstr(workingPath, "/") - workingPath) >= 0) {
        char thisSegment[idx + 1];
        strncpy(thisSegment, workingPath, idx);
        thisSegment[idx] = '\0';
    }
}

static importResult resolveImportPath(parserState *state, token tok) {
    printf("Error: import statements not implemented yet.");
    return (importResult){0};

    char *oceanRoot = "/usr/local/ocean"; // TODO: add better code for finding ocean root directory

    strlist path;

    // This checks that you have an ocean installation
    // before trying to search for any stdlib packages.
    // This means that you could *technically* compile
    // an ocean project without an installation as long
    // as you don't try to import ANY packages.
    if (access(oceanRoot, F_OK) != 0) {
        printf("Error: Could not find ocean installation.");
        exit(1);
    }

    // import paths are EITHER a path to a package (eg. a subpackage of current module, or
    // any stdlib package) or a module name followed by a package path, or nothing to indicate
    // main package of the module

    // first, try import path as a relative path from ocean root
    // ensure that you can't refer to a path relative to ocean root
    // that is not in ocean root (eg. ../myPackage)

    // absolute paths and paths that go ../ are NOT allowed

    // package structure for ocean projects:
    // module --> main package --> package 1, package 2, etc. subpackages can also have their own subpackages

    // note: search for folders matching import path, but check ocean.mod file before parsing to ensure module
    // matches folder name.
    size_t relativePathLen = strlen(tok.lexeme);
    char fullRootPath[strlen(oceanRoot) + relativePathLen + 1];
    strcpy(fullRootPath, oceanRoot);
    strncat(fullRootPath, tok.lexeme, relativePathLen);

    if (access(fullRootPath, F_OK) == 0) {
        // folder matching import path is found in ocean root
        // next, parse all files
        int originalFd = open(".", O_RDONLY);
        if (chdir(fullRootPath) != 1) {
            printf("Compiler Error: could not change directory (0)");
            if (ABORT_ON_ERROR) abort();
            exit(-1);
        }
        DIR *d;
        d = opendir(".");
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            struct stat dirstat;
            stat(dir->d_name, &dirstat);
            unsigned int mode = dirstat.st_mode;
            if (S_ISREG(mode)) {
                FILE *f = fopen(dir->d_name, "rb");
                fseek(f, 0, SEEK_END);
                size_t fsize = ftell(f);
                char *contents = malloc(fsize + 1);
                fread(contents, 1, ftell(f), f);
                contents[fsize] = '\0';
                fclose(f);
                tokenlist *ftokens = scanTokens(contents, dir->d_name);
                parseResult res = parseTokens(ftokens);
                if (res.hasError) {
                    state->hasError = 1;
                }
                
                if (strcmp(res.packageName, tok.lexeme) == 0) {
                    // package name matches
                }
            }
        }
        if (originalFd == -1) {
            printf("Compiler Error: could not open() current directory.");
            if (ABORT_ON_ERROR) abort();
        }
        if (fchdir(originalFd) != 0) {
            printf("Compiler Error: could not change directory (1)");
            if (ABORT_ON_ERROR) abort();
        }
    }
    
}
*/

token advance(parserState *state) {
    const token tok = state->tokens[++state->tokenIndex];\
    if (state->tokenIndex >= state->tokensLen) {\
        printf("Fatal Compiler Error: Parser attempted to read past end of token array.\n");\
        if (ABORT_ON_ERROR) abort(); else exit(-1);\
    }\
    return tok;
}

#define SYNCHRONIZE while (tok.type != SEMICOLON && tok.type != EOF_TOKEN && tok.type != RIGHT_CURLY_BRACE)

#define CHECK_AND_SYNC_DOT if (state.isSynchronizing) SYNCHRONIZE tok = advance(&state);

#define CHECK_AND_SYNC_PTR if (state->isSynchronizing) SYNCHRONIZE tok = advance(state);

#define CHECK_MALLOC(ptr) do {\
        if (ptr == NULL) {\
            printf("Compiler Error: Out of memory - Unable to allocate heap memory.");\
            if (ABORT_ON_ERROR) abort();\
            exit(-1);\
        }\
    } while (0)

#define CHECK_SYNC if (state->isSynchronizing) return (expr){0}

#define SET_TOK tok = state->tokens[state->tokenIndex]

#define DEFINE_TOK token SET_TOK

#define CONSTRUCT_EXPR_RES(typeP, underlyingP, res)\
    expr res = {0};\
    res.type = typeP;\
    res.underlying = malloc(sizeof(underlyingP));\
    CHECK_MALLOC(res.underlying);\
    memcpy(res.underlying, &underlyingP, sizeof(underlyingP));

#define isAssignmentOperator(t) (t > TOKEN_SPECIAL_ASSIGN_START && t < TOKEN_SPECIAL_ASSIGN_END)

#define isInequalityOperator(t) (t == LESS_THAN || t == LESS_EQUALS || t == GREATER_THAN || t == GREATER_EQUALS)

#define peek(state) (state->tokens[state->tokenIndex + 1])

#define expect(tokType, message, retType) do {\
        if (tok.type != tokType) {\
            printf("[%s:%ld:%ld] " message "\n", tok.filename, tok.line, tok.column);\
            state->isSynchronizing = 1;\
            return (retType){0};\
        }\
    } while (0)

static expr comma(parserState *state);

static expr assignment(parserState *state);

static expr atomic(parserState *state) {
    DEFINE_TOK;
    switch (tok.type) {
        case IDENTIFIER:
            struct expr_identifier ident = {0};
            ident.lexeme = tok;
            expr ident_expr = {0};
            ident_expr.underlying = malloc(sizeof(struct expr_identifier));
            ident_expr.type = EXPR_TYPE_IDENTIFIER;
            *(struct expr_identifier*) ident_expr.underlying = ident;
            tok = advance(state);
            return ident_expr;
        case LITERAL_INT:
            struct expr_literal lit_int = {0};
            lit_int.type = LITERAL_TYPE_INT;
            lit_int.value = malloc(sizeof(long));
            *(long*) lit_int.value = tok.numValue.integer;
            expr lit_int_expr = {0};
            lit_int_expr.type = EXPR_TYPE_LITERAL;
            lit_int_expr.underlying = malloc(sizeof(struct expr_literal));
            *(struct expr_literal*) lit_int_expr.underlying = lit_int;
            tok = advance(state);
            return lit_int_expr;
        case LITERAL_FLOAT:
            struct expr_literal lit_float = {0};
            lit_float.type = LITERAL_TYPE_FLOAT;
            lit_float.value = malloc(sizeof(double));
            *(double*) lit_float.value = tok.numValue.floating;
            expr lit_float_expr = {0};
            lit_float_expr.type = EXPR_TYPE_LITERAL;
            lit_float_expr.underlying = malloc(sizeof(struct expr_literal));
            *(struct expr_literal*) lit_float_expr.underlying = lit_float;
            tok = advance(state);
            return lit_float_expr;
        case LITERAL_STRING:            
            struct expr_literal lit_str = {0};
            lit_str.type = LITERAL_TYPE_STRING;
            lit_str.value = malloc(sizeof(char*));
            lit_str.value = tok.lexeme;
            expr lit_str_expr = {0};
            lit_str_expr.type = EXPR_TYPE_LITERAL;
            lit_str_expr.underlying = malloc(sizeof(struct expr_literal));
            *(struct expr_literal*) lit_str_expr.underlying = lit_str;
            tok = advance(state);
            return lit_str_expr;
        default:
            printf("[%s:%ld:%ld] Unexpected '%s', expected expression\n", tok.filename, tok.line, tok.column, tok.lexeme);
            state->isSynchronizing = 1;
            return (expr){0};
    }
    printf("[%s:%ld:%ld] expected literal or identifier\n", tok.filename, tok.line, tok.column);
    return (expr){0};
}

// primary includes:
// member access (for structs and packages), array indexing,
// grouping (), function calls, and postfix ++ and --
static expr primary(parserState *state) {
    DEFINE_TOK;
    if (tok.type == LEFT_PAREN) {
        tok = advance(state);
        expr grouped = comma(state);
        CHECK_SYNC;
        SET_TOK;
        expect(RIGHT_PAREN, "expected right paren", expr);
        tok = advance(state);
        return grouped;
    }
    expr parent = atomic(state);
    CHECK_SYNC;
    struct expr_unary unary_temp = {0};
    for (size_t i = 0; i < PARSER_MAX_RECURSION_DEPTH; i++) {
        SET_TOK;
        switch (tok.type) {
            case DOT:
                // member access
                tok = advance(state);
                struct expr_dot dot = {0};
                dot.parent = parent;
                dot.field = atomic(state);
                parent.underlying = malloc(sizeof(struct expr_dot));
                CHECK_MALLOC(parent.underlying);
                *(struct expr_dot*) parent.underlying = dot;
                parent.type = EXPR_TYPE_DOT;
            case LEFT_BRACKET:
                // array indexing
                tok = advance(state);
                expr index = comma(state);
                CHECK_SYNC;
                SET_TOK;
                expect(RIGHT_BRACKET, "expected right bracket", expr);
                struct expr_index index_n = {0};
                index_n.parent = parent;
                index_n.index = index;
                parent.underlying = malloc(sizeof(struct expr_index));
                CHECK_MALLOC(parent.underlying);
                *(struct expr_index*) parent.underlying = index_n;
                parent.type = EXPR_TYPE_INDEX;
            case LEFT_PAREN:
                // function call
                tok = advance(state);
                struct expr_call call = {0};
                call.func = parent;
                // parse func params
                exprlist *list = (exprlist*)malloc(sizeof(exprlist));
                init_exprlist(list);
                while (tok.type != RIGHT_PAREN) {
                    expr param = assignment(state);
                    call.nParams++;
                    push_exprlist(list, param);
                    SET_TOK;
                    if (tok.type == COMMA) {
                        tok = advance(state);
                    } else if (tok.type != RIGHT_PAREN) {
                        printf("[%s:%ld:%ld] expected comma or right paren\n", tok.filename, tok.line, tok.column);
                        state->isSynchronizing = 1;
                        return (expr){0};
                    }
                }
                tok = advance(state);
                call.params = (expr*)calloc(call.nParams, sizeof(expr));
                expr *arr = to_exprarray(list);
                memcpy(call.params, arr, sizeof(expr) * call.nParams);
                free(arr);
                parent.underlying = malloc(sizeof(struct expr_call));
                CHECK_MALLOC(parent.underlying);
                *(struct expr_call*) parent.underlying = call;
                parent.type = EXPR_TYPE_CALL;
                break;
            case PLUS_PLUS:
                // postfix increment
                tok = advance(state);
                unary_temp.child = parent;
                unary_temp.operation = UNARY_POSTFIX_INCREMENT;
                parent.underlying = malloc(sizeof(struct expr_unary));
                CHECK_MALLOC(parent.underlying);
                *(struct expr_unary*) parent.underlying = unary_temp;
                parent.type = EXPR_TYPE_UNARY;
                break;
            case MINUS_MINUS:
                // postfix decrement
                tok = advance(state);
                unary_temp.child = parent;
                unary_temp.operation = UNARY_POSTFIX_DECREMENT;
                parent.underlying = malloc(sizeof(struct expr_unary));
                CHECK_MALLOC(parent.underlying);
                *(struct expr_unary*) parent.underlying = unary_temp;
                parent.type = EXPR_TYPE_UNARY;
                break;
            default:
                goto endLoop;
        }

    }
    printf("Fatal compiler error: Parser maximum recursion depth reached.\n");
    if (ABORT_ON_ERROR) abort();
    exit(5);
    endLoop:
    return parent;
}

// secondary includes:
// sizeof(), address of (&), dereference (*),
// logical and bitwise not, unary negation, and
// prefix ++ and --
static expr secondary(parserState *state) {
    DEFINE_TOK;
    switch (tok.type) {
        case IDENTIFIER:
            if (strcmp(tok.lexeme, "sizeof") == 0 && peek(state).type == LEFT_PAREN) {
                tok = advance(state);
                tok = advance(state);
                expr inner = comma(state);
                CHECK_SYNC;
                SET_TOK;
                expect(RIGHT_PAREN, "expected right paren to close call to sizeof operator", expr);
                tok = advance(state);
                struct expr_unary unary_n = {0};
                unary_n.child = inner;
                unary_n.operation = UNARY_SIZEOF;
                CONSTRUCT_EXPR_RES(EXPR_TYPE_UNARY, unary_n, res);
                return res;
            }
            return primary(state);
            break;
        case AMPERSAND:
            tok = advance(state);
            expr var = secondary(state);
            CHECK_SYNC;
            struct expr_unary unary_n = {0};
            unary_n.child = var;
            unary_n.operation = UNARY_ADDRESS_OF;
            CONSTRUCT_EXPR_RES(EXPR_TYPE_UNARY, unary_n, res);
            return res;
        case STAR:
            tok = advance(state);
            expr ptr = secondary(state);
            CHECK_SYNC;
            struct expr_unary unary_n2 = {0};
            unary_n2.child = ptr;
            unary_n2.operation = UNARY_DEREFERENCE;
            CONSTRUCT_EXPR_RES(EXPR_TYPE_UNARY, unary_n2, res2);
            return res2;
        case EXCLAMATION:
            tok = advance(state);
            expr child = secondary(state);
            CHECK_SYNC;
            struct expr_unary unary_n3 = {0};
            unary_n3.child = child;
            unary_n3.operation = UNARY_LOGICAL_NOT;
            CONSTRUCT_EXPR_RES(EXPR_TYPE_UNARY, unary_n3, res3);
            return res3;
        case TILDE:
            tok = advance(state);
            expr child1 = secondary(state);
            CHECK_SYNC;
            struct expr_unary unary_n4 = {0};
            unary_n4.child = child1;
            unary_n4.operation = UNARY_BITWISE_NOT;
            CONSTRUCT_EXPR_RES(EXPR_TYPE_UNARY, unary_n4, res4);
            return res4;
        case MINUS:
            tok = advance(state);
            expr child2 = secondary(state);
            CHECK_SYNC;
            struct expr_unary unary_n5 = {0};
            unary_n5.child = child2;
            unary_n5.operation = UNARY_NEGATE;
            CONSTRUCT_EXPR_RES(EXPR_TYPE_UNARY, unary_n5, res5);
            return res5;
        case PLUS_PLUS:
            tok = advance(state);
            expr child3 = secondary(state);
            CHECK_SYNC;
            struct expr_unary unary_n6 = {0};
            unary_n6.child = child3;
            unary_n6.operation = UNARY_PREFIX_INCREMENT;
            CONSTRUCT_EXPR_RES(EXPR_TYPE_UNARY, unary_n6, res6);
            return res6;
        case MINUS_MINUS:
            tok = advance(state);
            expr child4 = secondary(state);
            CHECK_SYNC;
            struct expr_unary unary_n7 = {0};
            unary_n7.child = child4;
            unary_n7.operation = UNARY_PREFIX_DECREMENT;
            CONSTRUCT_EXPR_RES(EXPR_TYPE_UNARY, unary_n7, res7);
            return res7;
        default:
            return primary(state);
    }
    return (expr){0};
}

static expr multiplyDivide(parserState *state) {
    const expr left = secondary(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (tok.type == STAR || tok.type == SLASH || tok.type == MODULO) {
        struct expr_binary binary_n = {0};
        binary_n.left = left;
        switch (tok.type) {
            case STAR:
                binary_n.operation = EXPR_MULTIPLY;
                break;
            case SLASH:
                binary_n.operation = EXPR_DIVIDE;
                break;
            case MODULO:
                binary_n.operation = EXPR_MODULO;
                break;
            default:
                printf("Compiler Error: Unreachable default statement reached (0)\n");
                if (ABORT_ON_ERROR) abort();
        }
        tok = advance(state);
        binary_n.right = secondary(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, binary_n, res)
        return res;
    }
    return left;
}

static expr addSubtract(parserState *state) {
    const expr left = multiplyDivide(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (tok.type == PLUS || tok.type == MINUS) {
        struct expr_binary binary_n = {0};
        binary_n.left = left;
        if (tok.type == PLUS) {
            binary_n.operation = EXPR_ADD;
        } else {
            binary_n.operation = EXPR_SUBTRACT;
        }
        tok = advance(state);
        binary_n.right = multiplyDivide(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, binary_n, res)
        return res;
    }
    return left;
}

static expr bitshift(parserState *state) {
    const expr left = addSubtract(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (tok.type == BITSHIFT_LEFT || tok.type == BITSHIFT_RIGHT) {
        tok = advance(state);
        struct expr_binary binary_n = {0};
        binary_n.left = left;
        if (tok.type == BITSHIFT_LEFT) {
            binary_n.operation = EXPR_BITSHIFT_LEFT;
        } else {
            binary_n.operation = EXPR_BITSHIFT_RIGHT;
        }
        binary_n.right = addSubtract(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, binary_n, res)
        return res;
    }
    return left;
}

static expr inequality(parserState *state) {
    const expr left = bitshift(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (isInequalityOperator(tok.type)) {
        enum binaryExprType bType;
        switch (tok.type) {
            case LESS_THAN:
                bType = EXPR_LESS_THAN;
                break;
            case LESS_EQUALS:
                bType = EXPR_LESS_EQUALS;
                break;
            case GREATER_THAN:
                bType = EXPR_GREATER_THAN;
                break;
            case GREATER_EQUALS:
                bType = EXPR_GREATER_EQUALS;
                break;
            default:
                printf("Compiler Error: unknown inequality type\n");
                if (ABORT_ON_ERROR) abort();
                exit(-2);
        }

        tok = advance(state);
        struct expr_binary assign_n = {0};

        assign_n.operation = bType;
        assign_n.left = left;
        assign_n.right = bitshift(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, assign_n, res)
        return res;
    }
    return left;
}

static expr equality(parserState *state) {
    const expr left = inequality(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (tok.type == EQUALS_EQUALS || tok.type == NOT_EQUALS) {
        const char not = tok.type == NOT_EQUALS;
        tok = advance(state);
        struct expr_binary binary_n = {0};
        binary_n.left = left;
        if (not) {
            binary_n.operation = EXPR_NOT_EQUALS;
        } else {
            binary_n.operation = EXPR_EQUALITY;
        }
        binary_n.right = inequality(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, binary_n, res)
        return res;
    }
    return left;
}

static expr bitwiseAnd(parserState *state) {
    const expr left = equality(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (tok.type == AMPERSAND) {
        tok = advance(state);
        struct expr_binary binary_n = {0};
        binary_n.left = left;
        binary_n.operation = EXPR_BITWISE_AND;
        binary_n.right = bitwiseAnd(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, binary_n, res)
        return res;
    }
    return left;
}

static expr bitwiseXor(parserState *state) {
    const expr left = bitwiseAnd(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (tok.type == XOR) {
        tok = advance(state);
        struct expr_binary binary_n = {0};
        binary_n.left = left;
        binary_n.operation = EXPR_XOR;
        binary_n.right = bitwiseXor(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, binary_n, res)
        return res;
    }
    return left;
}

static expr bitwiseOr(parserState *state) {
    const expr left = bitwiseXor(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (tok.type == BITWISE_OR) {
        tok = advance(state);
        struct expr_binary binary_n = {0};
        binary_n.left = left;
        binary_n.operation = EXPR_BITWISE_OR;
        binary_n.right = bitwiseXor(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, binary_n, res)
        return res;
    }
    return left;
}

static expr logicalAnd(parserState *state) {
    const expr left = bitwiseOr(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (tok.type == LOGICAL_AND) {
        tok = advance(state);
        struct expr_binary binary_n = {0};
        binary_n.left = left;
        binary_n.operation = EXPR_LOGICAL_AND;
        binary_n.right = logicalAnd(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, binary_n, res)
        return res;
    }
    return left;
}

static expr logicalOr(parserState *state) {
    const expr left = logicalAnd(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (tok.type == LOGICAL_OR) {
        tok = advance(state);
        struct expr_binary binary_n = {0};
        binary_n.left = left;
        binary_n.operation = EXPR_LOGICAL_OR;
        binary_n.right = logicalAnd(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, binary_n, res)
        return res;
    }
    return left;
}

static expr ternary(parserState *state) {
    const expr condition = logicalOr(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (tok.type == QUESTION_MARK) {
        tok = advance(state);
        struct expr_ternary ternary_n = {0};
        ternary_n.condition = condition;
        ternary_n.ifTrue = ternary(state);
        CHECK_SYNC;
        SET_TOK;
        expect(COLON, "Expected colon for else case in ternary expression", expr);
        tok = advance(state);
        ternary_n.ifFalse = ternary(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_TERNARY, ternary_n, res)
        return res;
    }
    return condition;
}

static expr assignment(parserState *state) {
    const expr variable = ternary(state);
    CHECK_SYNC;
    DEFINE_TOK;
    if (isAssignmentOperator(tok.type)) {
        enum binaryExprType bType;
        switch (tok.type) {
            case EQUALS:
                bType = EXPR_ASSIGNMENT;
                break;
            case PLUS_EQUALS:
                bType = EXPR_ADD_EQUALS;
                break;
            case MINUS_EQUALS:
                bType = EXPR_SUBTRACT_EQUALS;
                break;
            case STAR_EQUALS:
                bType = EXPR_MULTIPLY_EQUALS;
                break;
            case SLASH_EQUALS:
                bType = EXPR_DIVIDE_EQUALS;
                break;
            case MODULO_EQUALS:
                bType = EXPR_MODULO_EQUALS;
                break;
            case XOR_EQUALS:
                bType = EXPR_XOR_EQUALS;
                break;
            case BITSHIFT_LEFT_EQUALS:
                bType = EXPR_BITSHIFT_LEFT_EQUALS;
                break;
            case BITSHIFT_RIGHT_EQUALS:
                bType = EXPR_BITSHIFT_RIGHT_EQUALS;
                break;
            default:
                printf("Compiler Error: unknown assignment type\n");
                if (ABORT_ON_ERROR) abort();
                exit(-2);
        }

        tok = advance(state);
        struct expr_binary assign_n = {0};

        assign_n.operation = bType;
        assign_n.left = variable;
        assign_n.right = assignment(state);
        CHECK_SYNC;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, assign_n, res)
        return res;
    }
    return variable;
}

static expr comma(parserState *state) {
    expr left = assignment(state);
    CHECK_SYNC;
    DEFINE_TOK;
    while (tok.type == COMMA) {
        tok = advance(state);
        struct expr_binary binary_n = {0};
        binary_n.left = left;
        binary_n.operation = EXPR_COMMA;
        binary_n.right = comma(state);
        SET_TOK;
        CONSTRUCT_EXPR_RES(EXPR_TYPE_BINARY, binary_n, res)
        left = res;
    }
    return left;
}

static expr *parseExpr(parserState *state) {
    expr expression = comma(state);
    if (state->isSynchronizing) {
        //printf("parseExpr synchronizing\n");
        return nullptr;
    }
    expr *heapExpr = malloc(sizeof(expression));
    CHECK_MALLOC(heapExpr);
    memcpy(heapExpr, &expression, sizeof(expression));
    return heapExpr;
}

static stmtlist *parseBlock(parserState *state);

static stmt parseStatement(parserState *state) {
    DEFINE_TOK;
    switch (tok.type) {
        case IDENTIFIER:
                token varToken = tok;
                tok = advance(state);
                if (tok.type == COLON) {
                    // parse var declaration
                    tok = advance(state);
                    if (tok.type == EQUALS) {
                        // inferred type, no type tokens
                        tok = advance(state);
                        expr *value = parseExpr(state);
                        CHECK_AND_SYNC_PTR;
                        struct stmt_var decl = {.var = varToken, .type = {.tokens = nullptr, .nTokens = 0}, .initValue = value};
                        stmt decl_stmt = {.type = STMT_VAR, .value = malloc(sizeof(struct stmt_var))};
                        *(struct stmt_var*)decl_stmt.value = decl;
                        return decl_stmt;
                    }
                    if (tok.type == IDENTIFIER) {
                        // explicit type, need type tokens
                        tokenlist *typeTokList = &(tokenlist){0};
                        init_tokenlist(typeTokList);
                        while (tok.type != EQUALS && tok.type != SEMICOLON) {
                            push_tokenlist(typeTokList, tok);
                            tok = advance(state);
                        }
                        if (tok.type == SEMICOLON) {
                            // no init value, just declaration
                            struct stmt_var decl = {.var = varToken, .type = {.tokens = to_tokenarray(typeTokList), .nTokens = listlen(typeTokList)}, .initValue = nullptr};
                            stmt decl_stmt = {.type = STMT_VAR, .value = malloc(sizeof(struct stmt_var))};
                            *(struct stmt_var*)decl_stmt.value = decl;
                            return decl_stmt;
                        }
                        // has init value and explicit type
                        tok = advance(state);
                        expr *value = parseExpr(state);
                        CHECK_AND_SYNC_PTR;
                        struct stmt_var decl = {.var = varToken, .type = {.tokens = to_tokenarray(typeTokList), .nTokens = listlen(typeTokList)}, .initValue = value};
                        stmt decl_stmt = {.type = STMT_VAR, .value = malloc(sizeof(struct stmt_var))};
                        *(struct stmt_var*)decl_stmt.value = decl;
                        return decl_stmt;
                    }
                } else {
                    // identifier not followed by a colon is an expression statement
                    expr *expression = parseExpr(state);
                    CHECK_AND_SYNC_PTR;
                    stmt expr_statement = {.type = STMT_EXPR, .value = expression};
                    return expr_statement;
                }
                break;
        case LEFT_CURLY_BRACE:
            stmtlist *statements = parseBlock(state);
            CHECK_AND_SYNC_PTR;
            if (state->isSynchronizing) {
                return (stmt){0};
            }
            struct stmt_block *block = malloc(sizeof(struct stmt_block));
            block->stmts = to_stmtarray(statements);
            block->nStmts = listlen(statements);
            return (stmt){.type = STMT_BLOCK, .value = &block};
        case IF:
            tok = advance(state);
            expect(LEFT_PAREN, "Expected condition for if statement", stmt);
            tok = advance(state);
            /*stmt preStmtValue = parseStatement(state);
            if (state->isSynchronizing) {
                SYNCHRONIZE tok = advance(state);
                return (stmt){0};
            }
            */
            stmt *preStmt = nullptr;
            /*
            SET_TOK;
            if (preStmtValue.type == STMT_EXPR) {
                // preStmtValue is the condition
            }
                // condition is actually a preStmt
                preStmt = malloc(sizeof(stmt));
                preStmt->type = STMT_EXPR;
                preStmt->value = condition;

                condition = parseExpr(state);
                if (state->isSynchronizing) {
                    SYNCHRONIZE tok = advance(state);
                    return (stmt){0};
                }
            }
            */
            expr *condition = parseExpr(state);
            // TODO: fix preStmt parsing for if statements
            tok = advance(state);
            stmt ifTrue = parseStatement(state);
            if (state->isSynchronizing) {
                SYNCHRONIZE tok = advance(state);
                return (stmt){0};
            }
            stmt *ifFalse = nullptr;
            if (tok.type == ELSE) {
                tok = advance(state);
                ifFalse = malloc(sizeof(stmt));
                *ifFalse = parseStatement(state);
                if (state->isSynchronizing) {
                    free(ifFalse);
                    SYNCHRONIZE tok = advance(state);
                    return (stmt){0};
                }
            }
            stmt ifStatement = {.type = STMT_IF, .value = malloc(sizeof(struct stmt_if))};
            ((struct stmt_if*)ifStatement.value)->preStmt = preStmt;
            ((struct stmt_if*)ifStatement.value)->condition = condition;
            ((struct stmt_if*)ifStatement.value)->ifTrue = malloc(sizeof(stmt));
            *((struct stmt_if*)ifStatement.value)->ifTrue = ifTrue;
            ((struct stmt_if*)ifStatement.value)->ifFalse = ifFalse;
            return ifStatement;
        case WHILE:
            tok = advance(state);
            expect(LEFT_PAREN, "Expected condition for while loop", stmt);
            expr *whileCondition = parseExpr(state);
            if (state->isSynchronizing) {
                SYNCHRONIZE tok = advance(state);
                return (stmt){0};
            }
            expect(RIGHT_PAREN, "Expected right paren to end while loop condition", stmt);
            if (tok.type == SEMICOLON) {
                // empty while loop body, still syntactically valid as it is possible condition has side effects
                stmt whileStmt = {.type = STMT_WHILE, .value = malloc(sizeof(struct stmt_while))};
                ((struct stmt_while*)whileStmt.value)->condition = whileCondition;
                ((struct stmt_while*)whileStmt.value)->body = nullptr;
                ((struct stmt_while*)whileStmt.value)->isDoWhile = 0;
                return whileStmt;
            }
            stmt body = parseStatement(state);
            if (state->isSynchronizing) {
                SYNCHRONIZE tok = advance(state);
                return (stmt){0};
            }
            stmt whileStmt = {.type = STMT_WHILE, .value = malloc(sizeof(struct stmt_while))};
            ((struct stmt_while*)whileStmt.value)->condition = whileCondition;
            ((struct stmt_while*)whileStmt.value)->body = malloc(sizeof(stmt));
            *((struct stmt_while*)whileStmt.value)->body = body;
            ((struct stmt_while*)whileStmt.value)->isDoWhile = 0;
            return whileStmt;
        case DO:
            // do while
            tok = advance(state);
            stmt doBody = parseStatement(state);
            if (state->isSynchronizing) {
                SYNCHRONIZE tok = advance(state);
                return (stmt){0};
            }
            SET_TOK;
            expect (WHILE, "Expected while", stmt);
            tok = advance(state);
            expect(LEFT_PAREN, "Expected condition for do-while loop", stmt);
            expr *doCondition = parseExpr(state);
            if (state->isSynchronizing) {
                SYNCHRONIZE tok = advance(state);
                return (stmt){0};
            }
            expect(RIGHT_PAREN, "Expected right paren to end while loop condition\n", stmt);
            tok = advance(state);
            expect(SEMICOLON, "Expected semicolon", stmt);
            stmt doStmt = {.type = STMT_WHILE, .value = malloc(sizeof(struct stmt_while))};
            struct stmt_while *doWhileVal = (struct stmt_while*)doStmt.value;
            doWhileVal->condition = doCondition;
            doWhileVal->body = malloc(sizeof(stmt));
            *doWhileVal->body = doBody;
            doWhileVal->isDoWhile = 1;
            return doStmt;
        case FOR:
            // for loop
            tok = advance(state);
            expect(LEFT_PAREN, "Expected condition for for loop", stmt);
            tok = advance(state);
            stmt forInitValue = parseStatement(state);
            if (state->isSynchronizing) {
                SYNCHRONIZE tok = advance(state);
                return (stmt){0};
            }
            if (forInitValue.type != STMT_VAR && forInitValue.type != STMT_EXPR && forInitValue.type != STMT_NULL) {
                // notice that we deliberately do not update the local var tok to match the state, as we want this
                // error to have the line/column of the token at the beginning of the invalid initializer, not the end
                printf("[%s:%ld:%ld]: Expected expression\n", tok.filename, tok.line, tok.column);
                state->isSynchronizing = 1;
                return (stmt){0};
            }
            // the previous call to parseStatement() already consumed the semicolon after the init statement, so we
            // don't need to expect() it.
            tok = advance(state);
            /*
            SET_TOK;
            expr *
            expect(RIGHT_PAREN, "Expected semicolon", stmt);
            tok = advance(state);
            */
        case SEMICOLON:
            // empty statement
            tok = advance(state);
            return (stmt){0};
        default:
            // expression statement
            expr *expression = parseExpr(state);
            if (state->isSynchronizing) {
                tok = state->tokens[state->tokenIndex];
                SYNCHRONIZE tok = advance(state);
                state->isSynchronizing = 0;
                return (stmt){0};
            }
            stmt expressionStmt = {.type = STMT_EXPR, .value = expression};
            return expressionStmt;
    }
    return (stmt){0};
}

static stmtlist *parseBlock(parserState *state) {
    DEFINE_TOK;
    expect(LEFT_CURLY_BRACE, "Expected block start", stmtlist*);
    tok = advance(state);
    stmtlist *statements = malloc(sizeof(stmtlist));
    init_stmtlist(statements);
    while (tok.type != RIGHT_CURLY_BRACE && tok.type != EOF_TOKEN) {
        stmt statement = parseStatement(state);
        if (state->isSynchronizing) {
            SYNCHRONIZE tok = advance(state);
            state->isSynchronizing = 0;
        } else push_stmtlist(statements, statement);
    }
    if (tok.type == EOF_TOKEN) {
        printf("[%s:%ld:%ld]: Expected end of block\n", tok.filename, tok.line, tok.column);
        state->isSynchronizing = 1;
        return nullptr;
    }
    return statements;
}

parseResult parseTokens(tokenlist *tokensList) {
    parserState state = {0};
    
    state.globalScope.parent = nullptr;
    state.globalScope.variables = hashmap_create();
    state.localScope = &state.globalScope;

    state.tokens = to_tokenarray(tokensList);
    state.tokenIndex = 0;
    state.tokensLen = tokensList->top;

    char *cwdPath = getcwd(nullptr, 0);
    if (cwdPath == nullptr) {
        printf("Compiler Error: could not get current working directory.\n");
        if (ABORT_ON_ERROR) {
            abort();
        }
        exit(-1);
    }
    FILE *packageRootFile = fopen(cwdPath, "r");
    free(cwdPath);
    if (packageRootFile == nullptr) {
        printf("Compiler Error: could not open current working directory.\n");
        if (ABORT_ON_ERROR) {
            abort();
        }
        exit(-1);
    }
    state.packageRootfd = packageRootFile->_file;

    state.hasError = 0;

    stmtlist *statements = &(stmtlist){0};
    init_stmtlist(statements);
    /*
    expr *parsed = parseExpr(&state);
    stmt expression = {.type = STMT_EXPR, .value = parsed};
    size_t i = push_stmtlist(statements, expression);
    parseResult res = {.hasError = state.isSynchronizing, .nStatements = 1, .statements = to_stmtarray(statements), .packageName = "testPkg"};
    return res;
*/

    for (token tok = state.tokens[state.tokenIndex]; state.tokenIndex < state.tokensLen; tok = advance(&state)) {
        switch (tok.type) {
            case PACKAGE:
                token keywordToken = tok;
                tok = advance(&state);
                if (tok.type != LITERAL_STRING) {
                    printf("[%s:%ld:%ld]: Package name expected after package statement\n", tok.filename, tok.line, tok.column);
                    SYNCHRONIZE tok = advance(&state);
                    break;
                }
                // TODO: check for 'as' after import name
                state.packageName = malloc(strlen(tok.lexeme) + 1);
                strcpy(state.packageName, tok.lexeme);
                break;
            case IMPORT:
                /*
                token importToken = tok;
                tok = advance(&state);
                if (tok.type != LITERAL_STRING) {
                    printf("[%s:%ld:%ld]: Import path expected after import statement\n", tok.filename, tok.line, tok.column);
                    SYNCHRONIZE tok = advance(&state);
                    break;
                }
                resolveImportPath(&state, tok);
                */
                printf("[%s:%ld:%ld]: Import not implemented yet, ignoring.\n", tok.filename, tok.line, tok.column);
                break;
            case EOF_TOKEN:
                goto loopEnd;
                break;
            default:
                stmt statement = parseStatement(&state);
                if (statement.type != STMT_NULL) {
                    push_stmtlist(statements, statement);
                } else if (state.isSynchronizing) {
                    state.hasError = 1;
                    tok = state.tokens[state.tokenIndex];
                    SYNCHRONIZE tok = advance(&state);
                    state.isSynchronizing = 0;
                }
                tok = state.tokens[state.tokenIndex];
                if (tok.type == EOF_TOKEN) {
                    goto loopEnd;
                }
        }
    }
    loopEnd:
    fclose(packageRootFile);
    free_tokenlist(tokensList);
    // for (size_t i = 0; i < state.tokensLen; i++) {
    //     free(state.tokens[i].lexeme);
    // }
    free(state.tokens);
    parseResult res = {0};
    res.hasError = state.hasError;
    res.packageName = state.packageName;
    res.statements = to_stmtarray(statements);
    res.nStatements = listlen(statements);

    return res;
}
