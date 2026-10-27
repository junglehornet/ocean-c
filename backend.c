#define _POSIX_C_SOURCE 200809L
#include "backend.h"
#include "array_type.h"
#include "buffer_type.h"
#include "value_list.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum { VOID, INT, I32, FLOAT, BOOL, RAW_POINTER, I8, BAD, BUFFER } Kind;
typedef struct {
    Kind kind;
    char operand[128];
} Value;
typedef struct Binding {
    const char *name;
    Value address;
    struct Binding *next;
} Binding;
typedef struct Scope {
    Binding *bindings;
    struct stmt_block *declarations;
    struct Scope *parent;
    Value savedStack;
    int hasStack;
} Scope;
typedef struct BufferKind {
    Kind kind, element;
    int shape; // 0: fixed buffer, 2: result tuple, 3: raw buffer pointer, 4:
               // array reference; 5: user struct; 6: typed pointer
    struct stmt_struct *record; // Shape 5: concrete user struct; type arguments are compiler-only.
    Kind *arguments;
    size_t nArguments;
    int layoutState;
    Kind *members;
    size_t count;
    char *ir;
    char *layout; // Inline byte-size header and payload layout for built-in arrays.
    struct BufferKind *next;
} BufferKind;
typedef struct TypeBinding {
    const char *name;
    Kind kind;
    struct TypeBinding *next;
} TypeBinding;
typedef struct MethodInstance {
    struct stmt_func *function;
    Kind record, receiver;
    Kind *arguments;
    size_t nArguments, nBindings;
    TypeBinding *bindings;
    unsigned id;
    struct MethodInstance *next;
} MethodInstance;
typedef struct {
    TypeBinding *typeBindings;
    MethodInstance *methods, *lastMethod, *method;
    unsigned nMethods;
    unsigned recordDepth;
    parseResult *program;
    FILE *out, *body, *slots;
    Scope *scope;
    struct stmt_func *function;
    unsigned next, label, strings;
    unsigned breakLabel, continueLabel;
    int terminated, errors;
    int boundsUsed;
    int dataCopyUsed;
    BufferKind *buffers;
    unsigned nBuffers;
    Scope *loopScope;
} Backend;
static BufferKind *bufferKind(Backend *b, Kind k) {
    for (BufferKind *t = b->buffers; t; t = t->next)
        if (t->kind == k)
            return t;
    return NULL;
}
static const char *irType(Backend *b, Kind k) {
    BufferKind *buffer = bufferKind(b, k);
    if (buffer)
        return buffer->ir;
    switch (k) {
    case VOID:
        return "void";
    case INT:
        return "i64";
    case I32:
        return "i32";
    case I8:
        return "i8";
    case FLOAT:
        return "double";
    case BOOL:
        return "i1";
    case RAW_POINTER:
        return "ptr";
    default:
        return "i64";
    }
}
static void fail(Backend *b, const char *message) {
    fprintf(stderr, "backend error: %s\n", message);
    b->errors++;
}
static void *checked(size_t size) {
    void *p = calloc(1, size);
    if (!p) {
        perror("backend allocation");
        exit(1);
    }
    return p;
}
static Value value(Kind k, const char *fmt, ...) {
    Value v = {.kind = k};
    va_list args;
    va_start(args, fmt);
    vsnprintf(v.operand, sizeof(v.operand), fmt, args);
    va_end(args);
    return v;
}
static Value temp(Backend *b, Kind k) { return value(k, "%%v%u", ++b->next); }
static Value moduleString(Backend *b, const char *text) {
    unsigned id = ++b->strings;
    size_t n = strlen(text);
    fprintf(b->out, "@.str%u = private unnamed_addr constant [%zu x i8] c\"", id, n + 1);
    for (size_t i = 0; i < n; i++)
        fprintf(b->out, "\\%02X", (unsigned char)text[i]);
    fprintf(b->out, "\\00\"\n");
    return value(RAW_POINTER, "@.str%u", id);
}
static token diagnosticLocation(Backend *b, token location) {
    if (location.line <= 0 && b->function)
        location = b->function->name;
    if (!location.filename)
        location.filename =
            (char *)(b->program->sourceFilename ? b->program->sourceFilename : "<source>");
    if (location.line <= 0)
        location.line = 1;
    if (location.column <= 0)
        location.column = 1;
    return location;
}
static char *sourceSnippet(Backend *b, token location) {
    const char *line = b->program->sourceText;
    if (!line) {
        char *result = checked(32);
        strcpy(result, "  <source unavailable>\n");
        return result;
    }
    for (long n = 1; n < location.line && *line; n++) {
        const char *next = strchr(line, '\n');
        if (!next) {
            line += strlen(line);
            break;
        }
        line = next + 1;
    }
    size_t length = strcspn(line, "\r\n");
    char prefix[64];
    size_t width = (size_t)snprintf(prefix, sizeof(prefix), " %ld | ", location.line);
    char *result = checked(2 * length + 2 * width + 16), *out = result;
    memcpy(out, prefix, width);
    out += width;
    memcpy(out, line, length);
    out += length;
    *out++ = '\n';
    for (size_t i = 0; i < width; i++)
        *out++ = prefix[i] == '|' ? '|' : ' ';
    size_t caret = (size_t)(location.column - 1);
    if (caret > length)
        caret = length;
    for (size_t i = 0; i < caret; i++)
        *out++ = line[i] == '\t' ? '\t' : ' ';
    *out++ = '^';
    *out++ = '\n';
    *out = 0;
    return result;
}
static Value pushTraceFrame(Backend *b, struct stmt_func *function, token location) {
    location = diagnosticLocation(b, location);
    Value frame = temp(b, RAW_POINTER), previous = temp(b, RAW_POINTER);
    fprintf(b->slots, "  %s = alloca %%__ocean.trace\n", frame.operand);
    fprintf(b->body, "  %s = load ptr, ptr @__ocean.trace_head\n", previous.operand);
    Value fields[] = {previous, moduleString(b, function->name.lexeme),
                      moduleString(b, location.filename), value(INT, "%ld", location.line),
                      value(INT, "%ld", location.column)};
    for (size_t i = 0; i < 5; i++) {
        Value slot = temp(b, RAW_POINTER);
        fprintf(b->body, "  %s = getelementptr %%__ocean.trace, ptr %s, i32 0, i32 %zu\n",
                slot.operand, frame.operand, i);
        fprintf(b->body, "  store %s %s, ptr %s\n", irType(b, fields[i].kind), fields[i].operand,
                slot.operand);
    }
    fprintf(b->body, "  store ptr %s, ptr @__ocean.trace_head\n", frame.operand);
    return previous;
}
static void popTraceFrame(Backend *b, Value previous) {
    fprintf(b->body, "  store ptr %s, ptr @__ocean.trace_head\n", previous.operand);
}
static Kind arrayKind(Backend *b, Kind element, size_t count) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (!v->shape && v->element == element && v->count == count)
            return v->kind;
    BufferKind *v = checked(sizeof(*v));
    v->kind = (Kind)(BUFFER + b->nBuffers++);
    v->element = element;
    v->count = count;
    size_t n = strlen(irType(b, element)) + 64;
    v->ir = checked(n);
    snprintf(v->ir, n, "[%zu x %s]", count, irType(b, element));
    v->next = b->buffers;
    b->buffers = v;
    return v->kind;
}
static Kind structKind(Backend *b, int shape, Kind element, Kind *members, size_t count) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (v->shape == shape && v->element == element && v->count == count &&
            !memcmp(v->members, members, sizeof(Kind) * count))
            return v->kind;
    BufferKind *v = checked(sizeof(*v));
    v->kind = (Kind)(BUFFER + b->nBuffers++);
    v->shape = shape;
    v->element = element;
    v->count = count;
    v->members = checked(sizeof(Kind) * count);
    memcpy(v->members, members, sizeof(Kind) * count);
    size_t n = 8;
    for (size_t i = 0; i < count; i++)
        n += strlen(irType(b, members[i])) + 2;
    v->ir = checked(n);
    strcpy(v->ir, "{ ");
    for (size_t i = 0; i < count; i++) {
        if (i)
            strcat(v->ir, ", ");
        strcat(v->ir, irType(b, members[i]));
    }
    strcat(v->ir, " }");
    v->next = b->buffers;
    b->buffers = v;
    return v->kind;
}
static Kind pointerBufferKind(Backend *b, Kind element) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (v->shape == 3 && v->element == element)
            return v->kind;
    BufferKind *v = checked(sizeof(*v));
    v->kind = (Kind)(BUFFER + b->nBuffers++);
    v->shape = 3;
    v->element = element;
    v->ir = checked(4);
    strcpy(v->ir, "ptr");
    v->next = b->buffers;
    b->buffers = v;
    return v->kind;
}
static Kind arrayReferenceKind(Backend *b, Kind element) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (v->shape == 4 && v->element == element)
            return v->kind;
    BufferKind *v = checked(sizeof(*v));
    v->kind = (Kind)(BUFFER + b->nBuffers++);
    v->shape = 4;
    v->element = element;
    v->ir = checked(4);
    strcpy(v->ir, "ptr");
    size_t n = strlen(irType(b, element)) + 32;
    v->layout = checked(n);
    snprintf(v->layout, n, "{ i64, [0 x %s] }", irType(b, element));
    v->next = b->buffers;
    b->buffers = v;
    return v->kind;
}
static Kind type(Backend *, typeTokens);
static struct stmt_struct *findRecord(Backend *b, const char *name) {
    for (Scope *scope = b->scope; scope; scope = scope->parent) {
        if (!scope->declarations) continue;
        for (size_t i = 0; i < scope->declarations->nStmts; i++) {
            stmt *s = &scope->declarations->stmts[i];
            if (s->type == STMT_STRUCT && !strcmp(((struct stmt_struct *)s->value)->name.lexeme, name))
                return s->value;
        }
    }
    for (size_t i = 0; i < b->program->nStatements; i++) {
        stmt *s = &b->program->statements[i];
        if (s->type == STMT_STRUCT && !strcmp(((struct stmt_struct *)s->value)->name.lexeme, name))
            return s->value;
    }
    return NULL;
}
static Kind pointerKind(Backend *b, Kind element) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (v->shape == 6 && v->element == element)
            return v->kind;
    BufferKind *v = checked(sizeof(*v));
    v->kind = (Kind)(BUFFER + b->nBuffers++);
    v->shape = 6;
    v->element = element;
    v->ir = checked(4);
    strcpy(v->ir, "ptr");
    v->next = b->buffers;
    b->buffers = v;
    return v->kind;
}
static int validateLayout(Backend *, BufferKind *);
static Kind recordKind(Backend *b, struct stmt_struct *record, Kind *arguments, size_t count) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (v->record == record && v->nArguments == count &&
            (!count || !memcmp(v->arguments, arguments, sizeof(Kind) * count)))
            return v->layoutState == 3 ? BAD : v->kind;
    if (count != record->nTypeParams) {
        fail(b, "struct type argument count mismatch");
        return BAD;
    }
    if (b->recordDepth >= 64) {
        fail(b, "struct specialization nesting is too deep");
        return BAD;
    }
    BufferKind *v = checked(sizeof(*v));
    v->kind = (Kind)(BUFFER + b->nBuffers++);
    v->shape = 5;
    v->record = record;
    v->nArguments = count;
    v->arguments = checked(sizeof(Kind) * (count + 1));
    if (count) memcpy(v->arguments, arguments, sizeof(Kind) * count);
    v->count = record->nFields;
    v->members = checked(sizeof(Kind) * (v->count + 1));
    v->ir = checked(48);
    snprintf(v->ir, 48, "%%ocean.struct.%u", (unsigned)v->kind);
    v->next = b->buffers;
    b->buffers = v; // Register before resolving fields, allowing recursive pointers.
    TypeBinding *previous = b->typeBindings;
    TypeBinding *bindings = checked(sizeof(*bindings) * (count + 1));
    for (size_t i = 0; i < count; i++) {
        bindings[i] = (TypeBinding){record->typeParamNames[i].lexeme, arguments[i], b->typeBindings};
        b->typeBindings = &bindings[i];
    }
    b->recordDepth++;
    for (size_t i = 0; i < v->count; i++) {
        typeTokens element, dimension;
        if (bufferTypeParts(record->fieldTypes[i], &element, &dimension) &&
            bufferDimensionKind(dimension) == 2) {
            fail(b, "runtime-sized inline struct fields are unsupported; use an unsized buffer or array reference");
            v->members[i] = BAD;
        } else v->members[i] = type(b, record->fieldTypes[i]);
    }
    b->recordDepth--;
    b->typeBindings = previous;
    free(bindings);
    if (!b->recordDepth && !validateLayout(b, v)) return BAD;
    return v->kind;
}
static size_t recordField(BufferKind *t, const char *name) {
    for (size_t i = 0; i < t->count; i++)
        if (!strcmp(t->record->fieldNames[i].lexeme, name))
            return i;
    return t->count;
}
static Kind type(Backend *b, typeTokens t) {
    if (!t.nTokens)
        return VOID;
    if (t.nTokens > 1 && t.tokens[t.nTokens - 1].type == STAR)
        return pointerKind(b, type(b, (typeTokens){t.nTokens - 1, t.tokens}));
    typeTokens arrayElement;
    if (arrayElementType(t, &arrayElement))
        return arrayReferenceKind(b, type(b, arrayElement));
    if (isBufferType(t)) {
        typeTokens element;
        typeTokens dimension;
        if (!bufferTypeParts(t, &element, &dimension) || bufferDimensionKind(dimension) < 0) {
            fail(b, "expected buffer<T>[size] with a positive size, or buffer<T>[]");
            return BAD;
        }
        Kind k = type(b, element);
        return k == BAD || k == VOID ? BAD
               : bufferDimensionKind(dimension) == 1
                   ? arrayKind(b, k, (size_t)dimension.tokens[0].numValue.integer)
                   : pointerBufferKind(b, k);
    }
    if (t.tokens[0].type == STAR)
        return pointerKind(b, type(b, (typeTokens){t.nTokens - 1, t.tokens + 1}));
    if (t.nTokens == 1)
        for (TypeBinding *binding = b->typeBindings; binding; binding = binding->next)
            if (!strcmp(binding->name, t.tokens[0].lexeme))
                return binding->kind;
    struct stmt_struct *record = findRecord(b, t.tokens[0].lexeme);
    if (record) {
        typeTokens *spans = NULL;
        size_t count = 0;
        if (t.nTokens > 1 && !parseTypeApplicationArguments(t, &spans, &count)) {
            fail(b, "invalid struct type arguments");
            return BAD;
        }
        Kind *arguments = checked(sizeof(Kind) * (count + 1));
        for (size_t i = 0; i < count; i++) {
            arguments[i] = type(b, spans[i]);
            free(spans[i].tokens);
        }
        free(spans);
        Kind result = recordKind(b, record, arguments, count);
        free(arguments);
        return result;
    }
    if (t.nTokens != 1) {
        fail(b, "only scalar types are supported");
        return BAD;
    }
    const char *s = t.tokens[0].lexeme;
    if (!strcmp(s, "int") || !strcmp(s, "i64"))
        return INT;
    if (!strcmp(s, "i8"))
        return I8;
    if (!strcmp(s, "i32"))
        return I32;
    if (!strcmp(s, "float") || !strcmp(s, "f64"))
        return FLOAT;
    if (!strcmp(s, "bool"))
        return BOOL;
    if (!strcmp(s, "string"))
        return arrayReferenceKind(b, I8);
    fail(b, "unsupported type (supported: int, i64, i32, i8, float, f64, bool, string)");
    return BAD;
}
static Kind resultType(Backend *b, struct stmt_func *f) {
    if (f->nResults <= 1)
        return f->nResults ? type(b, f->resultTypes[0]) : VOID;
    Kind *members = checked(sizeof(Kind) * f->nResults);
    for (size_t i = 0; i < f->nResults; i++)
        members[i] = type(b, f->resultTypes[i]);
    Kind result = structKind(b, 2, VOID, members, f->nResults);
    free(members);
    return result;
}
static Kind parameterKind(Backend *b, struct stmt_func *f, size_t i) {
    Kind k = type(b, f->paramTypeNames[i]);
    return f->isVariadic && !f->isExtern && i + 1 == f->nParams ? arrayReferenceKind(b, k) : k;
}

