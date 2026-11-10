#define _POSIX_C_SOURCE 200809L
#include "semantic.h"
#include "scanner.h"
#include "diagnostic.h"
#include "array_type.h"
#include "buffer_type.h"
#include "value_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Type Type;
struct Type {
    const char *name;
    struct stmt_type *declaration;
    Type *underlying;
    struct Scope *declarationScope;
    unsigned resolutionState, resolutionDepth;
    Type *element;
    Type *mapKey;
    Type *receiver;
    struct stmt_func *function;
    Type **functionParams, **functionResults; // Resolved while lexical type bindings are live.
    struct stmt_struct *record;
    struct stmt_enum *enumeration;
    typeTokens *enumArguments; // Source spelling retained for constructor monomorphization.
    size_t enumMember; // Nonzero only for a variant selector/type (index + 1).
    struct stmt_interface *interface;
    struct Context *context;
    int mixed;
    int dynamicArray;
    int immutableString;
    Type **arguments;
    size_t nArguments;
    size_t count;
    int bufferSize;
    int bufferFamily; // 1: bare buffer; 2: buffer<T> without a dimension.
    typeTokens dimension;
    typeTokens constraint; // Type-list spelling, retained for membership and operator checks.
    int abstractParameter;
    int typeList;
    const char *spelling; // Keep explicit numeric type names for diagnostics.
    Type *semanticBase; // Numeric family used for operations; spelling retains storage width.
};
typedef struct Symbol {
    token name;
    Type *type;
    int isType;
    int immutable;
    int freed;
    struct stmt_func *ownerFunction;
    struct Symbol *next;
} Symbol;
typedef struct Scope {
    struct Scope *parent;
    Symbol *symbols;
    token open, close;
} Scope;
typedef struct Allocation {
    void *ptr;
    struct Allocation *next;
} Allocation;
typedef struct Context {
    size_t errors;
    int externalArguments;
    unsigned typeIndirection;
    parseResult *program;
    Scope *scope;
    struct stmt_func *function;
    Allocation *allocations;
    semanticIndex *index;
    Symbol **indexedSymbols;
    Type **referenceTypes;
    Type **completionTypes;
    token *completionUses;
    size_t nCompletionTypes;
    int declaringMembers;
    token memberOwner;
} Context;
static Type nullValue = {.name = "null"};
static Type unknown = {.name = "?"}, integer = {.name = "int"}, floating = {.name = "float"},
            signedByte = {.name = "i8"},
            string = {.name = "dynamic-array", .element = &signedByte, .dynamicArray = 1, .immutableString = 1},
            boolean = {.name = "bool"}, any = {.name = "any"}, nothing = {.name = "void"};
