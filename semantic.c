#include "semantic.h"
#include "array_type.h"
#include "buffer_type.h"
#include "value_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Type Type;
struct Type {
    const char *name;
    Type *element;
    Type *receiver;
    struct stmt_func *function;
    struct stmt_struct *record;
    Type **arguments;
    size_t nArguments;
    size_t count;
    int bufferSize;
    typeTokens dimension;
    typeTokens constraint; // Inherited from a receiver struct parameter, when present.
};
typedef struct Symbol {
    token name;
    Type *type;
    int isType;
    int immutable;
    struct Symbol *next;
} Symbol;
typedef struct Scope {
    struct Scope *parent;
    Symbol *symbols;
} Scope;
typedef struct Allocation {
    void *ptr;
    struct Allocation *next;
} Allocation;
typedef struct Context {
    size_t errors;
    parseResult *program;
    Scope *scope;
    struct stmt_func *function;
    Type *implicitReceiver;
    Allocation *allocations;
} Context;
static Type unknown = {.name = "?"}, integer = {.name = "int"}, floating = {.name = "float"},
            signedByte = {.name = "i8"},
            string = {.name = "dynamic-array", .element = &signedByte},
            boolean = {.name = "bool"}, any = {.name = "any"}, nothing = {.name = "void"};
static void *allocate(Context *c, size_t n) {
    void *p = calloc(1, n);
    Allocation *a = malloc(sizeof(*a));
    if (!p || !a) {
        fprintf(stderr, "Out of memory in semantic analyzer\n");
        exit(1);
    }
    *a = (Allocation){p, c->allocations};
    c->allocations = a;
    return p;
}
static void error(Context *c, token t, const char *message) {
    fprintf(stderr, "[%s:%ld:%ld]: semantic error: %s%s%s\n", t.filename ? t.filename : "<source>",
            t.line, t.column, message, t.lexeme ? ": " : "", t.lexeme ? t.lexeme : "");
    c->errors++;
}
static Symbol *lookup(Context *c, const char *name) {
    if (!name)
        return NULL;
    for (Scope *s = c->scope; s; s = s->parent)
        for (Symbol *v = s->symbols; v; v = v->next)
            if (!strcmp(v->name.lexeme, name))
                return v;
    return NULL;
}
static Symbol *declare(Context *c, token name, Type *type, int isType) {
    if (!name.lexeme || name.type == EOF_TOKEN || !strcmp(name.lexeme, "_"))
        return NULL;
    for (Symbol *v = c->scope->symbols; v; v = v->next)
        if (!strcmp(v->name.lexeme, name.lexeme)) {
            error(c, name, "duplicate declaration");
            return NULL;
        }
    Symbol *v = allocate(c, sizeof(*v));
    *v = (Symbol){.name = name, .type = type, .isType = isType, .next = c->scope->symbols};
    c->scope->symbols = v;
    return v;
}
static int compatible(Type *a, Type *b) {
    if (a == &unknown || b == &unknown || a == &any || b == &any)
        return 1;
    if (a == b)
        return 1;
    if (!strcmp(a->name, "tuple") || !strcmp(b->name, "tuple")) {
        if (strcmp(a->name, b->name) || a->nArguments != b->nArguments)
            return 0;
        for (size_t i = 0; i < a->nArguments; i++)
            if (!compatible(a->arguments[i], b->arguments[i]))
                return 0;
        return 1;
    }
    if (a->element || b->element)
        return a->element && b->element && !strcmp(a->name, b->name) &&
               ((a->element == &signedByte || b->element == &signedByte)
                    ? a->element == b->element : compatible(a->element, b->element)) &&
               (!strcmp(a->name, "buffer")
                    ? (a->bufferSize != 1 || (b->bufferSize == 1 && a->count == b->count))
                    : a->count == b->count);
    if (a->function || b->function)
        return a->function == b->function;
    if (a->record || b->record) {
        if (a->record != b->record)
            return 0;
        if (a->nArguments && b->nArguments) {
            if (a->nArguments != b->nArguments)
                return 0;
            for (size_t i = 0; i < a->nArguments; i++)
                if (!compatible(a->arguments[i], b->arguments[i]))
                    return 0;
        }
        return 1;
    }
    return !strcmp(a->name, b->name) ||
           ((a == &integer || a == &signedByte) && (b == &integer || b == &signedByte));
}
static Type *builtinType(const char *n) {
    if (!strcmp(n, "any"))
        return &any;
    if (!strcmp(n, "bool"))
        return &boolean;
    if (!strcmp(n, "i8"))
        return &signedByte;
    if (!strcmp(n, "string"))
        return &string;
    if (!strcmp(n, "float") || !strcmp(n, "f32") || !strcmp(n, "f64"))
        return &floating;
    const char *ints[] = {"int", "uint", "byte", "rune", "i8",  "i16",
                          "i32", "i64",  "u8",   "u16",  "u32", "u64"};
    for (size_t i = 0; i < sizeof(ints) / sizeof(*ints); i++)
        if (!strcmp(n, ints[i]))
            return &integer;
    return NULL;
}
static Type *typeOf(Context *c, typeTokens span) {
    if (!span.nTokens)
        return &unknown;
    token t = span.tokens[0];
    if (span.nTokens > 1 && span.tokens[span.nTokens - 1].type == STAR) {
        Type *pointer = allocate(c, sizeof(*pointer));
        pointer->name = "pointer";
        pointer->element = typeOf(c, (typeTokens){span.nTokens - 1, span.tokens});
        return pointer;
    }
    typeTokens arrayElement;
    if (arrayElementType(span, &arrayElement)) {
        Type *v = allocate(c, sizeof(*v));
        v->name = "dynamic-array";
        v->element = typeOf(c, arrayElement);
        if (!strcmp(v->element->name, "buffer") && v->element->bufferSize == 2)
            error(c, t, "array elements must have a fixed storage size");
        return v;
    }
    if (isBufferType(span)) {
        typeTokens element;
        typeTokens dimension;
    typeTokens constraint; // Inherited from a receiver struct parameter, when present.
        if (!bufferTypeParts(span, &element, &dimension) || bufferDimensionKind(dimension) < 0) {
            error(c, t, "expected buffer<T>[size] with a positive size, or buffer<T>[]");
            return &unknown;
        }
        Type *v = allocate(c, sizeof(*v));
        v->name = "buffer";
        v->element = typeOf(c, element);
        v->bufferSize = bufferDimensionKind(dimension);
        v->dimension = dimension;
        if (v->bufferSize == 1)
            v->count = (size_t)dimension.tokens[0].numValue.integer;
        if (!strcmp(v->element->name, "buffer") && v->element->bufferSize == 2)
            error(c, t, "buffer element types must have a fixed storage size");
        return v;
    }
    if (t.type == LEFT_BRACKET || t.type == STAR) {
        size_t i = 1;
        if (t.type == LEFT_BRACKET) {
            while (i < span.nTokens && span.tokens[i].type != RIGHT_BRACKET)
                i++;
            i++;
        }
        Type *v = allocate(c, sizeof(*v));
        v->name = t.type == STAR ? "pointer" : "array";
        if (i < span.nTokens)
            v->element = typeOf(c, (typeTokens){span.nTokens - i, span.tokens + i});
        else
            v->element = &unknown;
        return v;
    }
    const char *n = t.lexeme ? t.lexeme : "any";
    Symbol *s = lookup(c, n);
    if (s) {
        if (!s->isType) {
            error(c, t, "expected a type");
            return &unknown;
        }
        if (s->type->record && span.nTokens > 1 &&
            (span.tokens[1].type == LEFT_BRACKET || span.tokens[1].type == LESS_THAN)) {
            typeTokens *arguments = NULL;
            size_t count = 0;
            if (!parseTypeApplicationArguments(span, &arguments, &count)) {
                error(c, t, "invalid struct type arguments");
                return &unknown;
            }
            Type *instance = allocate(c, sizeof(*instance));
            *instance = *s->type;
            instance->nArguments = count;
            instance->arguments = allocate(c, sizeof(Type *) * count);
            if (count != instance->record->nTypeParams)
                error(c, t, "type argument count mismatch");
            for (size_t i = 0; i < count; i++) {
                // Buffer dimension spans can retain their tokens for later checks.
                typeTokens retained = {arguments[i].nTokens,
                                       allocate(c, sizeof(token) * arguments[i].nTokens)};
                memcpy(retained.tokens, arguments[i].tokens, sizeof(token) * retained.nTokens);
                instance->arguments[i] = typeOf(c, retained);
                free(arguments[i].tokens);
            }
            free(arguments);
            return instance;
        }
        return s->type;
    }
    Type *builtin = builtinType(n);
    if (builtin) return builtin;
    error(c, t, "unresolved type");
    return &unknown;
}
static Type *expression(Context *, expr *);
static void statement(Context *, stmt *);
static void predeclare(Context *, stmt *);
static void block(Context *c, struct stmt_block *b, int nested) {
    Scope scope = {c->scope, NULL};
    if (nested)
        c->scope = &scope;
    for (size_t i = 0; i < b->nStmts; i++)
        predeclare(c, &b->stmts[i]);
    for (size_t i = 0; i < b->nStmts; i++)
        statement(c, &b->stmts[i]);
    if (nested)
        c->scope = scope.parent;
}
static void check(Context *c, Type *expected, Type *actual, token at) {
    if (!compatible(expected, actual))
        error(c, at, "type mismatch");
}
static Type *parameter(Context *, Type *, typeTokens);
static Type *expectedExpression(Context *, Type *, expr *, token);
static Type *checkComposite(Context *c, Type *target, expr *e, token at) {
    struct expr_composite *v = e->underlying;
    if (v->location.filename)
        at = v->location;
    if (target == &unknown) {
        if (!v->nEntries) {
            error(c, at, "cannot infer an empty literal; specify an array, buffer, or struct type");
            return &unknown;
        }
        Type *array = allocate(c, sizeof(*array));
        array->name = "dynamic-array";
        array->element = expectedExpression(c, &unknown, &v->values[0], at);
        v->inferredArray = 1;
        target = array;
    }
    if (!strcmp(target->name, "buffer") || !strcmp(target->name, "dynamic-array")) {
        if (!strcmp(target->name, "buffer") && target->bufferSize == 1 &&
            v->nEntries > target->count)
            error(c, at, "too many elements in buffer initializer");
        for (size_t i = 0; i < v->nEntries; i++) {
            if (v->keys[i].type)
                error(c, at, "array and buffer literals require positional elements");
            check(c, target->element, expectedExpression(c, target->element, &v->values[i], at),
                  at);
        }
        return target;
    }
    if (target->record) {
        struct stmt_struct *record = target->record;
        char *initialized = allocate(c, record->nFields + 1);
        for (size_t i = 0; i < v->nEntries; i++) {
            size_t j = i;
            if (v->keys[i].type) {
                if (v->keys[i].type != EXPR_TYPE_IDENTIFIER) {
                    error(c, at, "struct literal keys must be field names");
                    continue;
                }
                token key = ((struct expr_identifier *)v->keys[i].underlying)->lexeme;
                for (j = 0; j < record->nFields; j++)
                    if (!strcmp(key.lexeme, record->fieldNames[j].lexeme))
                        break;
            }
            if (j >= record->nFields) {
                error(c, at, "unknown composite field or too many struct literal elements");
                continue;
            }
            if (initialized[j])
                error(c, record->fieldNames[j], "duplicate struct field initializer");
            initialized[j] = 1;
            Type *field = parameter(c, target, record->fieldTypes[j]);
            check(c, field, expectedExpression(c, field, &v->values[i], at), at);
        }
        for (size_t i = 0; i < record->nVarParams; i++)
            if (!initialized[i])
                error(c, record->fieldNames[i], "missing struct parameter initializer");
        return target;
    }
    error(c, at, "literal requires an array, buffer, or struct assignment destination");
    return &unknown;
}
static Type *expectedExpression(Context *c, Type *expected, expr *e, token at) {
    if (e->type == EXPR_TYPE_COMPOSITE && !((struct expr_composite *)e->underlying)->type.type)
        return checkComposite(c, expected, e, at);
    return expression(c, e);
}
static void checkArrayDataLiteral(Context *c, Type *element, struct expr_composite *literal,
                                  token at) {
    for (size_t i = 0; i < literal->nEntries; i++) {
        if (literal->keys[i].type)
            error(c, at, "array data literals require positional elements");
        expr *item = &literal->values[i];
        if (!strcmp(element->name, "dynamic-array") && item->type == EXPR_TYPE_COMPOSITE) {
            checkArrayDataLiteral(c, element->element, item->underlying, at);
        } else
            check(c, element, expectedExpression(c, element, item, at), at);
    }
}
static void checkBufferDimension(Context *c, Type *t, token at) {
    if (strcmp(t->name, "buffer") || t->bufferSize != 2)
        return;
    expr *size = parseBufferDimension(t->dimension);
    if (!size) {
        error(c, at, "invalid runtime buffer size");
        return;
    }
    check(c, &integer, expression(c, size), at);
    free(size);
}
typedef struct {
    Type **items;
    size_t count;
} TypeList;
static TypeList valueTypes(Context *c, expr *values, size_t count, Type **expected,
                           size_t nExpected, token at, int assignmentContext) {
    TypeList list = {.items = allocate(c, sizeof(Type *) * (count + 1)), .count = count};
    for (size_t i = 0; i < count; i++) {
        Type *t = assignmentContext && i < nExpected
                      ? expectedExpression(c, expected[i], &values[i], at)
                      : expression(c, &values[i]);
        if (!strcmp(t->name, "tuple")) {
            if (count == 1)
                return (TypeList){t->arguments, t->nArguments};
            error(c, at, "multiple-return call must be the only expression in a value list");
        }
        if (t == &nothing)
            error(c, at, "function returns no value");
        list.items[i] = t;
    }
    return list;
}
static struct stmt_func *findMethod(Context *, struct stmt_struct *, const char *);
static void function(Context *c, struct stmt_func *f) {
    Scope scope = {c->scope, NULL};
    c->scope = &scope;
    struct stmt_func *previous = c->function;
    c->function = f;
    if (f->receiverType.nTokens) {
        typeTokens base = f->receiverType;
        if (base.tokens[0].type == STAR) { base.tokens++; base.nTokens--; }
        else if (base.tokens[base.nTokens - 1].type == STAR) base.nTokens--;
        Symbol *owner = lookup(c, base.tokens[0].lexeme);
        struct stmt_struct *record = owner && owner->isType ? owner->type->record : NULL;
        if (!record) error(c, base.tokens[0], "method receiver must name a struct");
        else if (record->nTypeParams != f->nReceiverTypeParams)
            error(c, base.tokens[0], "receiver type parameter count mismatch");
        if (record) {
            findMethod(c, record, f->name.lexeme);
            for (size_t i = 0; i < record->nFields; i++)
                if (!strcmp(record->fieldNames[i].lexeme, f->name.lexeme))
                    error(c, f->name, "method name conflicts with a struct field");
        }
        for (size_t i = 0; i < f->nReceiverTypeParams; i++) {
            token name = f->receiverTypeParamNames[i];
            // A known concrete type is not a fresh receiver parameter.
            if (lookup(c, name.lexeme) || builtinType(name.lexeme) ||
                !strcmp(name.lexeme, "void")) {
                error(c, name, "receiver type arguments must declare fresh type parameter names");
                continue;
            }
            Type *bound = allocate(c, sizeof(*bound));
            bound->name = name.lexeme;
            if (record && i < record->nTypeParams)
                bound->constraint = record->typeParamConstraints[i];
            declare(c, name, bound, 1);
        }
    }
    for (size_t i = 0; i < f->nTypeParams; i++)
        declare(c, f->typeParamNames[i], &any, 1);
    for (size_t i = 0; i < f->nParams; i++) {
        Type *t = typeOf(c, f->paramTypeNames[i]);
        if (f->isVariadic && !f->isExtern && i + 1 == f->nParams) {
            Type *list = allocate(c, sizeof(*list));
            list->name = "dynamic-array";
            list->element = t;
            if (!strcmp(t->name, "buffer") && t->bufferSize == 2)
                error(c, f->paramNames[i], "array elements must have a fixed storage size");
            t = list;
        }
        declare(c, f->paramNames[i], t, 0);
    }
    for (size_t i = 0; i < f->nParams; i++)
        checkBufferDimension(c, typeOf(c, f->paramTypeNames[i]), f->name);
    for (size_t i = 0; i < f->nResults; i++)
        declare(c, f->resultNames[i], typeOf(c, f->resultTypes[i]), 0);
    if (f->receiverName.type == IDENTIFIER)
        declare(c, f->receiverName,
                f->receiverType.nTokens ? typeOf(c, f->receiverType)
                : c->implicitReceiver   ? c->implicitReceiver
                                        : &unknown,
                0);
    block(c, &f->body, 0);
    c->function = previous;
    c->scope = scope.parent;
}
static Type *parameter(Context *c, Type *fn, typeTokens span) {
    size_t count = fn->function ? fn->function->nTypeParams
                   : fn->record ? fn->record->nTypeParams : 0;
    token *names = fn->function ? fn->function->typeParamNames
                   : fn->record ? fn->record->typeParamNames : NULL;
    Scope bindings = {.parent = c->scope};
    c->scope = &bindings;
    if (fn->function && fn->receiver) {
        for (size_t i = 0; i < fn->function->nReceiverTypeParams; i++)
            declare(c, fn->function->receiverTypeParamNames[i],
                    i < fn->receiver->nArguments ? fn->receiver->arguments[i] : &any, 1);
    }
    for (size_t i = 0; i < count; i++)
        declare(c, names[i], i < fn->nArguments ? fn->arguments[i] : &any, 1);
    Type *result = typeOf(c, span);
    c->scope = bindings.parent;
    return result;
}
static struct stmt_func *findMethod(Context *c, struct stmt_struct *record, const char *name) {
    struct stmt_func *found = NULL;
    for (size_t i = 0; i < c->program->nStatements; i++) {
        stmt *s = &c->program->statements[i];
        if (s->type != STMT_FUNC) continue;
        struct stmt_func *f = s->value;
        if (!f->receiverType.nTokens || strcmp(f->name.lexeme, name)) continue;
        token owner = f->receiverType.tokens[f->receiverType.tokens[0].type == STAR ? 1 : 0];
        if (strcmp(owner.lexeme, record->name.lexeme)) continue;
        if (found) error(c, f->name, "duplicate method declaration");
        found = f;
    }
    return found;
}
static token typeToken(enum tokenType kind, char *lexeme) {
    return (token){.type = kind, .lexeme = lexeme};
}
static int typeExpression(Context *c, expr *e, typeTokens *out) {
    *out = (typeTokens){0};
    if (e->type == EXPR_TYPE_IDENTIFIER) {
        token name = ((struct expr_identifier *)e->underlying)->lexeme;
        Symbol *symbol = lookup(c, name.lexeme);
        if (symbol ? !symbol->isType : !builtinStorageTypeName(name.lexeme))
            return 0;
        out->tokens = malloc(sizeof(token));
        if (!out->tokens)
            exit(1);
        out->tokens[0] = name;
        out->nTokens = 1;
        return 1;
    }
    if (e->type == EXPR_TYPE_INSTANTIATION) {
        struct expr_instantiation *v = e->underlying;
        if (v->parent.type != EXPR_TYPE_IDENTIFIER) return 0;
        token name = ((struct expr_identifier *)v->parent.underlying)->lexeme;
        Symbol *symbol = lookup(c, name.lexeme);
        if (strcmp(name.lexeme, "buffer") && !(symbol && symbol->isType && symbol->type->record))
            return 0;
        size_t n = 3 + (v->nTypeArguments ? v->nTypeArguments - 1 : 0);
        for (size_t i = 0; i < v->nTypeArguments; i++) n += v->typeArguments[i].nTokens;
        out->tokens = malloc(n * sizeof(token));
        if (!out->tokens) exit(1);
        out->nTokens = n;
        size_t cursor = 0;
        out->tokens[cursor++] = name;
        out->tokens[cursor++] = typeToken(LESS_THAN, "<");
        for (size_t i = 0; i < v->nTypeArguments; i++) {
            if (i) out->tokens[cursor++] = typeToken(COMMA, ",");
            memcpy(out->tokens + cursor, v->typeArguments[i].tokens, v->typeArguments[i].nTokens * sizeof(token));
            cursor += v->typeArguments[i].nTokens;
        }
        out->tokens[cursor] = typeToken(GREATER_THAN, ">");
        return 1;
    }
    if (e->type == EXPR_TYPE_INDEX) {
        struct expr_index *v = e->underlying;
        typeTokens base;
        if (!typeExpression(c, &v->parent, &base))
            return 0;
        char sizedBuffer = isBufferType(base) && v->index.type == EXPR_TYPE_LITERAL &&
                           ((struct expr_literal *)v->index.underlying)->type == LITERAL_TYPE_INT;
        if (v->index.type && !sizedBuffer) {
            free(base.tokens);
            return 0;
        }
        size_t extra = sizedBuffer ? 3 : 2;
        token *tokens = realloc(base.tokens, sizeof(token) * (base.nTokens + extra));
        if (!tokens)
            exit(1);
        tokens[base.nTokens] = typeToken(LEFT_BRACKET, "[");
        if (sizedBuffer) {
            tokens[base.nTokens + 1] = typeToken(LITERAL_INT, "<buffer size>");
            tokens[base.nTokens + 1].numValue.integer =
                *(long *)((struct expr_literal *)v->index.underlying)->value;
        }
        tokens[base.nTokens + extra - 1] = typeToken(RIGHT_BRACKET, "]");
        *out = (typeTokens){base.nTokens + extra, tokens};
        return 1;
    }
    return 0;
}
static Type *recordReceiver(Type *t) {
    while (!t->record && !strcmp(t->name, "pointer") && t->element)
        t = t->element;
    return t;
}
/* Immutability protects bindings/value fields, while indexed reference contents stay writable. */
static token immutableTarget(Context *c, expr *e) {
    token none = {.type = EOF_TOKEN};
    if (e->type == EXPR_TYPE_IDENTIFIER) {
        token name = ((struct expr_identifier *)e->underlying)->lexeme;
        Symbol *symbol = lookup(c, name.lexeme);
        return symbol && symbol->immutable ? name : none;
    }
    if (e->type == EXPR_TYPE_DOT) {
        struct expr_dot *v = e->underlying;
        Type *rawParent = expression(c, &v->parent);
        Type *parent = recordReceiver(rawParent);
        token field = ((struct expr_identifier *)v->field.underlying)->lexeme;
        if (!strcmp(parent->name, "dynamic-array") && !strcmp(field.lexeme, "size"))
            return field;
        if (parent->record) {
            for (size_t i = 0; i < parent->record->nVarParams; i++)
                if (!strcmp(field.lexeme, parent->record->fieldNames[i].lexeme))
                    return field;
            return !strcmp(rawParent->name, "pointer") ? none : immutableTarget(c, &v->parent);
        }
        if (strcmp(rawParent->name, "pointer") && strcmp(rawParent->name, "dynamic-array") &&
            strcmp(rawParent->name, "buffer"))
            return immutableTarget(c, &v->parent);
    }
    if (e->type == EXPR_TYPE_UNARY &&
        ((struct expr_unary *)e->underlying)->operation == UNARY_DEREFERENCE) {
        expr *child = &((struct expr_unary *)e->underlying)->child;
        if (child->type == EXPR_TYPE_UNARY &&
            ((struct expr_unary *)child->underlying)->operation == UNARY_ADDRESS_OF)
            return immutableTarget(c, &((struct expr_unary *)child->underlying)->child);
    }
    return none;
}
static void mutationError(Context *c, expr *target, token at, const char *fallback) {
    token immutable = immutableTarget(c, target);
    if (immutable.type != EOF_TOKEN)
        error(c, immutable, "cannot modify immutable struct parameter or field");
    else
        error(c, at, fallback);
}
static int lvalue(expr *e) {
    return e->type == EXPR_TYPE_IDENTIFIER || e->type == EXPR_TYPE_DOT ||
           e->type == EXPR_TYPE_INDEX ||
           (e->type == EXPR_TYPE_UNARY &&
            ((struct expr_unary *)e->underlying)->operation == UNARY_DEREFERENCE);
}
static int arrayDataTarget(Context *c, expr *e) {
    if (e->type != EXPR_TYPE_DOT)
        return 0;
    struct expr_dot *v = e->underlying;
    return !strcmp(((struct expr_identifier *)v->field.underlying)->lexeme.lexeme, "data") &&
           !strcmp(expression(c, &v->parent)->name, "dynamic-array");
}
static int writable(Context *c, expr *e) {
    if (!lvalue(e))
        return 0;
    if (immutableTarget(c, e).type != EOF_TOKEN)
        return 0;
    if (e->type == EXPR_TYPE_DOT) {
        struct expr_dot *v = e->underlying;
        Type *parent = expression(c, &v->parent);
        if (!strcmp(parent->name, "dynamic-array") &&
            strcmp(((struct expr_identifier *)v->field.underlying)->lexeme.lexeme, "data"))
            return 0;
    }
    return 1;
}
static Type *expression(Context *c, expr *e) {
    token at = {0};
    if (!e || !e->underlying)
        return &unknown;
    switch (e->type) {
    case EXPR_TYPE_IDENTIFIER: {
        token t = ((struct expr_identifier *)e->underlying)->lexeme;
        if (!strcmp(t.lexeme, "true") || !strcmp(t.lexeme, "false"))
            return &boolean;
        Symbol *s = lookup(c, t.lexeme);
        if (!s) {
            error(c, t, "unresolved name");
            return &unknown;
        }
        return s->type;
    }
    case EXPR_TYPE_LITERAL: {
        struct expr_literal *v = e->underlying;
        return v->type == LITERAL_TYPE_STRING  ? &string
               : v->type == LITERAL_TYPE_FLOAT ? &floating
                                               : &integer;
    }
    case EXPR_TYPE_FUNCTION: {
        Type *t = allocate(c, sizeof(*t));
        t->name = "function";
        t->function = e->underlying;
        function(c, t->function);
        return t;
    }
    case EXPR_TYPE_INSTANTIATION: {
        struct expr_instantiation *v = e->underlying;
        typeTokens constructorType;
        Symbol *namedType =
            v->parent.type == EXPR_TYPE_IDENTIFIER
                ? lookup(c, ((struct expr_identifier *)v->parent.underlying)->lexeme.lexeme)
                : NULL;
        int genericRecord = namedType && namedType->isType && namedType->type->record &&
                            namedType->type->record->nTypeParams;
        if (!genericRecord && v->nTypeArguments == 1 &&
            typeExpression(c, &v->parent, &constructorType)) {
            free(constructorType.tokens);
            expr *count = parseBufferDimension(v->typeArguments[0]);
            if (!count) {
                error(c, at, "invalid array item count");
                return &unknown;
            }
            struct expr_index *index = malloc(sizeof(*index));
            if (!index)
                exit(1);
            *index = (struct expr_index){v->parent, *count, v->location};
            free(count);
            free(v->typeArguments[0].tokens);
            free(v->typeArguments);
            free(v);
            e->type = EXPR_TYPE_INDEX;
            e->underlying = index;
            return expression(c, e);
        }
        if (v->nTypeArguments == 1 && v->typeArguments[0].nTokens == 1) {
            token arg = v->typeArguments[0].tokens[0];
            Symbol *s = lookup(c, arg.lexeme);
            if (s && !s->isType) {
                struct expr_index *idx = malloc(sizeof(*idx));
                struct expr_identifier *id = malloc(sizeof(*id));
                if (!idx || !id)
                    exit(1);
                *id = (struct expr_identifier){arg};
                *idx = (struct expr_index){v->parent, {EXPR_TYPE_IDENTIFIER, id}, v->location};
                free(v->typeArguments[0].tokens);
                free(v->typeArguments);
                free(v);
                e->type = EXPR_TYPE_INDEX;
                e->underlying = idx;
                return expression(c, e);
            }
        }
        Type *base = expression(c, &v->parent), *t = allocate(c, sizeof(*t));
        *t = *base;
        if (!base->function && !base->record)
            error(c, at, "value cannot be instantiated");
        size_t count = base->function ? base->function->nTypeParams
                       : base->record ? base->record->nTypeParams
                                      : 0;
        if (count != v->nTypeArguments)
            error(c, at, "type argument count mismatch");
        t->nArguments = v->nTypeArguments;
        t->arguments = allocate(c, sizeof(Type *) * t->nArguments);
        for (size_t i = 0; i < t->nArguments; i++)
            t->arguments[i] = typeOf(c, v->typeArguments[i]);
        return t;
    }
    case EXPR_TYPE_INDEX: {
        struct expr_index *v = e->underlying;
        typeTokens element;
        if (v->index.type && typeExpression(c, &v->parent, &element)) {
            struct expr_array_new *array = malloc(sizeof(*array));
            if (!array)
                exit(1);
            *array = (struct expr_array_new){element, v->index};
            free(v);
            e->type = EXPR_TYPE_ARRAY_NEW;
            e->underlying = array;
            return expression(c, e);
        }
        Type *p = expression(c, &v->parent);
        check(c, &integer, expression(c, &v->index), at);
        if (p->element)
            return p->element;
        if (p != &unknown)
            error(c, at, "value is not indexable");
        return &unknown;
    }
    case EXPR_TYPE_ARRAY_NEW: {
        struct expr_array_new *v = e->underlying;
        Type *t = allocate(c, sizeof(*t));
        t->name = "dynamic-array";
        t->element = typeOf(c, v->elementType);
        if (!strcmp(t->element->name, "buffer") && t->element->bufferSize == 2)
            error(c, at, "array elements must have a fixed storage size");
        check(c, &integer, expression(c, &v->count), at);
        if (v->count.type == EXPR_TYPE_UNARY &&
            ((struct expr_unary *)v->count.underlying)->operation == UNARY_NEGATE &&
            ((struct expr_unary *)v->count.underlying)->child.type == EXPR_TYPE_LITERAL &&
            ((struct expr_literal *)((struct expr_unary *)v->count.underlying)->child.underlying)
                    ->type == LITERAL_TYPE_INT &&
            *(long *)((struct expr_literal *)((struct expr_unary *)v->count.underlying)
                          ->child.underlying)
                    ->value > 0)
            error(c, at, "array item count must not be negative");
        return t;
    }
    case EXPR_TYPE_CALL: {
        struct expr_call *v = e->underlying;
        if (v->func.type == EXPR_TYPE_IDENTIFIER &&
            !strcmp(((struct expr_identifier *)v->func.underlying)->lexeme.lexeme, "len") &&
            !lookup(c, "len")) {
            if (v->nParams != 1 || v->spread)
                error(c, at, "len requires exactly one array argument");
            for (size_t i = 0; i < v->nParams; i++) {
                Type *arg = expression(c, &v->params[i]);
                if (strcmp(arg->name, "dynamic-array"))
                    error(c, at, "len requires an array");
            }
            return &integer;
        }
        Type *t = expression(c, &v->func);
        struct stmt_func *f = t->function;
        expr *callee = &v->func;
        if (callee->type == EXPR_TYPE_INSTANTIATION)
            callee = &((struct expr_instantiation *)callee->underlying)->parent;
        if (f && f->receiverType.nTokens && callee->type == EXPR_TYPE_DOT) {
            if (f->nTypeParams && v->func.type != EXPR_TYPE_INSTANTIATION)
                error(c, f->name, "generic method requires explicit method type arguments");
            expr *parent = &((struct expr_dot *)callee->underlying)->parent;
            Type *actual = expression(c, parent);
            int needsPointer = f->receiverType.tokens[0].type == STAR ||
                f->receiverType.tokens[f->receiverType.nTokens - 1].type == STAR;
            if (needsPointer && strcmp(actual->name, "pointer") && !lvalue(parent))
                error(c, f->name, "pointer receiver requires an addressable value");
            if (needsPointer && strcmp(actual->name, "pointer") &&
                immutableTarget(c, parent).type != EOF_TOKEN)
                mutationError(c, parent, f->name, "pointer receiver cannot borrow an immutable value");
        }
        if (!f && t != &unknown)
            error(c, at, "value is not callable");
        v->packsVariadic =
            f && f->isVariadic && !f->isExtern && (!v->spread || v->nParams > f->nParams);
        size_t fixed = f ? f->nParams - (f->isVariadic && !f->isExtern ? 1 : 0) : 0;
        Type **expected = allocate(c, sizeof(Type *) * (v->nParams + 1));
        for (size_t i = 0; i < v->nParams; i++) {
            expected[i] =
                f && f->nParams && !(f->isExtern && i >= fixed)
                    ? parameter(c, t, f->paramTypeNames[i < f->nParams ? i : f->nParams - 1])
                    : &unknown;
            if (v->spread && i + 1 == v->nParams)
                expected[i] = &unknown;
        }
        TypeList args = valueTypes(c, v->params, v->nParams, expected, v->nParams, at, 0);
        if (v->spread) {
            if (args.count != v->nParams) {
                error(c, at, "expanded argument must be an array, not multiple return values");
            } else if (!f || !f->isVariadic || f->isExtern || args.count < f->nParams) {
                error(c, at,
                      "expanded argument requires an Ocean variadic function and its fixed "
                      "arguments");
            } else {
                Type *last = args.items[args.count - 1];
                if (!strcmp(last->name, "buffer"))
                    error(c, at,
                          "buffer expansion is not supported; expanded argument must be an array");
                else if (strcmp(last->name, "dynamic-array"))
                    error(c, at, "expanded argument must be an array");
                else
                    check(c, parameter(c, t, f->paramTypeNames[f->nParams - 1]), last->element, at);
            }
        } else if (f && (args.count < fixed || (!f->isVariadic && args.count != fixed)))
            error(c, f->name, "argument count mismatch");
        for (size_t i = 0; f && i < args.count; i++) {
            if (v->spread && i + 1 == args.count)
                continue;
            if (i < fixed || (f->isVariadic && !f->isExtern)) {
                size_t j = i < fixed ? i : f->nParams - 1;
                check(c, parameter(c, t, f->paramTypeNames[j]), args.items[i], f->name);
            }
        }
        if (!f)
            return &unknown;
        if (f->nResults <= 1)
            return f->nResults ? parameter(c, t, f->resultTypes[0]) : &nothing;
        Type *tuple = allocate(c, sizeof(*tuple));
        tuple->name = "tuple";
        tuple->nArguments = f->nResults;
        tuple->arguments = allocate(c, sizeof(Type *) * f->nResults);
        for (size_t i = 0; i < f->nResults; i++)
            tuple->arguments[i] = parameter(c, t, f->resultTypes[i]);
        return tuple;
    }
    case EXPR_TYPE_BINARY: {
        struct expr_binary *v = e->underlying;
        if (v->operation >= EXPR_ADD_EQUALS && v->operation <= EXPR_BITWISE_OR_EQUALS &&
            arrayDataTarget(c, &v->left))
            error(c, at, "array.data supports copy assignment with =, not compound assignment");
        if (v->operation == EXPR_ASSIGNMENT && v->left.type == EXPR_TYPE_DOT) {
            struct expr_dot *target = v->left.underlying;
            token field = ((struct expr_identifier *)target->field.underlying)->lexeme;
            if (!strcmp(field.lexeme, "data")) {
                Type *array = expression(c, &target->parent);
                if (!strcmp(array->name, "dynamic-array")) {
                    if (v->right.type == EXPR_TYPE_COMPOSITE) {
                        struct expr_composite *literal = v->right.underlying;
                        if (literal->type.type) {
                            Type *source = expression(c, &v->right);
                            if (strcmp(source->name, "buffer") &&
                                strcmp(source->name, "dynamic-array"))
                                error(c, field,
                                      "array data literal type must be an array or buffer");
                            else
                                check(c, array->element, source->element, field);
                        }
                        checkArrayDataLiteral(c, array->element, literal, field);
                    } else {
                        Type *source = expression(c, &v->right);
                        if (strcmp(source->name, "buffer"))
                            error(c, field,
                                  "array.data assignment requires a buffer or composite literal");
                        else
                            check(c, array->element, source->element, field);
                    }
                    Type *result = allocate(c, sizeof(*result));
                    result->name = "buffer";
                    result->element = array->element;
                    return result;
                }
            }
        }
        if (v->operation == EXPR_ASSIGNMENT && (isComma(&v->left) || isDiscard(&v->left))) {
            size_t n = commaCount(&v->left), m = commaCount(&v->right);
            Type **targets = allocate(c, sizeof(Type *) * n);
            expr *rhs = allocate(c, sizeof(expr) * m);
            for (size_t i = 0; i < n; i++) {
                expr *target = commaItem(&v->left, i);
                if (arrayDataTarget(c, target))
                    error(c, at, "array.data copy assignment must be used on its own");
                targets[i] = isDiscard(target) ? &unknown : expression(c, target);
                if (!writable(c, target))
                    mutationError(c, target, at, "assignment requires a writable value");
            }
            for (size_t i = 0; i < m; i++)
                rhs[i] = *commaItem(&v->right, i);
            TypeList values = valueTypes(c, rhs, m, targets, n, at, 1);
            if (values.count != n)
                error(c, at, "assignment count mismatch");
            for (size_t i = 0; i < n && i < values.count; i++)
                check(c, targets[i], values.items[i], at);
            return &nothing;
        }
        Type *a = expression(c, &v->left);
        Type *b = v->operation == EXPR_ASSIGNMENT ? expectedExpression(c, a, &v->right, at)
                                                  : expression(c, &v->right);
        if (v->operation == EXPR_COMMA)
            return b;
        check(c, a, b, at);
        if (v->operation == EXPR_ASSIGNMENT ||
            (v->operation >= EXPR_ADD_EQUALS && v->operation <= EXPR_BITWISE_OR_EQUALS)) {
            if (!strcmp(a->name, "dynamic-array") && a->element == &signedByte &&
                v->operation != EXPR_ASSIGNMENT && v->operation != EXPR_ADD_EQUALS)
                error(c, at, "strings support only = and += assignment");
            if (!writable(c, &v->left))
                mutationError(c, &v->left, at, "assignment requires a writable value");
            return a;
        }
        if (v->operation == EXPR_LOGICAL_OR || v->operation == EXPR_LOGICAL_AND) {
            check(c, &boolean, a, at);
            return &boolean;
        }
        if (v->operation >= EXPR_EQUALITY && v->operation <= EXPR_GREATER_EQUALS)
            return &boolean;
        if (a != &unknown && a != &any && a != &integer && a != &signedByte && a != &floating &&
            !(!strcmp(a->name, "dynamic-array") && a->element == &signedByte &&
              v->operation == EXPR_ADD))
            error(c, at, "invalid operands");
        return a;
    }
    case EXPR_TYPE_UNARY: {
        struct expr_unary *v = e->underlying;
        if (v->operation == UNARY_SIZEOF && typeExpression(c, &v->child, &v->sizeofType)) {
            typeOf(c, v->sizeofType);
            return &integer;
        }
        Type *t = expression(c, &v->child);
        if (v->operation >= UNARY_POSTFIX_INCREMENT && v->operation <= UNARY_PREFIX_DECREMENT &&
            !writable(c, &v->child))
            mutationError(c, &v->child, at, "increment requires a writable value");
        if (v->operation == UNARY_ADDRESS_OF) {
            if (arrayDataTarget(c, &v->child))
                error(
                    c, at,
                    "array.data is inline storage; use array.data directly as its buffer address");
            token immutable = immutableTarget(c, &v->child);
            if (immutable.type != EOF_TOKEN)
                error(c, immutable,
                      "cannot take a mutable address of an immutable struct parameter or field");
            Type *p = allocate(c, sizeof(*p));
            p->name = "pointer";
            p->element = t;
            return p;
        }
        if (v->operation == UNARY_DEREFERENCE) {
            if (t->element)
                return t->element;
            error(c, at, "cannot dereference value");
            return &unknown;
        }
        if (v->operation == UNARY_LOGICAL_NOT) {
            check(c, &boolean, t, at);
            return &boolean;
        }
        if (v->operation == UNARY_SIZEOF)
            return &integer;
        if (t != &integer && t != &signedByte && t != &floating && t != &unknown && t != &any)
            error(c, at, "invalid unary operand");
        return t;
    }
    case EXPR_TYPE_TERNARY: {
        struct expr_ternary *v = e->underlying;
        check(c, &boolean, expression(c, &v->condition), at);
        Type *a = expression(c, &v->ifTrue);
        check(c, a, expression(c, &v->ifFalse), at);
        return a;
    }
    case EXPR_TYPE_TYPECAST: {
        struct expr_typecast *v = e->underlying;
        Type *a = expression(c, &v->child), *b = typeOf(c, v->type);
        if (!compatible(a, b) &&
            !((a == &integer || a == &floating) && (b == &integer || b == &floating)))
            error(c, at, "invalid conversion");
        return b;
    }
    case EXPR_TYPE_ASSERTION: {
        struct expr_assertion *v = e->underlying;
        expression(c, &v->child);
        return v->isTypeGuard ? &any : typeOf(c, v->type);
    }
    case EXPR_TYPE_DOT: {
        struct expr_dot *v = e->underlying;
        Type *t = recordReceiver(expression(c, &v->parent));
        token field = ((struct expr_identifier *)v->field.underlying)->lexeme;
        if (!strcmp(t->name, "dynamic-array")) {
            if (!strcmp(field.lexeme, "size"))
                return &integer;
            if (!strcmp(field.lexeme, "data")) {
                Type *buffer = allocate(c, sizeof(*buffer));
                buffer->name = "buffer";
                buffer->element = t->element;
                return buffer;
            }
            error(c, field, "array only has size and data fields");
            return &unknown;
        }
        if (!strcmp(t->name, "buffer"))
            error(c, field, "buffer has no members");
        if (t->record) {
            for (size_t i = 0; i < t->record->nFields; i++)
                if (!strcmp(field.lexeme, t->record->fieldNames[i].lexeme))
                    return parameter(c, t, t->record->fieldTypes[i]);
            struct stmt_func *method = findMethod(c, t->record, field.lexeme);
            if (method) {
                Type *callable = allocate(c, sizeof(*callable));
                callable->name = "function";
                callable->function = method;
                callable->receiver = t;
                return callable;
            }
            error(c, field, "unknown field");
        }
        return &unknown;
    }
    case EXPR_TYPE_COMPOSITE: {
        struct expr_composite *v = e->underlying;
        if (!v->type.type) {
            error(c, v->location,
                  "standalone literal requires an explicit type, such as int[]{...}, "
                  "buffer<int>{...}, or StructType{...}");
            return &unknown;
        }
        typeTokens prefix;
        Type *target = NULL;
        if (typeExpression(c, &v->type, &prefix)) {
            if (!v->literalType.nTokens)
                v->literalType = prefix;
            else
                free(prefix.tokens);
            target = typeOf(c, v->literalType);
        } else if (v->type.type == EXPR_TYPE_INSTANTIATION) {
            struct expr_instantiation *instance = v->type.underlying;
            if (instance->parent.type == EXPR_TYPE_IDENTIFIER) {
                Symbol *symbol = lookup(
                    c, ((struct expr_identifier *)instance->parent.underlying)->lexeme.lexeme);
                if (symbol && symbol->isType && symbol->type->record)
                    target = expression(c, &v->type);
            }
        }
        if (!target) {
            error(c, v->location, "literal prefix must name an array, buffer, or struct type");
            return &unknown;
        }
        return checkComposite(c, target, e, v->location);
    }
    default:
        return &unknown;
    }
}
static void predeclare(Context *c, stmt *s) {
    Type *t;
    switch (s->type) {
    case STMT_FUNC: {
        struct stmt_func *f = s->value;
        if (f->receiverType.nTokens)
            return;
        t = allocate(c, sizeof(*t));
        t->name = "function";
        t->function = f;
        declare(c, f->name, t, 0);
        break;
    }
    case STMT_STRUCT: {
        struct stmt_struct *v = s->value;
        t = allocate(c, sizeof(*t));
        t->name = v->name.lexeme;
        t->record = v;
        declare(c, v->name, t, 1);
        break;
    }
    case STMT_TYPE: {
        struct stmt_type *v = s->value;
        t = allocate(c, sizeof(*t));
        t->name = v->name.lexeme;
        declare(c, v->name, t, 1);
        break;
    }
    case STMT_ENUM: {
        struct stmt_enum *v = s->value;
        declare(c, v->name, &integer, 1);
        for (size_t i = 0; i < v->nMembers; i++)
            declare(c, v->names[i], &integer, 0);
        break;
    }
    case STMT_INTERFACE: {
        struct stmt_interface *v = s->value;
        declare(c, v->name, &any, 1);
        break;
    }
    default:
        break;
    }
}
static void statement(Context *c, stmt *s) {
    token at = {0};
    switch (s->type) {
    case STMT_VAR: {
        struct stmt_var *v = s->value;
        Type *declared = v->type.nTokens ? typeOf(c, v->type) : NULL;
        if (declared && declared->record && declared->record->nVarParams && !v->nInitValues)
            error(c, v->vars[0], "struct with parameters requires an initializer");
        if (declared && !strcmp(declared->name, "buffer")) {
            checkBufferDimension(c, declared, v->vars[0]);
            if (declared->bufferSize == 0 && !v->nInitValues)
                error(c, v->vars[0], "unsized buffer requires an initializer");
            if (declared->bufferSize == 2 && v->nInitValues)
                error(c, v->vars[0], "runtime-sized buffers cannot have initializers");
        }
        Type **expected = allocate(c, sizeof(Type *) * v->nVars);
        for (size_t i = 0; i < v->nVars; i++)
            expected[i] = declared ? declared : &unknown;
        TypeList values =
            valueTypes(c, v->initValues, v->nInitValues, expected, v->nVars, v->vars[0], 1);
        if (v->nInitValues && values.count != v->nVars)
            error(c, v->vars[0], "initializer count mismatch");
        for (size_t i = 0; i < v->nVars; i++) {
            Type *a = i < values.count ? values.items[i] : &unknown;
            if (declared)
                check(c, declared, a, v->vars[i]);
            declare(c, v->vars[i], declared ? declared : a, 0);
        }
        break;
    }
    case STMT_FUNC:
        function(c, s->value);
        break;
    case STMT_TYPE: {
        struct stmt_type *v = s->value;
        Type *t = typeOf(c, v->underlying);
        Symbol *sym = lookup(c, v->name.lexeme);
        if (sym && v->isAlias)
            sym->type = t;
        break;
    }
    case STMT_STRUCT: {
        struct stmt_struct *v = s->value;
        Symbol *record = lookup(c, v->name.lexeme);
        Type *receiverType = record ? record->type : &unknown;
        Scope scope = {c->scope, NULL};
        c->scope = &scope;
        for (size_t i = 0; i < v->nTypeParams; i++)
            declare(c, v->typeParamNames[i], &any, 1);
        for (size_t i = 0; i < v->nFields; i++) {
            Symbol *field = declare(c, v->fieldNames[i], typeOf(c, v->fieldTypes[i]), 0);
            if (field)
                field->immutable = i < v->nVarParams;
        }
        for (size_t i = 0; i < v->nFields; i++)
            checkBufferDimension(c, typeOf(c, v->fieldTypes[i]), v->fieldNames[i]);
        Type *oldReceiver = c->implicitReceiver;
        c->implicitReceiver = receiverType;
        for (size_t i = 0; i < v->nOperatorMethods; i++)
            function(c, &v->operatorMethods[i]);
        c->implicitReceiver = oldReceiver;
        c->scope = scope.parent;
        break;
    }
    case STMT_EXPR:
        expression(c, s->value);
        break;
    case STMT_BLOCK:
        block(c, s->value, 1);
        break;
    case STMT_RETURN: {
        struct stmt_return *v = s->value;
        struct stmt_func *f = c->function;
        if (!f) {
            error(c, at, "return outside function");
            break;
        }
        int named = f->nResults > 0;
        for (size_t i = 0; i < f->nResults; i++)
            if (f->resultNames[i].type == EOF_TOKEN)
                named = 0;
        Type **expected = allocate(c, sizeof(Type *) * (f->nResults + 1));
        for (size_t i = 0; i < f->nResults; i++)
            expected[i] = typeOf(c, f->resultTypes[i]);
        TypeList values = valueTypes(c, v->values, v->nValues, expected, f->nResults, f->name, 0);
        if (values.count != f->nResults && !(named && !v->nValues))
            error(c, f->name, "return count mismatch");
        for (size_t i = 0; i < values.count && i < f->nResults; i++)
            check(c, expected[i], values.items[i], f->name);
        break;
    }
    case STMT_IF: {
        struct stmt_if *v = s->value;
        Scope scope = {c->scope, NULL};
        c->scope = &scope;
        if (v->preStmt)
            statement(c, v->preStmt);
        check(c, &boolean, expression(c, v->condition), at);
        if (v->ifTrue)
            statement(c, v->ifTrue);
        if (v->ifFalse)
            statement(c, v->ifFalse);
        c->scope = scope.parent;
        break;
    }
    case STMT_FOR: {
        struct stmt_for *v = s->value;
        Scope scope = {c->scope, NULL};
        c->scope = &scope;
        if (v->initializer)
            statement(c, v->initializer);
        if (v->range) {
            Type *t = expression(c, v->range);
            for (size_t i = 0; i < v->nBindings; i++)
                declare(c, v->bindings[i],
                        i == 0       ? &integer
                        : t->element ? t->element
                                     : &unknown,
                        0);
        }
        if (v->condition)
            check(c, &boolean, expression(c, v->condition), at);
        if (v->update)
            expression(c, v->update);
        if (v->body)
            statement(c, v->body);
        c->scope = scope.parent;
        break;
    }
    case STMT_DEFER:
        expression(c, ((struct stmt_defer *)s->value)->call);
        break;
    case STMT_LABEL: {
        struct stmt_label *v = s->value;
        if (v->body)
            statement(c, v->body);
        break;
    }
    case STMT_SWITCH: {
        struct stmt_switch *v = s->value;
        Scope scope = {c->scope, NULL};
        c->scope = &scope;
        statement(c, &v->initializer);
        Type *t = v->value ? expression(c, v->value) : &boolean;
        for (size_t i = 0; i < v->nCases; i++) {
            Scope cs = {c->scope, NULL};
            c->scope = &cs;
            if (v->isTypeSwitch)
                declare(c, v->binding, &any, 0);
            for (size_t j = 0; j < v->cases[i].nValues; j++)
                check(c, t, expression(c, &v->cases[i].values[j]), at);
            block(c, &v->cases[i].body, 0);
            c->scope = cs.parent;
        }
        c->scope = scope.parent;
        break;
    }
    default:
        break;
    }
}
semanticResult analyzeSemantics(parseResult *program) {
    if (!program || program->hasError)
        return (semanticResult){1};
    Scope root = {0};
    Context c = {.scope = &root, .program = program};
    for (size_t i = 0; i < program->nStatements; i++)
        predeclare(&c, &program->statements[i]);
    for (size_t i = 0; i < program->nStatements; i++)
        statement(&c, &program->statements[i]);
    while (c.allocations) {
        Allocation *a = c.allocations;
        c.allocations = a->next;
        free(a->ptr);
        free(a);
    }
    return (semanticResult){c.errors};
}
