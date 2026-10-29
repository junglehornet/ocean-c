#include "semantic.h"
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
    Type *receiver;
    struct stmt_func *function;
    struct stmt_struct *record;
    struct stmt_interface *interface;
    struct Context *context;
    int mixed;
    Type **arguments;
    size_t nArguments;
    size_t count;
    int bufferSize;
    int bufferFamily; // 1: bare buffer; 2: buffer<T> without a dimension.
    typeTokens dimension;
    typeTokens constraint; // Inherited from a receiver struct parameter, when present.
    const char *spelling; // Keep explicit numeric type names for diagnostics.
    Type *semanticBase; // Numeric aliases still use the existing compatibility rules.
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
    unsigned typeIndirection;
    parseResult *program;
    Scope *scope;
    struct stmt_func *function;
    Type *implicitReceiver;
    Allocation *allocations;
} Context;
static Type nullValue = {.name = "null"};
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
static Type *canonical(Type *t) { return t->semanticBase ? t->semanticBase : t; }
static int interfaceFits(Context *, Type *, Type *, unsigned);
static int definedType(Type *t) { return t->declaration && !t->declaration->isAlias; }
static Type *representation(Type *t) {
    for (unsigned depth = 0; t->underlying && depth < 64; depth++) t = t->underlying;
    return canonical(t);
}
// Type arguments are constraints, not numeric assignment conversions.
static int matchesTypeConstraint(Type *expected, Type *actual) {
    if (canonical(expected) == &any || (canonical(actual) == &any && !definedType(expected)) || expected == &unknown || actual == &unknown) return 1;
    if (expected == actual) return 1;
    if (definedType(expected)) return definedType(actual) && expected->declaration == actual->declaration;
    if (expected->interface) return interfaceFits(expected->context, expected, actual, 0);
    if (definedType(actual)) return 0;
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
    if (expected->element || actual->element)
        return expected->element && actual->element && !strcmp(expected->name, actual->name) &&
               expected->count == actual->count && expected->bufferSize == actual->bufferSize &&
               matchesTypeConstraint(expected->element, actual->element);
    const char *a = expected->spelling ? expected->spelling : expected->name;
    const char *b = actual->spelling ? actual->spelling : actual->name;
    return !strcmp(a, b);
}
static int compatibleTypes(Type *a, Type *b) {
    a = canonical(a);
    // A boxed value does not establish the array shape or element type.
    // Require explicit narrowing before assigning it to an array destination.
    // Abstract generic parameters are checked when specialized, not boxed values.
    if (!strcmp(a->name, "dynamic-array") && b == &any)
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
    if (a->element || b->element)
        return a->element && b->element && !strcmp(a->name, b->name) &&
               ((a->element == &signedByte || b->element == &signedByte)
                    ? a->element == b->element : compatibleTypes(a->element, b->element)) &&
               (!strcmp(a->name, "buffer")
                    ? (a->bufferSize != 1 || (b->bufferSize == 1 && a->count == b->count))
                    : a->count == b->count);
    if (a->function || b->function)
        return a->function == b->function;
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
static Type *typeOf(Context *c, typeTokens span) {
    if (!span.nTokens)
        return &unknown;
    token t = span.tokens[0];
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
        v->mixed = span.tokens[0].type == MIXED;
        v->element = typeOf(c, arrayElement);
        if (v->mixed && canonical(v->element) != &any && !v->element->interface)
            error(c, t, "mixed array element type must be an interface");
        if (v->element == &nothing) error(c, t, "void is not an array element storage type");
        if (!strcmp(v->element->name, "buffer") && v->element->bufferSize == 2)
            error(c, t, "array elements must have a fixed storage size");
        return v;
    }
    if (t.type == MIXED) {
        error(c, t, "mixed requires an array type");
        return &unknown;
    }
    if (span.nTokens == 3 && t.type == INTERFACE && span.tokens[1].type == LEFT_CURLY_BRACE &&
        span.tokens[2].type == RIGHT_CURLY_BRACE) return &any;
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
    const char *n = t.lexeme ? t.lexeme : "any";
    Symbol *s = lookup(c, n);
    if (s) {
        if (!s->isType) {
            error(c, t, "expected a type");
            return &unknown;
        }
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
    Scope scope = {c->scope, NULL};
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
            fputs(t.lexeme ? t.lexeme : getTokenTypeStr(t.type), out);
    }
}
static void formatType(FILE *out, Type *t, unsigned depth) {
    if (definedType(t)) { fputs(t->declaration->displayName ? t->declaration->displayName : t->declaration->name.lexeme, out); return; }
    if (!t || depth > 32) {
        fputs("?", out);
        return;
    }
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
    } else if (!strcmp(t->name, "pointer")) {
        fputc('*', out);
        formatType(out, t->element, depth + 1);
    } else if (!strcmp(t->name, "dynamic-array") || !strcmp(t->name, "array")) {
        if (t->mixed) fputs("mixed ", out);
        formatType(out, t->element, depth + 1);
        fputs("[]", out);
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
    if (compatible(expected, actual))
        return;
    at = diagnosticLocation(c, at);
    diagnosticStart(at, "semantic", "error");
    fputs("type mismatch: expected ", stderr);
    formatType(stderr, expected, 0);
    fputs(", got ", stderr);
    formatType(stderr, actual, 0);
    diagnosticEnd(programSource(c->program, at.filename), at);
    c->errors++;
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
        if (array->element == &nullValue)
            error(c, at, "null array elements require an explicit pointer element type");
        v->inferredArray = 1;
        target = array;
    }
    if (!strcmp(target->name, "buffer") || !strcmp(target->name, "dynamic-array")) {
        if (!strcmp(target->name, "buffer") && target->bufferSize == 1 &&
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
            check(c, element, expectedExpression(c, element, item, at), item->location);
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
        Type *receiverOwner = owner && owner->isType ? owner->type : &unknown;
        if (!record && !definedType(receiverOwner)) error(c, base.tokens[0], "method receiver must name a struct or defined type");
        else if ((definedType(receiverOwner) ? 0 : record->nTypeParams) != f->nReceiverTypeParams)
            error(c, base.tokens[0], "receiver type parameter count mismatch");
        if (record || definedType(receiverOwner)) findMethod(c, receiverOwner, f->name.lexeme);
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
            bound->name = name.lexeme;
            if (record && i < record->nTypeParams)
                bound->constraint = record->typeParamConstraints[i];
            declare(c, name, bound, 1);
        }
    }
    for (size_t i = 0; i < f->nTypeParams; i++) {
        typeTokens span = f->typeParamConstraints[i];
        int compound = 0;
        for (size_t j = 0; j < span.nTokens; j++)
            if (span.tokens[j].type == TILDE || span.tokens[j].type == BITWISE_OR) compound = 1;
        Type *bound = compound ? &any : typeOf(c, span);
        if (bound == &any) {
            bound = allocate(c, sizeof(*bound));
            *bound = any;
            bound->semanticBase = &any; // An abstract parameter is resolved when specialized.
        }
        declare(c, f->typeParamNames[i], bound, 1);
    }
    for (size_t i = 0; i < f->nParams; i++) {
        Type *t = typeOf(c, f->paramTypeNames[i]);
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
                f->receiverType.nTokens ? typeOf(c, f->receiverType)
                : c->implicitReceiver   ? c->implicitReceiver
                                        : &unknown,
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
static int acceptsConstraint(Context *c, Type *owner, typeTokens span, Type *actual, unsigned depth) {
    if (depth > 32) return 0;
    if (actual->constraint.nTokens) {
        // Every alternative of an abstract receiver parameter must be accepted.
        typeTokens inherited = actual->constraint;
        for (size_t start = 0; start < inherited.nTokens;) {
            size_t end = constraintTermEnd(inherited, start);
            Type *term = parameter(c, owner, constraintTerm(inherited, start, end));
            if (!acceptsConstraint(c, owner, span, term, depth + 1)) return 0;
            start = end + 1;
        }
        return 1;
    }
    for (size_t start = 0; start < span.nTokens;) {
        size_t end = constraintTermEnd(span, start);
        int underlying = span.tokens[start].type == TILDE;
        Type *candidate = underlying ? representation(actual) : actual;
        Type *bound = parameter(c, owner, constraintTerm(span, start, end));
        if (matchesTypeConstraint(underlying ? representation(bound) : bound, candidate)) return 1;
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
    if (a->interface || b->interface) return a->interface && a->interface == b->interface;
    if (a->record || b->record) {
        if (a->record != b->record || a->nArguments != b->nArguments) return 0;
        for (size_t i = 0; i < a->nArguments; i++)
            if (!identicalTypes(a->arguments[i], b->arguments[i])) return 0;
        return 1;
    }
    if (a->element || b->element)
        return a->element && b->element && !strcmp(a->name, b->name) &&
               a->count == b->count && a->bufferSize == b->bufferSize &&
               a->bufferFamily == b->bufferFamily && a->mixed == b->mixed &&
               identicalTypes(a->element, b->element);
    const char *left = a->spelling ? a->spelling : a->name;
    const char *right = b->spelling ? b->spelling : b->name;
    if (!strcmp(left, "i64")) left = "int";
    if (!strcmp(right, "i64")) right = "int";
    if (!strcmp(left, "f64")) left = "float";
    if (!strcmp(right, "f64")) right = "float";
    return !strcmp(left, right);
}
static int interfaceFits(Context *c, Type *expected, Type *actual, unsigned depth) {
    if (depth > 32 || !c) return 0;
    if (actual == &unknown || canonical(actual) == &any || actual->interface) return 1;
    struct stmt_interface *contract = expected->interface;
    if (contract->nTypeParams) return 0;
    for (size_t i = 0; i < contract->nConstraints; i++) {
        typeTokens span = contract->constraints[i];
        int accepted = 0;
        for (size_t start = 0; start < span.nTokens;) {
            size_t end = constraintTermEnd(span, start);
            typeTokens term = constraintTerm(span, start, end);
            if (term.nTokens && (term.tokens[0].type == PLUS || term.tokens[0].type == EQUALS)) {
                term.tokens++; term.nTokens--;
            }
            int exclude = term.nTokens && term.tokens[0].type == MINUS;
            if (exclude) { term.tokens++; term.nTokens--; }
            Type *bound = typeOf(c, term);
            int fits = bound->interface ? interfaceFits(c, bound, actual, depth + 1)
                                       : matchesTypeConstraint(bound, actual);
            if (exclude ? !fits : fits) accepted = 1;
            start = end + 1;
        }
        if (!accepted) return 0;
    }
    Type *owner = actual;
    int pointer = !definedType(owner) && !strcmp(owner->name, "pointer");
    if (pointer) owner = owner->element;
    for (size_t i = 0; i < contract->nMethods; i++) {
        if (!owner || (!owner->record && !definedType(owner))) return 0;
        struct stmt_func *required = &contract->methods[i];
        struct stmt_func *provided = findMethod(c, owner, required->name.lexeme);
        if (!provided || provided->nTypeParams || required->nTypeParams ||
            provided->nParams != required->nParams || provided->nResults != required->nResults ||
            provided->isVariadic != required->isVariadic) return 0;
        typeTokens receiver = provided->receiverType;
        if (!pointer && (receiver.tokens[0].type == STAR || receiver.tokens[receiver.nTokens - 1].type == STAR)) return 0;
        Type callable = {.function = provided, .receiver = owner};
        for (size_t j = 0; j < required->nParams + required->nResults; j++) {
            Type *a = typeOf(c, j < required->nParams ? required->paramTypeNames[j]
                                                    : required->resultTypes[j - required->nParams]);
            Type *b = parameter(c, &callable, j < provided->nParams ? provided->paramTypeNames[j]
                                                    : provided->resultTypes[j - provided->nParams]);
            if (!identicalTypes(a, b)) return 0;
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
    while (!t->record && !definedType(t) && !strcmp(t->name, "pointer") && t->element)
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
static Type *expression(Context *c, expr *e) {
    token at = e ? e->location : (token){0};
    if (!e || !e->underlying)
        return &unknown;
    switch (e->type) {
    case EXPR_TYPE_ZERO: {
        struct expr_zero *v = e->underlying;
        Type *t = typeOf(c, v->type);
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
            function->name = "function"; function->function = f;
            return function;
        }
        if (!strcmp(t.lexeme, "true") || !strcmp(t.lexeme, "false"))
            return &boolean;
        Symbol *s = lookup(c, t.lexeme);
        if (!s) {
            error(c, t, fileExtern(c, t) ? "extern functions must be called with extern.name(...)" : "unresolved name");
            return &unknown;
        }
        return s->type;
    }
    case EXPR_TYPE_LITERAL: {
        struct expr_literal *v = e->underlying;
        if (v->type == LITERAL_TYPE_NULL) return &nullValue;
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
        t->element = typeOf(c, v->elementType);
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
        if (v->func.type == EXPR_TYPE_IDENTIFIER &&
            !((struct expr_identifier *)v->func.underlying)->externReference &&
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
                check(c, targets[i], values.items[i], commaItem(&v->left, i)->location);
            return &nothing;
        }
        Type *a = expression(c, &v->left);
        Type *b = v->operation == EXPR_ASSIGNMENT ? expectedExpression(c, a, &v->right, at)
                                                  : expression(c, &v->right);
        if (v->operation == EXPR_COMMA)
            return b;
        if ((v->operation == EXPR_ADD || v->operation == EXPR_ADD_EQUALS) &&
            (!strcmp(a->name, "dynamic-array") || !strcmp(b->name, "dynamic-array"))) {
            int leftArray = !strcmp(a->name, "dynamic-array");
            int rightArray = !strcmp(b->name, "dynamic-array");
            Type *result = NULL;
            // Equal array types concatenate; an array matching the other
            // operand's element type appends/prepends one nested value.
            if (leftArray && rightArray && matchesTypeConstraint(a, b))
                result = canonical(b->element) == &any ? b : a;
            else if (leftArray && compatible(a->element, b)) result = a;
            else if (rightArray && compatible(b->element, a)) result = b;
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
        else
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
        return a;
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
        if (representation(t) != &integer && representation(t) != &signedByte && representation(t) != &floating && t != &unknown && representation(t) != &any)
            error(c, at, "invalid unary operand");
        return t;
    }
    case EXPR_TYPE_TERNARY: {
        struct expr_ternary *v = e->underlying;
        check(c, &boolean, expression(c, &v->condition), v->condition.location);
        Type *a = expression(c, &v->ifTrue);
        Type *b = expression(c, &v->ifFalse);
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
        int aPointer = !strcmp(a->name, "pointer") || (!strcmp(a->name, "buffer") && !a->bufferSize);
        int bPointer = !strcmp(b->name, "pointer") || (!strcmp(b->name, "buffer") && !b->bufferSize);
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
    case EXPR_TYPE_ASSERTION: {
        struct expr_assertion *v = e->underlying;
        expression(c, &v->child);
        return v->isTypeGuard ? &any : typeOf(c, v->type);
    }
    case EXPR_TYPE_DOT: {
        struct expr_dot *v = e->underlying;
        Type *t = recordReceiver(expression(c, &v->parent));
        if (t == &nullValue)
            error(c, at, "null needs a typed pointer before member access");
        token field = ((struct expr_identifier *)v->field.underlying)->lexeme;
        if (definedType(t)) {
            struct stmt_func *method = findMethod(c, t, field.lexeme);
            if (method) {
                if (!exportedName(method->name.lexeme) && sourcePackage(c, at.filename) != sourcePackage(c, method->name.filename)) {
                    error(c, field, "method is not exported by its package"); return &unknown;
                }
                Type *callable = allocate(c, sizeof(*callable));
                callable->name = "function"; callable->function = method; callable->receiver = t;
                return callable;
            }
        }
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
            if (genericFamily(t)) {
                error(c, field, "narrow a generic family to concrete type arguments before member access");
                return &unknown;
            }
            for (size_t i = 0; i < t->record->nFields; i++)
                if (!strcmp(field.lexeme, t->record->fieldNames[i].lexeme))
                    return parameter(c, t, t->record->fieldTypes[i]);
            struct stmt_func *method = findMethod(c, t, field.lexeme);
            if (method) {
                if (!exportedName(method->name.lexeme) &&
                    sourcePackage(c, at.filename) != sourcePackage(c, method->name.filename)) {
                    error(c, field, "method is not exported by its package");
                    return &unknown;
                }
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
        if (f->receiverType.nTokens || f->isExtern)
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
        t->name = v->name.lexeme; t->declaration = v; t->declarationScope = c->scope;
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
    case STMT_VAR: {
        struct stmt_var *v = s->value;
        Type *declared = v->type.nTokens ? typeOf(c, v->type) : NULL;
        if (declared == &nothing) error(c, at, "void is not a variable storage type");
        if (declared && declared->record && declared->record->nVarParams && !v->nInitValues)
            error(c, v->vars[0], "struct with parameters requires an initializer");
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
        TypeList values =
            valueTypes(c, v->initValues, v->nInitValues, expected, v->nVars, v->vars[0], 1);
        if (v->nInitValues && values.count != v->nVars)
            error(c, v->vars[0], "initializer count mismatch");
        for (size_t i = 0; i < v->nVars; i++) {
            Type *a = i < values.count ? values.items[i] : &unknown;
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
        Symbol *record = lookup(c, v->name.lexeme);
        Type *receiverType = record ? record->type : &unknown;
        Scope scope = {c->scope, NULL};
        c->scope = &scope;
        for (size_t i = 0; i < v->nTypeParams; i++) {
            Type *bound = allocate(c, sizeof(*bound));
            *bound = any;
            bound->semanticBase = &any;
            declare(c, v->typeParamNames[i], bound, 1);
        }
        for (size_t i = 0; i < v->nFields; i++) {
            Type *fieldType = typeOf(c, v->fieldTypes[i]);
            if (fieldType == &nothing) error(c, v->fieldNames[i], "void is not a field storage type");
            Symbol *field = declare(c, v->fieldNames[i], fieldType, 0);
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
        Scope scope = {c->scope, NULL};
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
            check(c, &boolean, expression(c, v->condition), v->condition->location);
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
                check(c, t, expression(c, &v->cases[i].values[j]), v->cases[i].values[j].location);
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
    // Give extern declarations private semantic names while retaining C linkage.
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
    while (c.allocations) {
        Allocation *a = c.allocations;
        c.allocations = a->next;
        free(a->ptr);
        free(a);
    }
    return (semanticResult){c.errors};
}