static Type unsignedIntegerType = {.name = "int", .spelling = "uint", .semanticBase = &integer};
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
static token diagnosticLocation(Context *c, token at) {
    if (at.line <= 0 && c->function)
        at = c->function->name;
    if (!at.filename)
        at.filename =
            (char *)(c->program->sourceFilename ? c->program->sourceFilename : "<source>");
    if (at.line <= 0)
        at.line = 1;
    if (at.column <= 0)
        at.column = 1;
    return at;
}
static void error(Context *c, token t, const char *message) {
    t = diagnosticLocation(c, t);
    diagnostic(programSource(c->program, t.filename), t, "semantic", "error", "%s%s%s", message,
               t.lexeme ? ": " : "", t.lexeme ? t.lexeme : "");
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
static size_t indexSymbol(Context *, Symbol *);
static void indexReference(Context *, token, Symbol *);
static Symbol *declare(Context *c, token name, Type *type, int isType) {
    if (isType && name.lexeme && !strcmp(name.lexeme, "array"))
        error(c, name, "array is a reserved type name");
    if (!name.lexeme || name.type == EOF_TOKEN || !strcmp(name.lexeme, "_"))
        return NULL;
    for (Symbol *v = c->scope->symbols; v; v = v->next)
        if (!strcmp(v->name.lexeme, name.lexeme)) {
            error(c, name, "duplicate declaration");
            return NULL;
        }
    Symbol *v = allocate(c, sizeof(*v));
    *v = (Symbol){.name = name, .type = type, .isType = isType, .ownerFunction = c->function, .next = c->scope->symbols};
    c->scope->symbols = v;
    if (c->index) indexSymbol(c,v);
    return v;
}
static Type *canonical(Type *t) { return t->semanticBase ? t->semanticBase : t; }
static int interfaceFits(Context *, Type *, Type *, unsigned);
static typeTokens typeSetSpan(Type *);
static int typeSetFits(Context *, Type *, Type *, unsigned);
static int identicalTypes(Type *, Type *);
static size_t constraintTermEnd(typeTokens, size_t);
static typeTokens constraintTerm(typeTokens, size_t, size_t);

static int definedType(Type *t) { return t->declaration && !t->declaration->isAlias; }
static Type *representation(Type *t) {
    for (unsigned depth = 0; t->underlying && depth < 64; depth++) t = t->underlying;
    return canonical(t);
}
// Type arguments are constraints, not numeric assignment conversions.
static int matchesTypeConstraint(Type *, Type *);
/* An explicit any argument is concrete storage; abstract type parameters remain flexible. */
static int enumAnyArgumentsFit(Type *a, Type *b) {
    if (a->semanticBase == &any || b->semanticBase == &any) return 1;
    if (a == &any || b == &any) return a == b;
    if (definedType(a) || definedType(b)) return 1;
    if (a->element && b->element && !enumAnyArgumentsFit(a->element, b->element)) return 0;
    if (a->nArguments == b->nArguments)
        for (size_t i = 0; i < a->nArguments; i++)
            if (!enumAnyArgumentsFit(a->arguments[i], b->arguments[i])) return 0;
    return 1;
}
static int enumArgumentsFit(Type *a, Type *b) {
    for (size_t i = 0; i < a->nArguments; i++)
        if (!enumAnyArgumentsFit(a->arguments[i], b->arguments[i]) ||
            !matchesTypeConstraint(a->arguments[i], b->arguments[i]) ||
            !matchesTypeConstraint(b->arguments[i], a->arguments[i])) return 0;
    return 1;
}
static Type *parameter(Context *, Type *, typeTokens);
static int identicalFunctions(Type *a, Type *b) {
    if (!a->function || !b->function) return 0;
    struct stmt_func *left = a->function, *right = b->function;
    if ((left->nTypeParams && a->nArguments != left->nTypeParams) ||
        (right->nTypeParams && b->nArguments != right->nTypeParams)) return left == right;
    if (left->nParams != right->nParams || left->nResults != right->nResults ||
        left->isVariadic != right->isVariadic ||
        (left->isVariadic && left->isExtern != right->isExtern)) return 0;
    Context *context = a->context ? a->context : b->context;
    if (!context) return left == right;
    for (size_t i = 0; i < left->nParams + left->nResults; i++) {
        typeTokens l = i < left->nParams ? left->paramTypeNames[i] : left->resultTypes[i-left->nParams];
        typeTokens r = i < right->nParams ? right->paramTypeNames[i] : right->resultTypes[i-right->nParams];
        if (!identicalTypes(parameter(context, a, l), parameter(context, b, r))) return 0;
    }
    return 1;
}

static int matchesTypeConstraint(Type *expected, Type *actual) {
    if (typeSetSpan(expected).nTokens)
        return typeSetFits(expected->context, expected, actual, 0);

    if (canonical(expected) == &any || (canonical(actual) == &any && !definedType(expected)) || expected == &unknown || actual == &unknown) return 1;
    if (expected == actual) return 1;
    if (expected->enumeration || actual->enumeration)
        return expected->enumeration && expected->enumeration == actual->enumeration &&
               ((!expected->nArguments && !actual->nArguments) ||
                (expected->nArguments == actual->nArguments && enumArgumentsFit(expected, actual)));
    if (definedType(expected)) return definedType(actual) && expected->declaration == actual->declaration;
    if (expected->interface) return interfaceFits(expected->context, expected, actual, 0);
    if (definedType(actual)) return 0;
    if (!strcmp(expected->name, "fixed-array") && expected->bufferFamily == 2)
        return !strcmp(actual->name, "fixed-array") &&
               matchesTypeConstraint(expected->element, actual->element);
    if (!strcmp(expected->name, "buffer") && !strcmp(actual->name, "buffer") &&
        expected->bufferFamily)
        return expected->bufferFamily == 1 ||
               (actual->element && matchesTypeConstraint(expected->element, actual->element));
    if (expected->record || actual->record) {
        if (expected->record != actual->record) return 0;
        if (!expected->nArguments) return 1;
        if (expected->nArguments != actual->nArguments) return 0;
        for (size_t i = 0; i < expected->nArguments; i++)
            if (!matchesTypeConstraint(expected->arguments[i], actual->arguments[i])) return 0;
        return 1;
    }
    if (!strcmp(expected->name, "dynamic-array") && !strcmp(actual->name, "dynamic-array") &&
        !expected->mixed && actual->mixed) return 0;
    if (expected->mapKey || actual->mapKey)
        return expected->mapKey && actual->mapKey && identicalTypes(expected->mapKey, actual->mapKey) &&
               identicalTypes(expected->element, actual->element);
    if (expected->element || actual->element)
        return expected->element && actual->element && !strcmp(expected->name, actual->name) &&
               expected->count == actual->count && expected->bufferSize == actual->bufferSize &&
               matchesTypeConstraint(expected->element, actual->element);
    return identicalTypes(expected, actual);
}
// Preserve numeric spelling before canonicalization erases integer storage widths.
static int integerShape(Type *t, int *isUnsigned) {
    if (definedType(t) || (canonical(t) != &integer && canonical(t) != &signedByte)) return 0;
    const char *name = t->spelling ? t->spelling : t->name;
    *isUnsigned = name[0] == 'u' || !strcmp(name, "byte");
    if (!strcmp(name, "i8") || !strcmp(name, "u8") || !strcmp(name, "byte")) return 8;
    if (!strcmp(name, "i16") || !strcmp(name, "u16")) return 16;
    if (!strcmp(name, "i32") || !strcmp(name, "u32") || !strcmp(name, "rune")) return 32;
    return 64;
}
static int integerFits(Type *expected, Type *actual) {
    int eu, au;
    int ew = integerShape(expected, &eu), aw = integerShape(actual, &au);
    if (!ew || !aw) return 0;
    return eu == au ? ew >= aw : (!eu && au && ew > aw);
}
static int compatibleTypes(Type *a, Type *b) {
    if (a == b) return 1;
    if (typeSetSpan(a).nTokens) return typeSetFits(a->context, a, b, 0);
    if (typeSetSpan(b).nTokens) return typeSetFits(b->context, a, b, 0);

    int au, bu;
    if (integerShape(a, &au) && integerShape(b, &bu)) return integerFits(a, b);
    a = canonical(a);
    // A boxed value does not establish the array shape or element type.
    // Require explicit narrowing before assigning it to an array destination.
    // Abstract generic parameters are checked when specialized, not boxed values.
    if ((!strcmp(a->name, "dynamic-array") || a->mapKey) && b == &any)
        return 0;
    b = canonical(b);
    if (a == &nullValue || b == &nullValue) {
        Type *other = a == &nullValue ? b : a;
        return other == &nullValue || other == &unknown || other == &any ||
               !strcmp(other->name, "pointer");
    }
    if (a == &unknown || b == &unknown || a == &any || (b == &any && !definedType(a)))
        return 1;
    if (a == b)
        return 1;
    if (definedType(a)) return definedType(b) && a->declaration == b->declaration;
    if (a->interface) return interfaceFits(a->context, a, b, 0);
    if (definedType(b)) return 0;
    if (!strcmp(a->name, "tuple") || !strcmp(b->name, "tuple")) {
        if (strcmp(a->name, b->name) || a->nArguments != b->nArguments)
            return 0;
        for (size_t i = 0; i < a->nArguments; i++)
            if (!compatibleTypes(a->arguments[i], b->arguments[i]))
                return 0;
        return 1;
    }
    if ((!strcmp(a->name, "buffer") && a->bufferFamily) ||
        (!strcmp(b->name, "buffer") && b->bufferFamily == 1))
        return matchesTypeConstraint(a, b);
    if (!strcmp(a->name, "pointer") && !strcmp(b->name, "pointer") && a->element && b->element &&
        representation(a->element)->immutableString != representation(b->element)->immutableString) return 0;
    if (!strcmp(a->name, "fixed-array") && a->bufferFamily == 2)
        return !strcmp(b->name, "fixed-array") && matchesTypeConstraint(a->element, b->element);
    if (!strcmp(a->name, "fixed-array") && !strcmp(b->name, "fixed-array") && b->bufferFamily == 2)
        return 0;
    if (a->mapKey || b->mapKey)
        return a->mapKey && b->mapKey && identicalTypes(a->mapKey, b->mapKey) &&
               identicalTypes(a->element, b->element);
    if (a->element || b->element)
        return a->element && b->element && !strcmp(a->name, b->name) &&
               (compatibleTypes(a->element, b->element) &&
                (integerShape(a->element, &au) && integerShape(b->element, &bu)
                    ? integerFits(b->element, a->element) : 1)) &&
               (!strcmp(a->name, "buffer")
                    ? (a->bufferSize == 0 || (a->bufferSize == b->bufferSize &&
                       (a->bufferSize != 1 || a->count == b->count)))
                    : a->count == b->count);
    if (a->function || b->function)
        return identicalFunctions(a, b);
    if (a->enumeration || b->enumeration)
        return a->enumeration && a->enumeration == b->enumeration &&
               a->nArguments == b->nArguments && enumArgumentsFit(a, b);
    if (a->record || b->record) {
        if (a->record != b->record)
            return 0;
        if (a->nArguments) {
            if (a->nArguments != b->nArguments)
                return 0;
            for (size_t i = 0; i < a->nArguments; i++)
                if (!matchesTypeConstraint(a->arguments[i], b->arguments[i]))
                    return 0;
        }
        return 1;
    }
    return !strcmp(a->name, b->name) ||
           ((a == &integer || a == &signedByte) && (b == &integer || b == &signedByte));
}
static int voidPointer(Type *t) {
    t = canonical(t);
    return !strcmp(t->name, "pointer") && t->element && canonical(t->element) == &nothing;
}
static int compatible(Type *expected, Type *actual) {
    Type *actualBase = canonical(actual);
    if (!definedType(expected) && !definedType(actualBase) && voidPointer(expected) &&
        (!strcmp(actualBase->name, "pointer") ||
         (!strcmp(actualBase->name, "buffer") && actualBase->bufferSize == 0)))
        return 1;
    return compatibleTypes(expected, actual);
}
static int genericFamily(Type *t) {
    if (t == &any || t->interface) return 1;
    if (!strcmp(t->name, "buffer") && t->bufferFamily == 1) return 1;
    if (!t->record) return 0;
    if (t->record->nTypeParams && !t->nArguments) return 1;
    for (size_t i = 0; i < t->nArguments; i++)
        if (genericFamily(t->arguments[i])) return 1;
    return 0;
}
static Type *builtinType(const char *n) {
    if (!strcmp(n, "void")) return &nothing;
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
static Type *parameter(Context *, Type *, typeTokens);
static void check(Context *, Type *, Type *, token);
static void checkConstraint(Context *, Type *, typeTokens, Type *, token);
static Type *resolveDeclaredType(Context *, Type *);
static Type *enumMemberType(Context *, Type *, token);
static Type *enumType(Context *, struct stmt_enum *);
static void prepareEnum(struct stmt_enum *);
static Type *typeOf(Context *c, typeTokens span) {
    if (!span.nTokens)
        return &unknown;
    token t = span.tokens[0];
    if (t.type == FUNC) {
        struct stmt_func *signature = parseFunctionType(span);
        if (!signature) { error(c, t, "invalid function type"); return &unknown; }
        Type *result = allocate(c, sizeof(*result));
        result->name = "function"; result->function = signature; result->context = c;
        // Resolve now so malformed and unresolved nested signatures are diagnosed.
        result->functionParams = allocate(c, sizeof(Type *) * (signature->nParams + 1));
        result->functionResults = allocate(c, sizeof(Type *) * (signature->nResults + 1));
        for (size_t i = 0; i < signature->nParams; i++) result->functionParams[i] = typeOf(c, signature->paramTypeNames[i]);
        for (size_t i = 0; i < signature->nResults; i++) result->functionResults[i] = typeOf(c, signature->resultTypes[i]);
        return result;
    }
    if (t.type == TILDE || constraintTermEnd(span, 0) < span.nTokens) {
        Type *list = allocate(c, sizeof(*list));
        list->name = "type-list"; list->typeList = 1;
        list->constraint = span; list->context = c;
        return list;
    }
    if (span.nTokens > 2 && t.type == IDENTIFIER && span.tokens[1].type == LEFT_BRACKET &&
        span.tokens[2].type != RIGHT_BRACKET) {
        Symbol *symbol = lookup(c, t.lexeme);
        Type *named = symbol && symbol->isType ? symbol->type : NULL;
        if (named && named->declaration) named = resolveDeclaredType(c, named);
        if (named && named->enumeration && named->enumeration->nTypeParams) {
            error(c, span.tokens[1], "Enum type arguments require angle brackets <...>"); return &unknown;
        }
    }
    typeTokens mapKey, mapValue;
    if (mapTypeParts(span, &mapKey, &mapValue)) {
        Type *v = allocate(c, sizeof(*v)); v->name = "map";
        c->typeIndirection++;
        v->mapKey = typeOf(c, mapKey); v->element = typeOf(c, mapValue);
        c->typeIndirection--;
        if (v->mapKey == &nothing || v->element == &nothing)
            error(c, t, "void is not a map storage type");
        return v;
    }
    if (span.nTokens > 1 && span.tokens[span.nTokens - 1].type == STAR) {
        Type *pointer = allocate(c, sizeof(*pointer));
        pointer->name = "pointer";
        c->typeIndirection++;
        pointer->element = typeOf(c, (typeTokens){span.nTokens - 1, span.tokens});
        c->typeIndirection--;
        return pointer;
    }
    typeTokens arrayElement;
    if (arrayElementType(span, &arrayElement)) {
        Type *v = allocate(c, sizeof(*v));
        v->name = "dynamic-array";
        v->dynamicArray = 1;
        v->mixed = span.tokens[0].type == MIXED;
        v->element = typeOf(c, arrayElement);
        if (v->mixed && canonical(v->element) != &any && !v->element->interface)
            error(c, t, "mixed array element type must be an interface");
        if (v->element == &nothing) error(c, t, "void is not an array element storage type");
        if (!strcmp(v->element->name, "buffer") && v->element->bufferSize == 2)
            error(c, t, "array elements must have a fixed storage size");
        return v;
    }
    typeTokens unsizedElement;
    if (unsizedArrayTypeParts(span, &unsizedElement)) {
        Type *v = allocate(c, sizeof(*v));
        v->name = "fixed-array"; v->bufferFamily = 2;
        v->element = typeOf(c, unsizedElement);
        if (v->element == &nothing) error(c, t, "void is not an array element storage type");
        return v;
    }
    typeTokens sizedElement, sizedDimension;
    if (sizedArrayTypeParts(span, &sizedElement, &sizedDimension)) {
        Type *v = allocate(c, sizeof(*v));
        v->name = "fixed-array";
        v->element = typeOf(c, sizedElement);
        if (v->element == &nothing) error(c, t, "void is not an array element storage type");
        v->dimension = sizedDimension;
        v->bufferSize = bufferDimensionKind(sizedDimension);
        if (v->bufferSize < 0 && !(sizedDimension.nTokens == 1 && sizedDimension.tokens[0].type == LITERAL_INT &&
                                  sizedDimension.tokens[0].numValue.integer == 0))
            error(c, t, "array length must be nonnegative");
        if (sizedDimension.nTokens == 1 && sizedDimension.tokens[0].type == LITERAL_INT) {
            v->bufferSize = 1;
            v->count = (size_t)sizedDimension.tokens[0].numValue.integer;
        }
        return v;
    }
    if (t.type == IDENTIFIER && !strcmp(t.lexeme, "array")) {
        error(c, t, "array requires exactly one element type; use array<any> for any array");
        return &unknown;
    }
    if (t.type == MIXED) {
        error(c, t, "mixed requires an array type");
        return &unknown;
    }
    if (span.nTokens == 3 && t.type == INTERFACE && span.tokens[1].type == LEFT_CURLY_BRACE &&
        span.tokens[2].type == RIGHT_CURLY_BRACE) return &any;
    if (t.type == INTERFACE) {
        struct stmt_interface *contract=parseInterfaceType(span);
        if (!contract) { error(c,t,"invalid interface type"); return &unknown; }
        Type *result=allocate(c,sizeof(*result)); result->name="interface";
        result->interface=contract; result->context=c; return result;
    }
    if (isBufferType(span)) {
        if (span.nTokens == 1) {
            Type *family = allocate(c, sizeof(*family));
            family->name = "buffer";
            family->bufferFamily = 1;
            return family;
        }
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
        if (v->element == &nothing) error(c, t, "void is not a buffer element storage type");
        v->bufferSize = bufferDimensionKind(dimension);
        v->bufferFamily = span.tokens[span.nTokens - 1].type == GREATER_THAN ? 2 : 0;
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
        if (i < span.nTokens) {
            if (t.type == STAR) c->typeIndirection++;
            v->element = typeOf(c, (typeTokens){span.nTokens - i, span.tokens + i});
            if (t.type == STAR) c->typeIndirection--;
        } else
            v->element = &unknown;
        return v;
    }
    if (span.nTokens >= 3 && span.tokens[span.nTokens - 2].type == DOT) {
        Type *parent = typeOf(c, (typeTokens){span.nTokens - 2, span.tokens});
        if (parent->enumeration) return enumMemberType(c, parent, span.tokens[span.nTokens - 1]);
        error(c, t, "type has no enum variants"); return &unknown;
    }
    const char *n = t.lexeme ? t.lexeme : "any";
    Symbol *s = lookup(c, n);
    if (s) {
        indexReference(c,t,s);
        if (!s->isType) {
            error(c, t, "expected a type");
            return &unknown;
        }
        if (s->type->enumeration && s->type->enumeration->isBuiltin)
            span.tokens[0].lexeme = OCEAN_BUILTIN_RESULT_NAME;
        if (s->type->enumeration && span.nTokens == 3 && span.tokens[1].type == DOT)
            return enumMemberType(c, s->type, span.tokens[2]);
        if (s->type->declaration) {
            Type *resolved = resolveDeclaredType(c, s->type);
            if (resolved == &unknown) return resolved;
            if (span.nTokens == 1) return resolved;
            // Apply generic arguments through aliases to generic structs.
            s = allocate(c, sizeof(*s));
            s->isType = 1; s->type = resolved;
        }
        if (span.nTokens > 1 && (definedType(s->type) ||
            (s->type->record && s->type->nArguments))) {
            error(c, t, "this type does not accept type arguments"); return &unknown;
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
            if (instance->enumeration) instance->enumArguments = allocate(c, sizeof(typeTokens) * count);
            if (count != instance->record->nTypeParams)
                error(c, t, "type argument count mismatch");
            for (size_t i = 0; i < count; i++) {
                // Buffer dimension spans can retain their tokens for later checks.
                typeTokens retained = {arguments[i].nTokens,
                                       allocate(c, sizeof(token) * arguments[i].nTokens)};
                memcpy(retained.tokens, arguments[i].tokens, sizeof(token) * retained.nTokens);
                instance->arguments[i] = typeOf(c, retained);
                if (instance->enumeration) instance->enumArguments[i] = retained;
                free(arguments[i].tokens);
            }
            free(arguments);
            for (size_t i = 0; i < count && i < instance->record->nTypeParams; i++)
                checkConstraint(c, instance, instance->record->typeParamConstraints[i],
                                instance->arguments[i], t);
            return instance;
        }
        return s->type;
    }
    Type *builtin = builtinType(n);
    if (builtin) {
        if ((builtin == &integer || builtin == &floating) && strcmp(n, builtin->name)) {
            Type *named = allocate(c, sizeof(*named));
            *named = *builtin;
            named->spelling = n;
            named->semanticBase = builtin;
            return named;
        }
        return builtin;
    }
    error(c, t, "unresolved type");
    return &unknown;
}
static Type *resolveDeclaredType(Context *c, Type *t) {
    if (!t->declaration) return t;
    if (t->resolutionState == 2) return t->declaration->isAlias ? t->underlying : t;
    if (t->resolutionState == 3) return &unknown;
    if (t->resolutionState == 1) {
        if (!t->declaration->isAlias && c->typeIndirection > t->resolutionDepth) return t;
        error(c, t->declaration->name, "cyclic type declaration");
        t->resolutionState = 3;
        return &unknown;
    }
    struct stmt_type *declaration = t->declaration;
    Scope *previous = c->scope;
    c->scope = t->declarationScope;
    t->resolutionState = 1; t->resolutionDepth = c->typeIndirection;
    Type *base = typeOf(c, declaration->underlying);
    c->scope = previous;
    if (t->resolutionState == 3 || base == &unknown) { t->resolutionState = 3; return &unknown; }
    if (!declaration->isAlias && base == &nothing) {
        error(c, declaration->name, "void cannot be the underlying type of a distinct type");
        t->resolutionState = 3; return &unknown;
    }
    if (!strcmp(base->name, "buffer") && base->bufferSize == 2) {
        error(c, declaration->name, "runtime-sized buffers cannot be used in type declarations");
        t->resolutionState = 3; return &unknown;
    }
    if (declaration->isAlias) {
        t->underlying = base; t->resolutionState = 2;
        return base;
    }
    Scope *scope = t->declarationScope;
    *t = *base;
    t->declaration = declaration; t->underlying = base; t->declarationScope = scope;
    t->semanticBase = NULL; t->spelling = NULL; t->resolutionState = 2;
    return t;
}
static Type *expression(Context *, expr *);
static void statement(Context *, stmt *);
static void predeclare(Context *, stmt *);
static void block(Context *c, struct stmt_block *b, int nested) {
    Scope scope = {c->scope, NULL, b->open, b->close};
    if (nested)
        c->scope = &scope;
    for (size_t i = 0; i < b->nStmts; i++)
        predeclare(c, &b->stmts[i]);
    for (size_t i = 0; i < b->nStmts; i++)
        if (b->stmts[i].type == STMT_TYPE) {
            struct stmt_type *declaration = b->stmts[i].value;
            Symbol *symbol = lookup(c, declaration->name.lexeme);
            if (symbol) symbol->type = resolveDeclaredType(c, symbol->type);
        }
    for (size_t i = 0; i < b->nStmts; i++)
        statement(c, &b->stmts[i]);
    if (nested)
        c->scope = scope.parent;
}
// Render language type syntax, rather than internal names such as dynamic-array.
// Records print their arguments but not fields, so recursive structs terminate.
static void formatType(FILE *, Type *, unsigned);
static void formatTypeSpan(FILE *out, typeTokens span, Type *callable, unsigned depth) {
    for (size_t i = 0; i < span.nTokens; i++) {
        token t = span.tokens[i];
        if (i && (t.type == IDENTIFIER || t.type == ANY) &&
            (span.tokens[i - 1].type == IDENTIFIER || span.tokens[i - 1].type == ANY))
            fputc(' ', out);
        int substituted = 0;
        if (callable && callable->function && t.lexeme) {
            struct stmt_func *f = callable->function;
            for (size_t j = 0; j < f->nTypeParams && j < callable->nArguments; j++)
                if (!strcmp(t.lexeme, f->typeParamNames[j].lexeme)) {
                    formatType(out, callable->arguments[j], depth + 1);
                    substituted = 1;
                    break;
                }
            if (!substituted && callable->receiver)
                for (size_t j = 0; j < f->nReceiverTypeParams && j < callable->receiver->nArguments;
                     j++)
                    if (!strcmp(t.lexeme, f->receiverTypeParamNames[j].lexeme)) {
                        formatType(out, callable->receiver->arguments[j], depth + 1);
                        substituted = 1;
                        break;
                    }
        }
        if (!substituted)
            fputs(t.lexeme && !strcmp(t.lexeme, OCEAN_BUILTIN_RESULT_NAME) ? "Result" :
                  t.lexeme ? t.lexeme : getTokenTypeStr(t.type), out);
    }
}
static void formatType(FILE *out, Type *t, unsigned depth) {
    if (definedType(t)) { fputs(t->declaration->displayName ? t->declaration->displayName : t->declaration->name.lexeme, out); return; }
    if (!t || depth > 32) {
        fputs("?", out);
        return;
    }
    if (t->typeList) { formatTypeSpan(out, t->constraint, NULL, depth + 1); return; }
    if (t->spelling) {
        fputs(t->spelling, out);
        return;
    }
    if (t->function) {
        struct stmt_func *f = t->function;
        fputs("func(", out);
        for (size_t i = 0; i < f->nParams; i++) {
            if (i)
                fputs(", ", out);
            if (f->isVariadic && !f->isExtern && i + 1 == f->nParams)
                fputs("...", out);
            formatTypeSpan(out, f->paramTypeNames[i], t, depth);
        }
        if (f->isVariadic && f->isExtern)
            fputs(f->nParams ? ", ..." : "...", out);
        fputc(')', out);
        if (f->nResults) {
            fputs(f->nResults > 1 ? " (" : " ", out);
            for (size_t i = 0; i < f->nResults; i++) {
                if (i)
                    fputs(", ", out);
                formatTypeSpan(out, f->resultTypes[i], t, depth);
            }
            if (f->nResults > 1)
                fputc(')', out);
        }
    } else if (t->mapKey) {
        fputs("map[", out); formatType(out, t->mapKey, depth + 1);
        fputc(']', out); formatType(out, t->element, depth + 1);
    } else if (!strcmp(t->name, "pointer")) {
        fputc('*', out);
        formatType(out, t->element, depth + 1);
    } else if (!strcmp(t->name, "dynamic-array") || !strcmp(t->name, "array")) {
        if (t->mixed) fputs("mixed ", out);
        formatType(out, t->element, depth + 1);
        fputs("[]", out);
    } else if (!strcmp(t->name, "fixed-array") && t->bufferFamily == 2) {
        fputs("array<", out); formatType(out, t->element, depth + 1); fputc('>', out);
    } else if (!strcmp(t->name, "fixed-array")) {
        formatType(out, t->element, depth + 1);
        fputc('[', out);
        if (t->bufferSize == 1) fprintf(out, "%zu", t->count);
        else for (size_t i = 0; i < t->dimension.nTokens; i++) fputs(t->dimension.tokens[i].lexeme, out);
        fputc(']', out);
    } else if (!strcmp(t->name, "buffer")) {
        if (t->bufferFamily == 1) { fputs("buffer", out); return; }
        fputs("buffer<", out);
        formatType(out, t->element, depth + 1);
        fputc('>', out);
        if (t->bufferFamily == 2) return;
        fputc('[', out);
        if (t->bufferSize == 1)
            fprintf(out, "%zu", t->count);
        else if (t->dimension.nTokens)
            formatTypeSpan(out, t->dimension, NULL, depth);
        fputc(']', out);
    } else if (!strcmp(t->name, "tuple")) {
        fputc('(', out);
        for (size_t i = 0; i < t->nArguments; i++) {
            if (i)
                fputs(", ", out);
            formatType(out, t->arguments[i], depth + 1);
        }
        fputc(')', out);
    } else {
        fputs(t->name, out);
        if (t->nArguments) {
            fputc('<', out);
            for (size_t i = 0; i < t->nArguments; i++) {
                if (i)
                    fputs(", ", out);
                formatType(out, t->arguments[i], depth + 1);
            }
            fputc('>', out);
        }
    }
}
static void check(Context *c, Type *expected, Type *actual, token at) {
    if (actual->enumeration && actual->enumMember &&
        (actual->enumeration->forms[actual->enumMember - 1] == 1 || actual->enumeration->forms[actual->enumMember - 1] == 2)) {
        error(c, at, "enum payload variant requires construction"); return;
    }
    if (compatible(expected, actual))
        return;
    at = diagnosticLocation(c, at);
    diagnosticStart(at, "semantic", "error");
    fputs("type mismatch: expected ", stderr);
    formatType(stderr, expected, 0);
    fputs(", got ", stderr);
    formatType(stderr, actual, 0);
    int eu, au;
    if (integerShape(expected, &eu) && integerShape(actual, &au))
        fputs("; narrowing integer conversion requires an explicit -> cast", stderr);
    diagnosticEnd(programSource(c->program, at.filename), at);
    c->errors++;
}
static Type *parameter(Context *, Type *, typeTokens);
static Type *expectedExpression(Context *, Type *, expr *, token);
static Type *checkComposite(Context *c, Type *target, expr *e, token at) {
    struct expr_composite *v = e->underlying;
    if (target->enumeration) {
        error(c, at, "construct an enum through a qualified variant"); return &unknown;
    }
    if (v->location.filename)
        at = v->location;
    if (!strcmp(target->name, "fixed-array") && target->bufferFamily == 2) {
        error(c, at, "array<T> cannot instantiate an array; use T[n]");
        return target;
    }
    if (target == &unknown) {
        if (!v->nEntries) {
            error(c, at, "cannot infer an empty literal; specify an array, buffer, or struct type");
            return &unknown;
        }
        Type *array = allocate(c, sizeof(*array));
        array->name = "fixed-array";
        array->bufferSize = 1;
        array->count = v->nEntries;
        array->element = expectedExpression(c, &unknown, &v->values[0], at);
        if (array->element == &nullValue)
            error(c, at, "null array elements require an explicit pointer element type");
        v->inferredArray = 1;
        target = array;
    }
    if (target->mapKey) {
        for (size_t i = 0; i < v->nEntries; i++) {
            if (!v->keys[i].type) { error(c, at, "map literals require key: value entries"); continue; }
            check(c, target->mapKey, expectedExpression(c, target->mapKey, &v->keys[i], at), v->keys[i].location);
            check(c, target->element, expectedExpression(c, target->element, &v->values[i], at), v->values[i].location);
        }
        return target;
    }
    if (!strcmp(target->name, "buffer") || !strcmp(target->name, "dynamic-array") || !strcmp(target->name, "fixed-array")) {
        if ((!strcmp(target->name, "buffer") || !strcmp(target->name, "fixed-array")) && target->bufferSize == 1 &&
            v->nEntries > target->count)
            error(c, at, "too many elements in buffer initializer");
        Type *shared = NULL;
        for (size_t i = 0; i < v->nEntries; i++) {
            if (!strcmp(target->name, "dynamic-array") && !target->mixed &&
                (canonical(target->element) == &any || target->element->interface)) {
                Type *actual = expression(c, &v->values[i]);
                if (shared && (!matchesTypeConstraint(shared, actual) || !matchesTypeConstraint(actual, shared)))
                    error(c, v->values[i].location, "homogeneous array elements must have the same type");
                if (!shared) shared = actual;
            }
            if (v->keys[i].type)
                error(c, at, "array and buffer literals require positional elements");
            check(c, target->element, expectedExpression(c, target->element, &v->values[i], at),
                  v->values[i].location);
        }
        return target;
    }
    if (target->record) {
        if (target->record->nTypeParams && !target->nArguments) {
            error(c, at, "struct literals require concrete type arguments");
            return target;
        }
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
            if(v->keys[i].type==EXPR_TYPE_IDENTIFIER){Symbol *member=allocate(c,sizeof(*member));member->name=record->fieldNames[j];member->type=field;indexReference(c,((struct expr_identifier*)v->keys[i].underlying)->lexeme,member);}
            check(c, field, expectedExpression(c, field, &v->values[i], at), v->values[i].location);
        }
        for (size_t i = 0; i < record->nVarParams; i++)
            if (!initialized[i])
                error(c, record->fieldNames[i], "missing struct parameter initializer");
        return target;
    }
    error(c, at, "literal requires an array, buffer, or struct assignment destination");
    return &unknown;
}
// Only literal syntax (including a negated literal) participates in implicit
// integer-to-float conversion. Rewrite it so backend inference sees the float too.
static int coerceFloatLiteral(Type *expected, expr *e) {
    if (definedType(expected) || canonical(expected) != &floating) return 0;
    if (e->type == EXPR_TYPE_UNARY &&
        ((struct expr_unary *)e->underlying)->operation == UNARY_NEGATE) {
        expr *child = &((struct expr_unary *)e->underlying)->child;
        if (child->type == EXPR_TYPE_LITERAL) return coerceFloatLiteral(expected, child);
        return 0;
    }
    if (e->type != EXPR_TYPE_LITERAL) return 0;
    struct expr_literal *literal = e->underlying;
    if (literal->type != LITERAL_TYPE_INT) return 0;
    double *number = malloc(sizeof(*number));
    if (!number) exit(1);
    *number = literal->unsignedLiteral ? (double)*(unsigned long *)literal->value
                                      : (double)*(long *)literal->value;
    free(literal->value);
    literal->value = number;
    literal->type = LITERAL_TYPE_FLOAT;
    literal->unsignedLiteral = 0;
    return 1;
}
// Untyped literal syntax may use a destination integer type when its value fits.
static int integerLiteralFits(Type *expected, expr *e) {
    int isUnsigned, width = integerShape(expected, &isUnsigned);
    if (!width) return 0;
    int negate = 0;
    if (e->type == EXPR_TYPE_UNARY &&
        ((struct expr_unary *)e->underlying)->operation == UNARY_NEGATE) {
        negate = 1;
        e = &((struct expr_unary *)e->underlying)->child;
    }
    if (e->type != EXPR_TYPE_LITERAL) return 0;
    struct expr_literal *v = e->underlying;
    if (v->type != LITERAL_TYPE_INT) return 0;
    unsigned long magnitude;
    if (v->unsignedLiteral) magnitude = *(unsigned long *)v->value;
    else {
        long number = *(long *)v->value;
        if (number < 0) { negate = !negate; magnitude = 0UL - (unsigned long)number; }
        else magnitude = (unsigned long)number;
    }
    if (negate && magnitude) return !isUnsigned && magnitude <= (1UL << (width - 1));
    unsigned long maximum = isUnsigned ? (width == 64 ? ~0UL : (1UL << width) - 1)
                                       : (1UL << (width - 1)) - 1;
    return magnitude <= maximum;
}
static Type *contextualInteger(Context *c, Type *expected, Type *actual, expr *e, token at) {
    int isUnsigned;
    if (!integerShape(expected, &isUnsigned)) return actual;
    expr *literal = e;
    if (literal->type == EXPR_TYPE_UNARY &&
        ((struct expr_unary *)literal->underlying)->operation == UNARY_NEGATE)
        literal = &((struct expr_unary *)literal->underlying)->child;
    if (literal->type != EXPR_TYPE_LITERAL ||
        ((struct expr_literal *)literal->underlying)->type != LITERAL_TYPE_INT) return actual;
    if (integerLiteralFits(expected, e)) return expected;
    error(c, at, "integer literal is out of range; use an explicit -> cast to narrow");
    return actual;
}
static void typeIntegerLiteral(Type *expected, expr *e) {
    struct expr_typecast *cast = calloc(1, sizeof(*cast));
    cast->child = *e;
    cast->type.nTokens = 1;
    cast->type.tokens = calloc(1, sizeof(token));
    cast->type.tokens[0] = e->location;
    cast->type.tokens[0].type = IDENTIFIER;
    cast->type.tokens[0].lexeme = (char *)(expected->spelling ? expected->spelling : expected->name);
    e->type = EXPR_TYPE_TYPECAST;
    e->underlying = cast;
}
static Type *widenToTypeSet(Context *c, Type *expected, Type *actual, expr *value, unsigned depth) {
    if (depth > 32 || !typeSetSpan(expected).nTokens || typeSetSpan(actual).nTokens ||
        typeSetFits(c, expected, actual, 0))
        return actual;
    typeTokens list = typeSetSpan(expected);
    for (size_t start = 0; start < list.nTokens;) {
        size_t end = constraintTermEnd(list, start);
        Type *candidate = typeOf(c, constraintTerm(list, start, end));
        if (typeSetSpan(candidate).nTokens) {
            Type *widened = widenToTypeSet(c, candidate, actual, value, depth + 1);
            if (widened != actual) return widened;
        } else if (list.tokens[start].type != TILDE && !definedType(candidate) &&
                   !definedType(actual) &&
                   (integerFits(candidate, actual) ||
                    (canonical(candidate) == &floating && canonical(actual) == &floating &&
                     candidate->spelling == NULL && actual->spelling &&
                     !strcmp(actual->spelling, "f32")))) {
            typeIntegerLiteral(candidate, value);
            return candidate;
        }
        start = end + 1;
    }
    return actual;
}

static Type *expectedExpression(Context *c, Type *expected, expr *e, token at) {
    coerceFloatLiteral(expected, e);
    if (e->type == EXPR_TYPE_COMPOSITE && !((struct expr_composite *)e->underlying)->type.type)
        return checkComposite(c, expected, e, at);
    Type *actual = expression(c, e);
    actual = widenToTypeSet(c,expected,actual,e,0);
    return contextualInteger(c, expected, actual, e, at);
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
            check(c, element, expectedExpression(c, element, item, at), item->location);
    }
}
static void checkBufferDimension(Context *c, Type *t, token at) {
    if ((strcmp(t->name, "buffer") && strcmp(t->name, "fixed-array")) || t->bufferSize != 2)
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
        if (i < nExpected) coerceFloatLiteral(expected[i], &values[i]);
        Type *t = assignmentContext && i < nExpected
                      ? expectedExpression(c, expected[i], &values[i], at)
                      : expression(c, &values[i]);
        if (assignmentContext && count == 1 && nExpected == 2 && values[i].type == EXPR_TYPE_INDEX) {
            struct expr_index *index = values[i].underlying;
            Type *parent = expression(c, &index->parent);
            if (parent->mapKey || !strcmp(parent->name, "dynamic-array") || !strcmp(parent->name, "fixed-array")) {
                index->tryIndex = 1;
                Type **items = allocate(c, 2 * sizeof(*items));
                items[0] = t; items[1] = &boolean;
                return (TypeList){items, 2};
            }
        }
        if (!assignmentContext && i < nExpected)
            t = widenToTypeSet(c,expected[i],t,&values[i],0);
        if (!assignmentContext && i < nExpected)
            t = contextualInteger(c, expected[i], t, &values[i], values[i].location);
        if (t->enumeration && t->nArguments != t->enumeration->nTypeParams)
            error(c, at, "generic enum value requires explicit type arguments");
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
static struct stmt_func *findMethod(Context *, Type *, const char *);
static size_t constraintTermEnd(typeTokens, size_t);
static typeTokens constraintTerm(typeTokens, size_t, size_t);
// Build a small control-flow graph so warnings account for branches, loops,
// switch exits, and forward/backward gotos instead of inspecting the last stmt.
typedef struct FlowNode FlowNode;
typedef struct FlowEdge { FlowNode *to; struct FlowEdge *next; } FlowEdge;
struct FlowNode { FlowEdge *edges; int visited; };
typedef struct FlowLabel {
    token name;
    FlowNode *entry, *breakTo, *continueTo;
    struct FlowLabel *next;
} FlowLabel;
typedef struct FlowJump { token name; FlowNode *from; struct FlowJump *next; } FlowJump;
typedef struct Flow {
    Context *context;
    size_t count;
    FlowLabel *labels;
    FlowJump *jumps;
} Flow;
static FlowNode *flowNode(Flow *f) { f->count++; return allocate(f->context, sizeof(FlowNode)); }
static void flowEdge(Flow *f, FlowNode *from, FlowNode *to) {
    if (!to) return;
    FlowEdge *edge = allocate(f->context, sizeof(*edge));
    *edge = (FlowEdge){to, from->edges}; from->edges = edge;
}
static int flowBoolean(expr *e) {
    if (e && e->type == EXPR_TYPE_IDENTIFIER) {
        const char *name = ((struct expr_identifier *)e->underlying)->lexeme.lexeme;
        if (!strcmp(name, "true")) return 1;
        if (!strcmp(name, "false")) return 0;
    }
    return -1;
}
static FlowNode *flowStatement(Flow *, stmt *, FlowNode *, FlowNode *, FlowNode *, FlowNode *, FlowLabel *);
static FlowNode *flowBlock(Flow *f, struct stmt_block *b, FlowNode *next,
                           FlowNode *breakTo, FlowNode *continueTo, FlowNode *fallTo) {
    for (size_t i = b->nStmts; i > 0; i--)
        next = flowStatement(f, &b->stmts[i - 1], next, breakTo, continueTo, fallTo, NULL);
    return next;
}
static FlowNode *flowStatement(Flow *f, stmt *s, FlowNode *next, FlowNode *breakTo,
                               FlowNode *continueTo, FlowNode *fallTo, FlowLabel *label) {
    if (!s) return next;
    FlowNode *node = flowNode(f);
    switch (s->type) {
    case STMT_RETURN: case STMT_PANIC: case STMT_RETURN_PANIC: return node;
    case STMT_BLOCK:
        return flowBlock(f, s->value, next, breakTo, continueTo, fallTo);
    case STMT_LABEL: {
        struct stmt_label *slabel = s->value;
        FlowLabel *target = allocate(f->context, sizeof(*target));
        *target = (FlowLabel){.name = slabel->name, .entry = node, .next = f->labels};
        f->labels = target;
        flowEdge(f, node, flowStatement(f, slabel->body, next, breakTo, continueTo, fallTo, target));
        return node;
    }
    case STMT_GOTO: case STMT_BREAK: case STMT_CONTINUE: {
        token name = ((struct stmt_branch *)s->value)->label;
        if (name.type != EOF_TOKEN) {
            FlowJump *jump = allocate(f->context, sizeof(*jump));
            // Branch destinations are resolved after all labels have been built.
            *jump = (FlowJump){name, node, f->jumps}; f->jumps = jump;
            if (s->type != STMT_GOTO)
                jump->name.type = s->type == STMT_BREAK ? BREAK : CONTINUE;
        } else flowEdge(f, node, s->type == STMT_BREAK ? breakTo : continueTo);
        return node;
    }
    case STMT_FALLTHROUGH: flowEdge(f, node, fallTo); return node;
    case STMT_IF: {
        struct stmt_if *branch = s->value;
        int condition = flowBoolean(branch->condition);
        if (condition != 0) flowEdge(f, node, flowStatement(f, branch->ifTrue, next, breakTo, continueTo, fallTo, NULL));
        if (condition != 1) flowEdge(f, node, flowStatement(f, branch->ifFalse, next, breakTo, continueTo, fallTo, NULL));
        return flowStatement(f, branch->preStmt, node, breakTo, continueTo, fallTo, NULL);
    }
    case STMT_FOR: {
        struct stmt_for *loop = s->value;
        FlowNode *update = flowNode(f);
        flowEdge(f, update, node);
        if (label) { label->breakTo = next; label->continueTo = update; }
        FlowNode *body = flowStatement(f, loop->body, update, next, update, NULL, NULL);
        int condition = loop->condition ? flowBoolean(loop->condition) : 1;
        if (loop->range) condition = -1;
        if (condition != 0) flowEdge(f, node, body);
        if (condition != 1) flowEdge(f, node, next);
        return flowStatement(f, loop->initializer, loop->deferCondition ? body : node,
                             breakTo, continueTo, fallTo, NULL);
    }
    case STMT_SWITCH: {
        struct stmt_switch *sw = s->value;
        if (label) label->breakTo = next;
        FlowNode **cases = allocate(f->context, (sw->nCases + 1) * sizeof(*cases));
        FlowNode *following = next, *fallback = next;
        for (size_t i = sw->nCases; i > 0; i--) {
            struct switch_case *clause = &sw->cases[i - 1];
            cases[i - 1] = following = flowBlock(f, &clause->body, next, next, continueTo, following);
            if (!clause->nTypes && !clause->nValues) fallback = following;
        }
        int exhaustive = 0;
        for (size_t i = 0; i < sw->nCases && !exhaustive; i++) {
            struct switch_case *clause = &sw->cases[i];
            if (!clause->nTypes && !clause->nValues) continue; // Default is tested last.
            flowEdge(f, node, cases[i]);
            if (sw->isTypeSwitch)
                for (size_t j = 0; j < clause->nTypes; j++) {
                    typeTokens span = clause->types[j];
                    for (size_t start = 0; start < span.nTokens;) {
                        size_t end = constraintTermEnd(span, start);
                        typeTokens term = constraintTerm(span, start, end);
                        if (term.nTokens == 1 && term.tokens[0].type == ANY) exhaustive = 1;
                        start = end + 1;
                    }
                }
        }
        if (!exhaustive) flowEdge(f, node, fallback);
        return flowStatement(f, &sw->initializer, node, breakTo, continueTo, fallTo, NULL);
    }
    default: flowEdge(f, node, next); return node;
    }
}
static int functionFallsThrough(Context *c, struct stmt_func *function) {
    Flow graph = {.context = c};
    FlowNode *end = flowNode(&graph);
    FlowNode *entry = flowBlock(&graph, &function->body, end, NULL, NULL, NULL);
    for (FlowJump *jump = graph.jumps; jump; jump = jump->next)
        for (FlowLabel *label = graph.labels; label; label = label->next)
            if (!strcmp(jump->name.lexeme, label->name.lexeme)) {
                flowEdge(&graph, jump->from, jump->name.type == BREAK ? label->breakTo
                                          : jump->name.type == CONTINUE ? label->continueTo : label->entry);
                break;
            }
    FlowNode **pending = allocate(c, graph.count * sizeof(*pending));
    size_t count = 0;
    entry->visited = 1; pending[count++] = entry;
    while (count) {
        FlowNode *node = pending[--count];
        if (node == end) return 1;
        for (FlowEdge *edge = node->edges; edge; edge = edge->next)
            if (!edge->to->visited) { edge->to->visited = 1; pending[count++] = edge->to; }
    }
    return 0;
}

static size_t sourcePackage(Context *c, const char *filename);
static void function(Context *c, struct stmt_func *f) {
    Scope scope = {c->scope, NULL, f->body.open, f->body.close};
    c->scope = &scope;
    struct stmt_func *previous = c->function;
    c->function = f;
    if (f->receiverType.nTokens) {
        typeTokens base = f->receiverType;
        if (base.tokens[0].type == STAR) { base.tokens++; base.nTokens--; }
        else if (base.tokens[base.nTokens - 1].type == STAR) base.nTokens--;
        Symbol *owner = lookup(c, base.tokens[0].lexeme);
        struct stmt_struct *record = owner && owner->isType ? owner->type->record : NULL;
        Type *receiverOwner = owner && owner->isType ? owner->type : &unknown;
        token definition = definedType(receiverOwner) ? receiverOwner->declaration->name
                         : record ? record->name : (token){0};
        if (definition.filename && sourcePackage(c, definition.filename) != sourcePackage(c, f->name.filename))
            error(c, base.tokens[0], "cannot define a method on a type from another package");
        if (!record && !definedType(receiverOwner)) error(c, base.tokens[0], "method receiver must name a struct, enum, or defined type");
        else if ((definedType(receiverOwner) ? 0 : record->nTypeParams) != f->nReceiverTypeParams)
            error(c, base.tokens[0], "receiver type parameter count mismatch");
        if (record || definedType(receiverOwner)) findMethod(c, receiverOwner, f->name.lexeme);
        if (receiverOwner->enumeration) {
            struct stmt_enum *enumeration = receiverOwner->enumeration;
            for (size_t i = 0; i < enumeration->nMembers; i++) {
                if (!strcmp(enumeration->names[i].lexeme, f->name.lexeme))
                    error(c, f->name, "method name conflicts with an enum variant");
                for (size_t j = 0; j < enumeration->payloads[i].nFields; j++)
                    if (!strcmp(enumeration->payloads[i].fieldNames[j].lexeme, f->name.lexeme))
                        error(c, f->name, "method name conflicts with an enum payload field");
            }
        }
        if (record) {
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
            if (record && i < record->nTypeParams)
                *bound = *typeOf(c, record->typeParamConstraints[i]);
            else *bound = any;
            if (!strcmp(bound->name,"any") && !bound->interface) { bound->name=name.lexeme; bound->semanticBase=NULL; }
            bound->abstractParameter = 1; bound->context = c;
            declare(c, name, bound, 1);
        }
    }
    for (size_t i = 0; i < f->nTypeParams; i++) {
        Type *resolved = typeOf(c, f->typeParamConstraints[i]);
        Type *bound = allocate(c, sizeof(*bound));
        *bound = *resolved;
        bound->abstractParameter = 1;
        bound->context = c;
        if (resolved==&integer || resolved==&signedByte || resolved==&floating || resolved==&boolean || resolved==&any)
            bound->semanticBase=resolved;
        declare(c, f->typeParamNames[i], bound, 1);
    }
    for (size_t i = 0; i < f->nParams; i++) {
        Type *t = typeOf(c, f->paramTypeNames[i]);
        if (!f->isVariadic && !strcmp(t->name, "buffer") && t->element) {
            Type *parameter = allocate(c, sizeof(*parameter)); *parameter = *t;
            parameter->bufferSize = 0; parameter->count = 0; parameter->dimension = (typeTokens){0};
            t = parameter;
        }
        if (f->isVariadic && !f->isExtern && i + 1 == f->nParams) {
            Type *list = allocate(c, sizeof(*list));
            list->name = "dynamic-array";
            list->element = t;
            list->mixed = canonical(t) == &any || t->interface;
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
                typeOf(c, f->receiverType),
                0);
    size_t errorsBeforeBody = c->errors;
    block(c, &f->body, 0);
    if (!f->isExtern && f->nResults && c->errors == errorsBeforeBody &&
        !(f->nResults == 1 && typeOf(c, f->resultTypes[0]) == &nothing) && functionFallsThrough(c, f)) {
        token at = diagnosticLocation(c, f->name);
        diagnostic(programSource(c->program, at.filename), at, "semantic", "warning",
                   "value-returning function can reach its end without returning; returning a zero value");
    }
    c->function = previous;
    c->scope = scope.parent;
}
static Type *parameter(Context *c, Type *fn, typeTokens span) {
    if (fn->functionParams && fn->function) {
        for (size_t i = 0; i < fn->function->nParams; i++)
            if (span.tokens == fn->function->paramTypeNames[i].tokens && span.nTokens == fn->function->paramTypeNames[i].nTokens)
                return fn->functionParams[i];
        for (size_t i = 0; i < fn->function->nResults; i++)
            if (span.tokens == fn->function->resultTypes[i].tokens && span.nTokens == fn->function->resultTypes[i].nTokens)
                return fn->functionResults[i];
    }
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
// Find union separators without splitting nested type applications.
static size_t constraintTermEnd(typeTokens span, size_t start) {
    size_t nesting = 0;
    for (size_t i = start; i < span.nTokens; i++) {
        enum tokenType k = span.tokens[i].type;
        if (!nesting && k == BITWISE_OR) return i;
        if (k == LESS_THAN || k == LEFT_BRACKET || k == LEFT_PAREN || k == LEFT_CURLY_BRACE) nesting++;
        if ((k == GREATER_THAN || k == RIGHT_BRACKET || k == RIGHT_PAREN || k == RIGHT_CURLY_BRACE) && nesting) nesting--;
        if (k == BITSHIFT_RIGHT) nesting = nesting > 1 ? nesting - 2 : 0;
    }
    return span.nTokens;
}
static typeTokens constraintTerm(typeTokens span, size_t start, size_t end) {
    typeTokens term = {end - start, span.tokens + start};
    if (term.nTokens && term.tokens[0].type == TILDE) { term.tokens++; term.nTokens--; }
    return term;
}
static typeTokens typeSetSpan(Type *type) {
    if (type->typeList) return type->constraint;
    if (type->interface && type->interface->nConstraints) return type->interface->constraints[0];
    return (typeTokens){0};
}
static Type *underlyingIdentity(Type *type) {
    for (unsigned i = 0; type->underlying && i < 64; i++)
        type = type->underlying;
    return type;
}
// Exact terms retain width and defined-type identity. ~ removes only definitions,
// never integer widths. A set accepts another set only if it covers every term.
static int typeSetFits(Context *c, Type *expected, Type *actual, unsigned depth) {
    if (depth > 32 || !c) return 0;
    if (expected == actual) return 1;
    typeTokens source = typeSetSpan(actual), target = typeSetSpan(expected);
    if (source.nTokens) {
        for (size_t start = 0; start < source.nTokens;) {
            size_t end = constraintTermEnd(source, start);
            Type *term = typeOf(c, constraintTerm(source, start, end));
            if (source.tokens[start].type == TILDE) {
                int covered = 0;
                for (size_t j = 0; j < target.nTokens;) {
                    size_t stop = constraintTermEnd(target, j);
                    if (target.tokens[j].type == TILDE &&
                        identicalTypes(
                            underlyingIdentity(typeOf(c, constraintTerm(target, j, stop))),
                            underlyingIdentity(term)))
                        covered = 1;
                    j = stop + 1;
                }
                if (!covered) return 0;
            } else if (!typeSetFits(c, expected, term, depth + 1))
                return 0;
            start = end + 1;
        }
        return 1;
    }
    if (!target.nTokens) return matchesTypeConstraint(expected, actual);
    for (size_t start = 0; start < target.nTokens;) {
        size_t end = constraintTermEnd(target, start);
        Type *bound = typeOf(c, constraintTerm(target, start, end));
        if (typeSetSpan(bound).nTokens) {
            if (typeSetFits(c, bound, actual, depth + 1)) return 1;
        } else if (target.tokens[start].type == TILDE
                       ? identicalTypes(underlyingIdentity(bound), underlyingIdentity(actual))
                       : matchesTypeConstraint(bound, actual))
            return 1;
        start = end + 1;
    }
    return 0;
}

static int acceptsConstraint(Context *c, Type *owner, typeTokens span, Type *actual, unsigned depth) {
    if (depth > 32) return 0;
    if (typeSetSpan(actual).nTokens) {
        Type *expected=parameter(c,owner,span);
        return typeSetFits(c,expected,actual,depth+1);
    }
    for (size_t start = 0; start < span.nTokens;) {
        size_t end = constraintTermEnd(span, start);
        int underlying = span.tokens[start].type == TILDE;
        Type *candidate = underlying ? underlyingIdentity(actual) : actual;
        Type *bound = parameter(c, owner, constraintTerm(span, start, end));
        if (underlying ? identicalTypes(underlyingIdentity(bound), candidate) : matchesTypeConstraint(bound, candidate)) return 1;
        start = end + 1;
    }
    return 0;
}
static void checkConstraint(Context *c, Type *owner, typeTokens span, Type *actual, token at) {
    if (!acceptsConstraint(c, owner, span, actual, 0))
        error(c, at, "type argument does not satisfy its constraint");
}
static struct stmt_func *findMethod(Context *c, Type *ownerType, const char *name) {
    struct stmt_func *found = NULL;
    for (size_t i = 0; i < c->program->nStatements; i++) {
        stmt *s = &c->program->statements[i];
        if (s->type != STMT_FUNC) continue;
        struct stmt_func *f = s->value;
        if (!f->receiverType.nTokens || strcmp(f->name.lexeme, name)) continue;
        token owner = f->receiverType.tokens[f->receiverType.tokens[0].type == STAR ? 1 : 0];
        Symbol *symbol = lookup(c, owner.lexeme);
        if (!symbol || !symbol->isType) continue;
        Type *candidate = symbol->type;
        if (definedType(ownerType) || definedType(candidate)) {
            if (!definedType(ownerType) || !definedType(candidate) || ownerType->declaration != candidate->declaration) continue;
        } else if (!ownerType->record || ownerType->record != candidate->record) continue;
        if (found) error(c, f->name, "duplicate method declaration");
        found = f;
    }
    return found;
}
static int identicalTypes(Type *a, Type *b) {
    if (a->interface && !a->interface->nMethods && !a->interface->nConstraints && !a->interface->nTypeParams) a = &any;
    if (b->interface && !b->interface->nMethods && !b->interface->nConstraints && !b->interface->nTypeParams) b = &any;
    if (a == b) return 1;
    if (definedType(a) || definedType(b))
        return definedType(a) && definedType(b) && a->declaration == b->declaration;
    if (a->function || b->function) return identicalFunctions(a, b);
    if (a->interface || b->interface) return a->interface && a->interface == b->interface;
    if (a->record || b->record) {
        if (a->record != b->record || a->nArguments != b->nArguments) return 0;
        for (size_t i = 0; i < a->nArguments; i++)
            if (!identicalTypes(a->arguments[i], b->arguments[i])) return 0;
        return 1;
    }
    if (a->mapKey || b->mapKey)
        return a->mapKey && b->mapKey && identicalTypes(a->mapKey, b->mapKey) &&
               identicalTypes(a->element, b->element);
    if (a->element || b->element)
        return a->element && b->element && !strcmp(a->name, b->name) &&
               a->count == b->count && a->bufferSize == b->bufferSize &&
               a->bufferFamily == b->bufferFamily && a->mixed == b->mixed &&
               identicalTypes(a->element, b->element);
    const char *left = a->spelling ? a->spelling : a->name;
    const char *right = b->spelling ? b->spelling : b->name;
    if (!strcmp(left, "u64")) left = "uint";
    if (!strcmp(right, "u64")) right = "uint";
    if (!strcmp(left, "i64")) left = "int";
    if (!strcmp(right, "i64")) right = "int";
    if (!strcmp(left, "f64")) left = "float";
    if (!strcmp(right, "f64")) right = "float";
    return !strcmp(left, right);
}
static struct stmt_func *interfaceMethod(Context *c, Type *t, const char *name, unsigned depth) {
    if (!t || !t->interface || depth>32) return NULL;
    struct stmt_interface *contract=t->interface;
    for (size_t i=0;i<contract->nMethods;i++)
        if (!strcmp(contract->methods[i].name.lexeme,name)) return &contract->methods[i];
    for (size_t i=0;i<contract->nConstraints;i++) {
        typeTokens span=contract->constraints[i];
        // Only embedded interfaces contribute methods; union terms do not.
        int unionTerm=0;
        for (size_t j=0;j<span.nTokens;j++) if (span.tokens[j].type==BITWISE_OR) unionTerm=1;
        if (unionTerm || !span.nTokens || span.tokens[0].type==TILDE) continue;
        struct stmt_func *method=interfaceMethod(c,typeOf(c,span),name,depth+1);
        if (method) return method;
    }
    return NULL;
}
static int interfaceFits(Context *c, Type *expected, Type *actual, unsigned depth) {
    if (depth > 32 || !c) return 0;
    if (actual == &unknown || canonical(actual) == &any || actual->interface) return 1;
    struct stmt_interface *contract = expected->interface;
    if (contract->nTypeParams) return 0;
    if (contract->nConstraints && !typeSetFits(c, expected, actual, depth + 1)) return 0;
    Type *owner = actual;
    int pointer = !definedType(owner) && !strcmp(owner->name, "pointer");
    if (pointer) owner = owner->element;
    for (size_t i = 0; i < contract->nMethods; i++) {
        if (!owner || (!owner->record && !definedType(owner))) return 0;
        struct stmt_func *required = &contract->methods[i];
        struct stmt_func *provided = findMethod(c, owner, required->name.lexeme);
        if (!provided || provided->nTypeParams != required->nTypeParams ||
            provided->nParams != required->nParams || provided->nResults != required->nResults ||
            provided->isVariadic != required->isVariadic) return 0;
        typeTokens receiver = provided->receiverType;
        if (!pointer && (receiver.tokens[0].type == STAR || receiver.tokens[receiver.nTokens - 1].type == STAR)) return 0;
        Type callable = {.function = provided, .receiver = owner};
        Type requirement = {.function = required};
        size_t n=required->nTypeParams;
        Type **arguments=allocate(c,(n+1)*sizeof(Type*));
        for (size_t j=0;j<n;j++) {
            Type *parameterType=allocate(c,sizeof(Type));
            char *name=allocate(c,48); snprintf(name,48,"<interface-parameter-%zu>",j);
            parameterType->name=name; arguments[j]=parameterType;
        }
        callable.nArguments=requirement.nArguments=n;
        callable.arguments=requirement.arguments=arguments;
        for (size_t j=0;j<n;j++) {
            Type *a=parameter(c,&requirement,required->typeParamConstraints[j]);
            Type *b=parameter(c,&callable,provided->typeParamConstraints[j]);
            if (!(typeSetSpan(a).nTokens || typeSetSpan(b).nTokens
                    ? typeSetFits(c,a,b,0) && typeSetFits(c,b,a,0) : identicalTypes(a,b))) return 0;
        }
        for (size_t j = 0; j < required->nParams + required->nResults; j++) {
            Type *a = parameter(c,&requirement,j < required->nParams ? required->paramTypeNames[j]
                                                    : required->resultTypes[j - required->nParams]);
            Type *b = parameter(c, &callable, j < provided->nParams ? provided->paramTypeNames[j]
                                                    : provided->resultTypes[j - provided->nParams]);
            if (typeSetSpan(a).nTokens || typeSetSpan(b).nTokens) {
                if (!typeSetFits(c,a,b,0) || !typeSetFits(c,b,a,0)) return 0;
            } else if (!identicalTypes(a, b)) return 0;
        }
    }
    return 1;
}
static token typeToken(enum tokenType kind, char *lexeme) {
    return (token){.type = kind, .lexeme = lexeme};
}
static int typeExpression(Context *c, expr *e, typeTokens *out) {
    *out = (typeTokens){0};
    if (e->type == EXPR_TYPE_TYPE_NAME) {
        typeTokens span = ((struct expr_zero *)e->underlying)->type;
        out->tokens = malloc(sizeof(token) * span.nTokens);
        if (!out->tokens) exit(1);
        memcpy(out->tokens, span.tokens, sizeof(token) * span.nTokens);
        out->nTokens = span.nTokens;
        return 1;
    }
    if (e->type == EXPR_TYPE_IDENTIFIER) {
        token name = ((struct expr_identifier *)e->underlying)->lexeme;
        Symbol *symbol = lookup(c, name.lexeme);
        if (symbol ? !symbol->isType : (!builtinStorageTypeName(name.lexeme) && strcmp(name.lexeme, "void")))
            return 0;
        out->tokens = malloc(sizeof(token));
        if (!out->tokens)
            exit(1);
        out->tokens[0] = name;
        out->nTokens = 1;
        return 1;
    }
    if (e->type == EXPR_TYPE_UNARY &&
        ((struct expr_unary *)e->underlying)->operation == UNARY_DEREFERENCE) {
        typeTokens base;
        if (!typeExpression(c, &((struct expr_unary *)e->underlying)->child, &base)) return 0;
        token *tokens = realloc(base.tokens, sizeof(token) * (base.nTokens + 1));
        if (!tokens) exit(1);
        memmove(tokens + 1, tokens, sizeof(token) * base.nTokens);
        tokens[0] = typeToken(STAR, "*");
        *out = (typeTokens){base.nTokens + 1, tokens};
        return 1;
    }
    if (e->type == EXPR_TYPE_INSTANTIATION) {
        struct expr_instantiation *v = e->underlying;
        if (v->parent.type != EXPR_TYPE_IDENTIFIER) return 0;
        token name = ((struct expr_identifier *)v->parent.underlying)->lexeme;
        Symbol *symbol = lookup(c, name.lexeme);
        if (strcmp(name.lexeme, "buffer") && strcmp(name.lexeme, "array") && !(symbol && symbol->isType && symbol->type->record))
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
        char sizedBuffer = v->index.type == EXPR_TYPE_LITERAL &&
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
    while (!t->record && !definedType(t) && !strcmp(t->name, "pointer") && t->element)
        t = t->element;
    return t;
}
/* Immutability protects bindings/value fields, while indexed reference contents stay writable. */
static token immutableTarget(Context *c, expr *e) {
    token none = {.type = EOF_TOKEN};
    if (e->type == EXPR_TYPE_INDEX) {
        struct expr_index *v = e->underlying;
        if (representation(expression(c, &v->parent))->immutableString) return e->location;
    }
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
        if ((!strcmp(parent->name, "dynamic-array") || !strcmp(parent->name, "fixed-array")) &&
            (!strcmp(field.lexeme, "size") || !strcmp(field.lexeme, "length") || !strcmp(field.lexeme, "capacity")))
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
    if (target->type == EXPR_TYPE_INDEX && representation(expression(c, &((struct expr_index *)target->underlying)->parent))->immutableString) {
        error(c, at, "strings are immutable; reassign the string instead"); return;
    }
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
static struct stmt_func *fileExtern(Context *c, token reference) {
    for (size_t i = 0; i < c->program->nStatements; i++) {
        stmt *s = &c->program->statements[i];
        if (s->type != STMT_FUNC) continue;
        struct stmt_func *f = s->value;
        if (!f->isExtern || !f->name.filename || !reference.filename ||
            strcmp(f->name.filename, reference.filename)) continue;
        if (!strcmp(f->name.lexeme, reference.lexeme) ||
            (f->externalName && !strcmp(f->externalName, reference.lexeme))) return f;
    }
    return NULL;
}
static size_t sourcePackage(Context *c, const char *filename) {
    for (size_t i = 0; filename && i < c->program->nSources; i++)
        if (!strcmp(filename, c->program->sources[i].filename))
            return c->program->sources[i].packageId;
    return 0;
}
#include "semantic_enum.inc"

// Resolve operator capability universally, including unused generic bodies.
static int numericType(Type *t) {
    Type *base = representation(t);
    return base == &integer || base == &signedByte || base == &floating;
}
static enum binaryExprType baseOperation(enum binaryExprType operation) {
    static const enum binaryExprType operations[] = {
        EXPR_ADD, EXPR_SUBTRACT,      EXPR_MULTIPLY,       EXPR_DIVIDE,      EXPR_MODULO,
        EXPR_XOR, EXPR_BITSHIFT_LEFT, EXPR_BITSHIFT_RIGHT, EXPR_BITWISE_AND, EXPR_BITWISE_OR};
    return operation >= EXPR_ADD_EQUALS && operation <= EXPR_BITWISE_OR_EQUALS
               ? operations[operation - EXPR_ADD_EQUALS]
               : operation;
}
// Equality is a universal capability, including unconstrained type parameters.
// Concrete aggregates retain nominal identity; collection extents are values.
static int equalityCompatible(Type *a, Type *b) {
    if (a == &nullValue || b == &nullValue) return compatibleTypes(a, b) || compatibleTypes(b, a);
    if (voidPointer(a)) return compatible(a, b);
    if (voidPointer(b)) return compatible(b, a);
    if (a == &unknown || b == &unknown || a->abstractParameter || b->abstractParameter ||
        canonical(a) == &any || canonical(b) == &any || a->interface || b->interface ||
        typeSetSpan(a).nTokens || typeSetSpan(b).nTokens) return 1;
    if (definedType(a) || definedType(b)) return compatibleTypes(a, b);
    int leftCollection = !strcmp(a->name, "dynamic-array") || !strcmp(a->name, "fixed-array");
    int rightCollection = !strcmp(b->name, "dynamic-array") || !strcmp(b->name, "fixed-array");
    if (leftCollection && rightCollection) {
        if ((!strcmp(a->element->name, "dynamic-array") || !strcmp(a->element->name, "fixed-array")) &&
            (!strcmp(b->element->name, "dynamic-array") || !strcmp(b->element->name, "fixed-array")))
            return equalityCompatible(a->element, b->element);
        return identicalTypes(a->element, b->element);
    }
    return compatibleTypes(a, b) || compatibleTypes(b, a);
}

static Type *operatorResult(Context *c, enum binaryExprType op, Type *a, Type *b, unsigned depth) {
    if (depth > 32) return NULL;
    if (op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS) return &boolean;
    typeTokens list = typeSetSpan(a);
    if (list.nTokens) {
        Type *result = NULL;
        for (size_t start = 0; start < list.nTokens;) {
            size_t end = constraintTermEnd(list, start);
            Type *term = typeOf(c, constraintTerm(list, start, end));
            Type *part =
                operatorResult(c, op, term, a == b && a->abstractParameter ? term : b, depth + 1);
            if (!part) return NULL;
            if (!result)
                result = part;
            else if (!identicalTypes(result, part))
                result = a;
            start = end + 1;
        }
        // Arithmetic on one abstract T preserves its concrete specialization.
        return a == b && op != EXPR_LOGICAL_AND && op != EXPR_LOGICAL_OR &&
                       !(op >= EXPR_EQUALITY && op <= EXPR_GREATER_EQUALS)
                   ? a
                   : result;
    }
    list = typeSetSpan(b);
    if (list.nTokens) {
        Type *result = NULL;
        for (size_t start = 0; start < list.nTokens;) {
            size_t end = constraintTermEnd(list, start);
            Type *part =
                operatorResult(c, op, a, typeOf(c, constraintTerm(list, start, end)), depth + 1);
            if (!part) return NULL;
            if (!result)
                result = part;
            else if (!identicalTypes(result, part))
                result = b;
            start = end + 1;
        }
        return result;
    }
    if (a == &unknown || b == &unknown) return &unknown;
    if (canonical(a) == &any || canonical(b) == &any || a->interface || b->interface) return NULL;
    if (numericType(a) && numericType(b)) {
        if (definedType(a) || definedType(b)) {
            if (!identicalTypes(a, b)) return NULL;
        }
        int isFloat = representation(a) == &floating || representation(b) == &floating;
        int arithmetic = op >= EXPR_ADD && op <= EXPR_DIVIDE;
        int integral = op == EXPR_MODULO || op == EXPR_XOR || op == EXPR_AMPERSAND ||
                       op == EXPR_BITWISE_AND || op == EXPR_BITWISE_OR ||
                       op == EXPR_BITSHIFT_LEFT || op == EXPR_BITSHIFT_RIGHT;
        int comparison = op >= EXPR_EQUALITY && op <= EXPR_GREATER_EQUALS;
        if (!arithmetic && !comparison && !(integral && !isFloat)) return NULL;
        int au, bu, aw = integerShape(a, &au), bw = integerShape(b, &bu);
        if (!isFloat && aw && bw && au != bu && (au ? aw : bw) == 64) return NULL;
        return comparison ? &boolean : identicalTypes(a, b) ? a : isFloat ? &floating : &integer;
    }
    if (canonical(a) == &boolean && canonical(b) == &boolean &&
        (op == EXPR_LOGICAL_AND || op == EXPR_LOGICAL_OR || op == EXPR_EQUALITY ||
         op == EXPR_NOT_EQUALS))
        return &boolean;
    if (representation(a)->immutableString && representation(b)->immutableString) {
        if (op == EXPR_ADD) return &string;
        if (op >= EXPR_EQUALITY && op <= EXPR_GREATER_EQUALS) return &boolean;
    }
    if (op == EXPR_ADD && a->dynamicArray && b->dynamicArray && matchesTypeConstraint(a, b) &&
        matchesTypeConstraint(b, a))
        return a;
    if ((op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS) && !strcmp(a->name, "pointer") &&
        identicalTypes(a, b))
        return &boolean;
    return NULL;
}
static int abstractOperand(Type *t) {
    return t->abstractParameter || typeSetSpan(t).nTokens || t->interface || canonical(t) == &any;
}

static void addCaptureName(token **names, size_t *count, token name) {
    for (size_t i = 0; i < *count; i++) if (!strcmp((*names)[i].lexeme, name.lexeme)) return;
    token *grown = realloc(*names, (*count + 1) * sizeof(token));
    if (!grown) exit(1);
    *names = grown; grown[(*count)++] = name;
}
static void captureReference(Context *c, Symbol *symbol) {
    if (!c->function || !symbol->ownerFunction || symbol->ownerFunction == c->function || symbol->isType) return;
    struct stmt_func *owner = symbol->ownerFunction;
    // Only literal lexical ancestors can supply captured storage.
    struct stmt_func *ancestor = c->function;
    while (ancestor && ancestor != owner) ancestor = ancestor->lexicalParent;
    if (!ancestor) return;
    addCaptureName(&owner->capturedLocals, &owner->nCapturedLocals, symbol->name);
    for (struct stmt_func *function = c->function; function != owner; function = function->lexicalParent)
        addCaptureName(&function->captures, &function->nCaptures, symbol->name);
}

static Type *expression(Context *c, expr *e) {
    token at = e ? e->location : (token){0};
    if (!e || !e->underlying)
        return &unknown;
    switch (e->type) {
    case EXPR_TYPE_MAKE: case EXPR_TYPE_NEW: {
        Type *t = typeOf(c, ((struct expr_zero *)e->underlying)->type);
        if (e->type == EXPR_TYPE_MAKE) {
            if (!t->dynamicArray && !t->mapKey) error(c, at, "make requires a dynamic array type T[] or map[K]V");
            return t;
        }
        if (!strcmp(t->name, "fixed-array") && t->bufferFamily == 2)
            error(c, at, "array<T> cannot instantiate an array; use T[n]");
        if (t == &nothing) error(c, at, "new requires a storage type");
        Type *pointer = allocate(c, sizeof(*pointer));
        pointer->name = "pointer"; pointer->element = t;
        return pointer;
    }
    case EXPR_TYPE_ZERO: {
        struct expr_zero *v = e->underlying;
        Type *t = typeOf(c, v->type);
        if (!strcmp(t->name, "fixed-array") && t->bufferFamily == 2)
            error(c, at, "array<T> cannot instantiate an array; use T[n]");
        if (t == &nothing) error(c, at, "void has no value representation");
        if (!strcmp(t->name, "buffer") && t->bufferFamily == 1)
            error(c, at, "zero requires a concrete buffer element type");
        if (v->type.nTokens == 1 && v->type.tokens[0].type == ANY)
            error(c, at, "zero requires a storage type or a type parameter, not any");
        if (!strcmp(t->name, "buffer") && t->bufferSize == 2)
            error(c, at, "zero requires a fixed-size buffer type; "
                         "runtime-sized buffers have no value representation");
        return t;
    }
    case EXPR_TYPE_IDENTIFIER: {
        struct expr_identifier *identifier = e->underlying;
        token t = identifier->lexeme;
        if (identifier->externReference) {
            struct stmt_func *f = fileExtern(c, t);
            if (!f) { error(c, t, "extern function is not declared in this file"); return &unknown; }
            identifier->lexeme.lexeme = f->name.lexeme;
            Type *function = allocate(c, sizeof(*function));
            function->name = "function"; function->function = f; function->context = c;
            Symbol *symbol=allocate(c,sizeof(*symbol));symbol->name=f->name;symbol->type=function;indexReference(c,t,symbol);
            return function;
        }
        if (!strcmp(t.lexeme, "true") || !strcmp(t.lexeme, "false"))
            return &boolean;
        Symbol *s = lookup(c, t.lexeme);
        if (!s) {
            error(c, t, fileExtern(c, t) ? "extern functions must be called with extern.name(...)" : "unresolved name");
            return &unknown;
        }
        captureReference(c, s);
        indexReference(c,t,s);
        if (s->isType && s->type->enumeration && s->type->enumeration->isBuiltin)
            identifier->lexeme.lexeme = OCEAN_BUILTIN_RESULT_NAME;
        if (s->freed) error(c, t, "dynamic array has been freed; re-create it before use");
        return s->type;
    }
    case EXPR_TYPE_LITERAL: {
        struct expr_literal *v = e->underlying;
        if (v->type == LITERAL_TYPE_NULL) return &nullValue;
        if (v->type == LITERAL_TYPE_INT && v->unsignedLiteral) {
            return &unsignedIntegerType;
        }
        return v->type == LITERAL_TYPE_STRING  ? &string
               : v->type == LITERAL_TYPE_FLOAT ? &floating
                                               : &integer;
    }
    case EXPR_TYPE_FUNCTION: {
        Type *t = allocate(c, sizeof(*t));
        t->name = "function"; t->context = c;
        t->function = e->underlying;
        t->function->isLiteral = 1;
        t->function->lexicalParent = c->function;
        if (!t->function->name.lexeme) {
            t->function->name = e->location;
            t->function->name.lexeme = "<literal>";
        }
        if (t->function->nTypeParams) error(c, e->location, "function literals cannot declare type parameters");
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
            if ((s && !s->isType) || !strcmp(arg.lexeme, "true") || !strcmp(arg.lexeme, "false")) {
                struct expr_index *idx = malloc(sizeof(*idx));
                struct expr_identifier *id = malloc(sizeof(*id));
                if (!idx || !id)
                    exit(1);
                *id = (struct expr_identifier){.lexeme = arg};
                *idx = (struct expr_index){v->parent, {EXPR_TYPE_IDENTIFIER, id, arg}, v->location};
                free(v->typeArguments[0].tokens);
                free(v->typeArguments);
                free(v);
                e->type = EXPR_TYPE_INDEX;
                e->underlying = idx;
                return expression(c, e);
            }
        }
        Type *base = expression(c, &v->parent), *t = allocate(c, sizeof(*t));
        if (base->enumeration && v->location.type == LEFT_BRACKET) {
            error(c, v->location, "Enum type arguments require angle brackets <...>"); return &unknown;
        }
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
        if (base->enumeration) t->enumArguments = v->typeArguments;
        for (size_t i = 0; i < t->nArguments; i++)
            t->arguments[i] = typeOf(c, v->typeArguments[i]);
        typeTokens *constraints = base->function ? base->function->typeParamConstraints
                                   : base->record ? base->record->typeParamConstraints : NULL;
        for (size_t i = 0; constraints && i < count && i < t->nArguments; i++)
            checkConstraint(c, t, constraints[i], t->arguments[i], at);
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
        if (p->mapKey) {
            check(c, p->mapKey, expectedExpression(c, p->mapKey, &v->index, v->index.location), v->index.location);
            return p->element;
        }
        check(c, &integer, expression(c, &v->index), v->index.location);
        if (!strcmp(p->name, "buffer") && p->bufferFamily == 1) {
            error(c, at, "narrow buffer to an element type before indexing");
            return &unknown;
        }
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
        t->dynamicArray = 1;
        t->element = typeOf(c, v->elementType);
        if (v->buffer || (!strcmp(t->element->name, "buffer") && t->element->bufferSize == 0)) {
            if (!v->buffer) {
                t = t->element;
                v->elementType = (typeTokens){v->elementType.nTokens - 3, v->elementType.tokens + 2};
                v->buffer = 1;
            } else {
                t->name = "buffer";
            }
            t->bufferFamily = 0;
            t->bufferSize = 2;
            if (v->count.type == EXPR_TYPE_LITERAL &&
                ((struct expr_literal *)v->count.underlying)->type == LITERAL_TYPE_INT) {
                t->bufferSize = 1;
                t->count = *(long *)((struct expr_literal *)v->count.underlying)->value;
            }
        }
        if (t->element->interface && (t->element->interface->nMethods || t->element->interface->nConstraints) &&
            !(v->count.type == EXPR_TYPE_LITERAL &&
              ((struct expr_literal *)v->count.underlying)->type == LITERAL_TYPE_INT &&
              *(long *)((struct expr_literal *)v->count.underlying)->value == 0))
            error(c, at, "interface array allocation requires a concrete element type; use an array literal");
        if (t->element == &nothing) error(c, at, "void is not an array element storage type");
        if (!strcmp(t->element->name, "buffer") && t->element->bufferSize == 2)
            error(c, at, "array elements must have a fixed storage size");
        check(c, &integer, expression(c, &v->count), v->count.location);
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
        if (enumSelector(c, &v->func)) {
            Type *variant = expression(c, &v->func);
            if (variant->enumeration && variant->enumMember) {
                struct stmt_enum *decl = variant->enumeration; size_t i = variant->enumMember - 1;
                if (variant->nArguments != decl->nTypeParams) { error(c, at, "generic enum constructor requires explicit type arguments"); return &unknown; }
                if (decl->forms[i] != 1) { error(c, at, "only tuple enum variants use call construction"); return &unknown; }
                if (v->spread || v->nParams != decl->payloads[i].nFields) error(c, at, "enum constructor argument count mismatch");
                struct expr_composite *payload = calloc(1, sizeof(*payload));
                payload->nEntries = v->nParams; payload->values = v->params; payload->keys = calloc(v->nParams + 1, sizeof(expr));
                expr literal = {.type = EXPR_TYPE_COMPOSITE, .underlying = payload, .location = at};
                checkComposite(c, enumPayloadType(c, variant, i), &literal, at);
                *e = specializedEnumExpr(ENUM_CONSTRUCT, variant, i, literal, at); return enumValueType(c, variant);
            }
        }
        if (v->func.type == EXPR_TYPE_IDENTIFIER) {
            const char *name = ((struct expr_identifier *)v->func.underlying)->lexeme.lexeme;
            if (!strcmp(name, "compilerThreadSpawn") || !strcmp(name, "compilerThreadReturn")) {
                int spawn = !strcmp(name, "compilerThreadSpawn");
                if (!c->function || c->function->compilerIntrinsic != (spawn ? INTRINSIC_THREAD_SPAWN : INTRINSIC_THREAD_RETURN)) {
                    error(c, at, "thread intrinsics are restricted to the standard library threads package");
                    return &unknown;
                }
                if (v->nParams != (size_t)spawn || v->spread) error(c, at, "invalid thread intrinsic arguments");
                for (size_t i = 0; i < v->nParams; i++) {
                    Type *actual = expression(c, &v->params[i]);
                    if (spawn) check(c, typeOf(c, c->function->paramTypeNames[0]), actual, at);
                }
                return spawn ? &integer : &nothing;
            }
            int fields = !strcmp(name, "compilerReflectFields"), predicate = !strcmp(name, "compilerReflectIsStruct");
            if (fields || predicate) {
                enum compilerIntrinsic required = fields ? INTRINSIC_REFLECT_FIELDS : INTRINSIC_REFLECT_IS_STRUCT;
                if (!c->function || c->function->compilerIntrinsic != required) {
                    error(c, at, "reflection intrinsics are restricted to the standard library reflect package");
                    return &unknown;
                }
                if (v->nParams != 1 || v->spread) error(c, at, "reflection intrinsic requires exactly one value");
                for (size_t i = 0; i < v->nParams; i++) expression(c, &v->params[i]);
                return fields ? typeOf(c, c->function->resultTypes[0]) : &boolean;
            }
        }
        if (v->func.type == EXPR_TYPE_DOT) {
            struct expr_dot *dot = v->func.underlying;
            Type *parent = expression(c, &dot->parent);
            const char *name = ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme;
            Type *arrayReceiver = recordReceiver(parent);
            if (arrayReceiver->dynamicArray && !strcmp(name, "array")) {
                if (v->nParams || v->spread) error(c, at, "array() requires no arguments");
                for (size_t i = 0; i < v->nParams; i++) expression(c, &v->params[i]);
                Type *result = allocate(c, sizeof(*result));
                result->name = "fixed-array"; result->bufferFamily = 2;
                result->element = arrayReceiver->element;
                return result;
            }
            if (parent->dynamicArray && (!strcmp(name, "remove") || !strcmp(name, "pop"))) {
                if (parent->immutableString) error(c, at, "strings are immutable; reassign the string instead");
                if (!writable(c, &dot->parent)) error(c, at, "dynamic array mutation requires a writable dynamic array");
                size_t count = !strcmp(name, "remove") ? 1 : 0;
                if (v->nParams != count || v->spread)
                    error(c, at, "remove requires exactly one index; pop requires no arguments");
                for (size_t i = 0; i < v->nParams; i++)
                    check(c, &integer, expression(c, &v->params[i]), at);
                return parent->element;
            }
            if (parent->dynamicArray && (!strcmp(name, "append") || !strcmp(name, "prepend") || !strcmp(name, "add"))) {
                if (parent->immutableString) error(c, at, "strings are immutable; reassign the string instead");
                if (!writable(c, &dot->parent)) error(c, at, "dynamic array mutation requires a writable dynamic array");
                size_t first = !strcmp(name, "add") ? 1 : 0;
                if (v->nParams < first) error(c, at, "dynamic array add requires an index");
                for (size_t i = 0; i < v->nParams; i++) {
                    Type *actual = expression(c, &v->params[i]);
                    if (first && i == 0) check(c, &integer, actual, at);
                    else if (v->spread && i + 1 == v->nParams) {
                        if (strcmp(actual->name, "dynamic-array") && strcmp(actual->name, "fixed-array"))
                            error(c, at, "expanded dynamic array argument must be a collection");
                        else check(c, parent->element, actual->element, at);
                    } else check(c, parent->element,
                        contextualInteger(c, parent->element, actual, &v->params[i], at), at);
                }
                return &nothing;
            }
        }
        if (v->func.type == EXPR_TYPE_IDENTIFIER &&
            !strcmp(((struct expr_identifier *)v->func.underlying)->lexeme.lexeme, "recover") && !lookup(c, "recover")) {
            if (v->nParams || v->spread) error(c, at, "recover requires no arguments");
            for (size_t i = 0; i < v->nParams; i++) expression(c, &v->params[i]);
            return &any;
        }
        if (v->func.type == EXPR_TYPE_IDENTIFIER &&
            !strcmp(((struct expr_identifier *)v->func.underlying)->lexeme.lexeme, "free") && !lookup(c, "free")) {
            if (v->nParams != 1 || v->spread) error(c, at, "free requires exactly one dynamic array or pointer");
            for (size_t i = 0; i < v->nParams; i++) {
                Type *arg = expression(c, &v->params[i]);
                if (!arg->dynamicArray && strcmp(arg->name, "pointer")) error(c, at, "free requires a dynamic array or pointer");
                if (arg->dynamicArray && v->params[i].type == EXPR_TYPE_IDENTIFIER) {
                    Symbol *binding = lookup(c, ((struct expr_identifier *)v->params[i].underlying)->lexeme.lexeme);
                    if (binding && binding->ownerFunction == c->function && c->function) binding->freed = 1;
                }
            }
            return &nothing;
        }

        if (v->func.type == EXPR_TYPE_IDENTIFIER &&
            !((struct expr_identifier *)v->func.underlying)->externReference &&
            !strcmp(((struct expr_identifier *)v->func.underlying)->lexeme.lexeme, "delete") && !lookup(c, "delete")) {
            if (v->nParams != 2 || v->spread) { error(c, at, "delete requires a map and key"); return &nothing; }
            Type *map = expression(c, &v->params[0]);
            if (!map->mapKey) error(c, at, "delete requires a map");
            else {
                check(c, map->mapKey, expectedExpression(c, map->mapKey, &v->params[1], at), at);
                if (!writable(c, &v->params[0])) mutationError(c, &v->params[0], at, "delete requires a writable map");
            }
            return &nothing;
        }
        if (v->func.type == EXPR_TYPE_IDENTIFIER &&
            !((struct expr_identifier *)v->func.underlying)->externReference &&
            !strcmp(((struct expr_identifier *)v->func.underlying)->lexeme.lexeme, "len") &&
            !lookup(c, "len")) {
            if (v->nParams != 1 || v->spread)
                error(c, at, "len requires exactly one array argument");
            for (size_t i = 0; i < v->nParams; i++) {
                Type *arg = expression(c, &v->params[i]);
                if (!arg->mapKey && strcmp(arg->name, "dynamic-array") && strcmp(arg->name, "fixed-array") &&
                    (strcmp(arg->name, "buffer") || !arg->bufferSize))
                    error(c, at, "len requires an array, dynamic array, string, or buffer with known length");
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
        if (f) c->externalArguments++;
        TypeList args = valueTypes(c, v->params, v->nParams, expected, v->nParams, at, 0);
        if (f) c->externalArguments--;
        if (v->spread) {
            if (args.count != v->nParams) {
                error(c, at, "expanded argument must be an array, not multiple return values");
            } else if (!f || !f->isVariadic || args.count < f->nParams + (f->isExtern ? 1 : 0)) {
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
                else if (!f->isExtern)
                    check(c, parameter(c, t, f->paramTypeNames[f->nParams - 1]), last->element, at);
            }
        } else if (f && (args.count < fixed || (!f->isVariadic && args.count != fixed)))
            error(c, f->name, "argument count mismatch");
        for (size_t i = 0; f && i < args.count; i++) {
            if (v->spread && i + 1 == args.count)
                continue;
            if (i < fixed || (f->isVariadic && !f->isExtern)) {
                size_t j = i < fixed ? i : f->nParams - 1;
                check(c, parameter(c, t, f->paramTypeNames[j]), args.items[i],
                      v->params[args.count == v->nParams ? i : 0].location);
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
                    if (array->immutableString) error(c, at, "strings are immutable; reassign the string instead");
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
            if (m == 1) {
                token *names = allocate(c, sizeof(token) * n);
                for (size_t i = 0; i < n; i++) {
                    expr *target = commaItem(&v->left, i);
                    if (target->type == EXPR_TYPE_IDENTIFIER) names[i] = ((struct expr_identifier *)target->underlying)->lexeme;
                    else names[i].lexeme = "_";
                }
                orderEnumExtraction(c, &rhs[0], names, n);
            }
            TypeList values = valueTypes(c, rhs, m, targets, n, at, 1);
            for (size_t i = 0; i < m; i++) *commaItem(&v->right, i) = rhs[i];
            if (values.count != n)
                error(c, at, "assignment count mismatch");
            for (size_t i = 0; i < n && i < values.count; i++)
                check(c, targets[i], values.items[i], commaItem(&v->left, i)->location);
            return &nothing;
        }
        Symbol *recreated = v->operation == EXPR_ASSIGNMENT && v->left.type == EXPR_TYPE_IDENTIFIER
            ? lookup(c, ((struct expr_identifier *)v->left.underlying)->lexeme.lexeme) : NULL;
        Type *a = recreated && recreated->freed ? recreated->type : expression(c, &v->left);
        Type *b = v->operation == EXPR_ASSIGNMENT ? expectedExpression(c, a, &v->right, at)
                                                  : expression(c, &v->right);
        if (coerceFloatLiteral(a, &v->right)) b = expression(c, &v->right);
        if (coerceFloatLiteral(b, &v->left)) a = expression(c, &v->left);
        if (integerLiteralFits(a, &v->right) && !(integerFits(a, b) && integerFits(b, a))) {
            typeIntegerLiteral(a, &v->right);
            b = a;
        }
        if (integerLiteralFits(b, &v->left) && !(integerFits(a, b) && integerFits(b, a))) {
            typeIntegerLiteral(b, &v->left);
            a = b;
        }
        if (v->operation == EXPR_EQUALITY || v->operation == EXPR_NOT_EQUALS) {
            if (a->enumeration || b->enumeration) {
                if (a->enumeration && b->enumeration) {
                    if (a->enumeration != b->enumeration) error(c, at, "cannot compare variants from different enum types");
                    if (a->enumeration == b->enumeration && a->nArguments && b->nArguments && !compatibleTypes(a, b))
                        error(c, at, "cannot compare different enum specializations");
                    if (a->enumMember || b->enumMember) {
                        v->left = a->enumMember ? enumInteger(a->enumMember - 1, at) : enumExpr(ENUM_TAG, a->enumeration, 0, v->left, at);
                        v->right = b->enumMember ? enumInteger(b->enumMember - 1, at) : enumExpr(ENUM_TAG, b->enumeration, 0, v->right, at);
                    }
                    return &boolean;
                }
            }
            int au, bu;
            int numericPair = integerShape(a, &au) && integerShape(b, &bu);
            if (numericPair && au != bu && (au ? integerShape(a, &au) : integerShape(b, &bu)) == 64)
                error(c, at, "mixed signed and unsigned integers require an explicit -> cast");
            else if (!numericPair && !equalityCompatible(a, b)) check(c, a, b, at);
            return &boolean;
        }
        int collectionAddition = (v->operation == EXPR_ADD || v->operation == EXPR_ADD_EQUALS) &&
            (a->dynamicArray || b->dynamicArray || !strcmp(a->name,"fixed-array") || !strcmp(b->name,"fixed-array"));
        if (v->operation != EXPR_ASSIGNMENT && v->operation != EXPR_COMMA && !collectionAddition &&
            a != &nullValue && b != &nullValue && (abstractOperand(a) || abstractOperand(b))) {
            enum binaryExprType op = baseOperation(v->operation);
            v->operandTypes[0]=typeSetSpan(a);
            v->operandTypes[1]=typeSetSpan(b);
            Type *result = operatorResult(c, op, a, b, 0);
            if (!result) {
                error(c, at, "operator is not supported by every possible operand type");
                return &unknown;
            }
            if (op != v->operation) {
                if (!writable(c, &v->left)) mutationError(c, &v->left, at, "assignment requires a writable value");
                check(c, a, result, at);
                return a;
            }
            if (!typeSetSpan(result).nTokens && !result->abstractParameter &&
                (numericType(result) || result == &boolean || result == &string)) {
                v->checkedResult.nTokens = 1;
                v->checkedResult.tokens = calloc(1,sizeof(token));
                v->checkedResult.tokens[0] = at;
                v->checkedResult.tokens[0].type = IDENTIFIER;
                v->checkedResult.tokens[0].lexeme = (char *)(result == &string ? "string" : result->spelling ? result->spelling : result->name);
            }
            return result;
        }
        Type *integerResult = NULL;
        int au, bu, aw = integerShape(a, &au), bw = integerShape(b, &bu);
        int stores = v->operation == EXPR_ASSIGNMENT ||
                     (v->operation >= EXPR_ADD_EQUALS && v->operation <= EXPR_BITWISE_OR_EQUALS);
        if (aw && bw && !stores) {
            if (aw == bw && au == bu) integerResult = a;
            else if (au == bu) integerResult = au ? &unsignedIntegerType : &integer;
            else if ((au ? aw : bw) < 64) integerResult = &integer;
            else error(c, at, "mixed signed and unsigned integers require an explicit -> cast");
        }
        if (recreated) recreated->freed = 0;


        if (v->operation == EXPR_COMMA)
            return b;
        if ((v->operation == EXPR_ADD || v->operation == EXPR_ADD_EQUALS) &&
            !strcmp(a->name, "fixed-array") && !strcmp(b->name, "fixed-array")) {
            check(c, a->element, b->element, at);
            Type *result = allocate(c, sizeof(*result)); *result = *a;
            if (a->bufferSize == 1 && b->bufferSize == 1) result->count = a->count + b->count;
            else { result->bufferFamily = 2; result->bufferSize = 0; result->count = 0; }
            if (v->operation == EXPR_ADD_EQUALS) {
                if (v->left.type != EXPR_TYPE_IDENTIFIER || !writable(c, &v->left))
                    error(c, at, "array += requires a writable variable");
                else lookup(c, ((struct expr_identifier *)v->left.underlying)->lexeme.lexeme)->type = result;
            }
            return result;
        }
        if ((v->operation == EXPR_ADD || v->operation == EXPR_ADD_EQUALS) &&
            (!strcmp(a->name, "dynamic-array") || !strcmp(b->name, "dynamic-array"))) {
            int leftArray = !strcmp(a->name, "dynamic-array");
            int rightArray = !strcmp(b->name, "dynamic-array");
            Type *result = NULL;
            // Equal array types concatenate; an array matching the other
            // operand's element type appends/prepends one nested value.
            if (leftArray && rightArray && matchesTypeConstraint(a, b))
                result = canonical(b->element) == &any ? b : a;
            else if (leftArray && (compatible(a->element, b) || integerLiteralFits(a->element, &v->right))) result = a;
            else if (rightArray && (compatible(b->element, a) || integerLiteralFits(b->element, &v->left))) result = b;
            if (!result) {
                error(c, at, "array addition requires matching arrays or a compatible element");
                return a;
            }
            if (v->operation == EXPR_ADD_EQUALS) {
                if (!leftArray) error(c, at, "array += requires an array on the left");
                else {
                    if (canonical(a->element) != &any && canonical(result->element) == &any)
                        error(c, at, "array += cannot widen the element type to any");
                    check(c, a, result, at);
                }
                if (!writable(c, &v->left))
                    mutationError(c, &v->left, at, "assignment requires a writable value");
            }
            return result;
        }
        if ((v->operation == EXPR_EQUALITY || v->operation == EXPR_NOT_EQUALS) && voidPointer(b))
            check(c, b, a, at);
        else if (!integerResult)
            check(c, a, b, at);
        if ((a == &nullValue || b == &nullValue) &&
            v->operation != EXPR_ASSIGNMENT && v->operation != EXPR_EQUALITY &&
            v->operation != EXPR_NOT_EQUALS)
            error(c, at, "null supports only assignment and equality comparisons");
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
        if (a != &unknown && representation(a) != &any && representation(a) != &integer && representation(a) != &signedByte && representation(a) != &floating &&
            !(!strcmp(a->name, "dynamic-array") && a->element == &signedByte &&
              v->operation == EXPR_ADD))
            error(c, at, "invalid operands");
        return integerResult ? integerResult : a;
    }
    case EXPR_TYPE_UNARY: {
        struct expr_unary *v = e->underlying;
        if (v->operation == UNARY_SIZEOF && typeExpression(c, &v->child, &v->sizeofType)) {
            if (typeOf(c, v->sizeofType) == &nothing)
                error(c, at, "void has no storage size");
            return &integer;
        }
        Type *t = expression(c, &v->child);
        if (t == &nullValue)
            error(c, at, "null is not a primitive, addressable value, or storage type");
        if (v->operation >= UNARY_POSTFIX_INCREMENT && v->operation <= UNARY_PREFIX_DECREMENT &&
            !writable(c, &v->child))
            mutationError(c, &v->child, at, "increment requires a writable value");
        if (v->operation == UNARY_ADDRESS_OF) {
            if (v->child.type == EXPR_TYPE_INDEX && expression(c, &((struct expr_index *)v->child.underlying)->parent)->mapKey)
                error(c, at, "cannot take the address of a map entry");
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
            if (t->element == &nothing) {
                error(c, at, "cast a void pointer to a concrete pointer type before dereferencing");
                return &unknown;
            }
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
        if (abstractOperand(t)) {
            v->operandTypes=typeSetSpan(t);
            enum binaryExprType op = v->operation == UNARY_BITWISE_NOT ? EXPR_XOR : EXPR_SUBTRACT;
            if (!operatorResult(c, op, t, t, 0)) error(c, at, "operator is not supported by every possible operand type");
            return t;
        }
        if (representation(t) != &integer && representation(t) != &signedByte && representation(t) != &floating && t != &unknown && representation(t) != &any)
            error(c, at, "invalid unary operand");
        return t;
    }
    case EXPR_TYPE_TERNARY: {
        struct expr_ternary *v = e->underlying;
        check(c, &boolean, expression(c, &v->condition), v->condition.location);
        Type *a = expression(c, &v->ifTrue);
        Type *b = expectedExpression(c, a, &v->ifFalse, at);
        if (voidPointer(b)) {
            check(c, b, a, at);
            return b;
        }
        check(c, a, b, at);
        return a == &nullValue ? b : a;
    }
    case EXPR_TYPE_TYPECAST: {
        struct expr_typecast *v = e->underlying;
        Type *a = expression(c, &v->child), *b = typeOf(c, v->type);
        if (!strcmp(a->name, "pointer") && !strcmp(b->name, "pointer") && a->element && b->element &&
            representation(a->element)->immutableString != representation(b->element)->immutableString)
            error(c, at, "cannot cast between pointers to immutable strings and mutable values");
        int aPointer = !strcmp(a->name, "pointer") || !strcmp(a->name, "buffer");
        int bPointer = !strcmp(b->name, "pointer") || !strcmp(b->name, "buffer") || !strcmp(b->name, "fixed-array");
        checkBufferDimension(c, b, at);
        int aInteger = representation(a) == &integer || representation(a) == &signedByte;
        int bInteger = representation(b) == &integer || representation(b) == &signedByte;
        int aNumeric = aInteger || representation(a) == &floating || representation(a) == &boolean;
        int bNumeric = bInteger || representation(b) == &floating || representation(b) == &boolean;
        Type *ar = representation(a), *br = representation(b);
        if (!compatible(a, b) && !compatibleTypes(ar, br) && !(aNumeric && bNumeric) &&
            !(aPointer && bPointer) && !(aPointer && bInteger) && !(aInteger && bPointer))
            error(c, at, "invalid conversion");
        return b;
    }
    case EXPR_TYPE_ENUM: {
        struct expr_enum *v = e->underlying;
        if (v->operation == ENUM_TAG) return &integer;
        if (v->operation == ENUM_EXTRACT) return enumExtractionType(c, v, at);
        Type *t = enumNodeType(c, v);
        if (v->operation == ENUM_VARIANT) t->enumMember = v->member + 1;
        return t;
    }
    case EXPR_TYPE_ASSERTION: {
        struct expr_assertion *v = e->underlying;
        Type *source = expression(c, &v->child);
        if (v->isTypeGuard) {
            if (source->enumeration) error(c, at, "type-guard switches do not support enums; use match");
            return &any;
        }
        Type *target = typeOf(c, v->type);
        if (target->enumeration && (source->enumeration || target->enumMember)) {
            if (!target->enumMember) { error(c, at, "enum extraction requires a concrete variant"); return &unknown; }
            if (source->enumeration == target->enumeration && target->nArguments && !compatibleTypes(target, source))
                error(c, at, "enum extraction type arguments do not match the source");
            expr result = enumExpr(ENUM_EXTRACT, target->enumeration, target->enumMember - 1, v->child, at);
            if (v->pattern) {
                token *names; size_t *fields;
                size_t count = enumPattern(c, target->enumeration, target->enumMember - 1, v->pattern, &names, &fields);
                struct expr_enum *node = result.underlying; node->fields = fields; node->nFields = count;
                free(names);
            }
            *e = result; return enumExtractionType(c, result.underlying, at);
        }
        return target;
    }
    case EXPR_TYPE_DOT: {
        struct expr_dot *v = e->underlying;
        Type *t = recordReceiver(expression(c, &v->parent));
        if (t->enumeration) {
            expr *base = &v->parent;
            if (base->type == EXPR_TYPE_INSTANTIATION) base = &((struct expr_instantiation *)base->underlying)->parent;
            Symbol *symbol = base->type == EXPR_TYPE_IDENTIFIER ? lookup(c, ((struct expr_identifier *)base->underlying)->lexeme.lexeme) : NULL;
            token member = ((struct expr_identifier *)v->field.underlying)->lexeme;
            if (!symbol || !symbol->isType) {
                struct stmt_func *method = findMethod(c, t, member.lexeme);
                if (!method) { error(c, member, "extract a concrete enum variant before accessing its payload"); return &unknown; }
                if (!method->isBuiltin && !exportedName(method->name.lexeme) && sourcePackage(c, at.filename) != sourcePackage(c, method->name.filename)) {
                    error(c, member, "method is not exported by its package"); return &unknown;
                }
                Type *callable = allocate(c, sizeof(*callable));
                callable->name = "function"; callable->function = method; callable->receiver = t;
                Symbol *reference = allocate(c, sizeof(*reference)); reference->name = method->name; reference->type = callable;
                indexReference(c, member, reference);
                return callable;
            }
            Type *variant = enumMemberType(c, t, member);
            if (variant == &unknown) return variant;
            *e = specializedEnumExpr(ENUM_VARIANT, variant, variant->enumMember - 1, (expr){0}, at);
            return variant;
        }
        if (t == &nullValue)
            error(c, at, "null needs a typed pointer before member access");
        token field = ((struct expr_identifier *)v->field.underlying)->lexeme;
        if (c->index) {
            size_t n=c->nCompletionTypes++;
            c->completionTypes=realloc(c->completionTypes,c->nCompletionTypes*sizeof(*c->completionTypes));
            c->completionUses=realloc(c->completionUses,c->nCompletionTypes*sizeof(*c->completionUses));
            c->completionTypes[n]=t; c->completionUses[n]=field;
        }
        if (t->interface) {
            struct stmt_func *method=interfaceMethod(c,t,field.lexeme,0);
            if (method) {
                Type *callable=allocate(c,sizeof(*callable)); callable->name="function";
                v->interfaceMethod=method;
                callable->function=method; callable->receiver=t; return callable;
            }
            error(c,field,"interface has no such method"); return &unknown;
        }
        if (t->dynamicArray && (!strcmp(field.lexeme, "length") || !strcmp(field.lexeme, "capacity"))) return &integer;

        if (definedType(t)) {
            struct stmt_func *method = findMethod(c, t, field.lexeme);
            if (method) {
                if (!method->isBuiltin && !exportedName(method->name.lexeme) && sourcePackage(c, at.filename) != sourcePackage(c, method->name.filename)) {
                    error(c, field, "method is not exported by its package"); return &unknown;
                }
                Type *callable = allocate(c, sizeof(*callable));
                callable->name = "function"; callable->function = method; callable->receiver = t;
                Symbol *symbol=allocate(c,sizeof(*symbol));symbol->name=method->name;symbol->type=callable;indexReference(c,field,symbol);
                return callable;
            }
        }
        if (!strcmp(t->name, "dynamic-array") || !strcmp(t->name, "fixed-array")) {
            if (!strcmp(field.lexeme, "size"))
                return &integer;
            if (!strcmp(field.lexeme, "data")) {
                if (t->immutableString && !c->externalArguments)
                    error(c, field, "string data is read-only; copy to i8[] for mutable access");
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
            if (genericFamily(t) && !t->record->enumPayload) {
                error(c, field, "narrow a generic family to concrete type arguments before member access");
                return &unknown;
            }
            for (size_t i = 0; i < t->record->nFields; i++)
                if (!strcmp(field.lexeme, t->record->fieldNames[i].lexeme)) {
                    Type *fieldType=parameter(c,t,t->record->fieldTypes[i]);
                    Symbol *symbol=allocate(c,sizeof(*symbol));symbol->name=t->record->fieldNames[i];symbol->type=fieldType;
                    indexReference(c,field,symbol);
                    return fieldType;
                }
            struct stmt_func *method = findMethod(c, t, field.lexeme);
            if (method) {
                if (!method->isBuiltin && !exportedName(method->name.lexeme) &&
                    sourcePackage(c, at.filename) != sourcePackage(c, method->name.filename)) {
                    error(c, field, "method is not exported by its package");
                    return &unknown;
                }
                Type *callable = allocate(c, sizeof(*callable));
                callable->name = "function";
                callable->function = method;
                callable->receiver = t;
                Symbol *symbol=allocate(c,sizeof(*symbol));symbol->name=method->name;symbol->type=callable;
                indexReference(c,field,symbol);
                return callable;
            }
            error(c, field, "unknown field");
        }
        return &unknown;
    }
    case EXPR_TYPE_COMPOSITE: {
        struct expr_composite *literal = e->underlying;
        if (enumSelector(c, &literal->type)) {
            Type *variant = expression(c, &literal->type);
            if (variant->enumeration && variant->enumMember) {
                struct stmt_enum *decl = variant->enumeration; size_t i = variant->enumMember - 1;
                if (variant->nArguments != decl->nTypeParams) { error(c, at, "generic enum constructor requires explicit type arguments"); return &unknown; }
                if (decl->forms[i] != 2) { error(c, at, "only record enum variants use brace construction"); return &unknown; }
                literal->type = (expr){0};
                checkComposite(c, enumPayloadType(c, variant, i), e, at);
                expr payload = *e; *e = specializedEnumExpr(ENUM_CONSTRUCT, variant, i, payload, at); return enumValueType(c, variant);
            }
        }
        struct expr_composite *v = e->underlying;
        if (!v->type.type) {
            return checkComposite(c, &unknown, e, v->location);
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
        if (f->receiverType.nTokens || f->isExtern)
            return;
        t = allocate(c, sizeof(*t));
        t->name = "function"; t->context = c;
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
        t->name = v->name.lexeme; t->declaration = v; t->declarationScope = c->scope;
        declare(c, v->name, t, 1);
        break;
    }
    case STMT_ENUM: {
        struct stmt_enum *v = s->value;
        if (!strcmp(v->name.lexeme, "array")) error(c, v->name, "array is a reserved type name");
        declare(c, v->name, enumType(c, v), 1);
        break;
    }
    case STMT_INTERFACE: {
        struct stmt_interface *v = s->value;
        t = allocate(c, sizeof(*t));
        t->name = v->name.lexeme; t->interface = v; t->context = c;
        declare(c, v->name, t, 1);
        break;
    }
    default:
        break;
    }
}
static void statement(Context *c, stmt *s) {
    token at = s->location;
    switch (s->type) {
    case STMT_ENUM: {
        struct stmt_enum *v = s->value;
        Scope scope = {.parent = c->scope}; c->scope = &scope;
        for (size_t i = 0; i < v->nTypeParams; i++) {
            Type *bound = allocate(c, sizeof(*bound)); *bound = any;
            bound->semanticBase = &any;
            declare(c, v->typeParamNames[i], bound, 1);
        }
        for (size_t i = 0; i < v->nMembers; i++) {
            for (size_t j = 0; j < v->payloads[i].nFields; j++) {
                Type *field = typeOf(c, v->payloads[i].fieldTypes[j]);
                if (field == &nothing) error(c, v->names[i], "void is not an enum payload type");
            }
            if (v->values[i]) {
                Type *t = v->valueTypes[i].nTokens ? typeOf(c, v->valueTypes[i]) : NULL;
                Type *actual = t ? expectedExpression(c, t, v->values[i], v->names[i]) : expression(c, v->values[i]);
                if (t) check(c, t, actual, v->names[i]);
            }
        }
        c->scope = scope.parent;
        break;
    }
    case STMT_VAR: {
        struct stmt_var *v = s->value;
        Type *declared = v->type.nTokens ? typeOf(c, v->type) : NULL;
        if (declared == &nothing) error(c, at, "void is not a variable storage type");
        if (declared && !declared->enumeration && declared->record && declared->record->nVarParams && !v->nInitValues)
            error(c, v->vars[0], "struct with parameters requires an initializer");
        if (declared && !strcmp(declared->name, "fixed-array")) {
            checkBufferDimension(c, declared, v->vars[0]);
            if (declared->bufferFamily == 2 && !v->nInitValues)
                error(c, v->vars[0], "array<T> requires an existing array initializer; instantiate with T[n]");
        }
        if (declared && !strcmp(declared->name, "buffer")) {
            checkBufferDimension(c, declared, v->vars[0]);
            if (declared->bufferSize == 0 && !v->nInitValues)
                error(c, v->vars[0], "unsized buffer requires an initializer");
            if (declared->bufferSize == 2 && !c->function)
                error(c, at, "global buffers require fixed dimensions; use an array for runtime-sized storage");
            if (declared->bufferSize == 2 && v->nInitValues)
                error(c, v->vars[0], "runtime-sized buffers cannot have initializers");
        }
        Type **expected = allocate(c, sizeof(Type *) * v->nVars);
        for (size_t i = 0; i < v->nVars; i++)
            expected[i] = declared ? declared : &unknown;
        if (v->nInitValues == 1) orderEnumExtraction(c, &v->initValues[0], v->vars, v->nVars);
        TypeList values =
            valueTypes(c, v->initValues, v->nInitValues, expected, v->nVars, v->vars[0], 1);
        if (v->nInitValues && values.count != v->nVars)
            error(c, v->vars[0], "initializer count mismatch");
        for (size_t i = 0; i < v->nVars; i++) {
            Type *a = i < values.count ? values.items[i] : &unknown;
            if (a->enumeration && a->enumMember) {
                unsigned char form = a->enumeration->forms[a->enumMember - 1];
                if (form == 1 || form == 2) error(c, v->vars[i], "enum payload variant requires construction");
                a = enumValueType(c, a);
            }
            if (!declared && a == &nullValue)
                error(c, v->vars[i], "null requires an explicit pointer type");
            if (declared)
                check(c, declared, a, v->vars[i]);
            Symbol *global = !c->function ? lookup(c, v->vars[i].lexeme) : NULL;
            if (global && global->name.filename == v->vars[i].filename &&
                global->name.line == v->vars[i].line && global->name.column == v->vars[i].column)
                global->type = declared ? declared : a;
            else declare(c, v->vars[i], declared ? declared : a, 0);
        }
        break;
    }
    case STMT_FUNC:
        function(c, s->value);
        break;
    case STMT_TYPE: {
        struct stmt_type *v = s->value;
        Symbol *sym = lookup(c, v->name.lexeme);
        if (sym) resolveDeclaredType(c, sym->type);
        break;
    }
    case STMT_STRUCT: {
        struct stmt_struct *v = s->value;
        Scope scope = {c->scope, NULL};
        c->scope = &scope;
        for (size_t i = 0; i < v->nTypeParams; i++) {
            Type *bound = allocate(c, sizeof(*bound));
            *bound = any;
            bound->semanticBase = &any;
            declare(c, v->typeParamNames[i], bound, 1);
        }
        c->declaringMembers++; c->memberOwner=v->name;
        for (size_t i = 0; i < v->nFields; i++) {
            Type *fieldType = typeOf(c, v->fieldTypes[i]);
            if (fieldType == &nothing) error(c, v->fieldNames[i], "void is not a field storage type");
            Symbol *field = declare(c, v->fieldNames[i], fieldType, 0);
            if (field)
                field->immutable = i < v->nVarParams;
        }
        for (size_t i = 0; i < v->nFields; i++)
            checkBufferDimension(c, typeOf(c, v->fieldTypes[i]), v->fieldNames[i]);
        c->declaringMembers--; c->memberOwner=(token){0};
        c->scope = scope.parent;
        break;
    }
    case STMT_EXPR:
        expression(c, s->value);
        break;
    case STMT_BLOCK:
        block(c, s->value, 1);
        break;
    case STMT_PANIC:
    case STMT_RETURN_PANIC:
        check(c, &string, expression(c, s->value), ((expr *)s->value)->location);
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
            check(c, expected[i], values.items[i],
                  v->values[values.count == v->nValues ? i : 0].location);
        break;
    }
    case STMT_IF: {
        struct stmt_if *v = s->value;
        Scope scope = {c->scope, NULL, v->open, v->close};
        c->scope = &scope;
        if (v->preStmt)
            statement(c, v->preStmt);
        check(c, &boolean, expression(c, v->condition), v->condition->location);
        if (v->ifTrue)
            statement(c, v->ifTrue);
        if (v->ifFalse)
            statement(c, v->ifFalse);
        c->scope = scope.parent;
        break;
    }
    case STMT_FOR: {
        struct stmt_for *v = s->value;
        Scope scope = {c->scope, NULL, v->open, v->close};
        c->scope = &scope;
        if (v->initializer)
            statement(c, v->initializer);
        if (v->range) {
            Type *t = expression(c, v->range);
            for (size_t i = 0; i < v->nBindings; i++)
                declare(c, v->bindings[i],
                        i == 0       ? (t->mapKey ? t->mapKey : &integer)
                        : t->element ? t->element
                                     : &unknown,
                        0);
        }
        if (v->condition)
            check(c, &boolean, expression(c, v->condition), v->condition->location);
        if (v->update)
            expression(c, v->update);
        if (v->body)
            statement(c, v->body);
        c->scope = scope.parent;
        break;
    }
    case STMT_ASM: {
        struct stmt_asm *v = s->value;
        for (size_t i=0; i<v->nOperands; i++) {
            token name = v->operands[i];
            Symbol *symbol = lookup(c, name.lexeme);
            if (!symbol) { error(c, name, "unresolved asm operand"); continue; }
            indexReference(c, name, symbol);
            Type *t = representation(symbol->type);
            const char *kind = t->name;
            if (symbol->isType || symbol->immutable || symbol->freed ||
                (strcmp(kind, "pointer") && strcmp(kind, "int") && strcmp(kind, "uint") &&
                 strcmp(kind, "i32") && strcmp(kind, "u32") && strcmp(kind, "i16") &&
                 strcmp(kind, "u16") && strcmp(kind, "i8") && strcmp(kind, "u8") &&
                 strcmp(kind, "float") && strcmp(kind, "f32") && strcmp(kind, "bool")))
                error(c, name, "asm operands require mutable scalar variables or pointers");
        }
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
        if (v->isMatch) lowerMatch(c, v, expression(c, v->value), at);
        statement(c, &v->initializer);
        Type *t = v->value ? expression(c, v->value) : &boolean;
        for (size_t i = 0; i < v->nCases; i++) {
            Scope cs = {c->scope, NULL};
            c->scope = &cs;
            if (v->isTypeSwitch) {
                Type *binding = &any;
                for (size_t j = 0; j < v->cases[i].nTypes; j++) {
                    typeTokens span = v->cases[i].types[j];
                    for (size_t start = 0; start < span.nTokens;) {
                        size_t end = constraintTermEnd(span, start);
                        Type *constraint = typeOf(c, constraintTerm(span, start, end));
                        if (v->cases[i].nTypes == 1 && start == 0 && end == span.nTokens)
                            binding = constraint;
                        start = end + 1;
                    }
                }
                declare(c, v->binding, binding, 0);
            }
            for (size_t j = 0; j < v->cases[i].nValues; j++)
                check(c, t, expectedExpression(c, t, &v->cases[i].values[j], at), v->cases[i].values[j].location);
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
static void finishIndex(Context *c);
// Install compiler-owned definitions after package qualification, so a package's
// own Result declaration shadows the builtin without changing other packages.
static void prepareBuiltinResult(parseResult *program) {
    if (program->builtinsPrepared) return;
    program->builtinsPrepared = 1;
    for (size_t i=0; i<program->nStatements; i++) {
        stmt *s = &program->statements[i];
        token name = {0};
        if (s->type == STMT_ENUM) name = ((struct stmt_enum *)s->value)->name;
        if (s->type == STMT_STRUCT) name = ((struct stmt_struct *)s->value)->name;
        if (s->type == STMT_TYPE) name = ((struct stmt_type *)s->value)->name;
        if (s->type == STMT_INTERFACE) name = ((struct stmt_interface *)s->value)->name;
        if (s->type == STMT_FUNC && !((struct stmt_func *)s->value)->receiverType.nTokens)
            name = ((struct stmt_func *)s->value)->name;
        if (name.lexeme && !strcmp(name.lexeme, "Result")) return;
    }
    const char *source = oceanBuiltinResultSource();
    tokenlist *tokens = scanTokens((char *)source, OCEAN_BUILTIN_RESULT_FILE);
    for (size_t i=0; i<tokens->top; i++) {
        token *name = &tokens->arr[i];
        if (name->type == IDENTIFIER && !strcmp(name->lexeme, "Result")) name->lexeme = OCEAN_BUILTIN_RESULT_NAME;
    }
    parseResult builtin = parseTokensWithSource(tokens, source);
    free(tokens);
    if (builtin.hasError || builtin.nStatements != 4) abort();
    if (program->statementsCapacity < program->nStatements + builtin.nStatements) {
        program->statementsCapacity = program->nStatements + builtin.nStatements;
        program->statements = realloc(program->statements, sizeof(stmt) * program->statementsCapacity);
        if (!program->statements) abort();
    }
    for (size_t i=0; i<builtin.nStatements; i++) {
        stmt node = builtin.statements[i];
        if (node.type == STMT_ENUM) ((struct stmt_enum *)node.value)->isBuiltin = 1;
        if (node.type == STMT_FUNC) ((struct stmt_func *)node.value)->isBuiltin = 1;
        program->statements[program->nStatements++] = node;
    }
    free(builtin.statements);
}

static semanticResult analyzeIndexed(parseResult *program, semanticIndex *index) {
    if (!program || program->hasError)
        return (semanticResult){1};
    prepareBuiltinResult(program);
    Scope root = {0};
    Context c = {.scope = &root, .program = program};
    // Give extern declarations private semantic names while retaining C linkage.
    c.index = index;
    for (size_t i = 0; i < program->nStatements; i++) {
        stmt *s = &program->statements[i];
        if (s->type != STMT_FUNC) continue;
        struct stmt_func *f = s->value;
        if (!f->isExtern || f->externalName) continue;
        f->externalName = f->name.lexeme;
        size_t size = strlen(f->name.lexeme) + 48;
        char *name = malloc(size);
        if (!name) exit(1);
        snprintf(name, size, "__extern.%zu.%s", i, f->name.lexeme);
        f->name.lexeme = name;
    }
    for (size_t i = 0; i < program->nStatements; i++)
        predeclare(&c, &program->statements[i]);
    Symbol *builtinResult = lookup(&c, OCEAN_BUILTIN_RESULT_NAME);
    if (builtinResult && !lookup(&c, "Result")) {
        token alias = builtinResult->name; alias.lexeme = "Result";
        declare(&c, alias, builtinResult->type, 1);
    }
    for (size_t i = 0; i < program->nStatements; i++)
        if (program->statements[i].type == STMT_TYPE) {
            struct stmt_type *declaration = program->statements[i].value;
            Symbol *symbol = lookup(&c, declaration->name.lexeme);
            if (symbol) symbol->type = resolveDeclaredType(&c, symbol->type);
        }
    for (size_t i = 0; i < program->nStatements; i++) {
        stmt *s = &program->statements[i];
        if (s->type != STMT_VAR) continue;
        struct stmt_var *v = s->value;
        Type *type = v->type.nTokens ? typeOf(&c, v->type) : &unknown;
        for (size_t j = 0; j < v->nVars; j++) declare(&c, v->vars[j], type, 0);
    }
    size_t previousUnknown = (size_t)-1;
    for (;;) {
        for (size_t i = 0; i < program->nStatements; i++)
            if (program->statements[i].type == STMT_VAR) statement(&c, &program->statements[i]);
        size_t remaining = 0;
        for (Symbol *symbol = root.symbols; symbol; symbol = symbol->next)
            if (!symbol->isType && symbol->type == &unknown) remaining++;
        if (!remaining || remaining == previousUnknown || c.errors) break;
        previousUnknown = remaining;
    }
    if (!c.errors)
        for (Symbol *symbol = root.symbols; symbol; symbol = symbol->next)
            if (!symbol->isType && symbol->type == &unknown)
                error(&c, symbol->name, "cannot infer global type; add an explicit type");
    for (size_t i = 0; i < program->nStatements; i++)
        if (program->statements[i].type != STMT_VAR) statement(&c, &program->statements[i]);
    if (index) finishIndex(&c);
    while (c.allocations) {
        Allocation *a = c.allocations;
        c.allocations = a->next;
        free(a->ptr);
        free(a);
    }
    return (semanticResult){c.errors};
}

static size_t tokenOffset(Context *c, token t) {
    const char *text=programSource(c->program,t.filename);if(!text)return 0;
    size_t p=0;long line=1;while(text[p]&&line<t.line)if(text[p++]=='\n')line++;
    return p+(t.column>0?(size_t)t.column-1:0);
}
static int sameToken(token a,token b){return a.filename&&b.filename&&!strcmp(a.filename,b.filename)&&a.line==b.line&&a.column==b.column;}
static char *sourceName(Context *c,token t){
    const char *s=programSource(c->program,t.filename);size_t p=tokenOffset(c,t),n=0;uint32_t code;size_t k;
    while((k=unicodeDecode(s+p+n,&code))&&unicodeContinue(code))n+=k;
    return n?strndup(s+p,n):strdup(t.lexeme?t.lexeme:"");
}
static size_t indexSymbol(Context *c,Symbol *symbol){
    if(!c->index)return 0;
    semanticIndex *index=c->index;
    for(size_t i=0;i<index->nSymbols;i++)if(sameToken(index->symbols[i].declaration,symbol->name))return i;
    size_t i=index->nSymbols++;
    index->symbols=realloc(index->symbols,index->nSymbols*sizeof(*index->symbols));
    c->indexedSymbols=realloc(c->indexedSymbols,index->nSymbols*sizeof(*c->indexedSymbols));
    c->indexedSymbols[i]=symbol;
    token open=c->scope->open,close=c->scope->close;
    if(!open.filename&&c->function){open=c->function->body.open;close=c->function->body.close;}
    index->symbols[i]=(semanticSymbol){.declaration=symbol->name,.name=sourceName(c,symbol->name),
        .packageId=sourcePackage(c,symbol->name.filename),.begin=open.filename?tokenOffset(c,open):0,
        .end=close.filename?tokenOffset(c,close):(size_t)-1,.global=!c->scope->parent,.member=c->declaringMembers,
        .kind=symbol->isType?7:symbol->type&&symbol->type->function?3:6,.owner=c->memberOwner};
    return i;
}
static void indexReference(Context *c,token use,Symbol *symbol){
    if(!c->index||!use.filename||!symbol)return;
    semanticIndex *index=c->index;size_t binding=indexSymbol(c,symbol);
    for(size_t i=0;i<index->nReferences;i++)if(sameToken(index->references[i].use,use)&&index->references[i].symbol==binding)return;
    size_t i=index->nReferences++;
    index->references=realloc(index->references,index->nReferences*sizeof(*index->references));
    c->referenceTypes=realloc(c->referenceTypes,index->nReferences*sizeof(*c->referenceTypes));
    index->references[i]=(semanticReference){.use=use,.symbol=binding};c->referenceTypes[i]=symbol->type;
}
static char *typeText(Type *t){
    char *text=NULL;size_t size=0;FILE *out=open_memstream(&text,&size);if(!out)return strdup("?");
    if(t->immutableString)fputs("string",out);else formatType(out,t,0);fclose(out);
    // Package qualification is an implementation detail, not a source type name.
    char *p=text;while((p=strstr(p,"__pkg"))){char *q=p+5;while(*q>='0'&&*q<='9')q++;if(*q=='_')memmove(p,q+1,strlen(q+1)+1);else p=q;}
    return text;
}
static void finishIndex(Context *c){
    semanticIndex *index=c->index;
    for(size_t i=0;i<index->nReferences+c->nCompletionTypes;i++){
        Type *original=i<index->nReferences?c->referenceTypes[i]:c->completionTypes[i-index->nReferences],*t=recordReceiver(representation(original));
        semanticMembers group={.use=i<index->nReferences?index->references[i].use:c->completionUses[i-index->nReferences],.collection=t->dynamicArray?1:!strcmp(t->name,"fixed-array")?2:0,.immutable=t->immutableString};
        if(t->record){
            for(size_t j=0;j<t->record->nFields;j++){
                token name=t->record->fieldNames[j];
                if(!exportedName(name.lexeme)&&sourcePackage(c,name.filename)!=sourcePackage(c,group.use.filename))continue;
                Symbol *member=allocate(c,sizeof(*member));member->name=name;member->type=parameter(c,t,t->record->fieldTypes[j]);
                size_t id=indexSymbol(c,member);index->symbols[id].member=1;index->symbols[id].owner=t->record->name;
                group.symbols=realloc(group.symbols,(group.count+1)*sizeof(size_t));group.symbols[group.count++]=id;
            }
        }
        for(size_t j=0;j<c->program->nStatements;j++)if(c->program->statements[j].type==STMT_FUNC){
            struct stmt_func *f=c->program->statements[j].value;
            if(!f->receiverType.nTokens||findMethod(c,original,f->name.lexeme)!=f)continue;
            if(!f->isBuiltin&&!exportedName(f->name.lexeme)&&sourcePackage(c,f->name.filename)!=sourcePackage(c,group.use.filename))continue;
            Type *callable=allocate(c,sizeof(*callable));callable->name="function";callable->function=f;callable->receiver=original;
            Symbol *member=allocate(c,sizeof(*member));member->name=f->name;member->type=callable;
            size_t id=indexSymbol(c,member);index->symbols[id].member=1;index->symbols[id].owner=t->record?t->record->name:t->declaration?t->declaration->name:(token){0};
            group.symbols=realloc(group.symbols,(group.count+1)*sizeof(size_t));group.symbols[group.count++]=id;
        }
        if(group.count||group.collection){index->members=realloc(index->members,(index->nMembers+1)*sizeof(*index->members));index->members[index->nMembers++]=group;}
    }
    for(size_t i=0;i<index->nSymbols;i++){
        Type *t=c->indexedSymbols[i]->type;index->symbols[i].type=typeText(t);
        while(!definedType(t)&&t->element&&!strcmp(t->name,"pointer"))t=t->element;
        if(t->declaration)index->symbols[i].typeDeclaration=t->declaration->name;
        else if(t->record)index->symbols[i].typeDeclaration=t->record->name;
    }
    for(size_t i=0;i<index->nReferences;i++)index->references[i].type=typeText(c->referenceTypes[i]);
    free(c->indexedSymbols);free(c->referenceTypes);free(c->completionTypes);free(c->completionUses);
}
semanticResult analyzeSemantics(parseResult *program){return analyzeIndexed(program,NULL);}
semanticResult indexSemantics(parseResult *program,semanticIndex *index){memset(index,0,sizeof(*index));return analyzeIndexed(program,index);}
void freeSemanticIndex(semanticIndex *index){
    for(size_t i=0;i<index->nSymbols;i++){free(index->symbols[i].name);free(index->symbols[i].type);}
    for(size_t i=0;i<index->nReferences;i++)free(index->references[i].type);
    for(size_t i=0;i<index->nMembers;i++)free(index->members[i].symbols);
    free(index->symbols);free(index->references);free(index->members);memset(index,0,sizeof(*index));
}