static struct stmt_func *findFunction(Backend *b, const char *name) {
    for (size_t i = 0; i < b->program->nStatements; i++)
        if (b->program->statements[i].type == STMT_FUNC) {
            struct stmt_func *f = b->program->statements[i].value;
            if (!f->receiverType.nTokens && !strcmp(f->name.lexeme, name))
                return f;
        }
    return NULL;
}
/* Internal functions are mangled so the C ABI entry point can wrap Ocean main. */
static void functionName(FILE *out, struct stmt_func *f) {
    fprintf(out, "@\"%s%s\"", f->isExtern ? "" : "ocean.", f->name.lexeme);
}
static void methodName(FILE *out, MethodInstance *method) {
    fprintf(out, "@\"ocean.method.%u.%s\"", method->id, method->function->name.lexeme);
}
static MethodInstance *methodInstance(Backend *b, Kind receiver, const char *name,
                                      typeTokens *typeArguments, size_t count) {
    BufferKind *record = bufferKind(b, receiver);
    while (record && record->shape == 6) record = bufferKind(b, record->element);
    if (!record || record->shape != 5) {
        fail(b, "method receiver must be a struct or struct pointer");
        return NULL;
    }
    struct stmt_func *method = NULL;
    for (size_t i = 0; i < b->program->nStatements; i++) {
        stmt *s = &b->program->statements[i];
        if (s->type != STMT_FUNC) continue;
        struct stmt_func *f = s->value;
        if (!f->receiverType.nTokens || strcmp(f->name.lexeme, name)) continue;
        token owner = f->receiverType.tokens[f->receiverType.tokens[0].type == STAR ? 1 : 0];
        if (!strcmp(owner.lexeme, record->record->name.lexeme)) {
            if (method) fail(b, "duplicate method declaration");
            method = f;
        }
    }
    if (!method) { fail(b, "unknown method"); return NULL; }
    if (method->nTypeParams != count) {
        fail(b, "method type argument count mismatch; provide explicit method type arguments");
        return NULL;
    }
    Kind *arguments = checked(sizeof(Kind) * (count + 1));
    for (size_t i = 0; i < count; i++) arguments[i] = type(b, typeArguments[i]);
    for (MethodInstance *m = b->methods; m; m = m->next) {
        if (m->function != method || m->record != record->kind || m->nArguments != count) continue;
        size_t i = 0;
        while (i < count && m->arguments[i] == arguments[i]) i++;
        if (i == count) { free(arguments); return m; }
    }
    if (b->nMethods >= 256) {
        free(arguments);
        fail(b, "method specialization limit exceeded");
        return NULL;
    }
    MethodInstance *m = checked(sizeof(*m));
    m->function = method;
    m->record = record->kind;
    int pointer = method->receiverType.tokens[0].type == STAR ||
        method->receiverType.tokens[method->receiverType.nTokens - 1].type == STAR;
    m->receiver = pointer ? pointerKind(b, record->kind) : record->kind;
    m->id = ++b->nMethods;
    m->arguments = arguments;
    m->nArguments = count;
    m->nBindings = method->nReceiverTypeParams + count;
    m->bindings = checked(sizeof(TypeBinding) * (m->nBindings + 1));
    for (size_t i = 0; i < m->nBindings; i++) {
        size_t own = i < method->nReceiverTypeParams ? 0 : i - method->nReceiverTypeParams;
        m->bindings[i] = (TypeBinding){
            i < method->nReceiverTypeParams ? method->receiverTypeParamNames[i].lexeme
                                            : method->typeParamNames[own].lexeme,
            i < method->nReceiverTypeParams
                ? (i < record->nArguments ? record->arguments[i] : BAD) : arguments[own],
            i + 1 < m->nBindings ? &m->bindings[i + 1] : NULL};
    }
    if (b->lastMethod) b->lastMethod->next = m;
    else b->methods = m;
    b->lastMethod = m;
    return m;
}
// An explicitly instantiated method still has the same receiver expression.
static struct expr_dot *methodCallee(expr *callee, typeTokens **arguments, size_t *count) {
    *arguments = NULL;
    *count = 0;
    if (callee->type == EXPR_TYPE_INSTANTIATION) {
        struct expr_instantiation *instance = callee->underlying;
        *arguments = instance->typeArguments;
        *count = instance->nTypeArguments;
        callee = &instance->parent;
    }
    return callee->type == EXPR_TYPE_DOT ? callee->underlying : NULL;
}
static Kind methodResult(Backend *b, MethodInstance *m) {
    if (!m) return BAD;
    TypeBinding *previous = b->typeBindings;
    b->typeBindings = m->nBindings ? m->bindings : NULL;
    Kind result = resultType(b, m->function);
    b->typeBindings = previous;
    return result;
}
static Value find(Backend *b, const char *name) {
    for (Scope *s = b->scope; s; s = s->parent)
        for (Binding *v = s->bindings; v; v = v->next)
            if (!strcmp(v->name, name))
                return v->address;
    fail(b, "value has no local storage");
    return value(BAD, "null");
}
/* Keep all allocas in the entry block, including locals declared inside loops. */
static Value slot(Backend *b, Kind k) {
    Value v = temp(b, k);
    fprintf(b->slots, "  %s = alloca %s\n", v.operand, irType(b, k));
    return v;
}
static void bind(Backend *b, const char *name, Value address) {
    Binding *v = checked(sizeof(*v));
    *v = (Binding){name, address, b->scope->bindings};
    b->scope->bindings = v;
}
static void clearScope(Scope *s) {
    while (s->bindings) {
        Binding *v = s->bindings;
        s->bindings = v->next;
        free(v);
    }
}
static int integral(Kind k) { return k == INT || k == I32 || k == I8; }
static Value convert(Backend *b, Value v, Kind k) {
    if (v.kind == k)
        return v;
    Value r = temp(b, k);
    const char *op = NULL;
    if (integral(v.kind) && k == FLOAT)
        op = "sitofp";
    if (v.kind == FLOAT && integral(k))
        op = "fptosi";
    if (v.kind == BOOL && integral(k))
        op = "zext";
    if (integral(v.kind) && integral(k))
        op = (v.kind == I8 || (v.kind == I32 && k == INT)) ? "sext" : "trunc";
    if (!op) {
        fail(b, "unsupported scalar conversion or type mismatch");
        return value(BAD, "0");
    }
    fprintf(b->body, "  %s = %s %s %s to %s\n", r.operand, op, irType(b, v.kind), v.operand,
            irType(b, k));
    return r;
}
static Value coerce(Backend *b, Value v, Kind expected);
static void store(Backend *b, Value address, Value v) {
    BufferKind *target = bufferKind(b, address.kind);
    if (target && target->shape == 3)
        v = coerce(b, v, address.kind);
    if (integral(v.kind) && integral(address.kind))
        v = convert(b, v, address.kind);
    if (v.kind != address.kind) {
        fail(b, "storage type mismatch");
        return;
    }
    fprintf(b->body, "  store %s %s, ptr %s\n", irType(b, v.kind), v.operand, address.operand);
}
static Value load(Backend *b, Value address) {
    Value v = temp(b, address.kind);
    fprintf(b->body, "  %s = load %s, ptr %s\n", v.operand, irType(b, v.kind), address.operand);
    return v;
}
static Value coerce(Backend *b, Value v, Kind expected) {
    BufferKind *target = bufferKind(b, expected), *source = bufferKind(b, v.kind);
    if (target && target->shape == 3 && source && !source->shape &&
        source->element == target->element) {
        Value storage = slot(b, v.kind);
        fprintf(b->body, "  store %s %s, ptr %s\n", source->ir, v.operand, storage.operand);
        return value(expected, "%s", storage.operand);
    }
    if (integral(v.kind) && integral(expected))
        v = convert(b, v, expected);
    if (v.kind != expected)
        fail(b, "value type mismatch");
    return v;
}
static Value extract(Backend *b, Value aggregate, size_t index) {
    BufferKind *t = bufferKind(b, aggregate.kind);
    if (!t || !t->shape || index >= t->count) {
        fail(b, "invalid aggregate extraction");
        return value(BAD, "0");
    }
    Value result = temp(b, t->members[index]);
    fprintf(b->body, "  %s = extractvalue %s %s, %zu\n", result.operand, t->ir, aggregate.operand,
            index);
    return result;
}
static Value insert(Backend *b, Value aggregate, Value member, size_t index) {
    BufferKind *t = bufferKind(b, aggregate.kind);
    Kind expected = t->shape ? t->members[index] : t->element;
    member = coerce(b, member, expected);
    Value result = temp(b, aggregate.kind);
    fprintf(b->body, "  %s = insertvalue %s %s, %s %s, %zu\n", result.operand, t->ir,
            aggregate.operand, irType(b, member.kind), member.operand, index);
    return result;
}
static Value storageSize(Backend *b, Kind k) {
    if (k == VOID || k == BAD) {
        fail(b, "type has no storage size");
        return value(BAD, "0");
    }
    Value end = temp(b, RAW_POINTER), result = temp(b, INT);
    fprintf(b->body, "  %s = getelementptr %s, ptr null, i64 1\n", end.operand, irType(b, k));
    fprintf(b->body, "  %s = ptrtoint ptr %s to i64\n", result.operand, end.operand);
    return result;
}
static Value arrayData(Backend *b, Value array) {
    BufferKind *t = bufferKind(b, array.kind);
    Value pointer = temp(b, pointerBufferKind(b, t->element));
    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, i32 1\n", pointer.operand, t->layout,
            array.operand);
    return pointer;
}
static Value allocateArray(Backend *b, Kind element, Value count) {
    count = coerce(b, count, INT);
    Value elementSize = storageSize(b, element), bytes = temp(b, INT);
    fprintf(b->body, "  %s = mul i64 %s, %s\n", bytes.operand, count.operand, elementSize.operand);
    Kind kind = arrayReferenceKind(b, element);
    Value dataOffset = arrayData(b, value(kind, "null")), headerSize = temp(b, INT);
    fprintf(b->body, "  %s = ptrtoint ptr %s to i64\n", headerSize.operand, dataOffset.operand);
    Value total = temp(b, INT), result = temp(b, kind);
    fprintf(b->body, "  %s = add i64 %s, %s\n", total.operand, headerSize.operand, bytes.operand);
    fprintf(b->body, "  %s = alloca i8, i64 %s, align 8\n", result.operand, total.operand);
    store(b, value(INT, "%s", result.operand), bytes);
    return result;
}
/* sizeof inspects storage types without evaluating its operand. */
static Kind expressionKind(Backend *b, expr *e) {
    if (!e || !e->underlying)
        return BAD;
    switch (e->type) {
    case EXPR_TYPE_IDENTIFIER: {
        char *name = ((struct expr_identifier *)e->underlying)->lexeme.lexeme;
        if (!strcmp(name, "true") || !strcmp(name, "false"))
            return BOOL;
        return find(b, name).kind;
    }
    case EXPR_TYPE_LITERAL: {
        enum literalType t = ((struct expr_literal *)e->underlying)->type;
        return t == LITERAL_TYPE_STRING ? arrayReferenceKind(b, I8) : t == LITERAL_TYPE_FLOAT ? FLOAT : INT;
    }
    case EXPR_TYPE_COMPOSITE: {
        struct expr_composite *v = e->underlying;
        if (v->literalType.nTokens)
            return type(b, v->literalType);
        return v->inferredArray && v->nEntries
                   ? arrayReferenceKind(b, expressionKind(b, &v->values[0]))
                   : BAD;
    }
    case EXPR_TYPE_ARRAY_NEW:
        return arrayReferenceKind(b,
                                  type(b, ((struct expr_array_new *)e->underlying)->elementType));
    case EXPR_TYPE_INDEX: {
        BufferKind *t =
            bufferKind(b, expressionKind(b, &((struct expr_index *)e->underlying)->parent));
        return t ? t->element : BAD;
    }
    case EXPR_TYPE_DOT: {
        struct expr_dot *v = e->underlying;
        BufferKind *t = bufferKind(b, expressionKind(b, &v->parent));
        while (t && t->shape == 6)
            t = bufferKind(b, t->element);
        if (t && t->shape == 5) {
            size_t i = recordField(t, ((struct expr_identifier *)v->field.underlying)->lexeme.lexeme);
            return i < t->count ? t->members[i] : BAD;
        }
        if (t && t->shape == 4)
            return !strcmp(((struct expr_identifier *)v->field.underlying)->lexeme.lexeme, "size")
                       ? INT
                       : pointerBufferKind(b, t->element);
        return BAD;
    }
    case EXPR_TYPE_TYPECAST:
        return type(b, ((struct expr_typecast *)e->underlying)->type);
    case EXPR_TYPE_CALL: {
        struct expr_call *v = e->underlying;
        typeTokens *typeArguments;
        size_t count;
        struct expr_dot *dot = methodCallee(&v->func, &typeArguments, &count);
        if (dot) {
            return methodResult(b, methodInstance(b, expressionKind(b, &dot->parent),
                ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme, typeArguments, count));
        }
        if (v->func.type != EXPR_TYPE_IDENTIFIER)
            return BAD;
        char *name = ((struct expr_identifier *)v->func.underlying)->lexeme.lexeme;
        struct stmt_func *f = findFunction(b, name);
        return f ? resultType(b, f) : !strcmp(name, "len") ? INT : BAD;
    }
    case EXPR_TYPE_UNARY: {
        struct expr_unary *v = e->underlying;
        if (v->operation == UNARY_ADDRESS_OF)
            return pointerKind(b, expressionKind(b, &v->child));
        if (v->operation == UNARY_DEREFERENCE) {
            BufferKind *t = bufferKind(b, expressionKind(b, &v->child));
            return t && t->shape == 6 ? t->element : BAD;
        }
        return v->operation == UNARY_SIZEOF        ? INT
               : v->operation == UNARY_LOGICAL_NOT ? BOOL
                                                   : expressionKind(b, &v->child);
    }
    case EXPR_TYPE_BINARY: {
        struct expr_binary *v = e->underlying;
        if (v->operation == EXPR_COMMA)
            return expressionKind(b, &v->right);
        if ((v->operation >= EXPR_EQUALITY && v->operation <= EXPR_GREATER_EQUALS) ||
            v->operation == EXPR_LOGICAL_OR || v->operation == EXPR_LOGICAL_AND)
            return BOOL;
        Kind a = expressionKind(b, &v->left), c = expressionKind(b, &v->right);
        if (v->operation == EXPR_ASSIGNMENT || v->operation >= EXPR_ADD_EQUALS)
            return a;
        return a == FLOAT || c == FLOAT ? FLOAT : a == INT || c == INT ? INT : a;
    }
    case EXPR_TYPE_TERNARY:
        return expressionKind(b, &((struct expr_ternary *)e->underlying)->ifTrue);
    default:
        return BAD;
    }
}
static void label(Backend *b, unsigned n) {
    fprintf(b->body, "b%u:\n", n);
    b->terminated = 0;
}
static void jump(Backend *b, unsigned n) {
    if (!b->terminated)
        fprintf(b->body, "  br label %%b%u\n", n);
    b->terminated = 1;
}
static void branch(Backend *b, Value v, unsigned yes, unsigned no) {
    if (v.kind != BOOL)
        fail(b, "condition must have bool type");
    fprintf(b->body, "  br i1 %s, label %%b%u, label %%b%u\n", v.operand, yes, no);
    b->terminated = 1;
}
static Value expression(Backend *, expr *);
static Value address(Backend *b, expr *e);
static Value emptyValue(Backend *, Kind);
static Value expectedValue(Backend *, expr *, Kind);
static void copyArrayLiteral(Backend *, Value, struct expr_composite *, token);
static Value constructComposite(Backend *b, expr *e, Kind kind) {
    struct expr_composite *v = e->underlying;
    BufferKind *t = bufferKind(b, kind);
    if (!t) {
        fail(b, "literal needs a supported array, buffer, or struct type");
        return value(BAD, "0");
    }
    if (t->shape == 4) {
        Value array = allocateArray(b, t->element, value(INT, "%zu", v->nEntries));
        copyArrayLiteral(b, array, v, v->location);
        return array;
    }
    if (t->shape == 3) {
        Kind backing = arrayKind(b, t->element, v->nEntries);
        Value aggregate = constructComposite(b, e, backing), storage = slot(b, backing);
        store(b, storage, aggregate);
        return value(kind, "%s", storage.operand);
    }
    if (t->shape == 5) {
        Value aggregate = emptyValue(b, kind);
        for (size_t i = 0; i < v->nEntries; i++) {
            size_t field = v->keys[i].type
                ? recordField(t, ((struct expr_identifier *)v->keys[i].underlying)->lexeme.lexeme) : i;
            if (field >= t->count) {
                fail(b, "unknown struct field");
                return value(BAD, "0");
            }
            aggregate = insert(b, aggregate,
                               expectedValue(b, &v->values[i], t->members[field]), field);
        }
        return aggregate;
    }
    if (!t->shape) {
        if (v->nEntries > t->count) {
            fail(b, "too many elements in buffer initializer");
            return value(BAD, "0");
        }
        Value aggregate = emptyValue(b, kind);
        for (size_t i = 0; i < v->nEntries; i++) {
            if (v->keys[i].type)
                fail(b, "buffer literals require positional elements");
            Value element = expectedValue(b, &v->values[i], t->element);
            aggregate = insert(b, aggregate, coerce(b, element, t->element), i);
        }
        return aggregate;
    }
    fail(b, "unsupported composite literal type");
    return value(BAD, "0");
}
static Value expectedValue(Backend *b, expr *e, Kind expected) {
    if (e->type == EXPR_TYPE_COMPOSITE) {
        struct expr_composite *literal = e->underlying;
        Kind actual = literal->literalType.nTokens ? type(b, literal->literalType)
                      : literal->inferredArray && literal->nEntries
                          ? arrayReferenceKind(b, expressionKind(b, &literal->values[0]))
                          : expected;
        Value result = constructComposite(b, e, actual);
        return expected == BAD ? result : coerce(b, result, expected);
    }
    BufferKind *t = bufferKind(b, expected);
    if (t && t->shape == 3) {
        BufferKind *source = bufferKind(b, expressionKind(b, e));
        if (source && !source->shape && source->element == t->element &&
            (e->type == EXPR_TYPE_IDENTIFIER || e->type == EXPR_TYPE_INDEX ||
             e->type == EXPR_TYPE_DOT)) {
            Value storage = address(b, e);
            return value(expected, "%s", storage.operand);
        }
        return coerce(b, expression(b, e), expected);
    }
    return expression(b, e);
}
typedef struct {
    Value *items;
    size_t count;
} ValueList;
static ValueList evaluateValues(Backend *b, expr *items, size_t count, Kind *expected,
                                size_t nExpected) {
    ValueList result = {.items = checked(sizeof(Value) * (count + 1)), .count = count};
    for (size_t i = 0; i < count; i++) {
        Value v = expectedValue(b, &items[i], i < nExpected ? expected[i] : BAD);
        BufferKind *t = bufferKind(b, v.kind);
        if (t && t->shape == 2) {
            if (count != 1) {
                fail(b, "multiple-return call must be the only expression in a value list");
            } else {
                free(result.items);
                result.count = t->count;
                result.items = checked(sizeof(Value) * t->count);
                for (size_t j = 0; j < t->count; j++)
                    result.items[j] = extract(b, v, j);
                return result;
            }
        }
        result.items[i] = v;
    }
    return result;
}
static void checkArrayIndex(Backend *b, Value array, Value index, token location) {
    b->boundsUsed = 1;
    BufferKind *t = bufferKind(b, array.kind);
    Value bytes = load(b, value(INT, "%s", array.operand)), stride = storageSize(b, t->element);
    Value length = temp(b, INT), nonnegative = temp(b, BOOL), below = temp(b, BOOL),
          valid = temp(b, BOOL);
    fprintf(b->body, "  %s = sdiv i64 %s, %s\n", length.operand, bytes.operand, stride.operand);
    fprintf(b->body, "  %s = icmp sge i64 %s, 0\n", nonnegative.operand, index.operand);
    fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", below.operand, index.operand, length.operand);
    fprintf(b->body, "  %s = and i1 %s, %s\n", valid.operand, nonnegative.operand, below.operand);
    unsigned good = ++b->label, bad = ++b->label;
    branch(b, valid, good, bad);
    label(b, bad);
    location = diagnosticLocation(b, location);
    Value filename = moduleString(b, location.filename);
    char *text = sourceSnippet(b, location);
    Value snippet = moduleString(b, text);
    free(text);
    fprintf(b->body,
            "  call void @__ocean.bounds_fail(i64 %s, i64 %s, ptr %s, i64 %ld, i64 %ld, ptr %s)\n",
            index.operand, length.operand, filename.operand, location.line, location.column,
            snippet.operand);
    fprintf(b->body, "  unreachable\n");
    b->terminated = 1;
    label(b, good);
}
static Value arrayLength(Backend *b, Value array) {
    BufferKind *t = bufferKind(b, array.kind);
    Value bytes = load(b, value(INT, "%s", array.operand)), stride = storageSize(b, t->element),
          length = temp(b, INT);
    fprintf(b->body, "  %s = sdiv i64 %s, %s\n", length.operand, bytes.operand, stride.operand);
    return length;
}
static int hasNonzeroEmptyValue(Backend *b, Kind kind) {
    BufferKind *t = bufferKind(b, kind);
    if (t && t->shape == 5) {
        for (size_t i = 0; i < t->count; i++)
            if (hasNonzeroEmptyValue(b, t->members[i])) return 1;
    }
    return t && (t->shape == 4 || (!t->shape && hasNonzeroEmptyValue(b, t->element)));
}
static Value emptyValue(Backend *b, Kind kind) {
    if (kind == FLOAT)
        return value(FLOAT, "0x0000000000000000");
    BufferKind *t = bufferKind(b, kind);
    if (t && t->shape == 4)
        return allocateArray(b, t->element, value(INT, "0"));
    if (t && t->shape == 5) {
        Value result = value(kind, "zeroinitializer");
        for (size_t i = 0; i < t->count; i++)
            if (hasNonzeroEmptyValue(b, t->members[i]))
                result = insert(b, result, emptyValue(b, t->members[i]), i);
        return result;
    }
    if (t && !t->shape) {
        if (!hasNonzeroEmptyValue(b, t->element))
            return value(kind, "zeroinitializer");
        Value result = value(kind, "zeroinitializer");
        Value item = emptyValue(b, t->element);
        for (size_t i = 0; i < t->count; i++)
            result = insert(b, result, item, i);
        return result;
    }
    return value(kind, t ? "zeroinitializer" : "0");
}
static void copyArrayLiteral(Backend *, Value, struct expr_composite *, token);
static Value dataLiteralItem(Backend *b, Kind expected, expr *item, token location) {
    BufferKind *t = bufferKind(b, expected);
    if (t && item->type == EXPR_TYPE_COMPOSITE) {
        struct expr_composite *literal = item->underlying;
        if (literal->literalType.nTokens)
            return coerce(b, expectedValue(b, item, expected), expected);
        if (t->shape == 5)
            return constructComposite(b, item, expected);
        if (t->shape == 4) {
            Value array = allocateArray(b, t->element, value(INT, "%zu", literal->nEntries));
            copyArrayLiteral(b, array, literal, location);
            return array;
        }
        if (!t->shape && !literal->type.type) {
            Value result = emptyValue(b, expected);
            if (literal->nEntries > t->count) {
                fail(b, "too many buffer literal elements");
                return value(BAD, "0");
            }
            for (size_t i = 0; i < literal->nEntries; i++)
                result = insert(b, result,
                                dataLiteralItem(b, t->element, &literal->values[i], location), i);
            return result;
        }
    }
    return coerce(b, expectedValue(b, item, expected), expected);
}
static void copyArrayLiteral(Backend *b, Value array, struct expr_composite *literal,
                             token location) {
    b->boundsUsed = 1;
    BufferKind *t = bufferKind(b, array.kind);
    Value length = arrayLength(b, array), fits = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp sle i64 %zu, %s\n", fits.operand, literal->nEntries,
            length.operand);
    unsigned good = ++b->label, bad = ++b->label;
    branch(b, fits, good, bad);
    label(b, bad);
    location = diagnosticLocation(b, location);
    Value filename = moduleString(b, location.filename);
    char *text = sourceSnippet(b, location);
    Value snippet = moduleString(b, text);
    free(text);
    fprintf(b->body,
            "  call void @__ocean.array_literal_fail(i64 %zu, i64 %s, ptr %s, i64 %ld, i64 %ld, "
            "ptr %s)\n",
            literal->nEntries, length.operand, filename.operand, location.line, location.column,
            snippet.operand);
    fprintf(b->body, "  unreachable\n");
    b->terminated = 1;
    label(b, good);
    // Snapshot all RHS values before changing any destination elements.
    Value *items = checked(sizeof(Value) * (literal->nEntries + 1));
    for (size_t i = 0; i < literal->nEntries; i++)
        items[i] = dataLiteralItem(b, t->element, &literal->values[i], location);
    Value data = arrayData(b, array);
    for (size_t i = 0; i < literal->nEntries; i++) {
        Value address = temp(b, t->element);
        fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %zu\n", address.operand,
                irType(b, t->element), data.operand, i);
        store(b, address, items[i]);
    }
    free(items);
    Value empty = emptyValue(b, t->element), cursor = slot(b, INT);
    store(b, cursor, value(INT, "%zu", literal->nEntries));
    unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
    jump(b, condition);
    label(b, condition);
    Value i = load(b, cursor), more = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, length.operand);
    branch(b, more, body, end);
    label(b, body);
    Value address = temp(b, t->element);
    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", address.operand,
            irType(b, t->element), data.operand, i.operand);
    store(b, address, empty);
    Value next = temp(b, INT);
    fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
    store(b, cursor, next);
    jump(b, condition);
    label(b, end);
}
static Value assignArrayData(Backend *b, Value array, expr *source, token location) {
    BufferKind *t = bufferKind(b, array.kind);
    if (source->type == EXPR_TYPE_COMPOSITE)
        copyArrayLiteral(b, array, source->underlying, location);
    else {
        Value length = arrayLength(b, array);
        Value from = expectedValue(b, source, pointerBufferKind(b, t->element));
        Value to = arrayData(b, array), stride = storageSize(b, t->element), bytes = temp(b, INT);
        fprintf(b->body, "  %s = mul i64 %s, %s\n", bytes.operand, length.operand, stride.operand);
        fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n",
                to.operand, from.operand, bytes.operand);
        b->dataCopyUsed = 1;
    }
    return arrayData(b, array);
}
static int addressable(expr *e) {
    return e->type == EXPR_TYPE_IDENTIFIER || e->type == EXPR_TYPE_DOT || e->type == EXPR_TYPE_INDEX ||
           (e->type == EXPR_TYPE_UNARY && ((struct expr_unary *)e->underlying)->operation == UNARY_DEREFERENCE);
}
static Value materialize(Backend *b, expr *e) {
    int arrayMember = 0;
    if (e->type == EXPR_TYPE_DOT) {
        BufferKind *parent = bufferKind(b, expressionKind(b, &((struct expr_dot *)e->underlying)->parent));
        arrayMember = parent && parent->shape == 4;
    }
    if (addressable(e) && !arrayMember) return address(b, e);
    Value aggregate = expression(b, e), storage = slot(b, aggregate.kind);
    store(b, storage, aggregate);
    return storage;
}
static Value address(Backend *b, expr *e) {
    if (e->type == EXPR_TYPE_IDENTIFIER)
        return find(b, ((struct expr_identifier *)e->underlying)->lexeme.lexeme);
    if (e->type == EXPR_TYPE_UNARY &&
        ((struct expr_unary *)e->underlying)->operation == UNARY_DEREFERENCE) {
        Value pointer = expression(b, &((struct expr_unary *)e->underlying)->child);
        BufferKind *t = bufferKind(b, pointer.kind);
        if (t && t->shape == 6) return value(t->element, "%s", pointer.operand);
        fail(b, "dereference requires a pointer");
        return value(BAD, "null");
    }
    if (e->type == EXPR_TYPE_DOT) {
        struct expr_dot *field = e->underlying;
        const char *name = ((struct expr_identifier *)field->field.underlying)->lexeme.lexeme;
        Value base = materialize(b, &field->parent);
        BufferKind *t = bufferKind(b, base.kind);
        while (t && t->shape == 6) {
            Value pointer = load(b, base);
            base = value(t->element, "%s", pointer.operand);
            t = bufferKind(b, base.kind);
        }
        if (t && t->shape == 5) {
            size_t i = recordField(t, name);
            if (i < t->count) {
                Value result = temp(b, t->members[i]);
                fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, i32 %zu\n",
                        result.operand, t->ir, base.operand, i);
                return result;
            }
            fail(b, "unknown struct field");
            return value(BAD, "null");
        }
        fail(b, !strcmp(name, "size") ? "array size is immutable"
                                      : "inline array.data requires a copy assignment");
        return value(BAD, "null");
    }
    if (e->type == EXPR_TYPE_INDEX) {
        struct expr_index *v = e->underlying;
        Value base = materialize(b, &v->parent);
        BufferKind *t = bufferKind(b, base.kind);
        if (!t || (t->shape && t->shape != 3 && t->shape != 4)) {
            fail(b, "indexing requires a buffer or array");
            return value(BAD, "null");
        }
        Value pointer = t->shape ? load(b, base) : value(BAD, "");
        Value index = expression(b, &v->index);
        if (!integral(index.kind)) {
            fail(b, "buffer index must be an integer");
            return value(BAD, "null");
        }
        index = convert(b, index, INT);
        Value result = temp(b, t->element);
        // Raw buffers stay unchecked; arrays use their current byte-size header.
        if (t->shape == 3 || t->shape == 4) {
            if (t->shape == 4) {
                checkArrayIndex(b, pointer, index, v->location);
                pointer = arrayData(b, pointer);
            }
            fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", result.operand,
                    irType(b, t->element), pointer.operand, index.operand);
        } else {
            fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 0, i64 %s\n", result.operand,
                    t->ir, base.operand, index.operand);
        }
        return result;
    }
    fail(b, "expression has no writable storage");
    return value(BAD, "null");
}
static Value binary(Backend *b, enum binaryExprType op, Value a, Value c) {
    BufferKind *array = bufferKind(b, a.kind);
    if (array && array->shape == 6 && a.kind == c.kind &&
        (op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS)) {
        Value result = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp %s ptr %s, %s\n", result.operand,
                op == EXPR_EQUALITY ? "eq" : "ne", a.operand, c.operand);
        return result;
    }
    if (op == EXPR_ADD && a.kind == c.kind && array && array->shape == 4 && array->element == I8) {
        Value leftLength = arrayLength(b, a), rightLength = arrayLength(b, c);
        Value length = temp(b, INT);
        fprintf(b->body, "  %s = add i64 %s, %s\n", length.operand, leftLength.operand, rightLength.operand);
        Value result = allocateArray(b, I8, length), data = arrayData(b, result);
        Value left = arrayData(b, a), right = arrayData(b, c), tail = temp(b, RAW_POINTER);
        fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 %s\n", tail.operand, data.operand, leftLength.operand);
        fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n", data.operand, left.operand, leftLength.operand);
        fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n", tail.operand, right.operand, rightLength.operand);
        b->dataCopyUsed = 1;
        return result;
    }
    if (integral(a.kind) && integral(c.kind) && a.kind != c.kind) {
        a = convert(b, a, INT);
        c = convert(b, c, INT);
    }
    if (a.kind != c.kind)
        fail(b, "binary operand types differ");
    const char *instruction = NULL;
    int comparison = 0, fp = a.kind == FLOAT;
    switch (op) {
    case EXPR_ADD:
        instruction = fp ? "fadd" : "add";
        break;
    case EXPR_SUBTRACT:
        instruction = fp ? "fsub" : "sub";
        break;
    case EXPR_MULTIPLY:
        instruction = fp ? "fmul" : "mul";
        break;
    case EXPR_DIVIDE:
        instruction = fp ? "fdiv" : "sdiv";
        break;
    case EXPR_MODULO:
        instruction = "srem";
        break;
    case EXPR_XOR:
        instruction = "xor";
        break;
    case EXPR_AMPERSAND:
    case EXPR_BITWISE_AND:
        instruction = "and";
        break;
    case EXPR_BITWISE_OR:
        instruction = "or";
        break;
    case EXPR_BITSHIFT_LEFT:
        instruction = "shl";
        break;
    case EXPR_BITSHIFT_RIGHT:
        instruction = "ashr";
        break;
    case EXPR_EQUALITY:
        instruction = fp ? "fcmp oeq" : "icmp eq";
        comparison = 1;
        break;
    case EXPR_NOT_EQUALS:
        instruction = fp ? "fcmp une" : "icmp ne";
        comparison = 1;
        break;
    case EXPR_LESS_THAN:
        instruction = fp ? "fcmp olt" : "icmp slt";
        comparison = 1;
        break;
    case EXPR_GREATER_THAN:
        instruction = fp ? "fcmp ogt" : "icmp sgt";
        comparison = 1;
        break;
    case EXPR_LESS_EQUALS:
        instruction = fp ? "fcmp ole" : "icmp sle";
        comparison = 1;
        break;
    case EXPR_GREATER_EQUALS:
        instruction = fp ? "fcmp oge" : "icmp sge";
        comparison = 1;
        break;
    default:
        break;
    }
    if (!instruction || a.kind >= BUFFER || a.kind == RAW_POINTER || a.kind == VOID || a.kind == BAD ||
        (fp && !comparison && op != EXPR_ADD && op != EXPR_SUBTRACT && op != EXPR_MULTIPLY &&
         op != EXPR_DIVIDE)) {
        fail(b, "unsupported binary operator or operand type");
        return value(BAD, "0");
    }
    Value r = temp(b, comparison ? BOOL : a.kind);
    fprintf(b->body, "  %s = %s %s %s, %s\n", r.operand, instruction, irType(b, a.kind), a.operand,
            c.operand);
    return r;
}
static Value expression(Backend *b, expr *e) {
    if (!e || !e->underlying) {
        fail(b, "missing expression");
        return value(BAD, "0");
    }
    switch (e->type) {
    case EXPR_TYPE_LITERAL: {
        struct expr_literal *v = e->underlying;
        if (v->type == LITERAL_TYPE_INT)
            return value(INT, "%ld", *(long *)v->value);
        if (v->type == LITERAL_TYPE_FLOAT) {
            union {
                double d;
                unsigned long long bits;
            } f = {.d = *(double *)v->value};
            return value(FLOAT, "0x%016llX", f.bits);
        }
        if (v->type == LITERAL_TYPE_STRING) {
            Value result = allocateArray(b, I8, value(INT, "%zu", v->stringLength));
            Value data = arrayData(b, result);
            unsigned id = ++b->strings;
            fprintf(b->out, "@.bytes%u = private unnamed_addr constant [%zu x i8] c\"", id, v->stringLength);
            for (size_t i = 0; i < v->stringLength; i++)
                fprintf(b->out, "\\%02X", ((unsigned char *)v->value)[i]);
            fprintf(b->out, "\"\n");
            fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr @.bytes%u, i64 %zu, i1 false)\n",
                    data.operand, id, v->stringLength);
            b->dataCopyUsed = 1;
            return result;
        }
        break;
    }
    case EXPR_TYPE_IDENTIFIER: {
        const char *name = ((struct expr_identifier *)e->underlying)->lexeme.lexeme;
        if (!strcmp(name, "true") || !strcmp(name, "false"))
            return value(BOOL, "%d", !strcmp(name, "true"));
        return load(b, find(b, name));
    }
    case EXPR_TYPE_COMPOSITE:
        return expectedValue(b, e, expressionKind(b, e));
    case EXPR_TYPE_ARRAY_NEW: {
        struct expr_array_new *v = e->underlying;
        return allocateArray(b, type(b, v->elementType), expression(b, &v->count));
    }
    case EXPR_TYPE_INDEX:
        return load(b, address(b, e));
    case EXPR_TYPE_DOT: {
        struct expr_dot *v = e->underlying;
        BufferKind *parentType = bufferKind(b, expressionKind(b, &v->parent));
        if (parentType && (parentType->shape == 5 || parentType->shape == 6))
            return load(b, address(b, e));
        Value parent = expression(b, &v->parent);
        BufferKind *t = bufferKind(b, parent.kind);
        if (t && t->shape == 4) {
            char *field = ((struct expr_identifier *)v->field.underlying)->lexeme.lexeme;
            if (!strcmp(field, "size"))
                return load(b, value(INT, "%s", parent.operand));
            if (!strcmp(field, "data"))
                return arrayData(b, parent);
        }
        fail(b, "unsupported member access");
        return value(BAD, "0");
    }
    case EXPR_TYPE_TYPECAST: {
        struct expr_typecast *v = e->underlying;
        Value a = expression(b, &v->child);
        return convert(b, a, type(b, v->type));
    }
    case EXPR_TYPE_CALL: {
        struct expr_call *v = e->underlying;
        MethodInstance *method = NULL;
        Value receiver = value(BAD, "");
        const char *name = "";
        struct stmt_func *f = NULL;
        typeTokens *typeArguments;
        size_t count;
        struct expr_dot *dot = methodCallee(&v->func, &typeArguments, &count);
        if (dot) {
            name = ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme;
            Kind actual = expressionKind(b, &dot->parent);
            method = methodInstance(b, actual, name, typeArguments, count);
            if (!method) return value(BAD, "0");
            f = method->function;
            BufferKind *actualType = bufferKind(b, actual);
            BufferKind *wantedType = bufferKind(b, method->receiver);
            if (wantedType && wantedType->shape == 6 && !(actualType && actualType->shape == 6)) {
                if (!addressable(&dot->parent)) {
                    fail(b, "pointer receiver requires an addressable value");
                    return value(BAD, "0");
                }
                Value addr = address(b, &dot->parent);
                receiver = value(method->receiver, "%s", addr.operand);
            } else {
                receiver = expression(b, &dot->parent);
                while (actualType && actualType->shape == 6 && actualType->element != method->record) {
                    receiver = load(b, value(actualType->element, "%s", receiver.operand));
                    actualType = bufferKind(b, actualType->element);
                }
                if (actualType && actualType->shape == 6 && !(wantedType && wantedType->shape == 6))
                    receiver = load(b, value(actualType->element, "%s", receiver.operand));
            }
        } else if (v->func.type == EXPR_TYPE_IDENTIFIER) {
            name = ((struct expr_identifier *)v->func.underlying)->lexeme.lexeme;
            f = findFunction(b, name);
        } else {
            fail(b, "only direct function and method calls are supported");
            return value(BAD, "0");
        }
        if (!f && !strcmp(name, "len")) {
            if (v->nParams != 1 || v->spread) {
                fail(b, "len requires exactly one array argument");
                return value(BAD, "0");
            }
            Value array = expression(b, &v->params[0]);
            BufferKind *t = bufferKind(b, array.kind);
            if (!t || t->shape != 4) {
                fail(b, "len requires an array");
                return value(BAD, "0");
            }
            Value bytes = load(b, value(INT, "%s", array.operand)),
                  stride = storageSize(b, t->element), result = temp(b, INT);
            fprintf(b->body, "  %s = sdiv i64 %s, %s\n", result.operand, bytes.operand,
                    stride.operand);
            return result;
        }
        if (!f) {
            fail(b, "unknown function");
            return value(BAD, "0");
        }
        TypeBinding *previousBindings = b->typeBindings;
        if (method) b->typeBindings = method->nBindings ? method->bindings : NULL;
        Kind *parameterKinds = checked(sizeof(Kind) * (f->nParams + 1));
        for (size_t i = 0; i < f->nParams; i++) parameterKinds[i] = type(b, f->paramTypeNames[i]);
        Kind resultKind = resultType(b, f);
        b->typeBindings = previousBindings;
        size_t fixed = f->nParams - (f->isVariadic && !f->isExtern ? 1 : 0);
        Kind *expected = checked(sizeof(Kind) * (v->nParams + 1));
        for (size_t i = 0; i < v->nParams; i++) {
            expected[i] = i < fixed ? parameterKinds[i]
                          : f->isVariadic && !f->isExtern
                              ? parameterKinds[f->nParams - 1]
                              : BAD;
            if (v->spread && i + 1 == v->nParams)
                expected[i] = BAD;
        }
        ValueList evaluated = evaluateValues(b, v->params, v->nParams, expected, v->nParams);
        free(expected);
        if ((!f->isVariadic && evaluated.count != fixed) || evaluated.count < fixed ||
            (v->spread && (!f->isVariadic || f->isExtern || evaluated.count < f->nParams ||
                           evaluated.count != v->nParams))) {
            fail(b, "call argument count mismatch");
            free(evaluated.items);
            free(parameterKinds);
            return value(BAD, "0");
        }
        size_t nArgs = f->isVariadic && !f->isExtern ? f->nParams : evaluated.count;
        Value *args = checked(sizeof(Value) * (nArgs + 1));
        for (size_t i = 0; i < fixed; i++)
            args[i] = coerce(b, evaluated.items[i], parameterKinds[i]);
        if (f->isVariadic && !f->isExtern) {
            Kind element = parameterKinds[f->nParams - 1];
            size_t explicitCount = evaluated.count - fixed - (v->spread ? 1 : 0);
            Value tail = value(BAD, ""), tailLength = value(INT, "0");
            if (v->spread) {
                tail =
                    coerce(b, evaluated.items[evaluated.count - 1], arrayReferenceKind(b, element));
                if (!explicitCount) {
                    args[fixed] = tail;
                } else
                    tailLength = arrayLength(b, tail);
            }
            if (!v->spread || explicitCount) {
                Value count = value(INT, "%zu", explicitCount);
                if (v->spread) {
                    Value total = temp(b, INT);
                    fprintf(b->body, "  %s = add i64 %zu, %s\n", total.operand, explicitCount,
                            tailLength.operand);
                    count = total;
                }
                Value pack = allocateArray(b, element, count), data = arrayData(b, pack);
                for (size_t i = 0; i < explicitCount; i++) {
                    Value address = temp(b, element);
                    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %zu\n", address.operand,
                            irType(b, element), data.operand, i);
                    store(b, address, coerce(b, evaluated.items[fixed + i], element));
                }
                if (v->spread) {
                    Value destination = temp(b, pointerBufferKind(b, element)),
                          source = arrayData(b, tail);
                    Value stride = storageSize(b, element), bytes = temp(b, INT);
                    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %zu\n",
                            destination.operand, irType(b, element), data.operand, explicitCount);
                    fprintf(b->body, "  %s = mul i64 %s, %s\n", bytes.operand, tailLength.operand,
                            stride.operand);
                    fprintf(
                        b->body,
                        "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n",
                        destination.operand, source.operand, bytes.operand);
                    b->dataCopyUsed = 1;
                }
                args[fixed] = pack;
            }
        } else {
            for (size_t i = fixed; i < evaluated.count; i++) {
                args[i] = evaluated.items[i];
                if (args[i].kind == BOOL || args[i].kind == I8)
                    args[i] = convert(b, args[i], I32);
                BufferKind *extra = bufferKind(b, args[i].kind);
                if ((extra && extra->shape != 3) || args[i].kind == VOID)
                    fail(b, "C variadic arguments must be scalar values");
            }
        }
        free(evaluated.items);
        Value traceParent = value(RAW_POINTER, "null");
        if (!f->isExtern)
            traceParent = pushTraceFrame(b, f, v->location);
        Kind k = resultKind;
        Value r = k == VOID ? value(VOID, "") : temp(b, k);
        fprintf(b->body, "  ");
        if (k != VOID)
            fprintf(b->body, "%s = ", r.operand);
        fprintf(b->body, "call %s ", irType(b, k));
        if (f->isExtern && f->isVariadic) {
            fprintf(b->body, "(");
            for (size_t i = 0; i < fixed; i++)
                fprintf(b->body, "%s%s", i ? ", " : "", irType(b, parameterKinds[i]));
            fprintf(b->body, "%s...) ", fixed ? ", " : "");
        }
        if (method) methodName(b->body, method);
        else functionName(b->body, f);
        fprintf(b->body, "(");
        if (method) fprintf(b->body, "%s %s", irType(b, receiver.kind), receiver.operand);
        for (size_t i = 0; i < nArgs; i++)
            fprintf(b->body, "%s%s %s", (i || method) ? ", " : "", irType(b, args[i].kind), args[i].operand);
        fprintf(b->body, ")\n");
        if (!f->isExtern)
            popTraceFrame(b, traceParent);
        free(args);
        free(parameterKinds);
        return r;
    }
    case EXPR_TYPE_BINARY: {
        struct expr_binary *v = e->underlying;
        if (v->operation == EXPR_ASSIGNMENT && v->left.type == EXPR_TYPE_DOT) {
            struct expr_dot *target = v->left.underlying;
            token field = ((struct expr_identifier *)target->field.underlying)->lexeme;
            BufferKind *arrayType = bufferKind(b, expressionKind(b, &target->parent));
            if (!strcmp(field.lexeme, "data") && arrayType && arrayType->shape == 4) {
                Value array = expression(b, &target->parent);
                return assignArrayData(b, array, &v->right, field);
            }
        }

        if (v->operation == EXPR_ASSIGNMENT && (isComma(&v->left) || isDiscard(&v->left))) {
            size_t n = commaCount(&v->left), m = commaCount(&v->right);
            Value *dest = checked(sizeof(Value) * n);
            Kind *expected = checked(sizeof(Kind) * n);
            expr *rhs = checked(sizeof(expr) * m);
            for (size_t i = 0; i < n; i++) {
                expr *target = commaItem(&v->left, i);
                dest[i] = isDiscard(target) ? value(BAD, "") : address(b, target);
                expected[i] = dest[i].kind;
            }
            for (size_t i = 0; i < m; i++)
                rhs[i] = *commaItem(&v->right, i);
            ValueList values = evaluateValues(b, rhs, m, expected, n);
            if (values.count != n)
                fail(b, "assignment count mismatch");
            for (size_t i = 0; i < n && i < values.count; i++)
                if (dest[i].kind != BAD)
                    store(b, dest[i], values.items[i]);
            free(values.items);
            free(rhs);
            free(expected);
            free(dest);
            return value(VOID, "");
        }
        if (v->operation == EXPR_ASSIGNMENT ||
            (v->operation >= EXPR_ADD_EQUALS && v->operation <= EXPR_BITWISE_OR_EQUALS)) {
            Value addr = address(b, &v->left), old = {0};
            if (v->operation != EXPR_ASSIGNMENT)
                old = load(b, addr);
            Value r = expectedValue(b, &v->right, addr.kind);
            if (v->operation != EXPR_ASSIGNMENT) {
                enum binaryExprType ops[] = {
                    EXPR_ADD,         EXPR_SUBTRACT,  EXPR_MULTIPLY,      EXPR_DIVIDE,
                    EXPR_MODULO,      EXPR_XOR,       EXPR_BITSHIFT_LEFT, EXPR_BITSHIFT_RIGHT,
                    EXPR_BITWISE_AND, EXPR_BITWISE_OR};
                r = binary(b, ops[v->operation - EXPR_ADD_EQUALS], old, r);
            }
            store(b, addr, r);
            return r;
        }
        Value a = expression(b, &v->left);
        if (v->operation == EXPR_LOGICAL_OR || v->operation == EXPR_LOGICAL_AND) {
            Value dest = slot(b, BOOL);
            store(b, dest, a);
            unsigned rhs = ++b->label, end = ++b->label;
            if (v->operation == EXPR_LOGICAL_AND)
                branch(b, a, rhs, end);
            else
                branch(b, a, end, rhs);
            label(b, rhs);
            Value c = expression(b, &v->right);
            store(b, dest, c);
            jump(b, end);
            label(b, end);
            return load(b, dest);
        }
        Value c = expression(b, &v->right);
        if (v->operation == EXPR_COMMA)
            return c;
        return binary(b, v->operation, a, c);
    }
    case EXPR_TYPE_UNARY: {
        struct expr_unary *v = e->underlying;
        if (v->operation == UNARY_ADDRESS_OF) {
            Value storage = address(b, &v->child);
            return value(pointerKind(b, storage.kind), "%s", storage.operand);
        }
        if (v->operation == UNARY_DEREFERENCE)
            return load(b, address(b, e));
        if (v->operation == UNARY_SIZEOF)
            return storageSize(b, v->sizeofType.nTokens ? type(b, v->sizeofType)
                                                        : expressionKind(b, &v->child));
        if (v->operation >= UNARY_POSTFIX_INCREMENT && v->operation <= UNARY_PREFIX_DECREMENT) {
            Value addr = address(b, &v->child), old = load(b, addr);
            int decrement =
                v->operation == UNARY_POSTFIX_DECREMENT || v->operation == UNARY_PREFIX_DECREMENT;
            Value one = old.kind == FLOAT ? value(FLOAT, "0x3FF0000000000000") : value(INT, "1");
            Value r = binary(b, decrement ? EXPR_SUBTRACT : EXPR_ADD, old, one);
            store(b, addr, r);
            return v->operation == UNARY_POSTFIX_INCREMENT ||
                           v->operation == UNARY_POSTFIX_DECREMENT
                       ? old
                       : r;
        }
        Value a = expression(b, &v->child), r = temp(b, a.kind);
        if (v->operation == UNARY_NEGATE && a.kind == FLOAT)
            fprintf(b->body, "  %s = fneg double %s\n", r.operand, a.operand);
        else if (v->operation == UNARY_NEGATE && integral(a.kind))
            fprintf(b->body, "  %s = sub %s 0, %s\n", r.operand, irType(b, a.kind), a.operand);
        else if (v->operation == UNARY_LOGICAL_NOT && a.kind == BOOL)
            fprintf(b->body, "  %s = xor i1 %s, true\n", r.operand, a.operand);
        else if (v->operation == UNARY_BITWISE_NOT && integral(a.kind))
            fprintf(b->body, "  %s = xor %s %s, -1\n", r.operand, irType(b, a.kind), a.operand);
        else {
            fail(b, "unsupported unary operation");
            return value(BAD, "0");
        }
        return r;
    }
    case EXPR_TYPE_TERNARY: {
        struct expr_ternary *v = e->underlying;
        Value cond = expression(b, &v->condition);
        unsigned yes = ++b->label, no = ++b->label, end = ++b->label;
        branch(b, cond, yes, no);
        label(b, yes);
        Value a = expression(b, &v->ifTrue), dest = slot(b, a.kind);
        store(b, dest, a);
        jump(b, end);
        label(b, no);
        Value c = expression(b, &v->ifFalse);
        store(b, dest, c);
        jump(b, end);
        label(b, end);
        return load(b, dest);
    }
    default:
        break;
    }
    fail(b, "unsupported expression");
    return value(BAD, "0");
}
static int hasArrayAllocation(expr *e) {
    if (!e || !e->underlying)
        return 0;
    switch (e->type) {
    case EXPR_TYPE_LITERAL:
        return ((struct expr_literal *)e->underlying)->type == LITERAL_TYPE_STRING;
    case EXPR_TYPE_ARRAY_NEW:
        return 1;
    case EXPR_TYPE_BINARY: {
        struct expr_binary *v = e->underlying;
        if (v->operation == EXPR_ASSIGNMENT && v->left.type == EXPR_TYPE_DOT &&
            !strcmp(((struct expr_identifier *)((struct expr_dot *)v->left.underlying)
                         ->field.underlying)
                        ->lexeme.lexeme,
                    "data") &&
            v->right.type == EXPR_TYPE_COMPOSITE)
            return 1;
        // Addition may allocate a string even when both operands are variables.
        return v->operation == EXPR_ADD || v->operation == EXPR_ADD_EQUALS ||
               hasArrayAllocation(&v->left) || hasArrayAllocation(&v->right);
    }
    case EXPR_TYPE_UNARY: {
        struct expr_unary *v = e->underlying;
        return v->operation != UNARY_SIZEOF && hasArrayAllocation(&v->child);
    }
    case EXPR_TYPE_CALL: {
        struct expr_call *v = e->underlying;
        if (v->packsVariadic)
            return 1;
        for (size_t i = 0; i < v->nParams; i++)
            if (hasArrayAllocation(&v->params[i]))
                return 1;
        return 0;
    }
    case EXPR_TYPE_INDEX: {
        struct expr_index *v = e->underlying;
        return hasArrayAllocation(&v->parent) || hasArrayAllocation(&v->index);
    }
    case EXPR_TYPE_DOT:
        return hasArrayAllocation(&((struct expr_dot *)e->underlying)->parent);
    case EXPR_TYPE_TYPECAST:
        return hasArrayAllocation(&((struct expr_typecast *)e->underlying)->child);
    case EXPR_TYPE_TERNARY: {
        struct expr_ternary *v = e->underlying;
        return hasArrayAllocation(&v->condition) || hasArrayAllocation(&v->ifTrue) ||
               hasArrayAllocation(&v->ifFalse);
    }
    case EXPR_TYPE_COMPOSITE:
        return 1;
    default:
        return 0;
    }
}
static int statementAllocatesStack(stmt *s) {
    if (!s)
        return 0;
    if (s->type == STMT_VAR) {
        struct stmt_var *v = s->value;
        typeTokens element, dimension;
        if (!v->nInitValues && v->type.nTokens &&
            v->type.tokens[0].type == IDENTIFIER &&
            !builtinStorageTypeName(v->type.tokens[0].lexeme))
            return 1; // A struct can contain default empty arrays or strings.
        if (!v->nInitValues && (arrayElementType(v->type, &element) ||
            (v->type.nTokens == 1 && !strcmp(v->type.tokens[0].lexeme, "string"))))
            return 1;
        for (size_t i = 0; i < v->nInitValues; i++)
            if (hasArrayAllocation(&v->initValues[i]))
                return 1;
        return bufferTypeParts(v->type, &element, &dimension) &&
               bufferDimensionKind(dimension) == 2;
    }
    if (s->type == STMT_EXPR)
        return hasArrayAllocation(s->value);
    if (s->type == STMT_RETURN) {
        struct stmt_return *v = s->value;
        for (size_t i = 0; i < v->nValues; i++)
            if (hasArrayAllocation(&v->values[i]))
                return 1;
    }
    if (s->type == STMT_BLOCK) {
        struct stmt_block *v = s->value;
        for (size_t i = 0; i < v->nStmts; i++)
            if (statementAllocatesStack(&v->stmts[i]))
                return 1;
    } else if (s->type == STMT_IF) {
        struct stmt_if *v = s->value;
        return statementAllocatesStack(v->preStmt) || hasArrayAllocation(v->condition) ||
               statementAllocatesStack(v->ifTrue) || statementAllocatesStack(v->ifFalse);
    } else if (s->type == STMT_FOR) {
        struct stmt_for *v = s->value;
        return statementAllocatesStack(v->initializer) || hasArrayAllocation(v->condition) ||
               hasArrayAllocation(v->update) || statementAllocatesStack(v->body);
    }
    return 0;
}
static void saveStack(Backend *b, Scope *scope, int needed) {
    if (!needed)
        return;
    scope->hasStack = 1;
    scope->savedStack = temp(b, RAW_POINTER);
    fprintf(b->body, "  %s = call ptr @llvm.stacksave.p0()\n", scope->savedStack.operand);
}
static void restoreStack(Backend *b, Scope *scope) {
    if (scope->hasStack)
        fprintf(b->body, "  call void @llvm.stackrestore.p0(ptr %s)\n", scope->savedStack.operand);
}
static void restoreLoopScopes(Backend *b) {
    for (Scope *s = b->scope; s && s != b->loopScope; s = s->parent)
        restoreStack(b, s);
}
static void statement(Backend *, stmt *);
static void block(Backend *b, struct stmt_block *v) {
    Scope scope = {.parent = b->scope, .declarations = v};
    b->scope = &scope;
    stmt wrapper = {.type = STMT_BLOCK, .value = v};
    saveStack(b, &scope, statementAllocatesStack(&wrapper));
    for (size_t i = 0; i < v->nStmts; i++) {
        if (b->terminated)
            break;
        statement(b, &v->stmts[i]);
    }
    if (!b->terminated)
        restoreStack(b, &scope);
    b->scope = scope.parent;
    clearScope(&scope);
}
static void statement(Backend *b, stmt *s) {
    if (!s || b->terminated)
        return;
    switch (s->type) {
    case STMT_NULL:
    case STMT_STRUCT:
        break;
    case STMT_BLOCK:
        block(b, s->value);
        break;
    case STMT_EXPR:
        expression(b, s->value);
        break;
    case STMT_VAR: {
        struct stmt_var *v = s->value;
        typeTokens element, dimension;
        if (bufferTypeParts(v->type, &element, &dimension) && bufferDimensionKind(dimension) == 2) {
            expr *size = parseBufferDimension(dimension);
            if (!size) {
                fail(b, "invalid runtime buffer size");
                break;
            }
            Value count = expression(b, size);
            free(size);
            if (!integral(count.kind)) {
                fail(b, "runtime buffer size must be an integer");
                break;
            }
            count = convert(b, count, INT);
            Kind elementKind = type(b, element), pointerKind = pointerBufferKind(b, elementKind);
            for (size_t i = 0; i < v->nVars; i++) {
                if (v->vars[i].type == UNDERSCORE)
                    continue;
                Value storage = temp(b, pointerKind);
                fprintf(b->body, "  %s = alloca %s, i64 %s\n", storage.operand,
                        irType(b, elementKind), count.operand);
                Value binding = slot(b, pointerKind);
                bind(b, v->vars[i].lexeme, binding);
                store(b, binding, storage);
            }
            break;
        }
        Kind *expected = checked(sizeof(Kind) * v->nVars);
        for (size_t i = 0; i < v->nVars; i++)
            expected[i] = v->type.nTokens ? type(b, v->type) : BAD;
        ValueList values = evaluateValues(b, v->initValues, v->nInitValues, expected, v->nVars);
        free(expected);
        if (v->nInitValues && values.count != v->nVars) {
            fail(b, "initializer count mismatch");
            free(values.items);
            break;
        }
        Value *initial = values.items;
        for (size_t i = 0; i < v->nVars; i++) {
            if (v->vars[i].type == UNDERSCORE)
                continue;
            Kind k = v->type.nTokens ? type(b, v->type) : initial[i].kind;
            if (k == VOID || k == BAD) {
                fail(b, "variable needs a supported value type");
                continue;
            }
            Value addr = slot(b, k);
            bind(b, v->vars[i].lexeme, addr);
            Value init = v->nInitValues ? initial[i]
                                        : value(k, k >= BUFFER   ? "zeroinitializer"
                                                   : k == RAW_POINTER ? "null"
                                                   : k == FLOAT  ? "0x0000000000000000"
                                                                 : "0");
            BufferKind *array = bufferKind(b, k);
            if (!v->nInitValues && array && (array->shape == 4 || array->shape == 5))
                init = emptyValue(b, k);
            store(b, addr, init);
        }
        free(initial);
        break;
    }
    case STMT_RETURN: {
        struct stmt_return *v = s->value;
        Kind k = resultType(b, b->function);
        Kind *expected = checked(sizeof(Kind) * (b->function->nResults + 1));
        for (size_t i = 0; i < b->function->nResults; i++)
            expected[i] = type(b, b->function->resultTypes[i]);
        ValueList values =
            evaluateValues(b, v->values, v->nValues, expected, b->function->nResults);
        Value r = value(VOID, "");
        if (b->function->nResults > 1) {
            r = value(k, "zeroinitializer");
            for (size_t i = 0; i < b->function->nResults; i++) {
                Value item = i < values.count ? values.items[i]
                             : !v->nValues && b->function->resultNames[i].type != EOF_TOKEN
                                 ? load(b, find(b, b->function->resultNames[i].lexeme))
                                 : value(BAD, "0");
                r = insert(b, r, item, i);
            }
        } else if (values.count)
            r = values.items[0];
        else if (k != VOID && b->function->resultNames[0].type != EOF_TOKEN)
            r = load(b, find(b, b->function->resultNames[0].lexeme));
        free(expected);
        free(values.items);
        if (integral(r.kind) && integral(k))
            r = convert(b, r, k);
        if (r.kind != k)
            fail(b, "return value type mismatch");
        if (k == VOID)
            fprintf(b->body, "  ret void\n");
        else
            fprintf(b->body, "  ret %s %s\n", irType(b, k), r.operand);
        b->terminated = 1;
        break;
    }
    case STMT_IF: {
        struct stmt_if *v = s->value;
        Scope scope = {.parent = b->scope};
        b->scope = &scope;
        saveStack(b, &scope, statementAllocatesStack(s));
        statement(b, v->preStmt);
        Value cond = expression(b, v->condition);
        unsigned yes = ++b->label, no = ++b->label, end = ++b->label;
        branch(b, cond, yes, no);
        label(b, yes);
        statement(b, v->ifTrue);
        int yesReturns = b->terminated;
        jump(b, end);
        label(b, no);
        statement(b, v->ifFalse);
        int noReturns = b->terminated;
        jump(b, end);
        label(b, end);
        if (yesReturns && noReturns) {
            fprintf(b->body, "  unreachable\n");
            b->terminated = 1;
        }
        if (!b->terminated)
            restoreStack(b, &scope);
        b->scope = scope.parent;
        clearScope(&scope);
        break;
    }
    case STMT_FOR: {
        struct stmt_for *v = s->value;
        if (v->range) {
            fail(b, "range loops are unsupported");
            break;
        }
        Scope scope = {.parent = b->scope};
        b->scope = &scope;
        saveStack(b, &scope, statementAllocatesStack(s));
        Scope *oldLoopScope = b->loopScope;
        b->loopScope = &scope;
        unsigned condition = ++b->label, body = ++b->label, update = ++b->label, end = ++b->label;
        unsigned oldBreak = b->breakLabel, oldContinue = b->continueLabel;
        b->breakLabel = end;
        b->continueLabel = update;
        statement(b, v->initializer);
        jump(b, v->deferCondition ? body : condition);
        label(b, condition);
        if (v->condition) {
            Value c = expression(b, v->condition);
            branch(b, c, body, end);
        } else
            jump(b, body);
        label(b, body);
        Scope bodyScope = {.parent = b->scope};
        b->scope = &bodyScope;
        saveStack(b, &bodyScope, statementAllocatesStack(v->body));
        statement(b, v->body);
        if (!b->terminated)
            restoreStack(b, &bodyScope);
        b->scope = bodyScope.parent;
        clearScope(&bodyScope);
        jump(b, update);
        label(b, update);
        if (v->update)
            expression(b, v->update);
        jump(b, condition);
        label(b, end);
        b->breakLabel = oldBreak;
        b->continueLabel = oldContinue;
        b->loopScope = oldLoopScope;
        restoreStack(b, &scope);
        b->scope = scope.parent;
        clearScope(&scope);
        break;
    }
    case STMT_BREAK:
    case STMT_CONTINUE: {
        struct stmt_branch *v = s->value;
        unsigned target = s->type == STMT_BREAK ? b->breakLabel : b->continueLabel;
        if (v->label.type != EOF_TOKEN || !target)
            fail(b, "invalid or labeled break/continue");
        else {
            restoreLoopScopes(b);
            jump(b, target);
        }
        break;
    }
    default:
        fail(b, "unsupported statement");
        break;
    }
}
static void signature(Backend *b, struct stmt_func *f, FILE *out) {
    fprintf(out, "%s %s ", f->isExtern ? "declare" : "define", irType(b, resultType(b, f)));
    if (b->method) methodName(out, b->method);
    else functionName(out, f);
    fprintf(out, "(");
    if (b->method) fprintf(out, "%s %%receiver", irType(b, b->method->receiver));
    for (size_t i = 0; i < f->nParams; i++)
        fprintf(out, "%s%s %%arg%zu", (i || b->method) ? ", " : "", irType(b, parameterKind(b, f, i)), i);
    if (f->isExtern && f->isVariadic)
        fprintf(out, "%s...", f->nParams ? ", " : "");
    fprintf(out, ")");
}
static int copy(FILE *from, FILE *to) {
    if (fflush(from) || fseek(from, 0, SEEK_SET))
        return 1;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), from)))
        if (fwrite(buffer, 1, n, to) != n)
            return 1;
    return ferror(from) != 0;
}
static void emitBoundsRuntime(Backend *b) {
    if (!b->boundsUsed)
        return;
    struct stmt_func *print = findFunction(b, "dprintf"), *quit = findFunction(b, "exit");
    if (!print || !print->isExtern)
        fprintf(b->out, "declare i32 @dprintf(i32, ptr, ...)\n");
    if (!quit || !quit->isExtern)
        fprintf(b->out, "declare void @exit(i32) noreturn\n");
    Value message =
        moduleString(b, "Array index out of bounds: index %lld, length %lld\n  --> %s:%lld:%lld\n");
    Value literalMessage = moduleString(b, "Array data literal too large: literal length %lld "
                                           "exceeds array length %lld\n  --> %s:%lld:%lld\n");
    Value snippetFormat = moduleString(b, "%s");
    Value traceTitle = moduleString(b, "Call trace (most recent call first):\n");
    Value frameFormat = moduleString(b, "  at %s (%s:%lld:%lld)\n");
    fprintf(
        b->out,
        "define internal void @__ocean.runtime_error(ptr %%format, i64 %%index, i64 %%length, "
        "ptr %%file, i64 "
        "%%line, i64 %%column, ptr %%snippet) noreturn {\n"
        "entry:\n"
        "  call i32 (i32, ptr, ...) @dprintf(i32 2, ptr %%format, i64 %%index, i64 %%length, ptr "
        "%%file, i64 %%line, i64 %%column)\n"
        "  call i32 (i32, ptr, ...) @dprintf(i32 2, ptr %s, ptr %%snippet)\n"
        "  call i32 (i32, ptr, ...) @dprintf(i32 2, ptr %s)\n"
        "  %%head = load ptr, ptr @__ocean.trace_head\n"
        "  br label %%trace\n"
        "trace:\n"
        "  %%frame = phi ptr [ %%head, %%entry ], [ %%previous, %%next ]\n"
        "  %%trace.file = phi ptr [ %%file, %%entry ], [ %%caller.file, %%next ]\n"
        "  %%trace.line = phi i64 [ %%line, %%entry ], [ %%caller.line, %%next ]\n"
        "  %%trace.column = phi i64 [ %%column, %%entry ], [ %%caller.column, %%next ]\n"
        "  %%present = icmp ne ptr %%frame, null\n"
        "  br i1 %%present, label %%next, label %%done\n"
        "next:\n"
        "  %%name.slot = getelementptr %%__ocean.trace, ptr %%frame, i32 0, i32 1\n"
        "  %%name = load ptr, ptr %%name.slot\n"
        "  %%file.slot = getelementptr %%__ocean.trace, ptr %%frame, i32 0, i32 2\n"
        "  %%caller.file = load ptr, ptr %%file.slot\n"
        "  %%line.slot = getelementptr %%__ocean.trace, ptr %%frame, i32 0, i32 3\n"
        "  %%caller.line = load i64, ptr %%line.slot\n"
        "  %%column.slot = getelementptr %%__ocean.trace, ptr %%frame, i32 0, i32 4\n"
        "  %%caller.column = load i64, ptr %%column.slot\n"
        "  call i32 (i32, ptr, ...) @dprintf(i32 2, ptr %s, ptr %%name, ptr %%trace.file, i64 "
        "%%trace.line, i64 %%trace.column)\n"
        "  %%parent.slot = getelementptr %%__ocean.trace, ptr %%frame, i32 0, i32 0\n"
        "  %%previous = load ptr, ptr %%parent.slot\n"
        "  br label %%trace\n"
        "done:\n"
        "  call void @exit(i32 1)\n"
        "  unreachable\n"
        "}\n",
        snippetFormat.operand, traceTitle.operand, frameFormat.operand);
    fprintf(b->out,
            "define internal void @__ocean.bounds_fail(i64 %%index, i64 %%length, ptr %%file, i64 "
            "%%line, i64 %%column, ptr %%snippet) noreturn {\nentry:\n"
            "  call void @__ocean.runtime_error(ptr %s, i64 %%index, i64 %%length, ptr %%file, i64 "
            "%%line, i64 %%column, ptr %%snippet)\n  unreachable\n}\n"
            "define internal void @__ocean.array_literal_fail(i64 %%count, i64 %%length, ptr "
            "%%file, i64 %%line, i64 %%column, ptr %%snippet) noreturn {\nentry:\n"
            "  call void @__ocean.runtime_error(ptr %s, i64 %%count, i64 %%length, ptr %%file, i64 "
            "%%line, i64 %%column, ptr %%snippet)\n  unreachable\n}\n",
            message.operand, literalMessage.operand);
}
static int validateLayout(Backend *b, BufferKind *t) {
    if (!t || (t->shape != 5 && t->shape != 0)) return 1;
    if (t->layoutState == 2) return 1;
    if (t->layoutState == 3) return 0;
    if (t->layoutState == 1) {
        fail(b, "recursive struct fields must use pointers or array references");
        return 0;
    }
    t->layoutState = 1;
    int valid = 1;
    if (!t->shape) valid = validateLayout(b, bufferKind(b, t->element));
    else for (size_t i = 0; i < t->count; i++)
        if (!validateLayout(b, bufferKind(b, t->members[i]))) valid = 0;
    t->layoutState = valid ? 2 : 3;
    return valid;
}
static void emitFunction(Backend *b, struct stmt_func *f, FILE *output) {
    b->body = tmpfile();
    b->slots = tmpfile();
    if (!b->body || !b->slots) {
        fail(b, "could not create function buffers");
        if (b->body)
            fclose(b->body);
        if (b->slots)
            fclose(b->slots);
        return;
    }
    Scope scope = {0};
    b->scope = &scope;
    b->function = f;
    if (b->method) {
        Value addr = slot(b, b->method->receiver);
        bind(b, f->receiverName.lexeme, addr);
        store(b, addr, value(b->method->receiver, "%%receiver"));
    }
    b->terminated = 0;
    b->breakLabel = b->continueLabel = 0;
    for (size_t j = 0; j < f->nParams; j++) {
        Kind k = parameterKind(b, f, j);
        Value addr = slot(b, k);
        bind(b, f->paramNames[j].lexeme, addr);
        store(b, addr, value(k, "%%arg%zu", j));
    }
    for (size_t j = 0; j < f->nResults; j++)
        if (f->resultNames[j].type != EOF_TOKEN) {
            Kind k = type(b, f->resultTypes[j]);
            Value addr = slot(b, k);
            bind(b, f->resultNames[j].lexeme, addr);
            BufferKind *array = bufferKind(b, k);
            Value initial = array && (array->shape == 4 || array->shape == 5)
                                ? emptyValue(b, k)
                                : value(k, k >= BUFFER   ? "zeroinitializer"
                                           : k == RAW_POINTER ? "null"
                                           : k == FLOAT  ? "0x0000000000000000"
                                                         : "0");
            store(b, addr, initial);
        }
    block(b, &f->body);
    if (!b->terminated) {
        if (resultType(b, f) == VOID)
            fprintf(b->body, "  ret void\n");
        else
            fail(b, "value-returning function can reach its end without returning");
    }
    signature(b, f, output);
    fprintf(output, " {\nentry:\n");
    if (copy(b->slots, output) || copy(b->body, output))
        fail(b, "could not write function IR");
    fprintf(output, "}\n");
    fclose(b->slots);
    fclose(b->body);
    clearScope(&scope);
}
int emitLLVM(parseResult *program, FILE *output) {
    Backend b = {.program = program, .out = output};
    fprintf(output, "; Generated by ocean-c\n"
                    "%%__ocean.trace = type { ptr, ptr, ptr, i64, i64 }\n"
                    "@__ocean.trace_head = internal thread_local global ptr null\n"
                    "declare ptr @llvm.stacksave.p0()\n"
                    "declare void @llvm.stackrestore.p0(ptr)\n");
    FILE *functions = tmpfile();
    if (!functions) {
        fail(&b, "could not create module function buffer");
        return 1;
    }
    output = functions; // Identified struct definitions must precede aggregate constants.
    struct stmt_func *main = findFunction(&b, "main");
    if (!main || main->isExtern || main->nParams || main->nResults > 1)
        fail(&b, "executable requires func main() or func main() int");
    else if (main->nResults && resultType(&b, main) != INT)
        fail(&b, "main must return int or no value");
    // Non-generic declarations have one concrete layout even before first use.
    for (size_t i = 0; i < program->nStatements; i++) {
        stmt *s = &program->statements[i];
        if (s->type == STMT_STRUCT && !((struct stmt_struct *)s->value)->nTypeParams)
            recordKind(&b, s->value, NULL, 0);
    }
    for (size_t i = 0; i < program->nStatements; i++) {
        stmt *s = &program->statements[i];
        if (s->type == STMT_PACKAGE || s->type == STMT_NULL || s->type == STMT_STRUCT)
            continue;
        if (s->type != STMT_FUNC) {
            fail(&b, "unsupported declaration at file scope");
            continue;
        }
        struct stmt_func *f = s->value;
        if (f->receiverType.nTokens) continue; // Emit concrete methods when called.
        if (f->nTypeParams) {
            fail(&b, "generic functions are unsupported");
            continue;
        }
        if (f->isExtern) {
            BufferKind *result = bufferKind(&b, resultType(&b, f));
            if (result && result->shape == 5)
                fail(&b, "C structs passed or returned by value require ABI lowering; use pointers");
            for (size_t j = 0; j < f->nParams; j++) {
                BufferKind *param = bufferKind(&b, type(&b, f->paramTypeNames[j]));
                if (param && param->shape == 5)
                    fail(&b, "C structs passed or returned by value require ABI lowering; use pointers");
            }
            signature(&b, f, output);
            fprintf(output, "\n");
            continue;
        }
        emitFunction(&b, f, output);
    }
    for (MethodInstance *m = b.methods; m; m = m->next) {
        b.method = m;
        b.typeBindings = m->nBindings ? m->bindings : NULL;
        emitFunction(&b, m->function, output);
    }
    b.method = NULL;
    b.typeBindings = NULL;
    if (main && !main->isExtern && !main->nParams && main->nResults <= 1) {
        b.body = tmpfile();
        b.slots = tmpfile();
        b.function = main;
        if (!b.body || !b.slots)
            fail(&b, "could not create entry point buffers");
        else {
            Value previous = pushTraceFrame(&b, main, main->name);
            if (!main->nResults)
                fprintf(b.body, "  call void @\"ocean.main\"()\n");
            else
                fprintf(b.body, "  %%result = call i64 @\"ocean.main\"()\n");
            popTraceFrame(&b, previous);
            if (!main->nResults)
                fprintf(b.body, "  ret i32 0\n");
            else
                fprintf(b.body, "  %%status = trunc i64 %%result to i32\n  ret i32 %%status\n");
            fprintf(output, "define i32 @main() {\nentry:\n");
            if (copy(b.slots, output) || copy(b.body, output))
                fail(&b, "could not write entry point IR");
            fprintf(output, "}\n");
        }
        if (b.body)
            fclose(b.body);
        if (b.slots)
            fclose(b.slots);
    }
    output = b.out;
    for (BufferKind *t = b.buffers; t; t = t->next) {
        if (t->shape != 5) continue;
        validateLayout(&b, t);
        fprintf(output, "%s = type { ", t->ir);
        for (size_t i = 0; i < t->count; i++)
            fprintf(output, "%s%s", i ? ", " : "", irType(&b, t->members[i]));
        fprintf(output, " }\n");
    }
    if (copy(functions, output)) fail(&b, "could not write module functions");
    fclose(functions);
    if (b.dataCopyUsed)
        fprintf(output, "declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n");
    emitBoundsRuntime(&b);
    if (fflush(output) || ferror(output))
        fail(&b, "could not write LLVM IR");
    while (b.buffers) {
        BufferKind *next = b.buffers->next;
        free(b.buffers->arguments);
        free(b.buffers->members);
        free(b.buffers->layout);
        free(b.buffers->ir);
        free(b.buffers);
        b.buffers = next;
    }
    while (b.methods) {
        MethodInstance *next = b.methods->next;
        free(b.methods->bindings);
        free(b.methods->arguments);
        free(b.methods);
        b.methods = next;
    }
    return b.errors ? 1 : 0;
}
/* Stage beside the destination so a failed compiler/linker preserves old output.
 * exec arguments are passed directly; paths never become shell commands. */
int compileLLVM(const char *irPath, const char *outputPath) {
    size_t len = strlen(outputPath) + 16;
    char *staged = checked(len);
    snprintf(staged, len, "%s.XXXXXX", outputPath);
    int fd = mkstemp(staged);
    if (fd < 0) {
        perror("could not create executable output");
        free(staged);
        return 1;
    }
    close(fd);
    pid_t child = fork();
    if (child < 0) {
        perror("could not start clang");
        unlink(staged);
        free(staged);
        return 1;
    }
    if (!child) {
        execlp("clang", "clang", "-Wno-override-module", "-x", "ir", irPath, "-o", staged,
               (char *)NULL);
        perror("could not execute clang (install clang and add it to PATH)");
        _exit(127);
    }
    int status = 0, failed = 0;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) {
            perror("waiting for clang");
            failed = 1;
            break;
        }
    if (failed || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "backend error: clang failed to compile or link LLVM IR\n");
        failed = 1;
    } else if (rename(staged, outputPath)) {
        perror("could not save executable");
        failed = 1;
    }
    unlink(staged);
    free(staged);
    return failed;
}
