#define _POSIX_C_SOURCE 200809L
#include "backend.h"
#include "diagnostic.h"
#include "array_type.h"
#include "buffer_type.h"
#include "value_list.h"
#include <errno.h>
#include <ffi.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Windows stdout assembly calls the stable Win32 API instead of OS-versioned syscalls.
#if defined(_WIN32)
#define OCEAN_SYSTEM_LINK_LIBRARIES "-lkernel32",
#else
#define OCEAN_SYSTEM_LINK_LIBRARIES
#endif

typedef enum { VOID, INT, I32, FLOAT, BOOL, RAW_POINTER, I8, BAD, NULL_VALUE, SCALAR_ANY, I16, F32, UINT, U32, U16, U8, BUFFER } Kind;
typedef struct {
    Kind kind;
    const char *operand; // Backend-owned text; aggregate constants can be arbitrarily long.
    struct Scope *owner;
    int throughPointer;
    const char *homogeneousArray; // Indexed writes retain the array element identity.
    Kind elementRequirement;
    const char *length; // Lexical metadata for a sized raw buffer.
} Value;
typedef struct DynamicArrayTemporary {
    Value slot;
    Kind kind;
    struct DynamicArrayTemporary *next;
} DynamicArrayTemporary;
typedef struct Binding {
    const char *name;
    Value address;
    struct Binding *next;
    int autoRelease; // Scope-owned value; pointers never own their pointees.
} Binding;
typedef struct Scope {
    Binding *bindings;
    struct stmt_block *declarations;
    struct Scope *parent;
    Value savedStack;
    Value savedBoxes;
    DynamicArrayTemporary *savedArrayTemps;
    int hasTempMark;
    int hasStack;
    int hasBreak;
    const void *jumpRegion;
    int retainAllocations; // A copied value escapes into an enclosing lexical scope.
} Scope;
typedef struct BufferKind {
    Kind kind, element;
    struct stmt_type *definition;
    Kind underlying;
    Kind requirement; // Element interface constraint for boxed arrays.
    struct stmt_interface *contract;
    int shape; // 0: fixed buffer, 2: result tuple, 3: raw buffer pointer, 4:
               // indirect array value; 5: user struct; 6: typed pointer
    struct stmt_struct *record; // Shape 5: concrete user struct; type arguments are compiler-only.
    Kind *arguments;
    size_t nArguments;
    int immutableString; // Read-only i8 dynamic array with capacity equal to length.
    int dynamicArray; // Heap payload with a {length, capacity, data} header.
    int fixedArray; // Sized Ocean array, distinct from an unchecked buffer.
    int layoutState;
    int emptyEmitted; // Empty array headers have module lifetime, including returned zeros.
    const char *zeroOperand; // Cached LLVM constant for a concrete aggregate's default value.
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
typedef struct JumpRegion {
    struct JumpRegion *parent;
} JumpRegion;
typedef struct JumpDeclaration {
    stmt *statement;
    struct JumpDeclaration *previous;
} JumpDeclaration;
typedef struct JumpTarget {
    token name;
    unsigned label;
    const void *scope;
    JumpRegion *region;
    JumpDeclaration *declarations;
    struct JumpTarget *next;
    stmt *body;
    Scope *loopScope;
    unsigned breakLabel, continueLabel;
} JumpTarget;
typedef struct JumpSite {
    token name;
    JumpRegion *region;
    JumpDeclaration *declarations;
    struct JumpSite *next;
} JumpSite;
typedef struct JumpAllocation {
    void *pointer;
    struct JumpAllocation *next;
} JumpAllocation;
typedef struct JumpContext {
    JumpTarget *targets;
    JumpSite *sites;
    JumpAllocation *allocations;
} JumpContext;
typedef struct AnyType {
    Kind kind;
    struct AnyType *next;
    int emitted, arrayEmitted;
} AnyType;
typedef struct DeclaredKind {
    struct stmt_type *declaration;
    MethodInstance *method;
    Kind kind;
    int state;
    struct DeclaredKind *next;
} DeclaredKind;
typedef struct BackendDiagnostic {
    token location;
    const char *message;
    struct BackendDiagnostic *next;
} BackendDiagnostic;
typedef struct TypeConstraint {
    Kind pattern;
    unsigned id;
    int inlineBuffer; // Test whether a fixed buffer's payload is stored in the box bits.
    struct TypeConstraint *next;
} TypeConstraint;
typedef struct OperandText {
    char *text;
    struct OperandText *next;
} OperandText;
typedef struct {
    OperandText *operands;
    AnyType *anyTypes;
    TypeConstraint *constraints;
    unsigned nConstraints;
    int arrayViewUsed, homogeneousUsed;
    int reflectionUsed;
    Kind reflectionFieldsKind;
    unsigned fixedCScalars;
    Value boxRegion;
    int anyUsed;
    JumpContext *jumps;
    TypeBinding *typeBindings;
    MethodInstance *methods, *lastMethod, *method;
    unsigned nMethods;
    unsigned recordDepth, interfaceDepth, enumDepth;
    parseResult *program;
    FILE *out, *body, *slots, *globalOutput;
    int initializingGlobals;
    Scope *scope;
    Scope *globalScope;
    struct stmt_func *function;
    unsigned next, label, strings;
    unsigned breakLabel, continueLabel;
    int terminated, errors;
    int functionHasAsm;
    int forwardPanic; // Lexical forwarding context for call trace frames.
    token location;
    BackendDiagnostic *diagnostics;
    int boundsUsed;
    int dataCopyUsed;
    int stringComparisonUsed;
    int returnTransportUsed;
    int newUsed; // calloc provides byte-zeroed heap storage, including padding.
    DynamicArrayTemporary *dynamicArrayTemps; // Compile-time list of entry-block temporary slots.
    DynamicArrayTemporary *functionTemps;
    int readingIndex;
    int copyTransport; // 1: heap clone; 2: stack clone and free; 3: free owned storage.
    int nullChecksUsed;
    BufferKind *buffers;
    DeclaredKind *declaredKinds;
    unsigned nBuffers;
    int ffiUsed;
    Scope *loopScope;
    Scope *breakScope;
} Backend;
static BufferKind *bufferKind(Backend *b, Kind k) {
    for (BufferKind *t = b->buffers; t; t = t->next)
        if (t->kind == k)
            return t;
    return NULL;
}
static Kind representationKind(Backend *b, Kind kind) {
    for (unsigned depth = 0; depth < 64; depth++) {
        BufferKind *t = bufferKind(b, kind);
        if (!t || !t->definition || t->underlying == BAD) break;
        kind = t->underlying;
    }
    return kind;
}
static int definedKind(Backend *b, Kind kind) {
    BufferKind *t = bufferKind(b, kind);
    return t && t->definition;
}
static const char *irType(Backend *b, Kind k) {
    BufferKind *buffer = bufferKind(b, k);
    if (buffer)
        return buffer->ir;
    switch (k) {
    case SCALAR_ANY:
        return "{ ptr, i64 }";
    case VOID:
        return "void";
    case UINT:
    case INT:
        return "i64";
    case U32:
    case I32:
        return "i32";
    case U16:
    case I16:
        return "i16";
    case F32:
        return "float";
    case U8:
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
    token at = b->location.line > 0 ? b->location : b->function ? b->function->name : (token){0};
    if (!at.filename) at.filename = (char *)b->program->sourceFilename;
    if (at.line <= 0) at.line = 1;
    if (at.column <= 0) at.column = 1;
    b->errors++;
    for (BackendDiagnostic *d = b->diagnostics; d; d = d->next)
        if (d->location.line == at.line && d->location.column == at.column &&
            !strcmp(d->location.filename, at.filename) && !strcmp(d->message, message)) return;
    BackendDiagnostic *d = malloc(sizeof(*d));
    if (d) { *d = (BackendDiagnostic){at, message, b->diagnostics}; b->diagnostics = d; }
    diagnostic(programSource(b->program, at.filename), at, "backend", "error", "%s", message);
}
static void *checked(size_t size) {
    void *p = calloc(1, size);
    if (!p) {
        perror("backend allocation");
        exit(1);
    }
    return p;
}
static const char *ownOperand(Backend *b, char *text) {
    OperandText *item = checked(sizeof(*item));
    *item = (OperandText){text, b->operands};
    b->operands = item;
    return text;
}
static Value value(Backend *b, Kind k, const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        fail(b, "could not format LLVM operand");
        return (Value){.kind = BAD, .operand = "0"};
    }
    char *text = checked((size_t)length + 1);
    vsnprintf(text, (size_t)length + 1, fmt, args);
    va_end(args);
    return (Value){.kind = k, .operand = ownOperand(b, text)};
}
static Value temp(Backend *b, Kind k) { return value(b, k, "%%v%u", ++b->next); }
static Value moduleString(Backend *b, const char *text) {
    unsigned id = ++b->strings;
    size_t n = strlen(text);
    fprintf(b->out, "@.str%u = private unnamed_addr constant [%zu x i8] c\"", id, n + 1);
    for (size_t i = 0; i < n; i++)
        fprintf(b->out, "\\%02X", (unsigned char)text[i]);
    fprintf(b->out, "\\00\"\n");
    return value(b, RAW_POINTER, "@.str%u", id);
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
    const char *line = programSource(b->program, location.filename);
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
    char *text = sourceSnippet(b, location);
    Value snippet = moduleString(b, text);
    free(text);
    Value fields[] = {previous, moduleString(b, function->displayName ? function->displayName : function->name.lexeme),
                      moduleString(b, location.filename), value(b, INT, "%ld", location.line),
                      value(b, INT, "%ld", location.column), snippet,
                      value(b, BOOL, "%d", b->forwardPanic != 0)};
    for (size_t i = 0; i < 7; i++) {
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
        if (!v->definition && v->ir && !v->shape && !v->fixedArray && v->element == element && v->count == count)
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
static Kind fixedArrayKind(Backend *b, Kind element, size_t count) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (!v->definition && !v->shape && v->fixedArray && v->element == element && v->count == count)
            return v->kind;
    Kind raw = arrayKind(b, element, count);
    BufferKind *v = checked(sizeof(*v));
    *v = *bufferKind(b, raw);
    v->kind = (Kind)(BUFFER + b->nBuffers++);
    size_t n = strlen(irType(b, element)) + 80;
    v->ir = checked(n);
    snprintf(v->ir, n, "{ i64, [%zu x %s] }", count, irType(b, element));
    v->fixedArray = 1;
    v->next = b->buffers; b->buffers = v;
    return v->kind;
}
static Kind structKind(Backend *b, int shape, Kind element, Kind *members, size_t count) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (!v->definition && v->shape == shape && v->element == element && v->count == count &&
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
        if (!v->definition && v->ir && v->shape == 3 && !v->fixedArray && v->element == element)
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
static Kind arrayValueKind(Backend *b, Kind element) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (!v->definition && v->ir && v->shape == 4 && !v->dynamicArray && !v->fixedArray && !v->count && !v->requirement && v->element == element)
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
static Kind runtimeArrayKind(Backend *b, Kind element) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (!v->definition && v->shape == 4 && v->fixedArray && v->element == element) return v->kind;
    Kind base = arrayValueKind(b, element);
    BufferKind *v = checked(sizeof(*v)); *v = *bufferKind(b, base);
    v->kind = (Kind)(BUFFER + b->nBuffers++); v->fixedArray = 1;
    v->ir = strdup(v->ir); v->layout = strdup(v->layout);
    v->next = b->buffers; b->buffers = v;
    return v->kind;
}
static Kind dynamicArrayKind(Backend *b, Kind element) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (v->dynamicArray && !v->immutableString && !v->definition && v->element == element) return v->kind;
    Kind base = arrayValueKind(b, element);
    BufferKind *v = checked(sizeof(*v)); *v = *bufferKind(b, base);
    v->kind = (Kind)(BUFFER + b->nBuffers++); v->dynamicArray = 1;
    v->ir = strdup(v->ir); v->layout = strdup("{ i64, i64, ptr }");
    v->next = b->buffers; b->buffers = v;
    return v->kind;
}
static Kind stringKind(Backend *b) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (v->immutableString && !v->definition) return v->kind;
    Kind base = dynamicArrayKind(b, I8);
    BufferKind *v = checked(sizeof(*v)); *v = *bufferKind(b, base);
    v->kind = (Kind)(BUFFER + b->nBuffers++); v->immutableString = 1;
    v->ir = strdup(v->ir); v->layout = strdup(v->layout);
    v->zeroOperand = NULL; v->emptyEmitted = 0;
    v->next = b->buffers; b->buffers = v;
    return v->kind;
}
// Variadic packs may contain different types; ordinary any[] arrays may not.
static Kind mixedArrayKind(Backend *b) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (!v->definition && v->ir && v->shape == 4 && v->count == 1 && !v->requirement && v->element == SCALAR_ANY) return v->kind;
    BufferKind *base = bufferKind(b, arrayValueKind(b, SCALAR_ANY));
    BufferKind *v = checked(sizeof(*v));
    *v = *base;
    v->kind = (Kind)(BUFFER + b->nBuffers++);
    v->count = 1;
    v->ir = strdup(base->ir); v->layout = strdup(base->layout);
    v->zeroOperand = NULL; v->emptyEmitted = 0;
    v->next = b->buffers; b->buffers = v;
    return v->kind;
}
static Kind constrainedArrayKind(Backend *b, Kind requirement, int mixed) {
    if (requirement == SCALAR_ANY) return mixed ? mixedArrayKind(b) : arrayValueKind(b, SCALAR_ANY);
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (!v->definition && v->ir && v->shape == 4 && v->count == (size_t)mixed && v->requirement == requirement) return v->kind;
    BufferKind *base = bufferKind(b, arrayValueKind(b, SCALAR_ANY));
    BufferKind *v = checked(sizeof(*v)); *v = *base;
    v->kind = (Kind)(BUFFER + b->nBuffers++); v->count = mixed; v->requirement = requirement;
    v->ir = strdup(base->ir); v->layout = strdup(base->layout);
    v->zeroOperand = NULL; v->emptyEmitted = 0;
    v->next = b->buffers; b->buffers = v;
    return v->kind;
}
static struct stmt_interface *findInterface(Backend *b, const char *name) {
    for (Scope *scope = b->scope; scope; scope = scope->parent) {
        if (!scope->declarations) continue;
        for (size_t i = 0; i < scope->declarations->nStmts; i++) {
            stmt *s = &scope->declarations->stmts[i];
            if (s->type == STMT_INTERFACE && !strcmp(((struct stmt_interface *)s->value)->name.lexeme, name)) return s->value;
        }
    }
    for (size_t i = 0; i < b->program->nStatements; i++) {
        stmt *s = &b->program->statements[i];
        if (s->type == STMT_INTERFACE && !strcmp(((struct stmt_interface *)s->value)->name.lexeme, name)) return s->value;
    }
    return NULL;
}
static Kind interfacePattern(Backend *b, struct stmt_interface *contract) {
    if (!contract->nMethods && !contract->nConstraints && !contract->nTypeParams) return SCALAR_ANY;
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (v->shape == 12 && v->contract == contract) return v->kind;
    BufferKind *v = checked(sizeof(*v));
    v->kind = (Kind)(BUFFER + b->nBuffers++); v->shape = 12; v->contract = contract;
    v->next = b->buffers; b->buffers = v;
    return v->kind;
}
static Kind type(Backend *, typeTokens);
static struct stmt_struct *findRecord(Backend *b, const char *name) {
    for (Scope *scope = b->scope; scope; scope = scope->parent) {
        if (!scope->declarations) continue;
        for (size_t i = 0; i < scope->declarations->nStmts; i++) {
            stmt *s = &scope->declarations->stmts[i];
            if (s->type == STMT_ENUM) {
                struct stmt_enum *v = s->value;
                if (!strcmp(v->name.lexeme, name)) return &v->storage;
                for (size_t j = 0; j < v->nMembers; j++)
                    if (!strcmp(v->payloads[j].name.lexeme, name)) return &v->payloads[j];
            }
            if (s->type == STMT_STRUCT && !strcmp(((struct stmt_struct *)s->value)->name.lexeme, name))
                return s->value;
        }
    }
    for (size_t i = 0; i < b->program->nStatements; i++) {
        stmt *s = &b->program->statements[i];
        if (s->type == STMT_ENUM) {
            struct stmt_enum *v = s->value;
            if (!strcmp(v->name.lexeme, name)) return &v->storage;
            for (size_t j = 0; j < v->nMembers; j++)
                if (!strcmp(v->payloads[j].name.lexeme, name)) return &v->payloads[j];
        }
        if (s->type == STMT_STRUCT && !strcmp(((struct stmt_struct *)s->value)->name.lexeme, name))
            return s->value;
    }
    return NULL;
}
static Kind pointerKind(Backend *b, Kind element) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (!v->definition && v->ir && v->shape == 6 && v->element == element)
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
static Kind recordFieldKind(Backend *b, typeTokens span) {
    typeTokens element, dimension;
    if (sizedArrayTypeParts(span, &element, &dimension)) {
        Kind item = recordFieldKind(b, element);
        if (dimension.nTokens == 1 && dimension.tokens[0].type == LITERAL_INT)
            return fixedArrayKind(b, item, dimension.tokens[0].numValue.integer);
        return runtimeArrayKind(b, item);
    }
    return type(b, span);
}
static Kind recordKind(Backend *b, struct stmt_struct *record, Kind *arguments, size_t count) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (!v->definition && v->shape == 5 && v->record == record && v->nArguments == count &&
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
        } else v->members[i] = recordFieldKind(b, record->fieldTypes[i]);
    }
    b->recordDepth--;
    b->typeBindings = previous;
    free(bindings);
    if (!b->recordDepth && !validateLayout(b, v)) return BAD;
    return v->kind;
}
// Constraint patterns are separate from storage layouts. A family pattern retains
// its nominal identity even when nested in another generic argument.
static int concreteConstraint(Backend *b, Kind kind) {
    if (kind == SCALAR_ANY) return 0;
    BufferKind *t = bufferKind(b, kind);
    if (t && t->definition) return 1;
    if (!t) return 1;
    if (t->shape == 14) return concreteConstraint(b, t->element);
    if (t->shape == 8 || t->shape == 9 || t->shape == 12) return 0; // A union retains a boxed binding.
    if (t->record) {
        if (t->record->nTypeParams != t->nArguments) return 0;
        for (size_t i = 0; i < t->nArguments; i++)
            if (!concreteConstraint(b, t->arguments[i])) return 0;
        return 1;
    }
    return concreteConstraint(b, t->element);
}
static Kind constraintShape(Backend *b, int shape, Kind element, size_t count) {
    BufferKind *pattern = checked(sizeof(*pattern));
    pattern->kind = (Kind)(BUFFER + b->nBuffers++);
    pattern->shape = shape;
    pattern->element = element;
    pattern->count = count;
    pattern->next = b->buffers;
    b->buffers = pattern;
    return pattern->kind;
}
static struct stmt_type *findTypeDeclaration(Backend *b, const char *name, Scope **owner) {
    for (Scope *scope = b->scope; scope; scope = scope->parent) {
        if (!scope->declarations) continue;
        for (size_t i = 0; i < scope->declarations->nStmts; i++) {
            stmt *s = &scope->declarations->stmts[i];
            if (s->type == STMT_TYPE && !strcmp(((struct stmt_type *)s->value)->name.lexeme, name)) {
                *owner = scope; return s->value;
            }
        }
    }
    for (size_t i = 0; i < b->program->nStatements; i++) {
        stmt *s = &b->program->statements[i];
        if (s->type == STMT_TYPE && !strcmp(((struct stmt_type *)s->value)->name.lexeme, name)) {
            *owner = b->globalScope; return s->value;
        }
    }
    return NULL;
}
static Kind declaredKind(Backend *b, struct stmt_type *declaration, Scope *owner) {
    MethodInstance *method = owner == b->globalScope ? NULL : b->method;
    DeclaredKind *entry;
    for (entry = b->declaredKinds; entry; entry = entry->next)
        if (entry->declaration == declaration && entry->method == method) break;
    if (entry) {
        if (entry->state == 1 && declaration->isAlias) { fail(b, "cyclic type alias"); return BAD; }
        return entry->kind;
    }
    entry = checked(sizeof(*entry));
    *entry = (DeclaredKind){declaration, method, BAD, 1, b->declaredKinds};
    b->declaredKinds = entry;
    BufferKind *named = NULL;
    if (!declaration->isAlias) {
        named = checked(sizeof(*named)); named->kind = (Kind)(BUFFER + b->nBuffers++);
        named->definition = declaration; named->underlying = BAD; named->shape = 13;
        named->next = b->buffers; b->buffers = named;
        entry->kind = named->kind;
    }
    Scope *previous = b->scope; b->scope = owner;
    Kind base = type(b, declaration->underlying);
    b->scope = previous;
    if (declaration->isAlias) { entry->kind = base; entry->state = 2; return base; }
    Kind identity = named->kind;
    BufferKind *layout = bufferKind(b, base), *next = named->next;
    if (layout) {
        // Share immutable layout tables: recursive record fields can still be resolving.
        *named = *layout;
    }
    named->kind = identity; named->definition = declaration; named->underlying = base; named->next = next;
    named->ir = strdup(irType(b, base));
    named->layout = layout && layout->layout ? strdup(layout->layout) : NULL;
    named->emptyEmitted = 0; named->zeroOperand = NULL;
    entry->state = 2;
    return identity;
}
static const char *receiverTypeName(Backend *b, const char *name) {
    for (unsigned depth = 0; depth < 64; depth++) {
        Scope *owner = NULL;
        struct stmt_type *declaration = findTypeDeclaration(b, name, &owner);
        if (!declaration || !declaration->isAlias || !declaration->underlying.nTokens) return name;
        name = declaration->underlying.tokens[0].lexeme;
    }
    return name;
}
static Kind constraintKind(Backend *b, typeTokens span) {
    if (span.nTokens == 1) {
        Scope *owner = NULL;
        struct stmt_type *declaration = findTypeDeclaration(b, span.tokens[0].lexeme, &owner);
        if (declaration && declaration->isAlias) {
            Scope *previous = b->scope; b->scope = owner;
            Kind result = constraintKind(b, declaration->underlying);
            b->scope = previous; return result;
        }
        if (declaration) return declaredKind(b, declaration, owner);
        struct stmt_interface *contract = findInterface(b, span.tokens[0].lexeme);
        if (contract) return interfacePattern(b, contract);
    }
    size_t nesting = 0;
    for (size_t i = 0; i < span.nTokens; i++) {
        enum tokenType k = span.tokens[i].type;
        if (!nesting && k == BITWISE_OR) {
            Kind left = constraintKind(b, (typeTokens){i, span.tokens});
            Kind right = constraintKind(b, (typeTokens){span.nTokens - i - 1, span.tokens + i + 1});
            Kind result = constraintShape(b, 8, VOID, 0);
            BufferKind *pattern = bufferKind(b, result);
            pattern->arguments = checked(2 * sizeof(Kind));
            pattern->nArguments = 2;
            pattern->arguments[0] = left;
            pattern->arguments[1] = right;
            return result;
        }
        if (k == LESS_THAN || k == LEFT_BRACKET || k == LEFT_PAREN || k == LEFT_CURLY_BRACE) nesting++;
        if ((k == GREATER_THAN || k == RIGHT_BRACKET || k == RIGHT_PAREN || k == RIGHT_CURLY_BRACE) && nesting) nesting--;
        if (k == BITSHIFT_RIGHT) nesting = nesting > 1 ? nesting - 2 : 0;
    }
    if (span.nTokens && span.tokens[0].type == TILDE) {
        span.tokens++; span.nTokens--;
        return constraintShape(b, 14, constraintKind(b, span), 0);
    }
    if (isBufferType(span) && span.nTokens == 1)
        return constraintShape(b, 9, SCALAR_ANY, 0);
    typeTokens element, dimension;
    if (bufferTypeParts(span, &element, &dimension) &&
        span.tokens[span.nTokens - 1].type == GREATER_THAN)
        return constraintShape(b, 9, constraintKind(b, element), 0);
    if (unsizedArrayTypeParts(span, &element))
        return constraintShape(b, 16, constraintKind(b, element), 0);
    Kind child;
    int shape = -1;
    size_t count = 0;
    if (span.nTokens > 1 && span.tokens[span.nTokens - 1].type == STAR) {
        element = (typeTokens){span.nTokens - 1, span.tokens};
        shape = 6;
    } else if (span.nTokens && span.tokens[0].type == STAR) {
        element = (typeTokens){span.nTokens - 1, span.tokens + 1};
        shape = 6;
    } else if (arrayElementType(span, &element)) shape = 4;
    else if (sizedArrayTypeParts(span, &element, &dimension)) {
        if (dimension.nTokens == 1 && dimension.tokens[0].type == LITERAL_INT) {
            Kind item = constraintKind(b, element);
            return concreteConstraint(b, item) ? type(b, span) :
                constraintShape(b, 15, item, dimension.tokens[0].numValue.integer);
        }
        return type(b, span);
    }
    else if (bufferTypeParts(span, &element, &dimension)) {
        shape = bufferDimensionKind(dimension) == 1 ? 0 : 3;
        if (!shape) count = (size_t)dimension.tokens[0].numValue.integer;
    }
    if (shape >= 0) {
        child = constraintKind(b, element);
        if (shape == 4 && span.tokens[0].type == MIXED) count = 1;
        return concreteConstraint(b, child) ? type(b, span) : constraintShape(b, shape, child, count);
    }
    struct stmt_struct *record = span.nTokens ? findRecord(b, span.tokens[0].lexeme) : NULL;
    if (!record || !record->nTypeParams) return type(b, span);
    typeTokens *args = NULL;
    count = 0;
    if (span.nTokens > 1 && !parseTypeApplicationArguments(span, &args, &count))
        return type(b, span);
    BufferKind *pattern = checked(sizeof(*pattern));
    pattern->kind = (Kind)(BUFFER + b->nBuffers++);
    pattern->shape = 7;
    pattern->record = record;
    pattern->nArguments = count;
    pattern->arguments = checked(sizeof(Kind) * (count + 1));
    for (size_t i = 0; i < count; i++) {
        pattern->arguments[i] = constraintKind(b, args[i]);
        free(args[i].tokens);
    }
    free(args);
    pattern->next = b->buffers;
    b->buffers = pattern;
    return pattern->kind;
}
static int matchesInterface(Backend *, struct stmt_interface *, Kind);
static int matchesConstraint(Backend *b, Kind pattern, Kind actual) {
    if (pattern == SCALAR_ANY || pattern == actual) return 1;
    BufferKind *p = bufferKind(b, pattern), *a = bufferKind(b, actual);
    if (p && p->shape == 8) {
        for (size_t i = 0; i < p->nArguments; i++)
            if (matchesConstraint(b, p->arguments[i], actual)) return 1;
        return 0;
    }
    if (p && p->shape == 14) return matchesConstraint(b, representationKind(b, p->element), representationKind(b, actual));
    if (p && p->shape == 12) return matchesInterface(b, p->contract, actual);
    if ((p && p->definition) || (a && a->definition)) return 0;
    if (!p || !a) return 0;
    if (p->fixedArray && p->shape == 4)
        return a->fixedArray && matchesConstraint(b, p->element, a->element);
    if (p->shape == 4 && a->shape == 4) {
        if (a->fixedArray) return 0;
        if (!p->count && a->count) return 0;
        Kind bound = p->requirement ? p->requirement : p->element;
        Kind element = a->requirement ? a->requirement : a->element;
        return matchesConstraint(b, bound, element);
    }
    if (p->shape == 9)
        return !a->fixedArray && (a->shape == 0 || a->shape == 3) &&
               matchesConstraint(b, p->element, a->element);
    if (p->record) {
        if (p->record != a->record) return 0;
        if (!p->nArguments) return 1;
        if (p->nArguments != a->nArguments) return 0;
        for (size_t i = 0; i < p->nArguments; i++)
            if (!matchesConstraint(b, p->arguments[i], a->arguments[i])) return 0;
        return 1;
    }
    if (p->shape == 16) return a->fixedArray && matchesConstraint(b, p->element, a->element);
    if (p->shape == 15) return a->fixedArray && !a->shape && p->count == a->count &&
        matchesConstraint(b, p->element, a->element);
    return p->shape == a->shape && p->fixedArray == a->fixedArray && p->count == a->count &&
           matchesConstraint(b, p->element, a->element);
}
static int universalConstraint(Backend *b, Kind kind) {
    if (kind == SCALAR_ANY) return 1;
    BufferKind *pattern = bufferKind(b, kind);
    if (!pattern || pattern->shape != 8) return 0;
    for (size_t i = 0; i < pattern->nArguments; i++)
        if (universalConstraint(b, pattern->arguments[i])) return 1;
    return 0;
}
static Value testConstraint(Backend *b, Value box, Kind pattern, int inlineBuffer) {
    TypeConstraint *c = checked(sizeof(*c));
    *c = (TypeConstraint){.pattern = pattern, .id = ++b->nConstraints,
                          .inlineBuffer = inlineBuffer, .next = b->constraints};
    b->constraints = c;
    Value tag = temp(b, RAW_POINTER), result = temp(b, BOOL);
    fprintf(b->body, "  %s = extractvalue { ptr, i64 } %s, 0\n", tag.operand, box.operand);
    fprintf(b->body, "  %s = call i1 @__ocean.constraint.%u(ptr %s)\n",
            result.operand, c->id, tag.operand);
    return result;
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
    if (arrayElementType(t, &arrayElement)) {
        Kind element = type(b, arrayElement);
        if (t.tokens[0].type == MIXED || element == SCALAR_ANY)
            return constrainedArrayKind(b, constraintKind(b, arrayElement), t.tokens[0].type == MIXED);
        return dynamicArrayKind(b, element);
    }
    typeTokens unsizedElement;
    if (unsizedArrayTypeParts(t, &unsizedElement)) return runtimeArrayKind(b, type(b, unsizedElement));
    typeTokens sizedElement, sizedDimension;
    if (sizedArrayTypeParts(t, &sizedElement, &sizedDimension)) {
        if (sizedDimension.nTokens == 1 && sizedDimension.tokens[0].type == LITERAL_INT &&
            sizedDimension.tokens[0].numValue.integer >= 0)
            return fixedArrayKind(b, type(b, sizedElement), sizedDimension.tokens[0].numValue.integer);
        return runtimeArrayKind(b, type(b, sizedElement));
    }
    if (t.nTokens == 3 && t.tokens[0].type == INTERFACE &&
        t.tokens[1].type == LEFT_CURLY_BRACE && t.tokens[2].type == RIGHT_CURLY_BRACE) return SCALAR_ANY;
    if (t.nTokens == 1 && findInterface(b, t.tokens[0].lexeme)) return SCALAR_ANY;
    if (isBufferType(t)) {
        if (t.nTokens == 1) return SCALAR_ANY;
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
    Scope *declarationOwner = NULL;
    struct stmt_type *declaration = findTypeDeclaration(b, t.tokens[0].lexeme, &declarationOwner);
    if (declaration && t.nTokens == 1) return declaredKind(b, declaration, declarationOwner);
    if (declaration && declaration->isAlias && t.nTokens > 1) {
        typeTokens underlying = declaration->underlying;
        token *tokens = checked(sizeof(token) * (underlying.nTokens + t.nTokens - 1));
        memcpy(tokens, underlying.tokens, sizeof(token) * underlying.nTokens);
        memcpy(tokens + underlying.nTokens, t.tokens + 1, sizeof(token) * (t.nTokens - 1));
        Scope *previous = b->scope; b->scope = declarationOwner;
        Kind result = type(b, (typeTokens){underlying.nTokens + t.nTokens - 1, tokens});
        b->scope = previous; free(tokens); return result;
    }
    struct stmt_struct *record = findRecord(b, t.tokens[0].lexeme);
    if (record) {
        typeTokens *spans = NULL;
        size_t count = 0;
        if (t.nTokens > 1 && !parseTypeApplicationArguments(t, &spans, &count)) {
            fail(b, "invalid struct type arguments");
            return BAD;
        }
        if (t.nTokens == 1 && record->nTypeParams) return SCALAR_ANY;
        Kind *arguments = checked(sizeof(Kind) * (count + 1));
        int erased = 0;
        for (size_t i = 0; i < count; i++) {
            arguments[i] = type(b, spans[i]);
            if (arguments[i] == SCALAR_ANY) erased = 1;
            free(spans[i].tokens);
        }
        free(spans);
        Kind result = erased && !record->enumStorage ? SCALAR_ANY : recordKind(b, record, arguments, count);
        free(arguments);
        return result;
    }
    if (t.nTokens != 1) {
        fail(b, "only scalar types are supported");
        return BAD;
    }
    const char *s = t.tokens[0].lexeme;
    if (!strcmp(s, "uint") || !strcmp(s, "u64")) return UINT;
    if (!strcmp(s, "u32")) return U32;
    if (!strcmp(s, "u16")) return U16;
    if (!strcmp(s, "u8")) return U8;
    if (!strcmp(s, "any")) return SCALAR_ANY;
    if (!strcmp(s, "void")) return VOID;
    if (!strcmp(s, "int") || !strcmp(s, "i64"))
        return INT;
    if (!strcmp(s, "i8"))
        return I8;
    if (!strcmp(s, "i16")) return I16;
    if (!strcmp(s, "f32")) return F32;
    if (!strcmp(s, "i32"))
        return I32;
    if (!strcmp(s, "float") || !strcmp(s, "f64"))
        return FLOAT;
    if (!strcmp(s, "bool"))
        return BOOL;
    if (!strcmp(s, "string"))
        return stringKind(b);
    fail(b, "unsupported type (supported: int, i64, i32, i16, i8, uint, u64, u32, u16, u8, float, f64, f32, bool, string)");
    return BAD;
}
static int identicalKinds(Backend *b, Kind left, Kind right) {
    if (left == right) return 1;
    BufferKind *a = bufferKind(b, left), *c = bufferKind(b, right);
    if (!a || !c) return 0;
    if (a->contract || c->contract) return a->contract && a->contract == c->contract;
    if (a->record || c->record) {
        if (a->record != c->record || a->nArguments != c->nArguments) return 0;
        for (size_t i = 0; i < a->nArguments; i++)
            if (!identicalKinds(b, a->arguments[i], c->arguments[i])) return 0;
        return 1;
    }
    return a->shape == c->shape && a->fixedArray == c->fixedArray && a->dynamicArray == c->dynamicArray && a->immutableString == c->immutableString &&
           a->count == c->count && a->requirement == c->requirement &&
           identicalKinds(b, a->element, c->element);
}
static int matchesInterface(Backend *b, struct stmt_interface *contract, Kind actual) {
    if (b->interfaceDepth++ >= 32) { b->interfaceDepth--; return 0; }
    int accepted = !contract->nTypeParams;
    for (size_t i = 0; accepted && i < contract->nConstraints; i++) {
        typeTokens span = contract->constraints[i];
        int matches = 0;
        for (size_t start = 0; start < span.nTokens;) {
            size_t end = start, nesting = 0;
            for (; end < span.nTokens; end++) {
                enum tokenType t = span.tokens[end].type;
                if (!nesting && t == BITWISE_OR) break;
                if (t == LEFT_BRACKET || t == LESS_THAN || t == LEFT_PAREN) nesting++;
                if ((t == RIGHT_BRACKET || t == GREATER_THAN || t == RIGHT_PAREN) && nesting) nesting--;
                if (t == BITSHIFT_RIGHT) nesting = nesting > 1 ? nesting - 2 : 0;
            }
            typeTokens term = {end - start, span.tokens + start};
            int exclude = term.nTokens && term.tokens[0].type == MINUS;
            if (term.nTokens && (exclude || term.tokens[0].type == PLUS || term.tokens[0].type == EQUALS || term.tokens[0].type == TILDE)) {
                term.tokens++; term.nTokens--;
            }
            int fits = matchesConstraint(b, constraintKind(b, term), actual);
            if (exclude ? !fits : fits) matches = 1;
            start = end + 1;
        }
        if (!matches) accepted = 0;
    }
    BufferKind *owner = bufferKind(b, actual);
    int pointer = owner && !owner->definition && owner->shape == 6;
    if (pointer) owner = bufferKind(b, owner->element);
    for (size_t i = 0; accepted && i < contract->nMethods; i++) {
        struct stmt_func *required = &contract->methods[i], *provided = NULL;
        if (owner && owner->shape == 12) {
            for (size_t j = 0; j < owner->contract->nMethods; j++)
                if (!strcmp(owner->contract->methods[j].name.lexeme, required->name.lexeme)) provided = &owner->contract->methods[j];
        } else if (owner && (owner->record || owner->definition)) {
            for (size_t j = 0; j < b->program->nStatements; j++) {
                stmt *s = &b->program->statements[j];
                if (s->type != STMT_FUNC) continue;
                struct stmt_func *f = s->value;
                if (!f->receiverType.nTokens || strcmp(f->name.lexeme, required->name.lexeme)) continue;
                typeTokens receiver = f->receiverType;
                int needsPointer = receiver.tokens[0].type == STAR || receiver.tokens[receiver.nTokens - 1].type == STAR;
                if (needsPointer && !pointer) continue;
                size_t index = receiver.tokens[0].type == STAR ? 1 : 0;
                const char *name = owner->definition ? owner->definition->name.lexeme : owner->record->name.lexeme;
                if (!strcmp(receiverTypeName(b, receiver.tokens[index].lexeme), name)) provided = f;
            }
        }
        if (!provided || provided->nTypeParams || required->nTypeParams ||
            required->nParams != provided->nParams || required->nResults != provided->nResults ||
            required->isVariadic != provided->isVariadic) { accepted = 0; break; }
        TypeBinding *previous = b->typeBindings;
        TypeBinding *bindings = checked(sizeof(*bindings) * (provided->nReceiverTypeParams + 1));
        for (size_t j = 0; j < provided->nReceiverTypeParams && j < owner->nArguments; j++) {
            bindings[j] = (TypeBinding){provided->receiverTypeParamNames[j].lexeme, owner->arguments[j], b->typeBindings};
            b->typeBindings = &bindings[j];
        }
        for (size_t j = 0; accepted && j < required->nParams + required->nResults; j++) {
            typeTokens a = j < required->nParams ? required->paramTypeNames[j] : required->resultTypes[j - required->nParams];
            typeTokens c = j < provided->nParams ? provided->paramTypeNames[j] : provided->resultTypes[j - provided->nParams];
            Kind want = constraintKind(b, a), have = constraintKind(b, c);
            if (!identicalKinds(b, want, have)) accepted = 0;
        }
        b->typeBindings = previous;
        free(bindings);
    }
    b->interfaceDepth--;
    return accepted;
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
    typeTokens element, dimension;
    if ((!f->isVariadic || i + 1 < f->nParams) && bufferTypeParts(f->paramTypeNames[i], &element, &dimension))
        return pointerBufferKind(b, type(b, element));
    return f->isVariadic && !f->isExtern && i + 1 == f->nParams
               ? k == SCALAR_ANY ? constrainedArrayKind(b, constraintKind(b, f->paramTypeNames[i]), 1)
                                 : arrayValueKind(b, k) : k;
}

static struct stmt_func *findFunction(Backend *b, const char *name) {
    for (int aliases = 0; aliases < 2; aliases++)
        for (size_t i = 0; i < b->program->nStatements; i++) {
            stmt *s = &b->program->statements[i];
            if (s->type != STMT_FUNC) continue;
            struct stmt_func *f = s->value;
            if (f->receiverType.nTokens) continue;
            if ((!aliases && !strcmp(f->name.lexeme, name)) ||
                (aliases && f->isExtern && f->externalName && !strcmp(f->externalName, name))) return f;
        }
    return NULL;
}
/* Internal functions are mangled so the C ABI entry point can wrap Ocean main. */
static void functionName(FILE *out, struct stmt_func *f) {
    fprintf(out, "@\"%s%s\"", f->isExtern ? "" : "ocean.",
            f->isExtern && f->externalName ? f->externalName : f->name.lexeme);
}
static void methodName(FILE *out, MethodInstance *method) {
    fprintf(out, "@\"ocean.%s.%u.%s\"",
            method->function->receiverType.nTokens ? "method" : "function",
            method->id, method->function->name.lexeme);
}
static MethodInstance *methodInstance(Backend *b, Kind receiver, const char *name,
                                      typeTokens *typeArguments, size_t count) {
    BufferKind *record = bufferKind(b, receiver);
    while (record && !record->definition && record->shape == 6) record = bufferKind(b, record->element);
    if (!record || (!record->definition && record->shape != 5)) {
        fail(b, "method receiver must be a struct, defined type, or pointer to one");
        return NULL;
    }
    struct stmt_func *method = NULL;
    for (size_t i = 0; i < b->program->nStatements; i++) {
        stmt *s = &b->program->statements[i];
        if (s->type != STMT_FUNC) continue;
        struct stmt_func *f = s->value;
        if (!f->receiverType.nTokens || strcmp(f->name.lexeme, name)) continue;
        token owner = f->receiverType.tokens[f->receiverType.tokens[0].type == STAR ? 1 : 0];
        const char *recordName = record->definition ? record->definition->name.lexeme : record->record->name.lexeme;
        if (!strcmp(receiverTypeName(b, owner.lexeme), recordName)) {
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
// Direct generic functions share the concrete-specialization queue with methods.
static MethodInstance *functionInstance(Backend *b, struct stmt_func *f,
                                        typeTokens *spans, size_t count) {
    if (f->nTypeParams != count) {
        fail(b, "generic function requires explicit type arguments matching its type parameters");
        return NULL;
    }
    Kind *arguments = checked(sizeof(Kind) * (count + 1));
    for (size_t i = 0; i < count; i++) arguments[i] = type(b, spans[i]);
    for (MethodInstance *m = b->methods; m; m = m->next) {
        if (m->function == f && m->nArguments == count &&
            (!count || !memcmp(m->arguments, arguments, sizeof(Kind) * count))) {
            free(arguments);
            return m;
        }
    }
    if (b->nMethods >= 256) {
        free(arguments);
        fail(b, "function specialization limit exceeded");
        return NULL;
    }
    MethodInstance *m = checked(sizeof(*m));
    m->function = f;
    m->receiver = VOID;
    m->id = ++b->nMethods;
    m->arguments = arguments;
    m->nArguments = m->nBindings = count;
    m->bindings = checked(sizeof(TypeBinding) * (count + 1));
    for (size_t i = 0; i < count; i++)
        m->bindings[i] = (TypeBinding){f->typeParamNames[i].lexeme, arguments[i],
                                      i + 1 < count ? &m->bindings[i + 1] : NULL};
    if (b->lastMethod) b->lastMethod->next = m;
    else b->methods = m;
    b->lastMethod = m;
    return m;
}
static struct stmt_func *directCallee(Backend *b, expr *callee,
                                      typeTokens **arguments, size_t *count) {
    *arguments = NULL;
    *count = 0;
    if (callee->type == EXPR_TYPE_INSTANTIATION) {
        struct expr_instantiation *v = callee->underlying;
        *arguments = v->typeArguments;
        *count = v->nTypeArguments;
        callee = &v->parent;
    }
    if (callee->type != EXPR_TYPE_IDENTIFIER) return NULL;
    struct expr_identifier *identifier = callee->underlying;
    if (!identifier->externReference) return findFunction(b, identifier->lexeme.lexeme);
    for (size_t i = 0; i < b->program->nStatements; i++) {
        stmt *s = &b->program->statements[i];
        if (s->type != STMT_FUNC) continue;
        struct stmt_func *f = s->value;
        if (!f->isExtern || !f->name.filename || !identifier->lexeme.filename ||
            strcmp(f->name.filename, identifier->lexeme.filename)) continue;
        if (!strcmp(f->name.lexeme, identifier->lexeme.lexeme) ||
            (f->externalName && !strcmp(f->externalName, identifier->lexeme.lexeme))) return f;
    }
    return NULL;
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
    return value(b, BAD, "null");
}
/* Keep all allocas in the entry block, including locals declared inside loops. */
static Value emptyValue(Backend *, Kind);
static int containsDynamicArrays(Backend *b, Kind kind) {
    BufferKind *t = bufferKind(b, kind);
    if (!t) return 0;
    if (t->dynamicArray) return 1;
    if (t->shape == 4) return 1;
    if (!t->shape) return containsDynamicArrays(b, t->element);
    if (t->shape == 2 || t->shape == 5)
        for (size_t i = 0; i < t->count; i++) if (containsDynamicArrays(b, t->members[i])) return 1;
    return 0;
}
static Value slot(Backend *b, Kind k) {
    Value v = temp(b, k);
    v.owner = b->scope;
    fprintf(b->slots, "  %s = alloca %s\n", v.operand, irType(b, k));
    BufferKind *t = bufferKind(b, k);
    if (containsDynamicArrays(b, k)) {
        Value zero = emptyValue(b, k);
        fprintf(b->slots, "  store %s %s, ptr %s\n", irType(b, k), zero.operand, v.operand);
    }
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
static int unsignedInteger(Backend *b, Kind k) { k = representationKind(b, k); return k == UINT || k == U32 || k == U16 || k == U8; }
static int integral(Backend *b, Kind k) { k = representationKind(b, k); return k == INT || k == I32 || k == I16 || k == I8 || unsignedInteger(b, k); }
static int floating(Backend *b, Kind k) { k = representationKind(b, k); return k == FLOAT || k == F32; }
static int integerWidth(Backend *b, Kind k) { k = representationKind(b, k); return (k == I8 || k == U8) ? 8 : (k == I16 || k == U16) ? 16 : (k == I32 || k == U32) ? 32 : 64; }
static int implicitVoidPointer(Backend *b, Kind source, Kind target) {
    BufferKind *from = bufferKind(b, source), *to = bufferKind(b, target);
    return to && to->shape == 6 && to->element == VOID &&
           (source == RAW_POINTER || (from && (from->shape == 6 || from->shape == 3)));
}
static void branch(Backend *, Value, unsigned, unsigned);
static void label(Backend *, unsigned);
static Value boxScalar(Backend *, Value, int);
static Value unboxValue(Backend *, Value, Kind);
static Value coerce(Backend *, Value, Kind);
static void checkElementRequirement(Backend *, Value, Kind);
static void checkArrayRequirement(Backend *, Value, Kind);
static Value collectionCopy(Backend *, Value, Kind);
static Value convert(Backend *b, Value v, Kind k) {
    if (v.kind == NULL_VALUE) return coerce(b, v, k);
    if (representationKind(b, v.kind) == representationKind(b, k)) return value(b, k, "%s", v.operand);
    if (definedKind(b, v.kind) && representationKind(b, v.kind) == SCALAR_ANY)
        v.kind = SCALAR_ANY;
    if (definedKind(b, k) && representationKind(b, k) == SCALAR_ANY) {
        Value result = boxScalar(b, v, 1); result.kind = k; return result;
    }
    if (k == SCALAR_ANY) return boxScalar(b, v, 1);
    if (v.kind == SCALAR_ANY) return unboxValue(b, v, k);
    if (v.kind == k)
        return v;
    BufferKind *source = bufferKind(b, v.kind), *target = bufferKind(b, k);
    int sourcePointer = v.kind == RAW_POINTER || (source && (source->shape == 3 || source->shape == 6));
    int targetPointer = k == RAW_POINTER || (target && (target->shape == 3 || target->shape == 6));
    if (source && target && source->dynamicArray && target->dynamicArray && source->element == target->element)
        return collectionCopy(b, v, k);
    if (sourcePointer && targetPointer)
        return value(b, k, "%s", v.operand); // LLVM opaque pointers need no bitcast.
    Value r = temp(b, k);
    const char *op = NULL;
    if (sourcePointer && integral(b, k)) op = "ptrtoint";
    if (integral(b, v.kind) && targetPointer) op = "inttoptr";
    if (representationKind(b, k) == BOOL && (integral(b, v.kind) || floating(b, v.kind))) {
        fprintf(b->body, "  %s = %s %s %s, %s\n", r.operand,
                floating(b, v.kind) ? "fcmp une" : "icmp ne", irType(b, v.kind), v.operand,
                floating(b, v.kind) ? "0.0" : "0");
        return r;
    }
    if (representationKind(b, v.kind) == BOOL && floating(b, k)) op = "uitofp";
    if (integral(b, v.kind) && floating(b, k))
        op = unsignedInteger(b, v.kind) ? "uitofp" : "sitofp";
    if (floating(b, v.kind) && integral(b, k))
        op = unsignedInteger(b, k) ? "fptoui" : "fptosi";
    if (representationKind(b, v.kind) == BOOL && integral(b, k))
        op = "zext";
    if (integral(b, v.kind) && integral(b, k)) {
        if (integerWidth(b, v.kind) == integerWidth(b, k)) return value(b, k, "%s", v.operand);
        op = integerWidth(b, v.kind) < integerWidth(b, k) ? (unsignedInteger(b, v.kind) ? "zext" : "sext") : "trunc";
    }
    if (floating(b, v.kind) && floating(b, k)) op = representationKind(b, k) == FLOAT ? "fpext" : "fptrunc";
    if (!op) {
        fail(b, "unsupported scalar conversion or type mismatch");
        return value(b, BAD, "0");
    }
    fprintf(b->body, "  %s = %s %s %s to %s\n", r.operand, op, irType(b, v.kind), v.operand,
            irType(b, k));
    return r;
}
static void jump(Backend *, unsigned);
static void trackDynamicArrays(Backend *, Value);
static Value allocateCollection(Backend *, Kind, Value);
static Value arrayLength(Backend *, Value);
static Value elementAddress(Backend *, Value, Value);
static Value arrayData(Backend *, Value);
static Value collectionCopy(Backend *, Value, Kind);
static Value coerce(Backend *b, Value v, Kind expected);
static Value boxArray(Backend *, Value);
static int ownsArrays(Backend *, Kind);
static Value copyValue(Backend *, Value, unsigned);
static void copyInto(Backend *, Value, Value, unsigned);
static void rawStore(Backend *b, Value address, Value v) {
    BufferKind *target = bufferKind(b, address.kind);
    if (v.kind != address.kind && v.kind != NULL_VALUE &&
        (definedKind(b, v.kind) || definedKind(b, address.kind))) { fail(b, "distinct type requires an explicit cast"); return; }
    if (v.kind == NULL_VALUE || (target && target->shape == 3) ||
        implicitVoidPointer(b, v.kind, address.kind))
        v = coerce(b, v, address.kind);
    if ((integral(b, v.kind) && integral(b, address.kind)) || (floating(b, v.kind) && floating(b, address.kind)))
        v = convert(b, v, address.kind);
    if (v.kind != address.kind) {
        fail(b, "storage type mismatch");
        return;
    }
    fprintf(b->body, "  store %s %s, ptr %s\n", irType(b, v.kind), v.operand, address.operand);
}
static Value emptyValue(Backend *, Kind);
static Value load(Backend *, Value);
static void dropDynamicArrayPayload(Backend *, Value);
static void escapeDynamicArrays(Backend *, Value);
static void trackDynamicArrays(Backend *, Value);
static void dropNestedDynamicArrays(Backend *, Value, unsigned);
static void store(Backend *b, Value address, Value v) {
    BufferKind *targetArray = bufferKind(b, address.kind), *sourceArray = bufferKind(b, v.kind);
    if (targetArray && sourceArray && targetArray->shape == 4 && sourceArray->shape == 4 &&
        targetArray->element == SCALAR_ANY && address.kind != v.kind)
        v = coerce(b, v, address.kind);
    if (targetArray && targetArray->fixedArray && targetArray->shape == 4 && v.kind != address.kind)
        v = coerce(b, v, address.kind);
    if (address.kind == SCALAR_ANY) v = coerce(b, v, SCALAR_ANY);
    if (address.elementRequirement) checkElementRequirement(b, v, address.elementRequirement);
    if (address.homogeneousArray) {
        b->homogeneousUsed = 1;
        fprintf(b->body, "  call void @__ocean.array.check_write(ptr %s, { ptr, i64 } %s)\n",
                address.homogeneousArray, v.operand);
    }
    if (targetArray && targetArray->dynamicArray && v.kind == address.kind) {
        Value replacement = copyValue(b, v, 0);
        escapeDynamicArrays(b, replacement);
        dropDynamicArrayPayload(b, load(b, address));
        rawStore(b, address, replacement);
        return;
    }
    if (targetArray && targetArray->shape == 4 && !targetArray->dynamicArray && v.kind == address.kind) {
        // A string/reference value stored through *T may change size. Its new
        // backing must outlive this frame, including when T lives on the heap.
        int previous = b->copyTransport;
        if (address.throughPointer || address.owner == b->globalScope) b->copyTransport = 1;
        else if (address.owner)
            for (Scope *scope = b->scope; scope && scope != address.owner; scope = scope->parent)
                scope->retainAllocations = 1;
        Value replacement = copyValue(b, v, 0);
        b->copyTransport = previous;
        escapeDynamicArrays(b, replacement);
        dropNestedDynamicArrays(b, load(b, address), 0);
        rawStore(b, address, replacement);
        return;
    }
    if (containsDynamicArrays(b, address.kind) && v.kind == address.kind && targetArray &&
        (!targetArray->shape || targetArray->shape == 2 || targetArray->shape == 5)) {
        int previous = b->copyTransport;
        if (address.throughPointer || address.owner == b->globalScope) b->copyTransport = 1;
        Value replacement = copyValue(b, v, 0);
        b->copyTransport = previous;
        escapeDynamicArrays(b, replacement);
        dropNestedDynamicArrays(b, load(b, address), 0);
        rawStore(b, address, replacement);
        return;
    }
    if (ownsArrays(b, address.kind) && v.kind == address.kind) {
        if (address.throughPointer) {
            copyInto(b, address, v, 0);
            return;
        }
        // The storage of an outer value must survive inner block restoration.
        if (address.owner && address.owner != b->globalScope)
            for (Scope *scope = b->scope; scope && scope != address.owner; scope = scope->parent)
                scope->retainAllocations = 1;
        if (address.owner == b->globalScope) {
            int previous = b->copyTransport;
            b->copyTransport = 1;
            v = copyValue(b, v, 0);
            b->copyTransport = 3;
            copyValue(b, load(b, address), 0);
            b->copyTransport = previous;
        } else v = copyValue(b, v, 0);
    }
    escapeDynamicArrays(b, v);
    rawStore(b, address, v);
}
static Value load(Backend *b, Value address) {
    Value v = temp(b, address.kind);
    v.owner = address.owner;
    v.throughPointer = address.throughPointer;
    v.length = address.length;
    fprintf(b->body, "  %s = load %s, ptr %s\n", v.operand, irType(b, v.kind), address.operand);
    return v;
}
static Value bufferLiteralSlot(Backend *, Kind);
// C scalar metadata is carried with each `any` value. Numeric storage is
// native endian; narrow values occupy the beginning of an aligned eight-byte slot.
static const char *ffiType(Backend *b, Kind kind) {
    kind = representationKind(b, kind);
    if (kind == VOID) return "@ffi_type_void";
    if (kind == UINT) return "@ffi_type_uint64";
    if (kind == U32) return "@ffi_type_uint32";
    if (kind == U16) return "@ffi_type_uint16";
    if (kind == U8) return "@ffi_type_uint8";
    if (kind == INT) return "@ffi_type_sint64";
    if (kind == BOOL) return "@ffi_type_uint8";
    if (kind == I16) return "@ffi_type_sint16";
    if (kind == F32) return "@ffi_type_float";
    if (kind == I32) return "@ffi_type_sint32";
    if (kind == I8) return "@ffi_type_sint8";
    if (kind == FLOAT) return "@ffi_type_double";
    BufferKind *t = bufferKind(b, kind);
    if (kind == RAW_POINTER || kind == NULL_VALUE || (t && (t->shape == 3 || t->shape == 6)))
        return "@ffi_type_pointer";
    fail(b, "C variadic expansion and any values require scalar values; use .data for arrays");
    return "@ffi_type_void";
}
// Descriptors retain Ocean type identity separately from the C ABI type.
static Value anyMetadata(Backend *b, Kind kind) {
    b->anyUsed = 1;
    for (AnyType *t = b->anyTypes; t; t = t->next)
        if (t->kind == kind) return value(b, RAW_POINTER, "@__ocean.any.type.%u", (unsigned)kind);
    AnyType *t = checked(sizeof(*t));
    *t = (AnyType){.kind = kind, .next = b->anyTypes};
    b->anyTypes = t;
    return value(b, RAW_POINTER, "@__ocean.any.type.%u", (unsigned)kind);
}
static Value boxRegion(Backend *b) {
    if (!b->boxRegion.operand) {
        b->boxRegion = slot(b, RAW_POINTER);
        fprintf(b->slots, "  store ptr null, ptr %s\n", b->boxRegion.operand);
    }
    return b->boxRegion;
}
static Value trackBox(Backend *b, Value box) {
    Value region = boxRegion(b);
    fprintf(b->body, "  call void @__ocean.any.track(ptr %s, { ptr, i64 } %s)\n",
            region.operand, box.operand);
    return box;
}
static Value boxScalar(Backend *b, Value v, int promote) {
    if (v.kind == SCALAR_ANY) return v;
    (void)promote;
    Kind identity = v.kind;
    // Preserve Ocean identity while keeping small scalars ready for C varargs.
    if (representationKind(b, v.kind) == BOOL || representationKind(b, v.kind) == I8 || representationKind(b, v.kind) == I16 || representationKind(b, v.kind) == U8 || representationKind(b, v.kind) == U16) v = convert(b, v, I32);
    if (representationKind(b, v.kind) == F32) v = convert(b, v, FLOAT);
    if (v.kind == NULL_VALUE) v = value(b, RAW_POINTER, "@__ocean.null");
    BufferKind *t = bufferKind(b, v.kind);
    Value metadata = anyMetadata(b, identity == NULL_VALUE ? RAW_POINTER : identity);
    if (t && (t->shape == 4 || t->shape == 5 || !t->shape || (t->definition && representationKind(b, t->kind) == SCALAR_ANY))) {
        Value address = slot(b, v.kind), result = temp(b, SCALAR_ANY);
        rawStore(b, address, v);
        fprintf(b->body, "  %s = call { ptr, i64 } @__ocean.any.box.%u(ptr %s)\n",
                result.operand, (unsigned)v.kind, address.operand);
        return trackBox(b, result);
    }
    ffiType(b, v.kind); // Reject types with no value representation.
    b->ffiUsed = 1;
    Value storage = slot(b, INT);
    rawStore(b, storage, value(b, INT, "0"));
    fprintf(b->body, "  store %s %s, ptr %s\n", irType(b, v.kind), v.operand, storage.operand);
    Value bits = load(b, storage), tag = temp(b, SCALAR_ANY), result = temp(b, SCALAR_ANY);
    fprintf(b->body, "  %s = insertvalue { ptr, i64 } poison, ptr %s, 0\n", tag.operand, metadata.operand);
    fprintf(b->body, "  %s = insertvalue { ptr, i64 } %s, i64 %s, 1\n", result.operand, tag.operand, bits.operand);
    return result;
}
// Fixed C parameters retain their declared ABI rather than vararg promotion.
static Value boxFixedCScalar(Backend *b, Value v) {
    v.kind = representationKind(b, v.kind);
    if (v.kind != F32 && v.kind != I16 && v.kind != I8 && v.kind != U8 && v.kind != U16 && v.kind != BOOL)
        return boxScalar(b, v, 0);
    b->ffiUsed = 1;
    b->fixedCScalars |= 1u << v.kind;
    Value storage = slot(b, INT);
    rawStore(b, storage, value(b, INT, "0"));
    fprintf(b->body, "  store %s %s, ptr %s\n", irType(b, v.kind), v.operand, storage.operand);
    Value bits = load(b, storage), tag = temp(b, SCALAR_ANY), result = temp(b, SCALAR_ANY);
    fprintf(b->body, "  %s = insertvalue { ptr, i64 } poison, ptr @__ocean.c.fixed.%u, 0\n", tag.operand, (unsigned)v.kind);
    fprintf(b->body, "  %s = insertvalue { ptr, i64 } %s, i64 %s, 1\n", result.operand, tag.operand, bits.operand);
    return result;
}
static Value unboxArrayView(Backend *, Value, Kind);
static Value unboxMatched(Backend *b, Value box, Kind kind) {
    anyMetadata(b, kind); // Ensure aggregate helpers exist even for an underlying-type match.
    Value bits = temp(b, INT);
    fprintf(b->body, "  %s = extractvalue { ptr, i64 } %s, 1\n", bits.operand, box.operand);
    Value storage = slot(b, INT);
    rawStore(b, storage, bits);
    BufferKind *t = bufferKind(b, kind);
    if (t && (t->shape == 4 || t->shape == 5 || !t->shape || (t->definition && representationKind(b, t->kind) == SCALAR_ANY))) {
        Value result = temp(b, kind);
        fprintf(b->body, "  %s = call %s @__ocean.any.unbox.%u(i64 %s)\n",
                result.operand, irType(b, kind), (unsigned)kind, bits.operand);
        return copyValue(b, result, 0);
    }
    Value result = load(b, value(b, representationKind(b, kind) == F32 ? FLOAT : kind, "%s", storage.operand));
    return representationKind(b, kind) == F32 ? convert(b, result, kind) : result;
}static Value unboxValue(Backend *b, Value box, Kind kind) {
    BufferKind *array = bufferKind(b, kind);
    if (array && !array->definition && array->fixedArray && array->shape == 4) {
        Kind pattern = constraintShape(b, 16, array->element, 0);
        Value matches = testConstraint(b, box, pattern, 0);
        unsigned good = ++b->label, bad = ++b->label;
        branch(b, matches, good, bad); label(b, bad);
        fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
        b->nullChecksUsed = 1; b->terminated = 1; label(b, good);
        return array->element == SCALAR_ANY ? unboxArrayView(b, box, kind) : unboxMatched(b, box, kind);
    }
    if (array && !array->definition && array->shape == 4 && array->element == SCALAR_ANY)
        return unboxArrayView(b, box, kind);
    Value metadata = anyMetadata(b, kind), tag = temp(b, RAW_POINTER), bits = temp(b, INT), matches = temp(b, BOOL);
    fprintf(b->body, "  %s = extractvalue { ptr, i64 } %s, 0\n", tag.operand, box.operand);
    fprintf(b->body, "  %s = extractvalue { ptr, i64 } %s, 1\n", bits.operand, box.operand);
    fprintf(b->body, "  %s = icmp eq ptr %s, %s\n", matches.operand, tag.operand, metadata.operand);
    unsigned good = ++b->label, bad = ++b->label;
    branch(b, matches, good, bad);
    label(b, bad);
    fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
    b->nullChecksUsed = 1;
    b->terminated = 1;
    label(b, good);
    return unboxMatched(b, box, kind);
}

static Value coerce(Backend *b, Value v, Kind expected) {
    if (expected == SCALAR_ANY) return boxScalar(b, v, 1);
    if (v.kind != expected && v.kind != NULL_VALUE && (definedKind(b, v.kind) || definedKind(b, expected))) {
        fail(b, "distinct type requires an explicit cast"); return v;
    }
    BufferKind *target = bufferKind(b, expected), *source = bufferKind(b, v.kind);
    if (target && source && target->fixedArray && target->shape == 4 && source->fixedArray) {
        if (target->element == SCALAR_ANY && source->element != SCALAR_ANY) {
            Value boxed = boxArray(b, v); boxed.kind = expected; return boxed;
        }
        if (target->element == source->element) {
            if (!source->shape) {
                // Include a provenance word before the public size/data header.
                Value base = temp(b, RAW_POINTER), pointer = temp(b, expected);
                fprintf(b->body, "  %s = alloca { i64, %s }, align 8\n", base.operand, source->ir);
                rawStore(b, base, value(b, RAW_POINTER, "null"));
                fprintf(b->body, "  %s = getelementptr { i64, %s }, ptr %s, i32 0, i32 1\n", pointer.operand, source->ir, base.operand);
                rawStore(b, value(b, v.kind, "%s", pointer.operand), v);
                return pointer;
            }
            v.kind = expected; return v;
        }
        return collectionCopy(b, v, expected);
    }
    if (target && source && target->shape == 4 && source->shape == 4 &&
        target->element == source->element && (target->dynamicArray != source->dynamicArray || target->immutableString != source->immutableString))
        return collectionCopy(b, v, expected);
    if (target && source && target->shape == 4 && source->shape == 4 && target->element == SCALAR_ANY) {
        if (source->element != SCALAR_ANY) v = boxArray(b, v);
        if (!target->count) {
            b->homogeneousUsed = 1;
            fprintf(b->body, "  call void @__ocean.array.check(ptr %s)\n", v.operand);
        }
        v.kind = expected;
        checkArrayRequirement(b, v, target->requirement);
        return v;
    }
    if (implicitVoidPointer(b, v.kind, expected))
        return value(b, expected, "%s", v.operand);
    if (v.kind == NULL_VALUE) {
        if (target && target->shape == 6)
            return value(b, expected, "@__ocean.null");
        fail(b, "null requires a pointer destination");
        return value(b, BAD, "0");
    }
    if (target && target->shape == 3 && source && !source->shape &&
        source->element == target->element) {
        Value storage = bufferLiteralSlot(b, v.kind);
        store(b, storage, v);
        Value stored = load(b, storage);
        Value data = source->fixedArray ? arrayData(b, stored) : value(b, expected, "%s", storage.operand);
        data.kind = expected; return data;
    }
    if ((integral(b, v.kind) && integral(b, expected)) || (floating(b, v.kind) && floating(b, expected)))
        v = convert(b, v, expected);
    if (v.kind != expected)
        fail(b, "value type mismatch");
    return v;
}
/* Raw buffer literals created by a package initializer need module-lifetime
 * backing storage. Their owned array members participate in global cleanup. */
static Value bufferLiteralSlot(Backend *b, Kind kind) {
    if (!b->initializingGlobals) return slot(b, kind);
    unsigned id = ++b->next;
    Value address = value(b, kind, "@\"__ocean.buffer.%u\"", id);
    address.owner = b->globalScope;
    fprintf(b->globalOutput, "%s = internal global %s zeroinitializer\n", address.operand, irType(b, kind));
    char *name = checked(48);
    snprintf(name, 48, "__ocean.buffer.%u", id);
    bind(b, name, address);
    // Keep the binding itself in global scope rather than the temporary init scope.
    Binding *binding = b->scope->bindings;
    b->scope->bindings = binding->next;
    binding->next = b->globalScope->bindings;
    b->globalScope->bindings = binding;
    // Store a Backend-owned name; clearScope does not own Binding.name.
    binding->name = value(b, RAW_POINTER, "%s", name).operand;
    free(name);
    int previous = b->copyTransport;
    b->copyTransport = 1;
    Value initial = copyValue(b, emptyValue(b, kind), 0);
    escapeDynamicArrays(b, initial);
    rawStore(b, address, initial);
    b->copyTransport = previous;
    return address;
}
static Value extract(Backend *b, Value aggregate, size_t index) {
    BufferKind *t = bufferKind(b, aggregate.kind);
    if (!t || (t->shape && t->shape != 2 && t->shape != 5) || index >= t->count) {
        fail(b, "invalid aggregate extraction");
        return value(b, BAD, "0");
    }
    Value result = temp(b, t->shape ? t->members[index] : t->element);
    fprintf(b->body, "  %s = extractvalue %s %s, %s%zu\n", result.operand, t->ir, aggregate.operand,
            t->fixedArray && !t->shape ? "1, " : "", index);
    return result;
}
static Value insert(Backend *b, Value aggregate, Value member, size_t index) {
    BufferKind *t = bufferKind(b, aggregate.kind);
    Kind expected = t->shape ? t->members[index] : t->element;
    member = coerce(b, member, expected);
    Value result = temp(b, aggregate.kind);
    fprintf(b->body, "  %s = insertvalue %s %s, %s %s, %s%zu\n", result.operand, t->ir,
            aggregate.operand, irType(b, member.kind), member.operand,
            t->fixedArray && !t->shape ? "1, " : "", index);
    return result;
}
static Value storageSize(Backend *b, Kind k) {
    BufferKind *array = bufferKind(b, k);
    if (array && array->shape == 4)
        k = INT; // Fixed inline byte-count header; payload extent is runtime metadata.
    if (k == VOID || k == BAD) {
        fail(b, "type has no storage size");
        return value(b, BAD, "0");
    }
    // LLVM resolves this constant using the target data layout, including struct
    // padding. Do not materialize GEP/ptrtoint instructions, even at -O0.
    return value(b, INT, "ptrtoint (ptr getelementptr (%s, ptr null, i64 1) to i64)",
                 irType(b, k));
}
static Value arrayData(Backend *b, Value array) {
    BufferKind *t = bufferKind(b, array.kind);
    if (!t->shape) {
        Value storage = slot(b, array.kind); rawStore(b, storage, array);
        if (t->fixedArray) {
            Value data = temp(b, pointerBufferKind(b, t->element));
            fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, i32 1, i64 0\n", data.operand, t->ir, storage.operand);
            return data;
        }
        return value(b, pointerBufferKind(b, t->element), "%s", storage.operand);
    }
    if (t->shape == 3) return array;
    Value pointer = temp(b, pointerBufferKind(b, t->element));
    if (t->dynamicArray) {
        Value field = temp(b, RAW_POINTER);
        fprintf(b->body, "  %s = getelementptr { i64, i64, ptr }, ptr %s, i32 0, i32 2\n", field.operand, array.operand);
        fprintf(b->body, "  %s = load ptr, ptr %s\n", pointer.operand, field.operand);
        return pointer;
    }
    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, i32 1\n", pointer.operand, t->layout,
            array.operand);
    return pointer;
}
static void initializeArray(Backend *, Value, Value);
static Value allocateArrayStorage(Backend *b, Kind element, Value count) {
    count = coerce(b, count, INT);
    Value valid = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp sge i64 %s, 0\n", valid.operand, count.operand);
    unsigned good = ++b->label, bad = ++b->label;
    branch(b, valid, good, bad); label(b, bad);
    fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
    b->nullChecksUsed = 1; b->terminated = 1; label(b, good);
    Value elementSize = storageSize(b, element), bytes = temp(b, INT);
    fprintf(b->body, "  %s = mul i64 %s, %s\n", bytes.operand, count.operand, elementSize.operand);
    Kind kind = arrayValueKind(b, element);
    Value dataOffset = arrayData(b, value(b, kind, "null")), headerSize = temp(b, INT);
    fprintf(b->body, "  %s = ptrtoint ptr %s to i64\n", headerSize.operand, dataOffset.operand);
    Value total = temp(b, INT), result = temp(b, kind);
    fprintf(b->body, "  %s = add i64 %s, %s\n", total.operand, headerSize.operand, bytes.operand);
    Value allocationSize = temp(b, INT), base = temp(b, RAW_POINTER);
    fprintf(b->body, "  %s = add i64 %s, 8\n", allocationSize.operand, total.operand);
    fprintf(b->body, "  %s = alloca i8, i64 %s, align 8\n", base.operand, allocationSize.operand);
    rawStore(b, base, value(b, RAW_POINTER, "null"));
    fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 8\n", result.operand, base.operand);
    store(b, value(b, INT, "%s", result.operand), bytes);
    result.owner = b->scope;
    return result;
}
static Value allocateArray(Backend *b, Kind element, Value count) {
    Value result = allocateArrayStorage(b, element, count);
    initializeArray(b, result, count);
    trackDynamicArrays(b, result);
    return result;
}
static Value allocateCollection(Backend *b, Kind kind, Value count) {
    BufferKind *t = bufferKind(b, kind);
    if (!t->dynamicArray) {
        Value result = allocateArray(b, t->element, count); result.kind = kind; return result;
    }
    count = coerce(b, count, INT);
    Value bytes = temp(b, INT), stride = storageSize(b, t->element);
    fprintf(b->body, "  %s = mul i64 %s, %s\n", bytes.operand, count.operand, stride.operand);
    Value result = temp(b, kind), data = temp(b, RAW_POINTER);
    b->returnTransportUsed = 1;
    Value base = temp(b, RAW_POINTER);
    fprintf(b->body, "  %s = call ptr @\"\\01malloc\"(i64 32)\n", base.operand);
    rawStore(b, base, value(b, RAW_POINTER, "inttoptr (i64 1 to ptr)"));
    fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 8\n", result.operand, base.operand);
    fprintf(b->body, "  %s = call ptr @\"\\01malloc\"(i64 %s)\n", data.operand, bytes.operand);
    rawStore(b, value(b, INT, "%s", result.operand), count);
    Value cap = temp(b, INT), field = temp(b, RAW_POINTER);
    fprintf(b->body, "  %s = getelementptr { i64, i64, ptr }, ptr %s, i32 0, i32 1\n", cap.operand, result.operand);
    rawStore(b, cap, count);
    fprintf(b->body, "  %s = getelementptr { i64, i64, ptr }, ptr %s, i32 0, i32 2\n", field.operand, result.operand);
    rawStore(b, field, data);
    result.owner = b->scope;
    initializeArray(b, result, count);
    trackDynamicArrays(b, result);
    return result;
}
static Value collectionCopy(Backend *b, Value source, Kind target) {
    Value count = arrayLength(b, source), result = allocateCollection(b, target, count);
    Value cursor = slot(b, INT); rawStore(b, cursor, value(b, INT, "0"));
    unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
    jump(b, condition); label(b, condition);
    Value i = load(b, cursor), more = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
    branch(b, more, body, end); label(b, body);
    store(b, elementAddress(b, result, i), load(b, elementAddress(b, source, i)));
    Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
    rawStore(b, cursor, next); jump(b, condition); label(b, end);
    return result;
}
// Select the result array before evaluating operands, including nested arrays.
static Kind arrayAdditionKind(Backend *b, Kind left, Kind right) {
    BufferKind *a = bufferKind(b, left), *c = bufferKind(b, right);
    if (a && c && !a->shape && !c->shape && a->fixedArray && c->fixedArray && a->element == c->element)
        return fixedArrayKind(b, a->element, a->count + c->count);
    if (a && c && a->fixedArray && c->fixedArray && a->element == c->element)
        return runtimeArrayKind(b, a->element);
    int la = a && a->shape == 4, ra = c && c->shape == 4;
    if (la && ra && (left == right || a->element == c->element || a->element == SCALAR_ANY || c->element == SCALAR_ANY))
        return a->count ? left : c->count ? right : c->element == SCALAR_ANY ? right : left;
    if (la && (a->element == right || a->element == SCALAR_ANY ||
               (integral(b, a->element) && integral(b, right)) ||
               (right == NULL_VALUE && bufferKind(b, a->element) && bufferKind(b, a->element)->shape == 6) ||
               implicitVoidPointer(b, right, a->element))) return left;
    if (ra && (c->element == left || c->element == SCALAR_ANY ||
               (integral(b, c->element) && integral(b, left)) ||
               (left == NULL_VALUE && bufferKind(b, c->element) && bufferKind(b, c->element)->shape == 6) ||
               implicitVoidPointer(b, left, c->element))) return right;
    return BAD;
}
/* sizeof inspects storage types without evaluating its operand. */
static Kind expressionKind(Backend *, expr *);
#include "backend_enum.inc"

static Kind expressionKind(Backend *b, expr *e) {
    if (!e || !e->underlying)
        return BAD;
    switch (e->type) {
    case EXPR_TYPE_ENUM: return enumExpressionKind(b, e->underlying);
    case EXPR_TYPE_MAKE:
    case EXPR_TYPE_ZERO:
        return type(b, ((struct expr_zero *)e->underlying)->type);
    case EXPR_TYPE_NEW:
        return pointerKind(b, type(b, ((struct expr_zero *)e->underlying)->type));
    case EXPR_TYPE_IDENTIFIER: {
        char *name = ((struct expr_identifier *)e->underlying)->lexeme.lexeme;
        if (!strcmp(name, "true") || !strcmp(name, "false"))
            return BOOL;
        return find(b, name).kind;
    }
    case EXPR_TYPE_LITERAL: {
        enum literalType t = ((struct expr_literal *)e->underlying)->type;
        if (t == LITERAL_TYPE_NULL) return NULL_VALUE;
        if (t == LITERAL_TYPE_INT && ((struct expr_literal *)e->underlying)->unsignedLiteral) return UINT;
        return t == LITERAL_TYPE_STRING ? stringKind(b) : t == LITERAL_TYPE_FLOAT ? FLOAT : INT;
    }
    case EXPR_TYPE_COMPOSITE: {
        struct expr_composite *v = e->underlying;
        if (v->literalType.nTokens)
            return type(b, v->literalType);
        return v->inferredArray && v->nEntries
                   ? fixedArrayKind(b, expressionKind(b, &v->values[0]), v->nEntries)
                   : BAD;
    }
    case EXPR_TYPE_ARRAY_NEW: {
        struct expr_array_new *allocation = e->underlying;
        typeTokens element = allocation->elementType;
        Kind k = type(b, element);
        if (allocation->buffer) {
            if (allocation->count.type == EXPR_TYPE_LITERAL &&
                ((struct expr_literal *)allocation->count.underlying)->type == LITERAL_TYPE_INT)
                return arrayKind(b, k, *(long *)((struct expr_literal *)allocation->count.underlying)->value);
            return pointerBufferKind(b, k);
        }
        return k == SCALAR_ANY ? constrainedArrayKind(b, constraintKind(b, element), 0) : dynamicArrayKind(b, k);
    }
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
        if (t && t->dynamicArray && (!strcmp(((struct expr_identifier *)v->field.underlying)->lexeme.lexeme, "length") ||
                              !strcmp(((struct expr_identifier *)v->field.underlying)->lexeme.lexeme, "capacity"))) return INT;
        if (t && (t->shape == 4 || t->fixedArray))
            return !strcmp(((struct expr_identifier *)v->field.underlying)->lexeme.lexeme, "size")
                       ? INT
                       : pointerBufferKind(b, t->element);
        return BAD;
    }
    case EXPR_TYPE_ASSERTION:
        return type(b, ((struct expr_assertion *)e->underlying)->type);
    case EXPR_TYPE_TYPECAST:
        return type(b, ((struct expr_typecast *)e->underlying)->type);
    case EXPR_TYPE_CALL: {
        struct expr_call *v = e->underlying;
        typeTokens *typeArguments;
        size_t count;
        struct expr_dot *dot = methodCallee(&v->func, &typeArguments, &count);
        if (dot) {
            BufferKind *parent = bufferKind(b, expressionKind(b, &dot->parent));
            const char *name = ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme;
            BufferKind *arrayReceiver = parent;
            while (arrayReceiver && arrayReceiver->shape == 6) arrayReceiver = bufferKind(b, arrayReceiver->element);
            if (arrayReceiver && arrayReceiver->shape == 4 && !arrayReceiver->fixedArray && !strcmp(name, "array"))
                return runtimeArrayKind(b, arrayReceiver->element);
            if (parent && parent->dynamicArray && (!strcmp(name, "remove") || !strcmp(name, "pop"))) return parent->element;
            if (parent && parent->dynamicArray && (!strcmp(name, "append") || !strcmp(name, "prepend") || !strcmp(name, "add"))) return VOID;
            return methodResult(b, methodInstance(b, expressionKind(b, &dot->parent),
                ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme, typeArguments, count));
        }
        if (v->func.type == EXPR_TYPE_IDENTIFIER) {
            const char *name = ((struct expr_identifier *)v->func.underlying)->lexeme.lexeme;
            if (!strcmp(name, "compilerReflectFields") && b->function && b->function->compilerIntrinsic == INTRINSIC_REFLECT_FIELDS)
                return resultType(b, b->function);
            if (!strcmp(name, "compilerReflectIsStruct") && b->function && b->function->compilerIntrinsic == INTRINSIC_REFLECT_IS_STRUCT)
                return BOOL;
        }
        struct stmt_func *f = directCallee(b, &v->func, &typeArguments, &count);
        if (f && f->nTypeParams)
            return methodResult(b, functionInstance(b, f, typeArguments, count));
        if (f) return resultType(b, f);
        return v->func.type == EXPR_TYPE_IDENTIFIER &&
                       !((struct expr_identifier *)v->func.underlying)->externReference &&
                       !strcmp(((struct expr_identifier *)v->func.underlying)->lexeme.lexeme, "len")
                   ? INT : BAD;
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
        if (v->operation == EXPR_ADD) {
            Kind array = arrayAdditionKind(b, a, c);
            if (array != BAD) return array;
        }
        if (v->operation == EXPR_ASSIGNMENT || v->operation >= EXPR_ADD_EQUALS)
            return a;
        return a == FLOAT || c == FLOAT ? FLOAT : a == F32 || c == F32 ? F32
               : integral(b, a) && integral(b, c) && a != c ? INT : a;
    }
    case EXPR_TYPE_TERNARY: {
        struct expr_ternary *v = e->underlying;
        Kind first = expressionKind(b, &v->ifTrue);
        Kind second = expressionKind(b, &v->ifFalse);
        return first == NULL_VALUE || implicitVoidPointer(b, first, second) ? second : first;
    }
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
static void requirePointer(Backend *b, Value pointer) {
    b->nullChecksUsed = 1;
    Value present = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp ne ptr %s, @__ocean.null\n", present.operand, pointer.operand);
    unsigned good = ++b->label, bad = ++b->label;
    branch(b, present, good, bad);
    label(b, bad);
    fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
    b->terminated = 1;
    label(b, good);
}
static Value expression(Backend *, expr *);
static Value address(Backend *b, expr *e);
static Value emptyValue(Backend *, Kind);
static Value expectedValue(Backend *, expr *, Kind);
static Value conditionalValue(Backend *, expr *, Kind);
static Value arrayLength(Backend *, Value);
// Arrays are indirect only in the LLVM ABI. Every owning boundary copies the
// complete runtime-sized value; typed pointers and raw buffers remain aliases.
static int ownsArrays(Backend *b, Kind kind) {
    BufferKind *t = bufferKind(b, kind);
    if (representationKind(b, kind) == SCALAR_ANY) return 1;
    if (!t)
        return 0;
    if (t->shape == 4)
        return 1;
    if (!t->shape)
        return ownsArrays(b, t->element);
    if (t->shape == 2 || t->shape == 5)
        for (size_t i = 0; i < t->count; i++)
            if (ownsArrays(b, t->members[i]))
                return 1;
    return 0;
}
static DynamicArrayTemporary *dynamicArrayTempMark(Backend *b) { return b->dynamicArrayTemps; }
static void cleanDynamicArrayTemps(Backend *b, DynamicArrayTemporary *mark) {
    for (DynamicArrayTemporary *entry = b->dynamicArrayTemps; entry != mark; entry = entry->next) {
        Value saved = load(b, entry->slot), present = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp ne ptr %s, null\n", present.operand, saved.operand);
        unsigned drop = ++b->label, end = ++b->label;
        branch(b, present, drop, end); label(b, drop);
        rawStore(b, entry->slot, value(b, RAW_POINTER, "null"));
        saved.kind = entry->kind;
        dropNestedDynamicArrays(b, saved, 0);
        jump(b, end); label(b, end);
    }
}
// The prefix holds a temporary stack-slot address and a heap-provenance bit.
// Moving a value clears that slot directly; no runtime registry is needed.
static void detachDynamicArrayTemporary(Backend *b, Value item) {
    Value zero = emptyValue(b, item.kind), allocated = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp ne ptr %s, %s\n", allocated.operand, item.operand, zero.operand);
    unsigned inspect = ++b->label, clear = ++b->label, end = ++b->label;
    branch(b, allocated, inspect, end); label(b, inspect);
    Value prefix = temp(b, RAW_POINTER), owner = temp(b, RAW_POINTER), tracked = temp(b, BOOL);
    fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 -8\n", prefix.operand, item.operand);
    fprintf(b->body, "  %s = load ptr, ptr %s\n", owner.operand, prefix.operand);
    Value bits = temp(b, INT), addressBits = temp(b, INT), ownerSlot = temp(b, RAW_POINTER);
    Value heapBit = temp(b, INT), provenance = temp(b, RAW_POINTER);
    fprintf(b->body, "  %s = ptrtoint ptr %s to i64\n", bits.operand, owner.operand);
    fprintf(b->body, "  %s = and i64 %s, -2\n", addressBits.operand, bits.operand);
    fprintf(b->body, "  %s = inttoptr i64 %s to ptr\n", ownerSlot.operand, addressBits.operand);
    fprintf(b->body, "  %s = icmp ne i64 %s, 0\n", tracked.operand, addressBits.operand);
    branch(b, tracked, clear, end); label(b, clear);
    rawStore(b, value(b, RAW_POINTER, "%s", ownerSlot.operand), value(b, RAW_POINTER, "null"));
    fprintf(b->body, "  %s = and i64 %s, 1\n", heapBit.operand, bits.operand);
    fprintf(b->body, "  %s = inttoptr i64 %s to ptr\n", provenance.operand, heapBit.operand);
    rawStore(b, prefix, provenance);
    jump(b, end); label(b, end);
}
static void escapeDynamicArrays(Backend *b, Value item) {
    BufferKind *t = bufferKind(b, item.kind);
    if (!t) return;
    if (t->dynamicArray) {
        detachDynamicArrayTemporary(b, item);
        return;
    }
    if (t->shape == 4 && containsDynamicArrays(b, t->element)) {
        detachDynamicArrayTemporary(b, item);
        Value count = arrayLength(b, item), cursor = slot(b, INT);
        rawStore(b, cursor, value(b, INT, "0"));
        unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
        jump(b, condition); label(b, condition);
        Value i = load(b, cursor), more = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
        branch(b, more, body, end); label(b, body);
        escapeDynamicArrays(b, load(b, elementAddress(b, item, i)));
        Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
        rawStore(b, cursor, next); jump(b, condition); label(b, end);
        return;
    }
    if ((!t->shape || t->shape == 2 || t->shape == 5) && ownsArrays(b, item.kind))
        for (size_t i = 0; i < t->count; i++) escapeDynamicArrays(b, extract(b, item, i));
}
static void trackDynamicArrays(Backend *b, Value item) {
    BufferKind *t = bufferKind(b, item.kind);
    if (!t) return;
    if (t->dynamicArray || (t->shape == 4 && containsDynamicArrays(b, t->element))) {
        detachDynamicArrayTemporary(b, item);
        DynamicArrayTemporary *entry = checked(sizeof(*entry));
        entry->kind = item.kind; entry->slot = slot(b, RAW_POINTER);
        fprintf(b->slots, "  store ptr null, ptr %s\n", entry->slot.operand);
        entry->next = b->dynamicArrayTemps; b->dynamicArrayTemps = entry;
        Value zero = emptyValue(b, item.kind), allocated = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp ne ptr %s, %s\n", allocated.operand, item.operand, zero.operand);
        unsigned attach = ++b->label, end = ++b->label;
        branch(b, allocated, attach, end); label(b, attach);
        Value prefix = temp(b, RAW_POINTER);
        fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 -8\n", prefix.operand, item.operand);
        rawStore(b, entry->slot, value(b, RAW_POINTER, "%s", item.operand));
        Value old = load(b, prefix), heapBits = temp(b, INT), slotBits = temp(b, INT);
        Value taggedBits = temp(b, INT), tagged = temp(b, RAW_POINTER);
        fprintf(b->body, "  %s = ptrtoint ptr %s to i64\n", heapBits.operand, old.operand);
        fprintf(b->body, "  %s = ptrtoint ptr %s to i64\n", slotBits.operand, entry->slot.operand);
        fprintf(b->body, "  %s = or i64 %s, %s\n", taggedBits.operand, slotBits.operand, heapBits.operand);
        fprintf(b->body, "  %s = inttoptr i64 %s to ptr\n", tagged.operand, taggedBits.operand);
        rawStore(b, prefix, tagged);
        jump(b, end); label(b, end);
        return;
    }
    if ((!t->shape || t->shape == 2 || t->shape == 5) && ownsArrays(b, item.kind))
        for (size_t i = 0; i < t->count; i++) trackDynamicArrays(b, extract(b, item, i));
}
static void releaseScopeValues(Backend *b) {
    for (Scope *scope = b->scope; scope && scope != b->globalScope; scope = scope->parent)
        for (Binding *binding = scope->bindings; binding; binding = binding->next)
            if (binding->autoRelease) {
                dropNestedDynamicArrays(b, load(b, binding->address), 0);
                // Inlined calls can reuse this entry-block slot on the next
                // loop iteration. Never leave a freed header in that slot.
                rawStore(b, binding->address, emptyValue(b, binding->address.kind));
            }
}
static void dropNestedDynamicArrays(Backend *b, Value item, unsigned depth) {
    if (depth >= 32) { fail(b, "nested dynamic array ownership is too deep"); return; }
    BufferKind *t = bufferKind(b, item.kind);
    if (!t) return;
    if (t->dynamicArray) { dropDynamicArrayPayload(b, item); return; }
    if (t->shape == 4) {
        Value zero = emptyValue(b, item.kind), allocated = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp ne ptr %s, %s\n", allocated.operand, item.operand, zero.operand);
        unsigned inspect = ++b->label, heap = ++b->label, stack = ++b->label, end = ++b->label;
        branch(b, allocated, inspect, end); label(b, inspect);
        Value prefix = temp(b, RAW_POINTER), tag = temp(b, RAW_POINTER), bits = temp(b, INT), flag = temp(b, INT), onHeap = temp(b, BOOL);
        fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 -8\n", prefix.operand, item.operand);
        fprintf(b->body, "  %s = load ptr, ptr %s\n", tag.operand, prefix.operand);
        fprintf(b->body, "  %s = ptrtoint ptr %s to i64\n", bits.operand, tag.operand);
        fprintf(b->body, "  %s = and i64 %s, 1\n", flag.operand, bits.operand);
        fprintf(b->body, "  %s = icmp ne i64 %s, 0\n", onHeap.operand, flag.operand);
        branch(b, onHeap, heap, stack); label(b, heap);
        int previous = b->copyTransport; b->copyTransport = 3;
        copyValue(b, item, depth + 1); b->copyTransport = previous;
        jump(b, end); label(b, stack);
        if (ownsArrays(b, t->element)) {
            Value count = arrayLength(b, item), cursor = slot(b, INT);
            rawStore(b, cursor, value(b, INT, "0"));
            unsigned condition = ++b->label, body = ++b->label, done = ++b->label;
            jump(b, condition); label(b, condition);
            Value i = load(b, cursor), more = temp(b, BOOL);
            fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
            branch(b, more, body, done); label(b, body);
            dropNestedDynamicArrays(b, load(b, elementAddress(b, item, i)), depth + 1);
            Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
            rawStore(b, cursor, next); jump(b, condition); label(b, done);
        }
        jump(b, end); label(b, end);
        return;
    }
    if (!t->shape || t->shape == 2 || t->shape == 5)
        for (size_t i = 0; i < t->count; i++) dropNestedDynamicArrays(b, extract(b, item, i), depth + 1);
}
static void dropDynamicArrayPayload(Backend *b, Value dynamicArray) {
    b->returnTransportUsed = 1;
    Value zero = emptyValue(b, dynamicArray.kind), allocated = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp ne ptr %s, %s\n", allocated.operand, dynamicArray.operand, zero.operand);
    unsigned drop = ++b->label, end = ++b->label;
    branch(b, allocated, drop, end); label(b, drop);
    escapeDynamicArrays(b, dynamicArray);
    BufferKind *type = bufferKind(b, dynamicArray.kind);
    if (ownsArrays(b, type->element)) {
        Value count = arrayLength(b, dynamicArray), cursor = slot(b, INT);
        rawStore(b, cursor, value(b, INT, "0"));
        unsigned condition = ++b->label, body = ++b->label, done = ++b->label;
        jump(b, condition); label(b, condition);
        Value i = load(b, cursor), more = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
        branch(b, more, body, done); label(b, body);
        dropNestedDynamicArrays(b, load(b, elementAddress(b, dynamicArray, i)), 0);
        Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
        rawStore(b, cursor, next); jump(b, condition); label(b, done);
    }
    Value data = arrayData(b, dynamicArray);
    fprintf(b->body, "  call void @\"\\01free\"(ptr %s)\n", data.operand);
    Value base = temp(b, RAW_POINTER);
    fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 -8\n", base.operand, dynamicArray.operand);
    fprintf(b->body, "  call void @\"\\01free\"(ptr %s)\n", base.operand);
    jump(b, end); label(b, end);
}
static Value arrayBytes(Backend *b, Value array) {
    BufferKind *t = bufferKind(b, array.kind);
    if (!t->shape && t->fixedArray) {
        Value bytes = temp(b, INT);
        fprintf(b->body, "  %s = extractvalue %s %s, 0\n", bytes.operand, t->ir, array.operand);
        return bytes;
    }
    if (t->dynamicArray) {
        Value count = load(b, value(b, INT, "%s", array.operand)), stride = storageSize(b, t->element), bytes = temp(b, INT);
        fprintf(b->body, "  %s = mul i64 %s, %s\n", bytes.operand, count.operand, stride.operand);
        return bytes;
    }
    return load(b, value(b, INT, "%s", array.operand));
}
static Value arrayHeaderSize(Backend *b, Kind kind) {
    BufferKind *t = bufferKind(b, kind);
    return value(b, INT, "ptrtoint (ptr getelementptr (%s, ptr null, i32 0, i32 1) to i64)",
                 t->layout);
}
static Value elementAddress(Backend *b, Value array, Value index) {
    BufferKind *t = bufferKind(b, array.kind);
    Value data = arrayData(b, array), result = temp(b, t->element);
    result.owner = array.owner;
    result.throughPointer = array.throughPointer;
    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", result.operand,
            irType(b, t->element), data.operand, index.operand);
    return result;
}
static Value copyValue(Backend *b, Value source, unsigned depth) {
    if (!ownsArrays(b, source.kind))
        return source;
    if (depth >= 32) {
        fail(b, "recursive array-value layouts are unsupported; use explicit pointers for "
                "recursive ownership");
        return value(b, BAD, "0");
    }
    if (representationKind(b, source.kind) == SCALAR_ANY) {
        b->anyUsed = 1;
        if (b->copyTransport == 3) {
            fprintf(b->body, "  call void @__ocean.any.drop({ ptr, i64 } %s)\n", source.operand);
            return source;
        }
        Value result = temp(b, source.kind);
        fprintf(b->body, "  %s = call { ptr, i64 } @__ocean.any.copy({ ptr, i64 } %s)\n", result.operand, source.operand);
        if (b->copyTransport != 1) trackBox(b, result);
        if (b->copyTransport == 2)
            fprintf(b->body, "  call void @__ocean.any.drop({ ptr, i64 } %s)\n", source.operand);
        return result;
    }
    BufferKind *t = bufferKind(b, source.kind);
    if (t->dynamicArray) {
        int transport = b->copyTransport;
        Value result = source;
        if (transport != 3) {
            b->copyTransport = transport == 2 ? 0 : transport;
            result = collectionCopy(b, source, source.kind);
            b->copyTransport = transport;
        }
        if (transport == 2 || transport == 3) {
            dropDynamicArrayPayload(b, source);
        }
        return result;
    }
    if (t->shape == 4) {
        Value bytes = arrayBytes(b, source), header = arrayHeaderSize(b, source.kind);
        Value total = temp(b, INT), result = temp(b, source.kind);
        result.owner = b->scope;
        fprintf(b->body, "  %s = add i64 %s, %s\n", total.operand, bytes.operand, header.operand);
        Value base = temp(b, RAW_POINTER), allocationSize = temp(b, INT);
        fprintf(b->body, "  %s = add i64 %s, 8\n", allocationSize.operand, total.operand);
        if (b->copyTransport == 3) result = source;
        else if (b->copyTransport == 1) {
            b->returnTransportUsed = 1;
            fprintf(b->body, "  %s = call ptr @\"\\01malloc\"(i64 %s)\n", base.operand, allocationSize.operand);
            Value present = temp(b, BOOL);
            fprintf(b->body, "  %s = icmp ne ptr %s, null\n", present.operand, base.operand);
            unsigned good = ++b->label, bad = ++b->label;
            branch(b, present, good, bad);
            label(b, bad);
            fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
            b->nullChecksUsed = 1;
            b->terminated = 1;
            label(b, good);
        } else
            fprintf(b->body, "  %s = alloca i8, i64 %s, align 8\n", base.operand, allocationSize.operand);
        if (b->copyTransport != 3) {
            rawStore(b, base, value(b, RAW_POINTER, b->copyTransport == 1 ? "inttoptr (i64 1 to ptr)" : "null"));
            fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 8\n", result.operand, base.operand);
        }
        if (b->copyTransport != 3)
            fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n",
                    result.operand, source.operand, total.operand);
        b->dataCopyUsed = 1;
        if (ownsArrays(b, t->element)) {
            Value count = arrayLength(b, source), cursor = slot(b, INT);
            rawStore(b, cursor, value(b, INT, "0"));
            unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
            jump(b, condition);
            label(b, condition);
            Value i = load(b, cursor), more = temp(b, BOOL);
            fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand,
                    count.operand);
            branch(b, more, body, end);
            label(b, body);
            Value item = load(b, elementAddress(b, source, i));
            Value copied = copyValue(b, item, depth + 1);
            if (b->copyTransport != 3) {
                escapeDynamicArrays(b, copied);
                rawStore(b, elementAddress(b, result, i), copied);
            }
            Value next = temp(b, INT);
            fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
            rawStore(b, cursor, next);
            jump(b, condition);
            label(b, end);
        }
        if (b->copyTransport == 2 || b->copyTransport == 3) {
            b->returnTransportUsed = 1;
            // The element loop already released transferred child values.
            // Detach only this header, without revisiting freed descendants.
            detachDynamicArrayTemporary(b, source);
            Value base = temp(b, RAW_POINTER);
            fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 -8\n", base.operand, source.operand);
            fprintf(b->body, "  call void @\"\\01free\"(ptr %s)\n", base.operand);
        }
        if (b->copyTransport != 3) trackDynamicArrays(b, result);
        return result;
    }
    Value result = source;
    for (size_t i = 0; i < t->count; i++) {
        Kind member = t->shape ? t->members[i] : t->element;
        if (ownsArrays(b, member))
            result = insert(b, result, copyValue(b, extract(b, source, i), depth + 1), i);
    }
    return result;
}
static void initializeArray(Backend *b, Value array, Value count) {
    BufferKind *t = bufferKind(b, array.kind);
    count = coerce(b, count, INT);
    Value cursor = slot(b, INT);
    rawStore(b, cursor, value(b, INT, "0"));
    unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
    jump(b, condition);
    label(b, condition);
    Value i = load(b, cursor), more = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
    branch(b, more, body, end);
    label(b, body);
    rawStore(b, elementAddress(b, array, i), emptyValue(b, t->element));
    Value next = temp(b, INT);
    fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
    rawStore(b, cursor, next);
    jump(b, condition);
    label(b, end);
}
// Replacing a value through an explicit pointer must stay inside its owner's
// existing stack allocation. Never store a pointer to new callee-stack data.
static void copyInto(Backend *b, Value destination, Value source, unsigned depth) {
    if (depth >= 32) {
        fail(b, "recursive array replacement is unsupported");
        return;
    }
    if (!ownsArrays(b, destination.kind)) {
        rawStore(b, destination, source);
        return;
    }
    if (representationKind(b, destination.kind) == SCALAR_ANY) {
        rawStore(b, destination, copyValue(b, source, 0));
        return;
    }
    BufferKind *t = bufferKind(b, destination.kind);
    if (t->dynamicArray) { store(b, destination, source); return; }
    if (t->shape == 4) {
        Value target = load(b, destination), oldBytes = arrayBytes(b, target),
              bytes = arrayBytes(b, source);
        Value fits = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp ule i64 %s, %s\n", fits.operand, bytes.operand,
                oldBytes.operand);
        unsigned good = ++b->label, bad = ++b->label;
        branch(b, fits, good, bad);
        label(b, bad);
        b->nullChecksUsed = 1;
        fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
        b->terminated = 1;
        label(b, good);
        if (ownsArrays(b, t->element)) {
            Value count = arrayLength(b, source), cursor = slot(b, INT);
            rawStore(b, cursor, value(b, INT, "0"));
            unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
            jump(b, condition);
            label(b, condition);
            Value i = load(b, cursor), more = temp(b, BOOL);
            fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand,
                    count.operand);
            branch(b, more, body, end);
            label(b, body);
            copyInto(b, elementAddress(b, target, i), load(b, elementAddress(b, source, i)),
                     depth + 1);
            Value next = temp(b, INT);
            fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
            rawStore(b, cursor, next);
            jump(b, condition);
            label(b, end);
            rawStore(b, value(b, INT, "%s", target.operand), bytes);
        } else {
            Value total = temp(b, INT), header = arrayHeaderSize(b, source.kind);
            fprintf(b->body, "  %s = add i64 %s, %s\n", total.operand, bytes.operand,
                    header.operand);
            fprintf(b->body,
                    "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n",
                    target.operand, source.operand, total.operand);
            b->dataCopyUsed = 1;
        }
        return;
    }
    for (size_t i = 0; i < t->count; i++) {
        Kind member = t->shape ? t->members[i] : t->element;
        Value address = temp(b, member);
        fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, %s%s %zu\n", address.operand, t->ir,
                destination.operand, t->fixedArray && !t->shape ? "i32 1, " : "", t->shape ? "i32" : "i64", i);
        copyInto(b, address, extract(b, source, i), depth + 1);
    }
}
static void copyArrayLiteral(Backend *, Value, struct expr_composite *, token);
static Value parameterArrayValue(Backend *b, typeTokens span, Kind kind, expr *initializer) {
    typeTokens element, dimension;
    if (!sizedArrayTypeParts(span, &element, &dimension))
        return initializer ? expectedValue(b, initializer, kind) : emptyValue(b, kind);
    BufferKind *t = bufferKind(b, kind);
    struct expr_composite *literal = initializer && initializer->type == EXPR_TYPE_COMPOSITE ? initializer->underlying : NULL;
    if (initializer && !literal) return expectedValue(b, initializer, kind);
    if (!t->shape) {
        Value result = emptyValue(b, kind);
        for (size_t i = 0; i < t->count; i++) {
            expr *item = literal && i < literal->nEntries ? &literal->values[i] : NULL;
            result = insert(b, result, parameterArrayValue(b, element, t->element, item), i);
        }
        return result;
    }
    expr *size = parseBufferDimension(dimension);
    if (!size) { fail(b, "invalid parameterized array length"); return value(b, BAD, "0"); }
    Value count = coerce(b, expression(b, size), INT); free(size);
    Value result = allocateCollection(b, kind, count);
    if (literal) {
        Value fits = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp uge i64 %s, %zu\n", fits.operand, count.operand, literal->nEntries);
        unsigned good = ++b->label, bad = ++b->label;
        branch(b, fits, good, bad); label(b, bad);
        fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
        b->nullChecksUsed = 1; b->terminated = 1; label(b, good);
    }
    typeTokens nestedElement, nestedDimension;
    if (sizedArrayTypeParts(element, &nestedElement, &nestedDimension)) {
        Value cursor = slot(b, INT); rawStore(b, cursor, value(b, INT, "0"));
        unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
        jump(b, condition); label(b, condition);
        Value i = load(b, cursor), more = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
        branch(b, more, body, end); label(b, body);
        Value child = parameterArrayValue(b, element, t->element, NULL);
        escapeDynamicArrays(b, child);
        rawStore(b, elementAddress(b, result, i), child);
        Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
        rawStore(b, cursor, next); jump(b, condition); label(b, end);
    }
    for (size_t i = 0; literal && i < literal->nEntries; i++) {
        Value child = parameterArrayValue(b, element, t->element, &literal->values[i]);
        escapeDynamicArrays(b, child);
        Value address = elementAddress(b, result, value(b, INT, "%zu", i));
        dropNestedDynamicArrays(b, load(b, address), 0);
        rawStore(b, address, child);
    }
    return result;
}
static Value constructComposite(Backend *b, expr *e, Kind kind) {
    struct expr_composite *v = e->underlying;
    BufferKind *t = bufferKind(b, kind);
    if (!t) {
        fail(b, "literal needs a supported array, buffer, or struct type");
        return value(b, BAD, "0");
    }
    if (t->shape == 4) {
        Value array = allocateCollection(b, kind, value(b, INT, "%zu", v->nEntries));
        array.kind = kind;
        copyArrayLiteral(b, array, v, v->location);
        return array;
    }
    if (t->shape == 3) {
        Kind backing = arrayKind(b, t->element, v->nEntries);
        Value aggregate = constructComposite(b, e, backing), storage = bufferLiteralSlot(b, backing);
        store(b, storage, aggregate);
        return value(b, kind, "%s", storage.operand);
    }
    if (t->shape == 5) {
        Value aggregate = emptyValue(b, kind);
        if (t->record->nVarParams) {
            Scope parameters = {.parent = b->scope}; b->scope = &parameters;
            expr **initializers = checked(sizeof(expr *) * t->count);
            for (size_t i = 0; i < v->nEntries; i++) {
                size_t field = v->keys[i].type ? recordField(t, ((struct expr_identifier *)v->keys[i].underlying)->lexeme.lexeme) : i;
                if (field < t->count) initializers[field] = &v->values[i];
            }
            for (size_t i = 0; i < t->count; i++) {
                Value item = parameterArrayValue(b, t->record->fieldTypes[i], t->members[i], initializers[i]);
                aggregate = insert(b, aggregate, item, i);
                if (i < t->record->nVarParams) {
                    Value binding = slot(b, item.kind); rawStore(b, binding, item);
                    bind(b, t->record->fieldNames[i].lexeme, binding);
                }
            }
            free(initializers); b->scope = parameters.parent; clearScope(&parameters);
            return aggregate;
        }
        for (size_t i = 0; i < v->nEntries; i++) {
            size_t field = v->keys[i].type
                ? recordField(t, ((struct expr_identifier *)v->keys[i].underlying)->lexeme.lexeme) : i;
            if (field >= t->count) {
                fail(b, "unknown struct field");
                return value(b, BAD, "0");
            }
            aggregate = insert(b, aggregate,
                               expectedValue(b, &v->values[i], t->members[field]), field);
        }
        return aggregate;
    }
    if (!t->shape) {
        if (v->nEntries > t->count) {
            fail(b, "too many elements in buffer initializer");
            return value(b, BAD, "0");
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
    return value(b, BAD, "0");
}
static Value expectedValue(Backend *b, expr *e, Kind expected) {
    if (e->type == EXPR_TYPE_TERNARY) return conditionalValue(b, e, expected);
    if (e->type == EXPR_TYPE_COMPOSITE) {
        struct expr_composite *literal = e->underlying;
        Kind actual = literal->literalType.nTokens ? type(b, literal->literalType)
                      : literal->inferredArray && literal->nEntries
                          ? fixedArrayKind(b, expressionKind(b, &literal->values[0]), literal->nEntries)
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
            if (source->fixedArray) {
                Value data = temp(b, expected);
                fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, i32 1, i64 0\n", data.operand, source->ir, storage.operand);
                return data;
            }
            return value(b, expected, "%s", storage.operand);
        }
        return coerce(b, expression(b, e), expected);
    }
    Value result = expression(b, e);
    BufferKind *targetArray = bufferKind(b, expected), *sourceArray = bufferKind(b, result.kind);
    if (targetArray && sourceArray && targetArray->fixedArray && targetArray->shape == 4 && sourceArray->fixedArray && expected != result.kind)
        return coerce(b, result, expected);
    if (targetArray && sourceArray && targetArray->shape == 4 && sourceArray->shape == 4 &&
        (targetArray->element == SCALAR_ANY || targetArray->element == sourceArray->element) && expected != result.kind)
        result = coerce(b, result, expected);
    return expected != BAD && (result.kind == NULL_VALUE || implicitVoidPointer(b, result.kind, expected))
               ? coerce(b, result, expected) : result;
}
static Value conditionalValue(Backend *b, expr *e, Kind expected) {
    struct expr_ternary *v = e->underlying;
    Kind kind = expected == BAD ? expressionKind(b, e) : expected;
    Value cond = expression(b, &v->condition);
    unsigned yes = ++b->label, no = ++b->label, end = ++b->label;
    Value dest = kind == NULL_VALUE ? value(b, BAD, "") : slot(b, kind);
    branch(b, cond, yes, no);
    label(b, yes);
    Value a = expectedValue(b, &v->ifTrue, kind == NULL_VALUE ? BAD : kind);
    if (kind != NULL_VALUE) store(b, dest, a);
    jump(b, end);
    label(b, no);
    Value c = expectedValue(b, &v->ifFalse, kind == NULL_VALUE ? BAD : kind);
    if (kind != NULL_VALUE) store(b, dest, c);
    jump(b, end);
    label(b, end);
    if (kind == NULL_VALUE) return value(b, NULL_VALUE, "@__ocean.null");
    Value result = load(b, dest);
    rawStore(b, dest, emptyValue(b, kind));
    trackDynamicArrays(b, result);
    return result;
}
typedef struct {
    Value *items;
    size_t count;
} ValueList;
static Value materialize(Backend *, expr *);
static ValueList tryIndexValues(Backend *b, struct expr_index *v) {
    Value base = materialize(b, &v->parent);
    BufferKind *t = bufferKind(b, base.kind);
    Value parent = t->shape ? load(b, base) : base;
    if (t->dynamicArray) parent = copyValue(b, parent, 0);
    Value index = coerce(b, expression(b, &v->index), INT);
    Value length = !t->shape ? value(b, INT, "%zu", t->count) :
        parent.length ? value(b, INT, "%s", parent.length) : arrayLength(b, parent);
    Value ok = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp ult i64 %s, %s\n", ok.operand, index.operand, length.operand);
    Value result = slot(b, t->element);
    rawStore(b, result, emptyValue(b, t->element));
    unsigned yes = ++b->label, end = ++b->label;
    branch(b, ok, yes, end); label(b, yes);
    Value data = t->shape == 4 ? arrayData(b, parent) : parent;
    if (!t->shape && t->fixedArray) {
        data = temp(b, pointerBufferKind(b, t->element));
        fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, i32 1, i64 0\n", data.operand, t->ir, parent.operand);
    }
    Value item = temp(b, t->element);
    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", item.operand,
            irType(b, t->element), data.operand, index.operand);
    store(b, result, load(b, item));
    jump(b, end); label(b, end);
    Value *items = checked(2 * sizeof(*items));
    items[0] = load(b, result); items[1] = ok;
    rawStore(b, result, emptyValue(b, t->element));
    trackDynamicArrays(b, items[0]);
    return (ValueList){items, 2};
}
static ValueList evaluateValues(Backend *b, expr *items, size_t count, Kind *expected,
                                size_t nExpected, int snapshot) {
    if (count == 1 && items[0].type == EXPR_TYPE_INDEX &&
        ((struct expr_index *)items[0].underlying)->tryIndex)
        return tryIndexValues(b, items[0].underlying);
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
                    result.items[j] = snapshot ? copyValue(b, extract(b, v, j), 0) : extract(b, v, j);
                return result;
            }
        }
        result.items[i] = snapshot ? copyValue(b, v, 0) : v;
    }
    return result;
}
static void checkArrayIndex(Backend *b, Value array, Value index, token location) {
    b->boundsUsed = 1;
    BufferKind *t = bufferKind(b, array.kind);
    Value length = arrayLength(b, array);
    Value nonnegative = temp(b, BOOL), below = temp(b, BOOL),
          valid = temp(b, BOOL);
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
    if (!t->shape) return value(b, INT, "%zu", t->count);
    if (array.length) return value(b, INT, "%s", array.length);
    if (t->dynamicArray) return load(b, value(b, INT, "%s", array.operand));
    Value bytes = load(b, value(b, INT, "%s", array.operand)), stride = storageSize(b, t->element),
          length = temp(b, INT);
    fprintf(b->body, "  %s = sdiv i64 %s, %s\n", length.operand, bytes.operand, stride.operand);
    return length;
}
// A typed array can be forwarded to ...any without losing its element ABI.
static Value boxArray(Backend *b, Value source) {
    BufferKind *type = bufferKind(b, source.kind);
    Value count = arrayLength(b, source), result = allocateArray(b, SCALAR_ANY, count),
          sourceData = arrayData(b, source), destination = arrayData(b, result), cursor = slot(b, INT);
    store(b, cursor, value(b, INT, "0"));
    unsigned check = ++b->label, body = ++b->label, done = ++b->label;
    jump(b, check); label(b, check);
    Value i = load(b, cursor), more = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
    branch(b, more, body, done); label(b, body);
    Value from = temp(b, type->element), to = temp(b, SCALAR_ANY);
    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", from.operand,
            irType(b, type->element), sourceData.operand, i.operand);
    fprintf(b->body, "  %s = getelementptr { ptr, i64 }, ptr %s, i64 %s\n", to.operand,
            destination.operand, i.operand);
    store(b, to, boxScalar(b, load(b, from), 1));
    Value next = temp(b, INT);
    fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
    store(b, cursor, next); jump(b, check); label(b, done);
    return result;
}

static Value unboxArrayView(Backend *b, Value box, Kind kind) {
    b->arrayViewUsed = b->anyUsed = 1;
    Value heap = temp(b, kind);
    fprintf(b->body, "  %s = call ptr @__ocean.any.array({ ptr, i64 } %s)\n", heap.operand, box.operand);
    int previous = b->copyTransport;
    b->copyTransport = 2;
    Value result = copyValue(b, heap, 0);
    b->copyTransport = previous;
    BufferKind *target = bufferKind(b, kind);
    if (!target->count && !target->fixedArray) {
        b->homogeneousUsed = 1;
        fprintf(b->body, "  call void @__ocean.array.check(ptr %s)\n", result.operand);
    }
    checkArrayRequirement(b, result, target->requirement);
    return result;
}

static void checkElementRequirement(Backend *b, Value box, Kind requirement) {
    if (!requirement || requirement == SCALAR_ANY) return;
    Value matches = testConstraint(b, box, requirement, 0);
    unsigned good = ++b->label, bad = ++b->label;
    branch(b, matches, good, bad); label(b, bad);
    fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
    b->nullChecksUsed = 1; b->terminated = 1;
    label(b, good);
}
static void checkArrayRequirement(Backend *b, Value array, Kind requirement) {
    if (!requirement || requirement == SCALAR_ANY) return;
    Value count = arrayLength(b, array), cursor = slot(b, INT);
    rawStore(b, cursor, value(b, INT, "0"));
    unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
    jump(b, condition); label(b, condition);
    Value i = load(b, cursor), more = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
    branch(b, more, body, end); label(b, body);
    checkElementRequirement(b, load(b, elementAddress(b, array, i)), requirement);
    Value next = temp(b, INT);
    fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
    rawStore(b, cursor, next); jump(b, condition); label(b, end);
}
static int hasNonzeroEmptyValue(Backend *b, Kind kind) {
    if (representationKind(b, kind) == SCALAR_ANY) return 1;
    BufferKind *t = bufferKind(b, kind);
    if (t && (t->shape == 5 || t->shape == 2)) {
        for (size_t i = 0; i < t->count; i++)
            if (hasNonzeroEmptyValue(b, t->members[i])) return 1;
    }
    return t && (t->shape == 4 || (!t->shape && (t->fixedArray || hasNonzeroEmptyValue(b, t->element))));
}
static Value emptyValue(Backend *b, Kind kind) {
    if (representationKind(b, kind) == SCALAR_ANY) {
        Value metadata = anyMetadata(b, INT);
        b->ffiUsed = 1;
        return value(b, kind, "{ ptr %s, i64 0 }", metadata.operand);
    }
    if (kind == RAW_POINTER)
        return value(b, kind, "null");
    if (floating(b, kind))
        return value(b, kind, "0x0000000000000000");
    BufferKind *t = bufferKind(b, kind);
    if (t && t->zeroOperand)
        return (Value){.kind = kind, .operand = t->zeroOperand};
    if (t && t->shape == 4 && t->definition) {
        Value zero = emptyValue(b, t->underlying); zero.kind = kind; return zero;
    }
    if (t && t->shape == 4) {
        // Empty reference values have a valid constant header, rather than a
        // null pointer. This needs no allocation or construction at runtime.
        if (!t->emptyEmitted) {
            if (t->dynamicArray) fprintf(b->out, "@.zero.array.%u = private constant { i64, i64, ptr } zeroinitializer, align 8\n", (unsigned)kind);
            else fprintf(b->out, "@.zero.array.%u = private constant i64 0, align 8\n", (unsigned)kind);
            t->emptyEmitted = 1;
        }
        Value result = value(b, kind, "@.zero.array.%u", (unsigned)kind);
        t->zeroOperand = result.operand;
        return result;
    }
    if (t && (t->shape == 5 || t->shape == 2 || !t->shape) && hasNonzeroEmptyValue(b, kind)) {
        char *text = NULL;
        size_t length = 0;
        FILE *constant = open_memstream(&text, &length);
        if (!constant) {
            fail(b, "could not create aggregate zero constant");
            return value(b, BAD, "0");
        }
        int record = t->shape == 5 || t->shape == 2;
        if (!record && t->fixedArray) {
            fprintf(constant, "{ i64 ptrtoint (ptr getelementptr (%s, ptr null, i64 %zu) to i64), [%zu x %s] ", irType(b, t->element), t->count, t->count, irType(b, t->element));
        }
        fputs(record ? "{ " : "[ ", constant);
        for (size_t i = 0; i < t->count; i++) {
            Kind member = record ? t->members[i] : t->element;
            Value zero = emptyValue(b, member);
            fprintf(constant, "%s%s %s", i ? ", " : "", irType(b, member), zero.operand);
        }
        fputs(record ? " }" : " ]", constant);
        if (!record && t->fixedArray) fputs(" }", constant);
        if (fclose(constant)) {
            free(text);
            fail(b, "could not write aggregate zero constant");
            return value(b, BAD, "0");
        }
        t->zeroOperand = ownOperand(b, text);
        return (Value){.kind = kind, .operand = t->zeroOperand};
    }
    return value(b, kind, t ? "zeroinitializer" : "0");
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
            Value array = allocateCollection(b, expected, value(b, INT, "%zu", literal->nEntries));
            array.kind = expected;
            copyArrayLiteral(b, array, literal, location);
            return array;
        }
        if (!t->shape && !literal->type.type) {
            Value result = emptyValue(b, expected);
            if (literal->nEntries > t->count) {
                fail(b, "too many buffer literal elements");
                return value(b, BAD, "0");
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
    for (size_t i = 0; i < literal->nEntries; i++) {
        Value address = elementAddress(b, array, value(b, INT, "%zu", i));
        store(b, address, items[i]);
    }
    free(items);
    Value empty = emptyValue(b, t->element), cursor = slot(b, INT);
    store(b, cursor, value(b, INT, "%zu", literal->nEntries));
    unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
    jump(b, condition);
    label(b, condition);
    Value i = load(b, cursor), more = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, length.operand);
    branch(b, more, body, end);
    label(b, body);
    Value address = elementAddress(b, array, i);
    store(b, address, empty);
    Value next = temp(b, INT);
    fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
    store(b, cursor, next);
    jump(b, condition);
    label(b, end);
    checkArrayRequirement(b, array, t->requirement);
    if (t->element == SCALAR_ANY && !t->count) {
        b->homogeneousUsed = 1;
        fprintf(b->body, "  call void @__ocean.array.check(ptr %s)\n", array.operand);
    }
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
        if (ownsArrays(b, t->element)) {
            Value snapshot = allocateArrayStorage(b, t->element, length);
            Value data = arrayData(b, snapshot);
            fprintf(b->body,
                    "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n",
                    data.operand, from.operand, bytes.operand);
            snapshot = copyValue(b, snapshot, 0);
            Value cursor = slot(b, INT);
            rawStore(b, cursor, value(b, INT, "0"));
            unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
            jump(b, condition);
            label(b, condition);
            Value i = load(b, cursor), more = temp(b, BOOL);
            fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand,
                    length.operand);
            branch(b, more, body, end);
            label(b, body);
            store(b, elementAddress(b, array, i), load(b, elementAddress(b, snapshot, i)));
            Value next = temp(b, INT);
            fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
            rawStore(b, cursor, next);
            jump(b, condition);
            label(b, end);
        } else {
            fprintf(b->body,
                    "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n",
                    to.operand, from.operand, bytes.operand);
        }
        b->dataCopyUsed = 1;
    }
    if (t->element == SCALAR_ANY && !t->count) {
        b->homogeneousUsed = 1;
        fprintf(b->body, "  call void @__ocean.array.check(ptr %s)\n", array.operand);
    }
    checkArrayRequirement(b, array, t->requirement);
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
        while (parent && parent->shape == 6) parent = bufferKind(b, parent->element);
        arrayMember = parent && (parent->shape == 4 || parent->fixedArray);
    }
    if (addressable(e) && !arrayMember) return address(b, e);
    Value aggregate = expression(b, e), storage = bufferLiteralSlot(b, aggregate.kind);
    // Scratch slots borrow a fresh expression temporary. Their previous
    // contents may already have been released at the preceding statement.
    rawStore(b, storage, copyValue(b, aggregate, 0));
    return storage;
}
static int pureIndex(expr *e) {
    if (!e || !e->underlying) return 1;
    if (e->type == EXPR_TYPE_IDENTIFIER || e->type == EXPR_TYPE_LITERAL) return 1;
    if (e->type == EXPR_TYPE_BINARY) {
        struct expr_binary *v = e->underlying;
        return v->operation < EXPR_ADD_EQUALS && pureIndex(&v->left) && pureIndex(&v->right);
    }
    if (e->type == EXPR_TYPE_UNARY) {
        struct expr_unary *v = e->underlying;
        return v->operation < UNARY_POSTFIX_INCREMENT && pureIndex(&v->child);
    }
    return 0;
}
static Value address(Backend *b, expr *e) {
    if (e->type == EXPR_TYPE_IDENTIFIER)
        return find(b, ((struct expr_identifier *)e->underlying)->lexeme.lexeme);
    if (e->type == EXPR_TYPE_UNARY &&
        ((struct expr_unary *)e->underlying)->operation == UNARY_DEREFERENCE) {
        Value pointer = expression(b, &((struct expr_unary *)e->underlying)->child);
        BufferKind *t = bufferKind(b, pointer.kind);
        if (t && t->shape == 6) {
            requirePointer(b, pointer);
            Value result = value(b, t->element, "%s", pointer.operand);
            result.throughPointer = 1;
            return result;
        }
        fail(b, "dereference requires a pointer");
        return value(b, BAD, "null");
    }
    if (e->type == EXPR_TYPE_DOT) {
        struct expr_dot *field = e->underlying;
        const char *name = ((struct expr_identifier *)field->field.underlying)->lexeme.lexeme;
        Value base = materialize(b, &field->parent);
        BufferKind *t = bufferKind(b, base.kind);
        while (t && t->shape == 6) {
            Value pointer = load(b, base);
            requirePointer(b, pointer);
            base = value(b, t->element, "%s", pointer.operand);
            base.throughPointer = 1;
            t = bufferKind(b, base.kind);
        }
        if (t && t->shape == 5) {
            size_t i = recordField(t, name);
            if (i < t->count) {
                Value result = temp(b, t->members[i]);
                result.owner = base.owner;
                result.throughPointer = base.throughPointer;
                fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, i32 %zu\n",
                        result.operand, t->ir, base.operand, i);
                return result;
            }
            fail(b, "unknown struct field");
            return value(b, BAD, "null");
        }
        fail(b, !strcmp(name, "size") ? "array size is immutable"
                                      : "inline array.data requires a copy assignment");
        return value(b, BAD, "null");
    }
    if (e->type == EXPR_TYPE_INDEX) {
        struct expr_index *v = e->underlying;
        Value base = materialize(b, &v->parent);
        BufferKind *t = bufferKind(b, base.kind);
        if (!t || (t->shape && t->shape != 3 && t->shape != 4)) {
            fail(b, "indexing requires a buffer or array");
            return value(b, BAD, "null");
        }
        Value pointer = t->shape ? load(b, base) : value(b, BAD, "");
        if (t->dynamicArray && b->readingIndex && !pureIndex(&v->index)) pointer = copyValue(b, pointer, 0);
        int previousRead = b->readingIndex; b->readingIndex = 0;
        Value index = expression(b, &v->index);
        b->readingIndex = previousRead;
        if (!integral(b, index.kind)) {
            fail(b, "buffer index must be an integer");
            return value(b, BAD, "null");
        }
        index = convert(b, index, INT);
        Value result = temp(b, t->element);
        result.owner = base.owner;
        result.throughPointer = base.throughPointer;
        // Raw buffers stay unchecked; arrays use their current byte-size header.
        if (t->shape == 3 || t->shape == 4) {
            if (t->shape == 3 && t->fixedArray) {
                if (!pointer.length) { fail(b, "array length metadata is missing"); return value(b, BAD, "null"); }
                Value header = slot(b, INT), bytes = temp(b, INT), stride = storageSize(b, t->element);
                fprintf(b->body, "  %s = mul i64 %s, %s\n", bytes.operand, pointer.length, stride.operand);
                rawStore(b, header, bytes);
                checkArrayIndex(b, value(b, arrayValueKind(b, t->element), "%s", header.operand), index, v->location);
            }
            if (t->shape == 4) {
                checkArrayIndex(b, pointer, index, v->location);
                if (t->element == SCALAR_ANY && !t->count) result.homogeneousArray = pointer.operand;
                result.elementRequirement = t->requirement;
                pointer = arrayData(b, pointer);
            }
            fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", result.operand,
                    irType(b, t->element), pointer.operand, index.operand);
        } else {
            if (t->fixedArray) {
                Value header = slot(b, INT), bytes = temp(b, INT), stride = storageSize(b, t->element);
                fprintf(b->body, "  %s = mul i64 %zu, %s\n", bytes.operand, t->count, stride.operand);
                rawStore(b, header, bytes);
                checkArrayIndex(b, value(b, arrayValueKind(b, t->element), "%s", header.operand), index, v->location);
            }
            fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 0, %si64 %s\n", result.operand,
                    t->ir, base.operand, t->fixedArray ? "i32 1, " : "", index.operand);
        }
        return result;
    }
    fail(b, "expression has no writable storage");
    return value(b, BAD, "null");
}
static Value removeDynamicArray(Backend *b, struct expr_dot *dot, struct expr_call *call) {
    Value binding = address(b, &dot->parent), source = load(b, binding);
    // An index expression can mutate the receiver, so preserve its original value.
    int snapshot = call->nParams && !pureIndex(&call->params[0]);
    if (snapshot) source = copyValue(b, source, 0);
    Value length = arrayLength(b, source), index;
    if (call->nParams) index = coerce(b, expression(b, &call->params[0]), INT);
    else {
        index = temp(b, INT);
        fprintf(b->body, "  %s = sub i64 %s, 1\n", index.operand, length.operand);
    }
    checkArrayIndex(b, source, index, dot->parent.location);
    Value removed = copyValue(b, load(b, elementAddress(b, source, index)), 0);
    Value newLength = temp(b, INT);
    fprintf(b->body, "  %s = sub i64 %s, 1\n", newLength.operand, length.operand);
    Value result = allocateCollection(b, source.kind, newLength), cursor = slot(b, INT);
    rawStore(b, cursor, value(b, INT, "0"));
    unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
    jump(b, condition); label(b, condition);
    Value i = load(b, cursor), more = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp ult i64 %s, %s\n", more.operand, i.operand, newLength.operand);
    branch(b, more, body, end); label(b, body);
    Value after = temp(b, BOOL), shifted = temp(b, INT), from = temp(b, INT);
    fprintf(b->body, "  %s = icmp uge i64 %s, %s\n", after.operand, i.operand, index.operand);
    fprintf(b->body, "  %s = add i64 %s, 1\n", shifted.operand, i.operand);
    fprintf(b->body, "  %s = select i1 %s, i64 %s, i64 %s\n", from.operand, after.operand, shifted.operand, i.operand);
    store(b, elementAddress(b, result, i), load(b, elementAddress(b, source, from)));
    rawStore(b, cursor, shifted); jump(b, condition); label(b, end);
    if (snapshot) dropDynamicArrayPayload(b, source);
    escapeDynamicArrays(b, result);
    dropDynamicArrayPayload(b, load(b, binding)); rawStore(b, binding, result);
    return removed;
}
static Value mutateDynamicArray(Backend *b, struct expr_dot *dot, struct expr_call *call) {
    const char *name = ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme;
    Value binding = address(b, &dot->parent), source = load(b, binding);
    int snapshot = 0;
    for (size_t i = 0; i < call->nParams; i++) if (!pureIndex(&call->params[i])) snapshot = 1;
    if (snapshot) source = copyValue(b, source, 0);
    BufferKind *t = bufferKind(b, source.kind);
    Value oldLength = arrayLength(b, source);
    size_t first = !strcmp(name, "add") ? 1 : 0;
    Value index = first ? coerce(b, expression(b, &call->params[0]), INT) :
                  !strcmp(name, "prepend") ? value(b, INT, "0") : oldLength;
    // An insertion at length is valid; negative positions fail the unsigned check.
    Value valid = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp ule i64 %s, %s\n", valid.operand, index.operand, oldLength.operand);
    unsigned good = ++b->label, bad = ++b->label;
    branch(b, valid, good, bad); label(b, bad);
    fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
    b->nullChecksUsed = 1; b->terminated = 1; label(b, good);
    size_t explicitCount = call->nParams - first - (call->spread ? 1 : 0);
    Value *items = checked(sizeof(Value) * (explicitCount + 1));
    for (size_t i = 0; i < explicitCount; i++) items[i] = expectedValue(b, &call->params[first + i], t->element);
    Value tail = value(b, BAD, ""), tailLength = value(b, INT, "0");
    if (call->spread) { tail = expression(b, &call->params[call->nParams - 1]); tailLength = arrayLength(b, tail); }
    Value added = temp(b, INT), length = temp(b, INT);
    fprintf(b->body, "  %s = add i64 %zu, %s\n", added.operand, explicitCount, tailLength.operand);
    fprintf(b->body, "  %s = add i64 %s, %s\n", length.operand, oldLength.operand, added.operand);
    Value result = allocateCollection(b, source.kind, length), cursor = slot(b, INT);
    rawStore(b, cursor, value(b, INT, "0"));
    unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
    jump(b, condition); label(b, condition);
    Value i = load(b, cursor), more = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, oldLength.operand);
    branch(b, more, body, end); label(b, body);
    Value after = temp(b, BOOL), shifted = temp(b, INT), destination = temp(b, INT);
    fprintf(b->body, "  %s = icmp sge i64 %s, %s\n", after.operand, i.operand, index.operand);
    fprintf(b->body, "  %s = add i64 %s, %s\n", shifted.operand, i.operand, added.operand);
    fprintf(b->body, "  %s = select i1 %s, i64 %s, i64 %s\n", destination.operand, after.operand, shifted.operand, i.operand);
    store(b, elementAddress(b, result, destination), load(b, elementAddress(b, source, i)));
    Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
    rawStore(b, cursor, next); jump(b, condition); label(b, end);
    for (size_t j = 0; j < explicitCount; j++) {
        Value at = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, %zu\n", at.operand, index.operand, j);
        store(b, elementAddress(b, result, at), items[j]);
    }
    if (call->spread) {
        rawStore(b, cursor, value(b, INT, "0"));
        condition = ++b->label; body = ++b->label; end = ++b->label;
        jump(b, condition); label(b, condition);
        i = load(b, cursor); more = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, tailLength.operand);
        branch(b, more, body, end); label(b, body);
        Value offset = temp(b, INT), at = temp(b, INT);
        fprintf(b->body, "  %s = add i64 %s, %zu\n", offset.operand, index.operand, explicitCount);
        fprintf(b->body, "  %s = add i64 %s, %s\n", at.operand, offset.operand, i.operand);
        store(b, elementAddress(b, result, at), load(b, elementAddress(b, tail, i)));
        next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
        rawStore(b, cursor, next); jump(b, condition); label(b, end);
    }
    free(items);
    if (snapshot) dropDynamicArrayPayload(b, source);
    escapeDynamicArrays(b, result);
    dropDynamicArrayPayload(b, load(b, binding)); rawStore(b, binding, result);
    return value(b, VOID, "");
}
static Value addArrayValues(Backend *b, Value left, Value right, Kind resultKind) {
    BufferKind *resultType = bufferKind(b, resultKind);
    if (!resultType->shape && resultType->fixedArray) {
        Value result = emptyValue(b, resultKind);
        BufferKind *a = bufferKind(b, left.kind), *c = bufferKind(b, right.kind);
        for (size_t i = 0; i < a->count; i++) result = insert(b, result, extract(b, left, i), i);
        for (size_t i = 0; i < c->count; i++) result = insert(b, result, extract(b, right, i), a->count + i);
        return result;
    }
    Value pieces[] = {left, right};
    int arrays[2];
    Value lengths[2];
    for (size_t i = 0; i < 2; i++) {
        BufferKind *piece = bufferKind(b, pieces[i].kind);
        arrays[i] = piece && (piece->shape == 4 || piece->fixedArray) && pieces[i].kind != resultType->element;
        lengths[i] = arrays[i] ? arrayLength(b, pieces[i]) : value(b, INT, "1");
    }
    Value length = temp(b, INT);
    fprintf(b->body, "  %s = add i64 %s, %s\n", length.operand, lengths[0].operand, lengths[1].operand);
    Value result = allocateCollection(b, resultKind, length);
    result.kind = resultKind;
    for (size_t part = 0; part < 2; part++) {
        Value offset = part ? lengths[0] : value(b, INT, "0");
        if (!arrays[part]) {
            store(b, elementAddress(b, result, offset), pieces[part]);
            continue;
        }
        BufferKind *source = bufferKind(b, pieces[part].kind);
        if (source->element == resultType->element && !ownsArrays(b, resultType->element)) {
            Value destination = elementAddress(b, result, offset), data = arrayData(b, pieces[part]);
            Value bytes = arrayBytes(b, pieces[part]);
            fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n",
                    destination.operand, data.operand, bytes.operand);
            b->dataCopyUsed = 1;
            continue;
        }
        Value cursor = slot(b, INT);
        rawStore(b, cursor, value(b, INT, "0"));
        unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
        jump(b, condition); label(b, condition);
        Value index = load(b, cursor), more = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, index.operand, lengths[part].operand);
        branch(b, more, body, end); label(b, body);
        Value target = temp(b, INT);
        fprintf(b->body, "  %s = add i64 %s, %s\n", target.operand, offset.operand, index.operand);
        store(b, elementAddress(b, result, target), load(b, elementAddress(b, pieces[part], index)));
        Value next = temp(b, INT);
        fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, index.operand);
        rawStore(b, cursor, next); jump(b, condition); label(b, end);
    }
    checkArrayRequirement(b, result, resultType->requirement);
    if (resultType->element == SCALAR_ANY && !resultType->count) {
        b->homogeneousUsed = 1;
        fprintf(b->body, "  call void @__ocean.array.check(ptr %s)\n", result.operand);
    }
    return result;
}
static Value binary(Backend *b, enum binaryExprType op, Value a, Value c) {
    if (op == EXPR_ADD) {
        Kind result = arrayAdditionKind(b, a.kind, c.kind);
        if (result != BAD) return addArrayValues(b, a, c, result);
    }
    if (a.kind == NULL_VALUE || c.kind == NULL_VALUE) {
        if (op != EXPR_EQUALITY && op != EXPR_NOT_EQUALS) {
            fail(b, "null supports only equality comparisons");
            return value(b, BAD, "0");
        }
        if (a.kind == NULL_VALUE && c.kind == NULL_VALUE)
            return value(b, BOOL, "%d", op == EXPR_EQUALITY);
        if (a.kind == NULL_VALUE) a = coerce(b, a, c.kind);
        else c = coerce(b, c, a.kind);
    }
    if (op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS) {
        if (implicitVoidPointer(b, c.kind, a.kind)) c = coerce(b, c, a.kind);
        else if (implicitVoidPointer(b, a.kind, c.kind)) a = coerce(b, a, c.kind);
    }
    BufferKind *array = bufferKind(b, a.kind);
    if (array && array->shape == 6 && a.kind == c.kind &&
        (op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS)) {
        Value result = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp %s ptr %s, %s\n", result.operand,
                op == EXPR_EQUALITY ? "eq" : "ne", a.operand, c.operand);
        return result;
    }
    BufferKind *rightArray = bufferKind(b, c.kind);
    if ((op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS) &&
        array && array->shape == 4 && array->element == I8 && rightArray && rightArray->shape == 4 && rightArray->element == I8) {
        Value leftLength = arrayBytes(b, a), rightLength = arrayBytes(b, c);
        Value sameLength = temp(b, BOOL), result = slot(b, BOOL);
        rawStore(b, result, value(b, BOOL, "false"));
        fprintf(b->body, "  %s = icmp eq i64 %s, %s\n", sameLength.operand,
                leftLength.operand, rightLength.operand);
        unsigned compare = ++b->label, end = ++b->label;
        branch(b, sameLength, compare, end);
        label(b, compare);
        Value left = arrayData(b, a), right = arrayData(b, c), compared = temp(b, I32);
        fprintf(b->body, "  %s = call i32 @\"\\01memcmp\"(ptr %s, ptr %s, i64 %s)\n",
                compared.operand, left.operand, right.operand, leftLength.operand);
        Value equal = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp eq i32 %s, 0\n", equal.operand, compared.operand);
        rawStore(b, result, equal);
        jump(b, end);
        label(b, end);
        b->stringComparisonUsed = 1;
        Value answer = load(b, result);
        if (op == EXPR_NOT_EQUALS) {
            Value inverted = temp(b, BOOL);
            fprintf(b->body, "  %s = xor i1 %s, true\n", inverted.operand, answer.operand);
            answer = inverted;
        }
        return answer;
    }
    if (integral(b, a.kind) && integral(b, c.kind) && a.kind != c.kind) {
        Kind common = unsignedInteger(b, a.kind) || unsignedInteger(b, c.kind) ? UINT : INT;
        a = convert(b, a, common);
        c = convert(b, c, common);
    }
    if ((floating(b, a.kind) || floating(b, c.kind)) && a.kind != c.kind &&
        (floating(b, a.kind) || integral(b, a.kind)) && (floating(b, c.kind) || integral(b, c.kind))) {
        Kind common = a.kind == FLOAT || c.kind == FLOAT ? FLOAT : F32;
        a = convert(b, a, common); c = convert(b, c, common);
    }
    if (a.kind != c.kind)
        fail(b, "binary operand types differ");
    const char *instruction = NULL;
    int comparison = 0, fp = floating(b, a.kind);
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
        instruction = fp ? "fdiv" : unsignedInteger(b, a.kind) ? "udiv" : "sdiv";
        break;
    case EXPR_MODULO:
        instruction = unsignedInteger(b, a.kind) ? "urem" : "srem";
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
        instruction = unsignedInteger(b, a.kind) ? "lshr" : "ashr";
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
        instruction = fp ? "fcmp olt" : unsignedInteger(b, a.kind) ? "icmp ult" : "icmp slt";
        comparison = 1;
        break;
    case EXPR_GREATER_THAN:
        instruction = fp ? "fcmp ogt" : unsignedInteger(b, a.kind) ? "icmp ugt" : "icmp sgt";
        comparison = 1;
        break;
    case EXPR_LESS_EQUALS:
        instruction = fp ? "fcmp ole" : unsignedInteger(b, a.kind) ? "icmp ule" : "icmp sle";
        comparison = 1;
        break;
    case EXPR_GREATER_EQUALS:
        instruction = fp ? "fcmp oge" : unsignedInteger(b, a.kind) ? "icmp uge" : "icmp sge";
        comparison = 1;
        break;
    default:
        break;
    }
    if (!instruction || representationKind(b, a.kind) >= BUFFER || a.kind == RAW_POINTER || a.kind == VOID || a.kind == BAD ||
        (fp && !comparison && op != EXPR_ADD && op != EXPR_SUBTRACT && op != EXPR_MULTIPLY &&
         op != EXPR_DIVIDE)) {
        fail(b, "unsupported binary operator or operand type");
        return value(b, BAD, "0");
    }
    Value r = temp(b, comparison ? BOOL : a.kind);
    fprintf(b->body, "  %s = %s %s %s, %s\n", r.operand, instruction, irType(b, a.kind), a.operand,
            c.operand);
    return r;
}

static Value expressionImpl(Backend *, expr *);
static Value enumExpression(Backend *b, struct expr_enum *v) {
    struct stmt_enum *decl = v->declaration;
    if (v->operation == ENUM_TAG) return extract(b, expression(b, &v->child), 0);
    if (v->operation == ENUM_EXTRACT) {
        Value source = expression(b, &v->child);
        Value tag = extract(b, source, 0);
        Value valid = binary(b, EXPR_EQUALITY, tag, value(b, INT, "%zu", v->member));
        unsigned good = ++b->label, bad = ++b->label;
        branch(b, valid, good, bad); label(b, bad);
        fputs("  call void @llvm.trap()\n  unreachable\n", b->body);
        b->nullChecksUsed = 1; b->terminated = 1; label(b, good);
        Kind payloadKind = enumPayloadKind(b, decl, v->member, source.kind);
        Value boxed = extract(b, source, 1);
        Value payload = payloadKind == SCALAR_ANY ? boxed : unboxValue(b, boxed, payloadKind);
        if (decl->forms[v->member] == 3 || (!v->fields && decl->forms[v->member] == 2)) return payload;
        BufferKind *record = bufferKind(b, payload.kind);
        size_t count = v->fields ? v->nFields : record->count;
        if (count == 1) return extract(b, payload, v->fields ? v->fields[0] : 0);
        Value result = emptyValue(b, enumExpressionKind(b, v));
        for (size_t i = 0; i < count; i++) result = insert(b, result, extract(b, payload, v->fields ? v->fields[i] : i), i);
        return result;
    }
    Kind owner = enumStorageKind(b, v);
    Value result = emptyValue(b, owner);
    result = insert(b, result, value(b, INT, "%zu", v->member), 0);
    if (decl->forms[v->member]) {
        Kind kind = enumPayloadKind(b, decl, v->member, owner);
        Value payload;
        if (decl->forms[v->member] == 3) {
            if (b->enumDepth >= 64) { fail(b, "cyclic or excessively nested fixed enum value"); return result; }
            b->enumDepth++;
            BufferKind *storage = bufferKind(b, owner);
            TypeBinding *previous = b->typeBindings;
            TypeBinding *bindings = checked((storage->nArguments + 1) * sizeof(TypeBinding));
            for (size_t i = 0; i < storage->nArguments; i++) {
                bindings[i] = (TypeBinding){decl->typeParamNames[i].lexeme, storage->arguments[i], b->typeBindings};
                b->typeBindings = &bindings[i];
            }
            payload = expectedValue(b, decl->values[v->member], kind);
            b->typeBindings = previous; free(bindings);
            b->enumDepth--;
        }
        else if (v->operation == ENUM_CONSTRUCT) payload = constructComposite(b, &v->child, kind);
        else { fail(b, "enum payload variant requires construction"); return result; }
        result = insert(b, result, boxScalar(b, coerce(b, payload, kind), 1), 1);
    }
    return result;
}

static Value expression(Backend *b, expr *e) {
    token previous = b->location;
    if (e && e->location.line > 0) b->location = e->location;
    Value result = expressionImpl(b, e);
    b->location = previous;
    return result;
}
static Value expressionImpl(Backend *b, expr *e) {
    if (!e || !e->underlying) {
        fail(b, "missing expression");
        return value(b, BAD, "0");
    }
    switch (e->type) {
    case EXPR_TYPE_ENUM: return enumExpression(b, e->underlying);
    case EXPR_TYPE_MAKE:
    case EXPR_TYPE_ZERO: {
        Kind k = type(b, ((struct expr_zero *)e->underlying)->type);
        if (k == VOID || k == BAD) {
            fail(b, "zero requires a supported storage type");
            return value(b, BAD, "0");
        }
        return e->type == EXPR_TYPE_MAKE ? allocateCollection(b, k, value(b, INT, "0")) : emptyValue(b, k);
    }
    case EXPR_TYPE_NEW: {
        Kind element = type(b, ((struct expr_zero *)e->underlying)->type);
        if (element == VOID || element == BAD) {
            fail(b, "new requires a supported storage type");
            return value(b, BAD, "0");
        }
        Value result = temp(b, pointerKind(b, element)), size = storageSize(b, element);
        // Even a zero-sized type gets a distinct, usable allocation. calloc
        // clears padding too; emptyValue supplies language zero values such as
        // the valid empty headers used by strings and dynamic arrays.
        Value nonempty = temp(b, BOOL), bytes = temp(b, INT);
        fprintf(b->body, "  %s = icmp ne i64 %s, 0\n", nonempty.operand, size.operand);
        fprintf(b->body, "  %s = select i1 %s, i64 %s, i64 1\n", bytes.operand, nonempty.operand, size.operand);
        b->newUsed = 1;
        fprintf(b->body, "  %s = call ptr @\"\\01calloc\"(i64 1, i64 %s)\n", result.operand, bytes.operand);
        Value present = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp ne ptr %s, null\n", present.operand, result.operand);
        unsigned good = ++b->label, bad = ++b->label;
        branch(b, present, good, bad); label(b, bad);
        fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
        b->nullChecksUsed = 1; b->terminated = 1; label(b, good);
        rawStore(b, value(b, element, "%s", result.operand), emptyValue(b, element));
        typeTokens arrayElement, dimension;
        typeTokens requested = ((struct expr_zero *)e->underlying)->type;
        if (sizedArrayTypeParts(requested, &arrayElement, &dimension) && arrayHasRuntimeDimension(requested)) {
            Value initial = parameterArrayValue(b, requested, element, NULL);
            Value destination = value(b, element, "%s", result.operand); destination.throughPointer = 1;
            store(b, destination, initial);
        }
        return result;
    }
    case EXPR_TYPE_LITERAL: {
        struct expr_literal *v = e->underlying;
        if (v->type == LITERAL_TYPE_NULL) return value(b, NULL_VALUE, "@__ocean.null");
        if (v->type == LITERAL_TYPE_INT) {
            if (v->unsignedLiteral) return value(b, UINT, "%lu", (unsigned long)*(long *)v->value);
            return value(b, INT, "%ld", *(long *)v->value);
        }
        if (v->type == LITERAL_TYPE_FLOAT) {
            union {
                double d;
                unsigned long long bits;
            } f = {.d = *(double *)v->value};
            return value(b, FLOAT, "0x%016llX", f.bits);
        }
        if (v->type == LITERAL_TYPE_STRING) {
            Value result = allocateCollection(b, stringKind(b), value(b, INT, "%zu", v->stringLength));
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
            return value(b, BOOL, "%d", !strcmp(name, "true"));
        return load(b, find(b, name));
    }
    case EXPR_TYPE_COMPOSITE:
        return expectedValue(b, e, expressionKind(b, e));
    case EXPR_TYPE_ARRAY_NEW: {
        struct expr_array_new *v = e->underlying;
        if (v->buffer) {
            Kind kind = expressionKind(b, e);
            BufferKind *t = bufferKind(b, kind);
            if (!t->shape) return emptyValue(b, kind);
            Value count = expression(b, &v->count), result = temp(b, kind);
            fprintf(b->body, "  %s = alloca %s, i64 %s\n", result.operand,
                    irType(b, t->element), count.operand);
            result.length = count.operand;
            return result;
        }
        Value result = allocateCollection(b, expressionKind(b, e), expression(b, &v->count));
        result.kind = expressionKind(b, e);
        return result;
    }
    case EXPR_TYPE_INDEX: {
        int previous = b->readingIndex; b->readingIndex = 1;
        Value result = load(b, address(b, e)); b->readingIndex = previous;
        return result;
    }
    case EXPR_TYPE_DOT: {
        struct expr_dot *v = e->underlying;
        BufferKind *parentType = bufferKind(b, expressionKind(b, &v->parent));
        BufferKind *underlying = parentType;
        while (underlying && underlying->shape == 6) underlying = bufferKind(b, underlying->element);
        if (underlying && underlying->shape == 5) return load(b, address(b, e));
        const char *fieldName = ((struct expr_identifier *)v->field.underlying)->lexeme.lexeme;
        if (underlying && underlying->fixedArray && !underlying->shape && !strcmp(fieldName, "data")) {
            Value storage = materialize(b, &v->parent);
            BufferKind *actual = bufferKind(b, storage.kind);
            while (actual && actual->shape == 6) {
                Value pointer = load(b, storage); requirePointer(b, pointer);
                storage = value(b, actual->element, "%s", pointer.operand);
                actual = bufferKind(b, actual->element);
            }
            Value data = temp(b, pointerBufferKind(b, underlying->element));
            fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, i32 1, i64 0\n", data.operand, underlying->ir, storage.operand);
            return data;
        }
        Value parent = expression(b, &v->parent);
        BufferKind *t = bufferKind(b, parent.kind);
        while (t && t->shape == 6) {
            requirePointer(b, parent);
            parent = load(b, value(b, t->element, "%s", parent.operand));
            t = bufferKind(b, parent.kind);
        }
        if (t && (t->shape == 4 || t->fixedArray)) {
            char *field = ((struct expr_identifier *)v->field.underlying)->lexeme.lexeme;
            if (t->dynamicArray && !strcmp(field, "length")) return arrayLength(b, parent);
            if (t->dynamicArray && !strcmp(field, "capacity")) {
                Value cap = temp(b, INT);
                fprintf(b->body, "  %s = getelementptr { i64, i64, ptr }, ptr %s, i32 0, i32 1\n", cap.operand, parent.operand);
                return load(b, cap);
            }
            if (!strcmp(field, "size"))
                return arrayBytes(b, parent);
            if (!strcmp(field, "data")) {
                if (t->immutableString) parent = collectionCopy(b, parent, dynamicArrayKind(b, I8));
                if (b->initializingGlobals && parent.owner != b->globalScope) {
                    Value storage = bufferLiteralSlot(b, parent.kind);
                    store(b, storage, parent);
                    parent = load(b, storage);
                }
                return arrayData(b, parent);
            }
        }
        fail(b, "unsupported member access");
        return value(b, BAD, "0");
    }
    case EXPR_TYPE_ASSERTION: {
        struct expr_assertion *v = e->underlying;
        return unboxValue(b, expression(b, &v->child), type(b, v->type));
    }
    case EXPR_TYPE_TYPECAST: {
        struct expr_typecast *v = e->underlying;
        Kind target = type(b, v->type);
        BufferKind *wanted = bufferKind(b, target);
        Kind source = expressionKind(b, &v->child);
        BufferKind *from = bufferKind(b, source);
        Value a;
        if (from && !from->shape && from->fixedArray && wanted && wanted->shape == 3) {
            a = arrayData(b, expression(b, &v->child));
        } else if (from && !from->shape && wanted && (wanted->shape == 3 || !wanted->shape || wanted->fixedArray))
            a = value(b, pointerBufferKind(b, from->element), "%s", materialize(b, &v->child).operand);
        else a = expression(b, &v->child);
        typeTokens element, dimension;
        if (wanted && (bufferTypeParts(v->type, &element, &dimension) ||
                       sizedArrayTypeParts(v->type, &element, &dimension))) {
            if (wanted->fixedArray && bufferKind(b, a.kind) && bufferKind(b, a.kind)->shape == 3) {
                expr *size = parseBufferDimension(dimension);
                if (!size) { fail(b, "invalid array length"); return value(b, BAD, "0"); }
                Value count = coerce(b, expression(b, size), INT); free(size);
                Value result = wanted->shape ? allocateCollection(b, target, count) : emptyValue(b, target);
                if (!wanted->shape) {
                    for (size_t i = 0; i < wanted->count; i++) {
                        Value item = temp(b, wanted->element);
                        fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %zu\n", item.operand, irType(b, wanted->element), a.operand, i);
                        result = insert(b, result, copyValue(b, load(b, item), 0), i);
                    }
                    trackDynamicArrays(b, result);
                } else {
                    Value cursor = slot(b, INT); rawStore(b, cursor, value(b, INT, "0"));
                    unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
                    jump(b, condition); label(b, condition);
                    Value i = load(b, cursor), more = temp(b, BOOL);
                    fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
                    branch(b, more, body, end); label(b, body);
                    Value item = temp(b, wanted->element);
                    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", item.operand, irType(b, wanted->element), a.operand, i.operand);
                    store(b, elementAddress(b, result, i), load(b, item));
                    Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
                    rawStore(b, cursor, next); jump(b, condition); label(b, end);
                }
                return result;
            }
            if (!wanted->shape && bufferKind(b, a.kind) && bufferKind(b, a.kind)->shape == 3)
                return load(b, value(b, target, "%s", a.operand));
            if (wanted->shape == 3) {
                Value result = value(b, target, "%s", a.operand);
                if (dimension.nTokens) {
                    expr *size = parseBufferDimension(dimension);
                    if (!size) { fail(b, "invalid collection length"); return value(b, BAD, "0"); }
                    result.length = coerce(b, expression(b, size), INT).operand;
                    free(size);
                }
                return result;
            }
        }
        return convert(b, a, target);
    }
    case EXPR_TYPE_CALL: {
        struct expr_call *v = e->underlying;
        MethodInstance *method = NULL;
        Value receiver = value(b, BAD, "");
        const char *name = "";
        struct stmt_func *f = NULL;
        typeTokens *typeArguments;
        size_t count;
        struct expr_dot *dot = methodCallee(&v->func, &typeArguments, &count);
        if (dot) {
            name = ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme;
            Kind actual = expressionKind(b, &dot->parent);
            BufferKind *dynamicArray = bufferKind(b, actual);
            BufferKind *arrayReceiver = dynamicArray;
            while (arrayReceiver && arrayReceiver->shape == 6) arrayReceiver = bufferKind(b, arrayReceiver->element);
            if (arrayReceiver && arrayReceiver->shape == 4 && !arrayReceiver->fixedArray && !strcmp(name, "array")) {
                Value source = expression(b, &dot->parent);
                BufferKind *sourceType = bufferKind(b, source.kind);
                while (sourceType && sourceType->shape == 6) {
                    requirePointer(b, source);
                    source = load(b, value(b, sourceType->element, "%s", source.operand));
                    sourceType = bufferKind(b, source.kind);
                }
                return collectionCopy(b, source, runtimeArrayKind(b, arrayReceiver->element));
            }
            if (dynamicArray && dynamicArray->dynamicArray && (!strcmp(name, "remove") || !strcmp(name, "pop")))
                return removeDynamicArray(b, dot, v);
            if (dynamicArray && dynamicArray->dynamicArray && (!strcmp(name, "append") || !strcmp(name, "prepend") || !strcmp(name, "add")))
                return mutateDynamicArray(b, dot, v);
            method = methodInstance(b, actual, name, typeArguments, count);
            if (!method) return value(b, BAD, "0");
            f = method->function;
            BufferKind *actualType = bufferKind(b, actual);
            BufferKind *wantedType = bufferKind(b, method->receiver);
            if (wantedType && wantedType->shape == 6 && !wantedType->definition && !(actualType && actualType->shape == 6 && !actualType->definition)) {
                if (!addressable(&dot->parent)) {
                    fail(b, "pointer receiver requires an addressable value");
                    return value(b, BAD, "0");
                }
                Value addr = address(b, &dot->parent);
                receiver = value(b, method->receiver, "%s", addr.operand);
            } else {
                receiver = expression(b, &dot->parent);
                while (actualType && actualType->shape == 6 && !actualType->definition && actualType->element != method->record) {
                    requirePointer(b, receiver);
                    receiver = load(b, value(b, actualType->element, "%s", receiver.operand));
                    actualType = bufferKind(b, actualType->element);
                }
                if (actualType && actualType->shape == 6 && !actualType->definition && !(wantedType && wantedType->shape == 6 && !wantedType->definition)) {
                    requirePointer(b, receiver);
                    receiver = load(b, value(b, actualType->element, "%s", receiver.operand));
                }
            }
        } else if (v->func.type == EXPR_TYPE_IDENTIFIER || v->func.type == EXPR_TYPE_INSTANTIATION) {
            f = directCallee(b, &v->func, &typeArguments, &count);
            expr *callee = v->func.type == EXPR_TYPE_INSTANTIATION
                               ? &((struct expr_instantiation *)v->func.underlying)->parent : &v->func;
            if (callee->type != EXPR_TYPE_IDENTIFIER) {
                fail(b, "only direct generic function calls are supported");
                return value(b, BAD, "0");
            }
            name = ((struct expr_identifier *)callee->underlying)->lexeme.lexeme;
            if (f && f->nTypeParams) {
                method = functionInstance(b, f, typeArguments, count);
                if (!method) return value(b, BAD, "0");
            }
        } else {
            fail(b, "only direct function and method calls are supported");
            return value(b, BAD, "0");
        }
        if (!strcmp(name, "compilerReflectFields") || !strcmp(name, "compilerReflectIsStruct")) {
            int fields = !strcmp(name, "compilerReflectFields");
            if (!b->function || b->function->compilerIntrinsic != (fields ? INTRINSIC_REFLECT_FIELDS : INTRINSIC_REFLECT_IS_STRUCT) ||
                v->nParams != 1 || v->spread) {
                fail(b, "reflection intrinsics are restricted to the standard library reflect package");
                return value(b, BAD, "0");
            }
            Kind kind = fields ? resultType(b, b->function) : BOOL;
            Value source = coerce(b, expression(b, &v->params[0]), SCALAR_ANY), result = temp(b, kind);
            b->reflectionUsed = 1;
            if (fields) b->reflectionFieldsKind = kind;
            fprintf(b->body, "  %s = call %s @__ocean.reflect.%s({ ptr, i64 } %s)\n", result.operand,
                    irType(b, kind), fields ? "fields" : "is_struct", source.operand);
            if (fields) {
                int previous = b->copyTransport; b->copyTransport = 2;
                result = copyValue(b, result, 0); b->copyTransport = previous;
            }
            return result;
        }
        if (!f && !strcmp(name, "free")) {
            Value item = expression(b, &v->params[0]);
            BufferKind *t = bufferKind(b, item.kind);
            b->returnTransportUsed = 1;
            if (t && t->dynamicArray) {
                dropDynamicArrayPayload(b, item);
                if (addressable(&v->params[0])) rawStore(b, address(b, &v->params[0]), emptyValue(b, item.kind));
            } else {
                if (t && t->shape == 6) dropNestedDynamicArrays(b, load(b, value(b, t->element, "%s", item.operand)), 0);
                fprintf(b->body, "  call void @\"\\01free\"(ptr %s)\n", item.operand);
            }
            return value(b, VOID, "");
        }
        if (!f && !strcmp(name, "len")) {
            if (v->nParams != 1 || v->spread) {
                fail(b, "len requires exactly one array argument");
                return value(b, BAD, "0");
            }
            Value array = expression(b, &v->params[0]);
            BufferKind *t = bufferKind(b, array.kind);
            if (t && !t->shape) return value(b, INT, "%zu", t->count);
            if (array.length) return value(b, INT, "%s", array.length);
            if (!t || t->shape != 4) {
                fail(b, "len requires a collection with known length");
                return value(b, BAD, "0");
            }
            return arrayLength(b, array);
        }
        if (!f) {
            fail(b, "unknown function");
            return value(b, BAD, "0");
        }
        if (method && f->receiverType.nTokens && ownsArrays(b, receiver.kind))
            receiver = copyValue(b, receiver, 0); // Snapshot before later arguments run.
        TypeBinding *previousBindings = b->typeBindings;
        if (method) b->typeBindings = method->nBindings ? method->bindings : NULL;
        Kind *parameterKinds = checked(sizeof(Kind) * (f->nParams + 1));
        for (size_t i = 0; i < f->nParams; i++)
            parameterKinds[i] = f->isVariadic && i + 1 == f->nParams ? type(b, f->paramTypeNames[i]) : parameterKind(b, f, i);
        Kind variadicArrayKind = f->isVariadic && !f->isExtern && f->nParams
                                     ? parameterKind(b, f, f->nParams - 1) : BAD;
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
        ValueList evaluated = evaluateValues(b, v->params, v->nParams, expected, v->nParams, 1);
        free(expected);
        if ((!f->isVariadic && evaluated.count != fixed) || evaluated.count < fixed ||
            (v->spread && (!f->isVariadic || evaluated.count < f->nParams + (f->isExtern ? 1 : 0) ||
                           evaluated.count != v->nParams))) {
            fail(b, "call argument count mismatch");
            free(evaluated.items);
            free(parameterKinds);
            return value(b, BAD, "0");
        }
        int dynamicC = f->isExtern && f->isVariadic && v->spread;
        for (size_t i = fixed; f->isExtern && f->isVariadic && i < evaluated.count; i++)
            if (evaluated.items[i].kind == SCALAR_ANY) dynamicC = 1;
        if (dynamicC) {
            b->ffiUsed = 1;
            size_t prefix = evaluated.count - (v->spread ? 1 : 0);
            Value tail = v->spread ? evaluated.items[prefix] : value(b, BAD, "0");
            BufferKind *array = bufferKind(b, tail.kind);
            if ((v->spread && (!array || array->shape != 4)) || !fixed) {
                fail(b, "C variadic expansion requires an array and at least one fixed parameter");
                free(evaluated.items); free(parameterKinds);
                return value(b, BAD, "0");
            }
            Value length = v->spread ? arrayLength(b, tail) : value(b, INT, "0"),
                  total = temp(b, INT), boxes = temp(b, RAW_POINTER), saved = temp(b, RAW_POINTER);
            fprintf(b->body, "  %s = call ptr @llvm.stacksave.p0()\n", saved.operand);
            fprintf(b->body, "  %s = add i64 %s, %zu\n", total.operand, length.operand, prefix);
            fprintf(b->body, "  %s = alloca { ptr, i64 }, i64 %s, align 8\n", boxes.operand, total.operand);
            for (size_t i = 0; i < prefix; i++) {
                Value item = evaluated.items[i];
                if (i < fixed) item = coerce(b, item, parameterKinds[i]);
                item = i < fixed ? boxFixedCScalar(b, item) : boxScalar(b, item, 1);
                Value address = temp(b, SCALAR_ANY);
                fprintf(b->body, "  %s = getelementptr { ptr, i64 }, ptr %s, i64 %zu\n", address.operand, boxes.operand, i);
                store(b, address, item);
            }
            if (v->spread) {
                Value data = arrayData(b, tail), index = slot(b, INT);
                store(b, index, value(b, INT, "0"));
                unsigned check = ++b->label, body = ++b->label, done = ++b->label;
                jump(b, check); label(b, check);
                Value i = load(b, index), more = temp(b, BOOL);
                fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, length.operand);
                branch(b, more, body, done); label(b, body);
                Value address = temp(b, array->element);
                fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", address.operand, irType(b, array->element), data.operand, i.operand);
                Value item = boxScalar(b, load(b, address), 1), destination = temp(b, SCALAR_ANY), offset = temp(b, INT);
                fprintf(b->body, "  %s = add i64 %s, %zu\n", offset.operand, i.operand, prefix);
                fprintf(b->body, "  %s = getelementptr { ptr, i64 }, ptr %s, i64 %s\n", destination.operand, boxes.operand, offset.operand);
                store(b, destination, item);
                Value next = temp(b, INT);
                fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
                store(b, index, next); jump(b, check); label(b, done);
            }
            Value result = slot(b, INT);
            store(b, result, value(b, INT, "0"));
            fprintf(b->body, "  call void @__ocean.ffi_call(ptr "); functionName(b->body, f);
            fprintf(b->body, ", i32 %zu, i64 %s, ptr %s, ptr %s, ptr %s)\n", fixed, total.operand, boxes.operand, ffiType(b, resultKind), result.operand);
            fprintf(b->body, "  call void @llvm.stackrestore.p0(ptr %s)\n", saved.operand);
            free(evaluated.items); free(parameterKinds);
            return resultKind == VOID ? value(b, VOID, "") : load(b, value(b, resultKind, "%s", result.operand));
        }
        size_t nArgs = f->isVariadic && !f->isExtern ? f->nParams : evaluated.count;
        Value *args = checked(sizeof(Value) * (nArgs + 1));
        for (size_t i = 0; i < fixed; i++)
            args[i] = coerce(b, evaluated.items[i], parameterKinds[i]);
        if (f->isVariadic && !f->isExtern) {
            Kind element = parameterKinds[f->nParams - 1];
            size_t explicitCount = evaluated.count - fixed - (v->spread ? 1 : 0);
            Value tail = value(b, BAD, ""), tailLength = value(b, INT, "0");
            if (v->spread) {
                tail = evaluated.items[evaluated.count - 1];
                BufferKind *source = bufferKind(b, tail.kind);
                if (element == SCALAR_ANY && source && source->shape == 4 &&
                    source->element != SCALAR_ANY)
                    tail = boxArray(b, tail);
                else
                    tail = coerce(b, tail, element == SCALAR_ANY ? mixedArrayKind(b) : arrayValueKind(b, element));
                if (!explicitCount) {
                    if (element == SCALAR_ANY) tail.kind = mixedArrayKind(b);
                    args[fixed] = coerce(b, tail, variadicArrayKind);
                } else
                    tailLength = arrayLength(b, tail);
            }
            if (!v->spread || explicitCount) {
                Value count = value(b, INT, "%zu", explicitCount);
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
                    if (containsDynamicArrays(b, element)) {
                        Value cursor = slot(b, INT); rawStore(b, cursor, value(b, INT, "0"));
                        unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
                        jump(b, condition); label(b, condition);
                        Value i = load(b, cursor), more = temp(b, BOOL);
                        fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, tailLength.operand);
                        branch(b, more, body, end); label(b, body);
                        Value from = temp(b, element), to = temp(b, element);
                        fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", from.operand, irType(b, element), source.operand, i.operand);
                        fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", to.operand, irType(b, element), destination.operand, i.operand);
                        rawStore(b, to, emptyValue(b, element));
                        store(b, to, load(b, from));
                        Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
                        rawStore(b, cursor, next); jump(b, condition); label(b, end);
                    } else {
                        fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n",
                                destination.operand, source.operand, bytes.operand);
                        b->dataCopyUsed = 1;
                    }
                }
                if (element == SCALAR_ANY) pack.kind = mixedArrayKind(b);
                args[fixed] = coerce(b, pack, variadicArrayKind);
            }
        } else {
            for (size_t i = fixed; i < evaluated.count; i++) {
                args[i] = evaluated.items[i];
                if (args[i].kind == NULL_VALUE) args[i].kind = RAW_POINTER;
                if (representationKind(b, args[i].kind) == BOOL || representationKind(b, args[i].kind) == I8 || representationKind(b, args[i].kind) == I16 || representationKind(b, args[i].kind) == U8 || representationKind(b, args[i].kind) == U16)
                    args[i] = convert(b, args[i], I32);
                if (representationKind(b, args[i].kind) == F32) args[i] = convert(b, args[i], FLOAT);
                BufferKind *extra = bufferKind(b, args[i].kind);
                if ((extra && extra->shape != 3 && extra->shape != 13 && extra->shape != 6) || args[i].kind == VOID)
                    fail(b, "C variadic arguments must be scalar values");
            }
        }
        free(evaluated.items);
        Value traceParent = value(b, RAW_POINTER, "null");
        if (!f->isExtern)
            traceParent = pushTraceFrame(b, f, v->location);
        Kind k = resultKind;
        Value r = k == VOID ? value(b, VOID, "") : temp(b, k);
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
        if (method && f->receiverType.nTokens)
            fprintf(b->body, "%s %s", irType(b, receiver.kind), receiver.operand);
        for (size_t i = 0; i < nArgs; i++)
            fprintf(b->body, "%s%s %s", (i || (method && f->receiverType.nTokens)) ? ", " : "",
                    irType(b, args[i].kind), args[i].operand);
        fprintf(b->body, ")\n");
        if (!f->isExtern)
            popTraceFrame(b, traceParent);
        if (!f->isExtern && ownsArrays(b, resultKind)) {
            b->copyTransport = 2;
            r = copyValue(b, r, 0);
            b->copyTransport = 0;
        }
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
                dest[i] = isDiscard(target) ? value(b, BAD, "") : address(b, target);
                expected[i] = dest[i].kind;
            }
            for (size_t i = 0; i < m; i++)
                rhs[i] = *commaItem(&v->right, i);
            ValueList values = evaluateValues(b, rhs, m, expected, n, 1);
            if (values.count != n)
                fail(b, "assignment count mismatch");
            for (size_t i = 0; i < n && i < values.count; i++)
                if (dest[i].kind != BAD)
                    store(b, dest[i], values.items[i]);
            free(values.items);
            free(rhs);
            free(expected);
            free(dest);
            return value(b, VOID, "");
        }
        if (v->operation == EXPR_ASSIGNMENT ||
            (v->operation >= EXPR_ADD_EQUALS && v->operation <= EXPR_BITWISE_OR_EQUALS)) {
            Value addr = address(b, &v->left), old = {0};
            if (v->operation != EXPR_ASSIGNMENT)
                old = load(b, addr);
            BufferKind *target = bufferKind(b, addr.kind);
            int arrayAdd = v->operation == EXPR_ADD_EQUALS && target && (target->shape == 4 || target->fixedArray);
            if (arrayAdd) old = copyValue(b, old, 0);
            Value r = arrayAdd ? expression(b, &v->right) : expectedValue(b, &v->right, addr.kind);
            if (v->operation != EXPR_ASSIGNMENT) {
                enum binaryExprType ops[] = {
                    EXPR_ADD,         EXPR_SUBTRACT,  EXPR_MULTIPLY,      EXPR_DIVIDE,
                    EXPR_MODULO,      EXPR_XOR,       EXPR_BITSHIFT_LEFT, EXPR_BITSHIFT_RIGHT,
                    EXPR_BITWISE_AND, EXPR_BITWISE_OR};
                r = binary(b, ops[v->operation - EXPR_ADD_EQUALS], old, r);
            }
            if (v->operation == EXPR_ADD_EQUALS && target && target->fixedArray && !target->shape && r.kind != addr.kind &&
                v->left.type == EXPR_TYPE_IDENTIFIER) {
                Value replacement = slot(b, r.kind); store(b, replacement, r);
                bind(b, ((struct expr_identifier *)v->left.underlying)->lexeme.lexeme, replacement);
            } else store(b, addr, r);
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
        if (v->operation == EXPR_ADD &&
            arrayAdditionKind(b, a.kind, expressionKind(b, &v->right)) != BAD)
            a = copyValue(b, a, 0); // Snapshot before RHS side effects.
        Value c = expression(b, &v->right);
        if (v->operation == EXPR_COMMA)
            return c;
        return binary(b, v->operation, a, c);
    }
    case EXPR_TYPE_UNARY: {
        struct expr_unary *v = e->underlying;
        if (v->operation == UNARY_ADDRESS_OF) {
            Value storage = v->child.type == EXPR_TYPE_COMPOSITE
                                ? materialize(b, &v->child) : address(b, &v->child);
            if (v->child.type == EXPR_TYPE_COMPOSITE) escapeDynamicArrays(b, load(b, storage));
            return value(b, pointerKind(b, storage.kind), "%s", storage.operand);
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
            Value one = floating(b, old.kind) ? value(b, old.kind, "0x3FF0000000000000") : value(b, old.kind, "1");
            Value r = binary(b, decrement ? EXPR_SUBTRACT : EXPR_ADD, old, one);
            store(b, addr, r);
            return v->operation == UNARY_POSTFIX_INCREMENT ||
                           v->operation == UNARY_POSTFIX_DECREMENT
                       ? old
                       : r;
        }
        Value a = expression(b, &v->child), r = temp(b, a.kind);
        if (v->operation == UNARY_NEGATE && floating(b, a.kind))
            fprintf(b->body, "  %s = fneg %s %s\n", r.operand, irType(b, a.kind), a.operand);
        else if (v->operation == UNARY_NEGATE && integral(b, a.kind))
            fprintf(b->body, "  %s = sub %s 0, %s\n", r.operand, irType(b, a.kind), a.operand);
        else if (v->operation == UNARY_LOGICAL_NOT && a.kind == BOOL)
            fprintf(b->body, "  %s = xor i1 %s, true\n", r.operand, a.operand);
        else if (v->operation == UNARY_BITWISE_NOT && integral(b, a.kind))
            fprintf(b->body, "  %s = xor %s %s, -1\n", r.operand, irType(b, a.kind), a.operand);
        else {
            fail(b, "unsupported unary operation");
            return value(b, BAD, "0");
        }
        return r;
    }
    case EXPR_TYPE_TERNARY:
        return conditionalValue(b, e, BAD);
    default:
        break;
    }
    fail(b, "unsupported expression");
    return value(b, BAD, "0");
}
static int hasArrayAllocation(expr *e) {
    if (!e || !e->underlying)
        return 0;
    switch (e->type) {
    case EXPR_TYPE_ZERO:
        return 0; // Scalars/aggregates are SSA values; empty headers have module lifetime.
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
        // Array addition allocates even when both operands are variables.
        return v->operation == EXPR_ASSIGNMENT || v->operation == EXPR_ADD || v->operation == EXPR_ADD_EQUALS ||
               hasArrayAllocation(&v->left) || hasArrayAllocation(&v->right);
    }
    case EXPR_TYPE_UNARY: {
        struct expr_unary *v = e->underlying;
        return v->operation != UNARY_SIZEOF && hasArrayAllocation(&v->child);
    }
    case EXPR_TYPE_CALL:
        return 1;
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
    if (s->type == STMT_LABEL)
        return statementAllocatesStack(((struct stmt_label *)s->value)->body);
    if (s->type == STMT_VAR)
        return 1; // Inferred variables can own arrays nested inside other values.
    if (s->type == STMT_EXPR || s->type == STMT_PANIC || s->type == STMT_RETURN_PANIC)
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
    } else if (s->type == STMT_SWITCH) {
        return 1;
    } else if (s->type == STMT_FOR) {
        struct stmt_for *v = s->value;
        return v->range || statementAllocatesStack(v->initializer) || hasArrayAllocation(v->condition) ||
               hasArrayAllocation(v->update) || statementAllocatesStack(v->body);
    }
    return 0;
}
static void saveStack(Backend *b, Scope *scope, int needed) {
    scope->savedArrayTemps = dynamicArrayTempMark(b); scope->hasTempMark = 1;
    Value region = boxRegion(b);
    scope->savedBoxes = load(b, region);
    if (!needed)
        return;
    scope->hasStack = 1;
    scope->savedStack = temp(b, RAW_POINTER);
    fprintf(b->body, "  %s = call ptr @llvm.stacksave.p0()\n", scope->savedStack.operand);
}
static void restoreStack(Backend *b, Scope *scope) {
    for (Binding *binding = scope->bindings; binding; binding = binding->next)
        if (binding->autoRelease) {
            dropNestedDynamicArrays(b, load(b, binding->address), 0);
            rawStore(b, binding->address, emptyValue(b, binding->address.kind));
        }
    if (scope->hasTempMark) cleanDynamicArrayTemps(b, scope->savedArrayTemps);
    if (scope->savedBoxes.operand && !scope->retainAllocations)
        fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr %s)\n", boxRegion(b).operand, scope->savedBoxes.operand);
    if (scope->hasStack && !scope->retainAllocations)
        fprintf(b->body, "  call void @llvm.stackrestore.p0(ptr %s)\n", scope->savedStack.operand);
}
static void jumpError(Backend *b, token name, const char *message) {
    name = diagnosticLocation(b, name);
    diagnostic(programSource(b->program, name.filename), name, "backend", "error",
               "%s: %s", message, name.lexeme);
    b->errors++;
}
static void *jumpAllocate(JumpContext *context, size_t size) {
    void *result = checked(size);
    JumpAllocation *allocation = checked(sizeof(*allocation));
    *allocation = (JumpAllocation){result, context->allocations};
    context->allocations = allocation;
    return result;
}
static JumpTarget *jumpTarget(JumpContext *context, const char *name) {
    for (JumpTarget *target = context->targets; target; target = target->next)
        if (!strcmp(target->name.lexeme, name)) return target;
    return NULL;
}
static JumpDeclaration *scanJumps(Backend *b, JumpContext *context, stmt *s,
                                 JumpRegion *region, const void *scope,
                                 JumpDeclaration *declarations) {
    if (!s) return declarations;
    if (s->type == STMT_VAR) {
        JumpDeclaration *declaration = jumpAllocate(context, sizeof(*declaration));
        *declaration = (JumpDeclaration){s, declarations};
        return declaration;
    }
    if (s->type == STMT_LABEL) {
        struct stmt_label *node = s->value;
        if (jumpTarget(context, node->name.lexeme)) jumpError(b, node->name, "duplicate label in function");
        JumpTarget *target = jumpAllocate(context, sizeof(*target));
        *target = (JumpTarget){.name = node->name, .label = ++b->label, .scope = scope,
                               .region = region, .declarations = declarations, .next = context->targets};
        context->targets = target;
        target->body = node->body;
        return scanJumps(b, context, node->body, region, scope, declarations);
    }
    if (s->type == STMT_GOTO) {
        JumpSite *site = jumpAllocate(context, sizeof(*site));
        *site = (JumpSite){((struct stmt_branch *)s->value)->label, region, declarations, context->sites};
        context->sites = site;
    }
    if (s->type == STMT_SWITCH) {
        struct stmt_switch *node = s->value;
        JumpRegion *nested = jumpAllocate(context, sizeof(*nested));
        nested->parent = region;
        JumpDeclaration *visible = scanJumps(b, context, &node->initializer, nested, node, declarations);
        for (size_t i = 0; i < node->nCases; i++) {
            stmt wrapper = {.type = STMT_BLOCK, .value = &node->cases[i].body};
            scanJumps(b, context, &wrapper, nested, node, visible);
        }
    }
    if (s->type == STMT_BLOCK || s->type == STMT_IF || s->type == STMT_FOR) {
        JumpRegion *nested = jumpAllocate(context, sizeof(*nested));
        nested->parent = region;
        if (s->type == STMT_BLOCK) {
            struct stmt_block *block = s->value;
            JumpDeclaration *visible = declarations;
            for (size_t i = 0; i < block->nStmts; i++)
                visible = scanJumps(b, context, &block->stmts[i], nested, block, visible);
        } else if (s->type == STMT_IF) {
            struct stmt_if *node = s->value;
            JumpDeclaration *visible = scanJumps(b, context, node->preStmt, nested, node, declarations);
            stmt *branches[] = {node->ifTrue, node->ifFalse};
            for (size_t i = 0; i < 2; i++) {
                JumpRegion *branch = jumpAllocate(context, sizeof(*branch));
                branch->parent = nested;
                scanJumps(b, context, branches[i], branch, node, visible);
            }
        } else {
            struct stmt_for *node = s->value;
            JumpDeclaration *visible = scanJumps(b, context, node->initializer, nested, node, declarations);
            JumpRegion *body = jumpAllocate(context, sizeof(*body));
            body->parent = nested;
            scanJumps(b, context, node->body, body, node->body, visible);
        }
    }
    return declarations;
}
static void prepareJumps(Backend *b, JumpContext *context, struct stmt_block *body) {
    stmt wrapper = {.type = STMT_BLOCK, .value = body};
    scanJumps(b, context, &wrapper, NULL, NULL, NULL);
    for (JumpSite *site = context->sites; site; site = site->next) {
        JumpTarget *target = jumpTarget(context, site->name.lexeme);
        if (!target) { jumpError(b, site->name, "goto refers to an undefined label"); continue; }
        JumpRegion *region = site->region;
        while (region && region != target->region) region = region->parent;
        if (region != target->region) { jumpError(b, site->name, "goto cannot enter another block or control-flow branch"); continue; }
        for (JumpDeclaration *required = target->declarations; required; required = required->previous) {
            JumpDeclaration *visible = site->declarations;
            while (visible && visible->statement != required->statement) visible = visible->previous;
            if (!visible) { jumpError(b, site->name, "goto cannot jump over a variable declaration"); break; }
        }
    }
}
static void clearJumps(JumpContext *context) {
    while (context->allocations) {
        JumpAllocation *allocation = context->allocations;
        context->allocations = allocation->next;
        free(allocation->pointer);
        free(allocation);
    }
}
static void statement(Backend *, stmt *);
static void blockLimit(Backend *b, struct stmt_block *v, size_t count) {
    Scope scope = {.parent = b->scope, .declarations = v, .jumpRegion = v};
    b->scope = &scope;
    stmt wrapper = {.type = STMT_BLOCK, .value = v};
    saveStack(b, &scope, statementAllocatesStack(&wrapper));
    for (size_t i = 0; i < count; i++) {
        int dead = b->terminated && v->stmts[i].type != STMT_LABEL;
        if (dead) label(b, ++b->label);
        statement(b, &v->stmts[i]);
        if (dead) {
            if (!b->terminated) fprintf(b->body, "  unreachable\n");
            b->terminated = 1;
        }
    }
    if (!b->terminated)
        restoreStack(b, &scope);
    b->scope = scope.parent;
    clearScope(&scope);
}
static void block(Backend *b, struct stmt_block *v) { blockLimit(b, v, v->nStmts); }
static void statementImpl(Backend *, stmt *);
static void statement(Backend *b, stmt *s) {
    token previous = b->location;
    int previousForward = b->forwardPanic;
    if (s && s->forwardPanic) b->forwardPanic = 1;
    if (s && s->location.line > 0) b->location = s->location;
    DynamicArrayTemporary *mark = b->dynamicArrayTemps;
    if (!b->terminated && statementAllocatesStack(s)) mark = dynamicArrayTempMark(b);
    statementImpl(b, s);
    if (!b->terminated) cleanDynamicArrayTemps(b, mark);
    b->location = previous;
    b->forwardPanic = previousForward;
}
static void emitAssembly(Backend *b, struct stmt_asm *v, const char *architecture) {
    int x86 = !strcmp(architecture, "x86_64");
    b->functionHasAsm = 1;
    Value *operands = checked(sizeof(*operands) * (v->nOperands + 1));
    for (size_t i=0; i<v->nOperands; i++) operands[i] = find(b, v->operands[i].lexeme);
    fprintf(b->body, "  call void asm sideeffect alignstack%s \"", x86 ? " inteldialect" : "");
    // Escape LLVM string bytes and LLVM's operand-template dollar marker.
    size_t reference = 0;
    for (size_t i=0; v->assembly[i]; i++) {
        if (reference < v->nReferences && i == v->references[reference].offset) {
            struct asm_reference ref = v->references[reference++];
            fprintf(b->body, "$%zu", ref.operand);
            i += ref.length - 1;
            continue;
        }
        unsigned char byte = v->assembly[i];
        if (byte == '$') fputs("$$", b->body);
        else if (byte < 32 || byte >= 127 || byte == '"' || byte == '\\') fprintf(b->body, "\\%02X", byte);
        else fputc(byte, b->body);
    }
    fputs("\\0A\", \"", b->body);
    // Every named memory slot is both an input and an output. Separate indirect
    // constraints express read/write effects without LLVM's unsupported '+' form.
    for (size_t i=0; i<v->nOperands; i++) fputs("=*m,", b->body);
    for (size_t i=0; i<v->nOperands; i++) fputs("*m,", b->body);
    fputs("~{memory}", b->body);
    // Conservatively spill ordinary integer/vector registers across each block.
    // Stack/frame pointers and platform-reserved registers must be preserved.
    if (x86) {
        const char *registers[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        for (size_t i=0; i<sizeof(registers)/sizeof(*registers); i++) fprintf(b->body, ",~{%s}", registers[i]);
        for (int i=0; i<16; i++) fprintf(b->body, ",~{xmm%d}", i);
        fputs(",~{dirflag},~{fpsr},~{flags}", b->body);
    } else if (!strcmp(architecture, "aarch64")) {
        for (int i=0; i<=30; i++) if (i != 18 && i != 29 && !(v->nOperands && i >= 9 && i <= 15)) fprintf(b->body, ",~{x%d}", i);
        for (int i=0; i<32; i++) fprintf(b->body, ",~{v%d}", i);
        fputs(",~{cc}", b->body);
    } else {
        for (int i=1; i<32; i++) if (i != 2 && i != 3 && i != 4 && i != 8 && !(v->nOperands && i >= 28)) fprintf(b->body, ",~{x%d}", i);
        for (int i=0; i<32; i++) fprintf(b->body, ",~{f%d}", i);
    }
    fputs("\"(", b->body);
    for (size_t i=0; i<v->nOperands*2; i++) {
        Value slot = operands[i % v->nOperands];
        fprintf(b->body, "%sptr elementtype(%s) %s", i ? ", " : "", irType(b, slot.kind), slot.operand);
    }
    fputs(")\n", b->body);
    free(operands);
}

// The native driver invokes host Clang; selectors follow that same host.
static void assemblyStatement(Backend *b, struct stmt_asm *v) {
    const char *architecture = "", *platform = "";
#if defined(__x86_64__) || defined(_M_X64)
    architecture = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    architecture = "aarch64";
#elif defined(__riscv) && __riscv_xlen == 64
    architecture = "riscv64";
#endif
#if defined(__linux__)
    platform = "linux";
#elif defined(__APPLE__) && defined(__MACH__)
    platform = "macos";
#elif defined(_WIN32)
    platform = "windows";
#endif
    if (strcmp(v->architecture.lexeme, architecture) ||
        (v->platform.type != EOF_TOKEN && strcmp(v->platform.lexeme, platform))) return;
    emitAssembly(b, v, architecture);
}

static void statementImpl(Backend *b, stmt *s) {
    if (!s || (b->terminated && s->type != STMT_LABEL))
        return;
    switch (s->type) {
    case STMT_ASM: assemblyStatement(b, s->value); break;
    case STMT_LABEL: {
        struct stmt_label *node = s->value;
        JumpTarget *target = jumpTarget(b->jumps, node->name.lexeme);
        if (!target) { fail(b, "missing label target"); break; }
        jump(b, target->label);
        label(b, target->label);
        statement(b, node->body);
        break;
    }
    case STMT_GOTO: {
        JumpTarget *target = jumpTarget(b->jumps, ((struct stmt_branch *)s->value)->label.lexeme);
        if (!target) { fail(b, "goto refers to an undefined label"); break; }
        for (Scope *scope = b->scope; scope && scope->jumpRegion != target->scope; scope = scope->parent)
            restoreStack(b, scope);
        jump(b, target->label);
        break;
    }
    case STMT_NULL:
    case STMT_TYPE:
    case STMT_INTERFACE:
    case STMT_ENUM:
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
        int runtimeArray = sizedArrayTypeParts(v->type, &element, &dimension);
        if (runtimeArray && arrayHasRuntimeDimension(v->type)) {
            Kind kind = type(b, v->type);
            for (size_t i = 0; i < v->nVars; i++) {
                if (v->vars[i].type == UNDERSCORE) continue;
                Value storage = parameterArrayValue(b, v->type, kind, v->nInitValues ? &v->initValues[i] : NULL);
                Value binding = slot(b, kind);
                bind(b, v->vars[i].lexeme, binding);
                b->scope->bindings->autoRelease = 1;
                escapeDynamicArrays(b, storage); rawStore(b, binding, storage);
            }
            break;
        }

        if ((runtimeArray || bufferTypeParts(v->type, &element, &dimension)) && bufferDimensionKind(dimension) == 2) {
            expr *size = parseBufferDimension(dimension);
            if (!size) {
                fail(b, "invalid runtime buffer size");
                break;
            }
            Value count = expression(b, size);
            free(size);
            if (!integral(b, count.kind)) {
                fail(b, "runtime buffer size must be an integer");
                break;
            }
            count = convert(b, count, INT);
            Kind elementKind = type(b, element), pointerKind = runtimeArray ? runtimeArrayKind(b, elementKind) : pointerBufferKind(b, elementKind);
            for (size_t i = 0; i < v->nVars; i++) {
                if (v->vars[i].type == UNDERSCORE)
                    continue;
                Value storage = temp(b, pointerKind);
                fprintf(b->body, "  %s = alloca %s, i64 %s\n", storage.operand,
                        irType(b, elementKind), count.operand);
                Value binding = slot(b, pointerKind);
                binding.length = count.operand;
                bind(b, v->vars[i].lexeme, binding);
                store(b, binding, storage);
                Value cursor = slot(b, INT); rawStore(b, cursor, value(b, INT, "0"));
                unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
                jump(b, condition); label(b, condition);
                Value index = load(b, cursor), more = temp(b, BOOL);
                fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, index.operand, count.operand);
                branch(b, more, body, end); label(b, body);
                Value item = temp(b, elementKind);
                fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", item.operand, irType(b, elementKind), storage.operand, index.operand);
                rawStore(b, item, emptyValue(b, elementKind));
                Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, index.operand);
                rawStore(b, cursor, next); jump(b, condition); label(b, end);
                if (v->nInitValues) {
                    if (v->initValues[i].type != EXPR_TYPE_COMPOSITE) fail(b, "runtime collection initializer requires a literal");
                    else {
                        struct expr_composite *literal = v->initValues[i].underlying;
                        Value fits = temp(b, BOOL);
                        fprintf(b->body, "  %s = icmp uge i64 %s, %zu\n", fits.operand, count.operand, literal->nEntries);
                        unsigned good = ++b->label, bad = ++b->label;
                        branch(b, fits, good, bad); label(b, bad);
                        fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n"); b->nullChecksUsed = 1; b->terminated = 1; label(b, good);
                        for (size_t j = 0; j < literal->nEntries; j++) {
                            item = temp(b, elementKind);
                            fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %zu\n", item.operand, irType(b, elementKind), storage.operand, j);
                            store(b, item, expectedValue(b, &literal->values[j], elementKind));
                        }
                    }
                }
            }
            break;
        }
        Kind *expected = checked(sizeof(Kind) * v->nVars);
        for (size_t i = 0; i < v->nVars; i++)
            expected[i] = v->type.nTokens ? type(b, v->type) : BAD;
        ValueList values = evaluateValues(b, v->initValues, v->nInitValues, expected, v->nVars, 1);
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
            if (v->nInitValues && i < values.count) addr.length = initial[i].length;
            bind(b, v->vars[i].lexeme, addr);
            b->scope->bindings->autoRelease = containsDynamicArrays(b, k);
            Value init = v->nInitValues ? initial[i] : emptyValue(b, k);
            store(b, addr, init);
        }
        free(initial);
        break;
    }
    case STMT_PANIC:
    case STMT_RETURN_PANIC: {
        Value message = expression(b, s->value);
        BufferKind *messageType = bufferKind(b, message.kind);
        if (!messageType || messageType->shape != 4 || messageType->element != I8) {
            fail(b, "panic message must have string type");
            break;
        }
        Value data = arrayData(b, message), length = arrayBytes(b, message);
        token location = diagnosticLocation(b, s->location);
        Value filename = moduleString(b, location.filename);
        char *text = sourceSnippet(b, location);
        Value snippet = moduleString(b, text);
        free(text);
        b->boundsUsed = 1;
        fprintf(b->body,
                "  call void @__ocean.%s(ptr %s, i64 %s, ptr %s, i64 %ld, i64 %ld, ptr %s)\n"
                "  unreachable\n", s->type == STMT_RETURN_PANIC ? "return_panic" : "panic",
                data.operand, length.operand, filename.operand, location.line, location.column,
                snippet.operand);
        b->terminated = 1;
        break;
    }
    case STMT_RETURN: {
        struct stmt_return *v = s->value;
        Kind k = resultType(b, b->function);
        Kind *expected = checked(sizeof(Kind) * (b->function->nResults + 1));
        for (size_t i = 0; i < b->function->nResults; i++)
            expected[i] = type(b, b->function->resultTypes[i]);
        ValueList values =
            evaluateValues(b, v->values, v->nValues, expected, b->function->nResults, v->nValues > 1);
        Value r = value(b, VOID, "");
        if (b->function->nResults > 1) {
            r = value(b, k, "zeroinitializer");
            for (size_t i = 0; i < b->function->nResults; i++) {
                Value item = i < values.count ? values.items[i]
                             : !v->nValues && b->function->resultNames[i].type != EOF_TOKEN
                                 ? load(b, find(b, b->function->resultNames[i].lexeme))
                                 : value(b, BAD, "0");
                r = insert(b, r, item, i);
            }
        } else if (values.count)
            r = values.items[0];
        else if (k != VOID && b->function->resultNames[0].type != EOF_TOKEN)
            r = load(b, find(b, b->function->resultNames[0].lexeme));
        free(expected);
        free(values.items);
        if (r.kind == NULL_VALUE || implicitVoidPointer(b, r.kind, k)) r = coerce(b, r, k);
        if ((integral(b, r.kind) && integral(b, k)) || (floating(b, r.kind) && floating(b, k)))
            r = convert(b, r, k);
        if (r.kind != k)
            fail(b, "return value type mismatch");
        if (ownsArrays(b, k)) {
            b->copyTransport = 1;
            r = copyValue(b, r, 0);
            b->copyTransport = 0;
        }
        escapeDynamicArrays(b, r);
        releaseScopeValues(b);
        cleanDynamicArrayTemps(b, b->functionTemps);
        fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr null)\n", boxRegion(b).operand);
        if (k == VOID)
            fprintf(b->body, "  ret void\n");
        else
            fprintf(b->body, "  ret %s %s\n", irType(b, k), r.operand);
        b->terminated = 1;
        break;
    }
    case STMT_IF: {
        struct stmt_if *v = s->value;
        Scope scope = {.parent = b->scope, .jumpRegion = v};
        b->scope = &scope;
        saveStack(b, &scope, statementAllocatesStack(s));
        statement(b, v->preStmt);
        DynamicArrayTemporary *conditionTemps = dynamicArrayTempMark(b);
        Value cond = expression(b, v->condition);
        cleanDynamicArrayTemps(b, conditionTemps);
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
        BufferKind *rangeType = v->range ? bufferKind(b, expressionKind(b, v->range)) : NULL;
        if (v->range && (!rangeType || (rangeType->shape != 4 && !rangeType->fixedArray))) {
            fail(b, "range requires an array");
            break;
        }
        Scope scope = {.parent = b->scope, .jumpRegion = v};
        b->scope = &scope;
        saveStack(b, &scope, statementAllocatesStack(s));
        Scope *oldLoopScope = b->loopScope;
        b->loopScope = &scope;
        unsigned condition = ++b->label, body = ++b->label, update = ++b->label, end = ++b->label;
        unsigned oldBreak = b->breakLabel, oldContinue = b->continueLabel;
        Scope *oldBreakScope = b->breakScope;
        b->breakScope = &scope;
        b->breakLabel = end;
        b->continueLabel = update;
        for (JumpTarget *target = b->jumps->targets; target; target = target->next)
            if (target->body == s) {
                target->loopScope = &scope;
                target->breakLabel = end;
                target->continueLabel = update;
            }
        statement(b, v->initializer);
        Value range = {0}, index = {0};
        if (v->range) {
            // Evaluate the iterable before introducing its loop bindings.
            range = expression(b, v->range);
            index = slot(b, INT);
            store(b, index, value(b, INT, "0"));
        }
        jump(b, v->deferCondition ? body : condition);
        label(b, condition);
        if (v->range) {
            Value i = load(b, index), length = arrayLength(b, range), more = temp(b, BOOL);
            fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, length.operand);
            branch(b, more, body, end);
        } else if (v->condition) {
            DynamicArrayTemporary *conditionTemps = dynamicArrayTempMark(b);
            Value c = expression(b, v->condition);
            cleanDynamicArrayTemps(b, conditionTemps);
            branch(b, c, body, end);
        } else
            jump(b, body);
        label(b, body);
        Scope bodyScope = {.parent = b->scope, .jumpRegion = v->body};
        b->scope = &bodyScope;
        saveStack(b, &bodyScope, v->range || statementAllocatesStack(v->body));
        if (v->range) {
            // The visible binding is a per-iteration value, not the loop counter.
            Value binding = slot(b, INT);
            store(b, binding, load(b, index));
            bind(b, v->bindings[0].lexeme, binding);
        }
        if (v->range && v->nBindings == 2) {
            Value item = slot(b, rangeType->element);
            Value i = load(b, index);
            checkArrayIndex(b, range, i, v->range->location);
            store(b, item, load(b, elementAddress(b, range, i)));
            bind(b, v->bindings[1].lexeme, item);
            b->scope->bindings->autoRelease = 1;
        }
        statement(b, v->body);
        if (!b->terminated)
            restoreStack(b, &bodyScope);
        b->scope = bodyScope.parent;
        clearScope(&bodyScope);
        jump(b, update);
        label(b, update);
        if (v->range) {
            Value i = load(b, index), next = temp(b, INT);
            fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
            store(b, index, next);
        }
        if (v->update) {
            DynamicArrayTemporary *updateTemps = dynamicArrayTempMark(b);
            expression(b, v->update);
            cleanDynamicArrayTemps(b, updateTemps);
        }
        jump(b, condition);
        label(b, end);
        b->breakScope = oldBreakScope;
        b->breakLabel = oldBreak;
        b->continueLabel = oldContinue;
        b->loopScope = oldLoopScope;
        for (JumpTarget *target = b->jumps->targets; target; target = target->next)
            if (target->body == s) target->loopScope = NULL;
        restoreStack(b, &scope);
        b->scope = scope.parent;
        clearScope(&scope);
        break;
    }
    case STMT_SWITCH: {
        struct stmt_switch *v = s->value;

        Scope scope = {.parent = b->scope, .jumpRegion = v};
        b->scope = &scope;
        saveStack(b, &scope, 1);
        statement(b, &v->initializer);
        Value box;
        if (v->isTypeSwitch) {
            struct expr_assertion *guard = v->value->underlying;
            box = boxScalar(b, expression(b, &guard->child), 0);
        } else box = v->value ? expression(b, v->value) : value(b, BOOL, "true");
        unsigned end = ++b->label, oldBreak = b->breakLabel;
        Scope *oldBreakScope = b->breakScope;
        b->breakLabel = end;
        b->breakScope = &scope;
        for (JumpTarget *target = b->jumps->targets; target; target = target->next)
            if (target->body == s) {
                target->loopScope = &scope;
                target->breakLabel = end;
            }
        unsigned *bodies = checked(sizeof(unsigned) * (v->nCases + 1));
        unsigned fallback = end;
        int exhaustive = 0;
        for (size_t i = 0; i < v->nCases; i++) {
            bodies[i] = ++b->label;
            if (!(v->isTypeSwitch ? v->cases[i].nTypes : v->cases[i].nValues)) fallback = bodies[i];
        }
        for (size_t i = 0; i < v->nCases; i++) {
            for (size_t j = 0; j < (v->isTypeSwitch ? v->cases[i].nTypes : v->cases[i].nValues); j++) {
                unsigned next = ++b->label;
                Value matches;
                if (v->isTypeSwitch) {
                    Kind pattern = constraintKind(b, v->cases[i].types[j]);
                    if (universalConstraint(b, pattern)) exhaustive = 1;
                    matches = testConstraint(b, box, pattern, 0);
                } else matches = binary(b, EXPR_EQUALITY, box, expression(b, &v->cases[i].values[j]));
                branch(b, matches, bodies[i], next);
                label(b, next);
            }
        }
        jump(b, fallback);
        int reachesEnd = fallback == end && !exhaustive;
        for (size_t i = 0; i < v->nCases; i++) {
            label(b, bodies[i]);
            Scope clause = {.parent = b->scope};
            b->scope = &clause;
            saveStack(b, &clause, 1);
            if (v->isTypeSwitch && v->binding.type != EOF_TOKEN && strcmp(v->binding.lexeme, "_")) {
                Kind kind = SCALAR_ANY;
                if (v->cases[i].nTypes == 1 &&
                    concreteConstraint(b, constraintKind(b, v->cases[i].types[0])))
                {
                    typeTokens span = v->cases[i].types[0];
                    if (span.tokens[0].type == TILDE) { span.tokens++; span.nTokens--; }
                    kind = type(b, span);
                }
                Value bound;
                Kind pattern = v->cases[i].nTypes == 1
                                   ? constraintKind(b, v->cases[i].types[0]) : SCALAR_ANY;
                BufferKind *family = bufferKind(b, pattern);
                int underlyingMatch = family && family->shape == 14;
                if (underlyingMatch) family = bufferKind(b, family->element);
                if (family && family->shape == 16)
                    kind = runtimeArrayKind(b, concreteConstraint(b, family->element) ? family->element : SCALAR_ANY);
                if (family && family->shape == 4 && !concreteConstraint(b, family->element))
                    kind = constrainedArrayKind(b, family->element, family->count != 0);
                if (family && family->shape == 9 && concreteConstraint(b, family->element)) {
                    // A typed family case exposes a view into the boxed snapshot.
                    typeTokens span = v->cases[i].types[0];
                    if (span.tokens[0].type == TILDE) { span.tokens++; span.nTokens--; }
                    kind = type(b, span);
                    Value small = testConstraint(b, box, pattern, 1);
                    Value bits = temp(b, INT), indirect = temp(b, kind), local = slot(b, INT);
                    fprintf(b->body, "  %s = extractvalue { ptr, i64 } %s, 1\n", bits.operand, box.operand);
                    rawStore(b, local, bits);
                    fprintf(b->body, "  %s = inttoptr i64 %s to ptr\n", indirect.operand, bits.operand);
                    bound = temp(b, kind);
                    fprintf(b->body, "  %s = select i1 %s, ptr %s, ptr %s\n",
                            bound.operand, small.operand, local.operand, indirect.operand);
                } else bound = kind == SCALAR_ANY ? box : underlyingMatch ? unboxMatched(b, box, kind) : unboxValue(b, box, kind);
                Value address = slot(b, kind);
                store(b, address, bound);
                bind(b, v->binding.lexeme, address);
                b->scope->bindings->autoRelease = containsDynamicArrays(b, kind);
            }
            struct stmt_block *body = &v->cases[i].body;
            int falls = body->nStmts && body->stmts[body->nStmts - 1].type == STMT_FALLTHROUGH;
            blockLimit(b, body, body->nStmts - falls);
            if (!b->terminated) {
                restoreStack(b, &clause);
                if (falls && i + 1 < v->nCases) jump(b, bodies[i + 1]);
                else { reachesEnd = 1; jump(b, end); }
            }
            b->scope = clause.parent;
            clearScope(&clause);
        }
        label(b, end);
        b->breakLabel = oldBreak;
        b->breakScope = oldBreakScope;
        for (JumpTarget *target = b->jumps->targets; target; target = target->next)
            if (target->body == s) target->loopScope = NULL;
        if (!reachesEnd && !scope.hasBreak) {
            fprintf(b->body, "  unreachable\n");
            b->terminated = 1;
        } else restoreStack(b, &scope);
        b->scope = scope.parent;
        clearScope(&scope);
        free(bodies);
        break;
    }
    case STMT_BREAK:
    case STMT_CONTINUE: {
        struct stmt_branch *v = s->value;
        unsigned target = s->type == STMT_BREAK ? b->breakLabel : b->continueLabel;
        if (v->label.type != EOF_TOKEN) {
            JumpTarget *loop = jumpTarget(b->jumps, v->label.lexeme);
            if (!loop || !loop->loopScope || (s->type == STMT_CONTINUE && !loop->continueLabel)) {
                fail(b, "break requires an enclosing loop or switch; continue requires an enclosing loop");
                break;
            }
            if (s->type == STMT_BREAK) loop->loopScope->hasBreak = 1;
            for (Scope *scope = b->scope; scope && scope != loop->loopScope; scope = scope->parent)
                restoreStack(b, scope);
            jump(b, s->type == STMT_BREAK ? loop->breakLabel : loop->continueLabel);
        } else if (!target)
            fail(b, "invalid break/continue");
        else {
            Scope *stop = s->type == STMT_BREAK ? b->breakScope : b->loopScope;
            if (s->type == STMT_BREAK && stop) stop->hasBreak = 1;
            for (Scope *scope = b->scope; scope && scope != stop; scope = scope->parent)
                restoreStack(b, scope);
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
    Kind result = resultType(b, f);
    fprintf(out, "%s %s ", f->isExtern ? "declare" : ownsArrays(b, result) ? "define internal" : "define",
            irType(b, result));
    if (b->method) methodName(out, b->method);
    else functionName(out, f);
    fprintf(out, "(");
    int hasReceiver = b->method && f->receiverType.nTokens;
    if (hasReceiver) fprintf(out, "%s %%receiver", irType(b, b->method->receiver));
    for (size_t i = 0; i < f->nParams; i++)
        fprintf(out, "%s%s %%arg%zu", (i || hasReceiver) ? ", " : "",
                irType(b, parameterKind(b, f, i)), i);
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
        "define internal void @__ocean.runtime_diagnostic(ptr %%file, i64 "
        "%%line, i64 %%column, ptr %%snippet) noreturn {\n"
        "entry:\n"
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
    Value panicPrefix = moduleString(b, "Panic: ");
    Value panicLocation = moduleString(b, "\n  --> %s:%lld:%lld\n");
    fprintf(b->out,
            "declare i64 @\"\\01write\"(i32, ptr, i64)\n"
            "define internal void @__ocean.panic(ptr %%message, i64 %%size, ptr %%file, i64 %%line, "
            "i64 %%column, ptr %%snippet) noreturn {\nentry:\n"
            "  call i64 @\"\\01write\"(i32 2, ptr %s, i64 7)\n"
            "  call i64 @\"\\01write\"(i32 2, ptr %%message, i64 %%size)\n"
            "  call i32 (i32, ptr, ...) @dprintf(i32 2, ptr %s, ptr %%file, i64 %%line, i64 %%column)\n"
            "  call void @__ocean.runtime_diagnostic(ptr %%file, i64 %%line, i64 %%column, ptr %%snippet)\n"
            "  unreachable\n}\n"
            "define internal void @__ocean.runtime_error(ptr %%format, i64 %%index, i64 %%length, "
            "ptr %%file, i64 %%line, i64 %%column, ptr %%snippet) noreturn {\nentry:\n"
            "  call i32 (i32, ptr, ...) @dprintf(i32 2, ptr %%format, i64 %%index, i64 %%length, "
            "ptr %%file, i64 %%line, i64 %%column)\n"
            "  call void @__ocean.runtime_diagnostic(ptr %%file, i64 %%line, i64 %%column, ptr %%snippet)\n"
            "  unreachable\n}\n", panicPrefix.operand, panicLocation.operand);
    fprintf(b->out,
            "define internal void @__ocean.return_panic(ptr %%message, i64 %%size, ptr %%file, "
            "i64 %%line, i64 %%column, ptr %%snippet) noreturn {\nentry:\n"
            "  %%initial = load ptr, ptr @__ocean.trace_head\n"
            "  %%present = icmp ne ptr %%initial, null\n"
            "  br i1 %%present, label %%parent, label %%fallback\n"
            "parent:\n"
            "  %%initial.previous = load ptr, ptr %%initial\n"
            "  %%has.caller = icmp ne ptr %%initial.previous, null\n"
            "  br i1 %%has.caller, label %%select, label %%fallback\n"
            "select:\n"
            "  %%head = phi ptr [ %%initial, %%parent ], [ %%previous, %%advance ]\n"
            "  %%previous = load ptr, ptr %%head\n"
            "  %%forward.slot = getelementptr %%__ocean.trace, ptr %%head, i32 0, i32 6\n"
            "  %%forward = load i1, ptr %%forward.slot\n"
            "  br i1 %%forward, label %%advance, label %%caller\n"
            "advance:\n"
            "  %%grandparent = load ptr, ptr %%previous\n"
            "  %%can.forward = icmp ne ptr %%grandparent, null\n"
            "  br i1 %%can.forward, label %%select, label %%caller\n"
            "caller:\n"
            "  %%file.slot = getelementptr %%__ocean.trace, ptr %%head, i32 0, i32 2\n"
            "  %%caller.file = load ptr, ptr %%file.slot\n"
            "  %%line.slot = getelementptr %%__ocean.trace, ptr %%head, i32 0, i32 3\n"
            "  %%caller.line = load i64, ptr %%line.slot\n"
            "  %%column.slot = getelementptr %%__ocean.trace, ptr %%head, i32 0, i32 4\n"
            "  %%caller.column = load i64, ptr %%column.slot\n"
            "  %%snippet.slot = getelementptr %%__ocean.trace, ptr %%head, i32 0, i32 5\n"
            "  %%caller.snippet = load ptr, ptr %%snippet.slot\n"
            "  call void @__ocean.panic(ptr %%message, i64 %%size, ptr %%caller.file, "
            "i64 %%caller.line, i64 %%caller.column, ptr %%caller.snippet)\n"
            "  unreachable\n"
            "fallback:\n"
            "  call void @__ocean.panic(ptr %%message, i64 %%size, ptr %%file, "
            "i64 %%line, i64 %%column, ptr %%snippet)\n"
            "  unreachable\n}\n");
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
    if (t && t->definition) {
        BufferKind *underlying = bufferKind(b, t->underlying);
        if (!underlying) return 1;
        return validateLayout(b, underlying);
    }
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
    b->functionHasAsm = 0;
    b->boxRegion = (Value){0};
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
    Scope scope = {.parent = b->globalScope};
    b->scope = &scope;
    b->function = f;
    b->terminated = 0;
    b->functionTemps = dynamicArrayTempMark(b);
    if (b->method && f->receiverType.nTokens) {
        Value addr = slot(b, b->method->receiver);
        bind(b, f->receiverName.lexeme, addr);
        b->scope->bindings->autoRelease = 1;
        store(b, addr, value(b, b->method->receiver, "%%receiver"));
    }
    b->terminated = 0;
    b->breakScope = NULL;
    b->breakLabel = b->continueLabel = 0;
    for (size_t j = 0; j < f->nParams; j++) {
        Kind k = parameterKind(b, f, j);
        Value addr = slot(b, k);
        bind(b, f->paramNames[j].lexeme, addr);
        b->scope->bindings->autoRelease = 1;
        store(b, addr, value(b, k, "%%arg%zu", j));
    }
    for (size_t j = 0; j < f->nResults; j++)
        if (f->resultNames[j].type != EOF_TOKEN) {
            Kind k = type(b, f->resultTypes[j]);
            Value addr = slot(b, k);
            bind(b, f->resultNames[j].lexeme, addr);
            b->scope->bindings->autoRelease = 1;
            Value initial = emptyValue(b, k);
            store(b, addr, initial);
        }
    JumpContext jumps = {0};
    JumpContext *previousJumps = b->jumps;
    prepareJumps(b, &jumps, &f->body);
    b->jumps = &jumps;
    block(b, &f->body);
    b->jumps = previousJumps;
    clearJumps(&jumps);
    if (!b->terminated) {
        releaseScopeValues(b);
        cleanDynamicArrayTemps(b, b->functionTemps);
        fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr null)\n", boxRegion(b).operand);
        if (resultType(b, f) == VOID)
            fprintf(b->body, "  ret void\n");
        else {
            Kind kind = resultType(b, f);
            Value zero = emptyValue(b, kind);
            if (ownsArrays(b, kind)) {
                int previous = b->copyTransport; b->copyTransport = 1;
                zero = copyValue(b, zero, 0);
                b->copyTransport = previous;
                escapeDynamicArrays(b, zero);
            }
            fprintf(b->body, "  ret %s %s\n", irType(b, kind), zero.operand);
        }
    }
    size_t packageId = 1;
    for (size_t i=0;i<b->program->nSources;i++)
        if (f->name.filename && !strcmp(f->name.filename,b->program->sources[i].filename)) packageId=b->program->sources[i].packageId;
    fprintf(output,"; ocean-package: %zu\n",packageId);
    signature(b, f, output);
    if (b->functionHasAsm) fputs(" noinline noredzone", output);
    fprintf(output, " {\nentry:\n");
    if (copy(b->slots, output) || copy(b->body, output))
        fail(b, "could not write function IR");
    fprintf(output, "}\n");
    fclose(b->slots);
    fclose(b->body);
    clearScope(&scope);
    b->scope = NULL;
    b->function = NULL;
}
static int globalKindReady(Backend *b, expr *e) {
    if (!e || !e->underlying) return 1;
    switch (e->type) {
    case EXPR_TYPE_IDENTIFIER: {
        const char *name = ((struct expr_identifier *)e->underlying)->lexeme.lexeme;
        if (!strcmp(name, "true") || !strcmp(name, "false")) return 1;
        for (Binding *binding = b->globalScope->bindings; binding; binding = binding->next)
            if (!strcmp(name, binding->name)) return 1;
        return 0;
    }
    case EXPR_TYPE_DOT: return globalKindReady(b, &((struct expr_dot *)e->underlying)->parent);
    case EXPR_TYPE_INDEX: return globalKindReady(b, &((struct expr_index *)e->underlying)->parent);
    case EXPR_TYPE_UNARY: {
        struct expr_unary *v = e->underlying;
        return v->operation == UNARY_SIZEOF || globalKindReady(b, &v->child);
    }
    case EXPR_TYPE_BINARY: {
        struct expr_binary *v = e->underlying;
        if (v->operation >= EXPR_EQUALITY && v->operation <= EXPR_GREATER_EQUALS) return 1;
        return globalKindReady(b, &v->left) && globalKindReady(b, &v->right);
    }
    case EXPR_TYPE_TERNARY: {
        struct expr_ternary *v = e->underlying;
        return globalKindReady(b, &v->ifTrue) && globalKindReady(b, &v->ifFalse);
    }
    case EXPR_TYPE_CALL: {
        struct expr_call *v = e->underlying;
        typeTokens *arguments; size_t count;
        struct expr_dot *dot = methodCallee(&v->func, &arguments, &count);
        return !dot || globalKindReady(b, &dot->parent);
    }
    case EXPR_TYPE_COMPOSITE: {
        struct expr_composite *v = e->underlying;
        return v->literalType.nTokens || !v->inferredArray || !v->nEntries || globalKindReady(b, &v->values[0]);
    }
    default: return 1;
    }
}
static void emitGlobals(Backend *b, FILE *output) {
    size_t count = 0;
    int *done = checked(sizeof(int) * (b->program->nStatements + 1));
    // Declare explicit globals first, then infer forward references once their
    // types are known. Runtime initialization still follows declaration order.
    for (size_t pass = 0; pass <= b->program->nStatements + 1; pass++) {
      int pending = 0, progress = 0;
      for (size_t i = 0; i < b->program->nStatements; i++) {
        stmt *statement = &b->program->statements[i];
        if (statement->type != STMT_VAR) continue;
        struct stmt_var *v = statement->value;
        if (done[i]) continue;
        if (!v->type.nTokens) {
            int ready = pass != 0;
            for (size_t j = 0; j < v->nInitValues; j++)
                if (!globalKindReady(b, &v->initValues[j])) ready = 0;
            if (!ready) { pending++; continue; }
        }
        done[i] = 1; progress = 1; count++;
        Kind initializer = !v->type.nTokens && v->nInitValues ? expressionKind(b, &v->initValues[0]) : BAD;
        BufferKind *tuple = bufferKind(b, initializer);
        for (size_t j = 0; j < v->nVars; j++) {
            if (v->vars[j].type == UNDERSCORE) continue;
            Kind k = v->type.nTokens ? type(b, v->type)
                     : tuple && tuple->shape == 2 ? tuple->members[j]
                     : expressionKind(b, &v->initValues[v->nInitValues == 1 ? 0 : j]);
            if (k == BAD || k == VOID) { fail(b, "global requires a supported storage type"); continue; }
            Value address = value(b, k, "@\"ocean.global.%s\"", v->vars[j].lexeme);
            address.owner = b->globalScope;
            bind(b, v->vars[j].lexeme, address);
            fprintf(output, "%s = internal global %s zeroinitializer\n", address.operand, irType(b, k));
        }
      }
      if (!pending) break;
      if (!progress && pass) { fail(b, "cannot infer global types; add explicit types"); break; }
    }
    free(done);
    if (!count || b->errors) return;
    for (int destroy = 0; destroy < 2; destroy++) {
        b->body = tmpfile(); b->slots = tmpfile();
        if (!b->body || !b->slots) {
            if (b->body) fclose(b->body);
            if (b->slots) fclose(b->slots);
            fail(b, "cannot create global initializer buffers"); return;
        }
        b->boxRegion = (Value){0};
        b->terminated = 0;
        b->initializingGlobals = !destroy;
        Scope initializerScope = {.parent = b->globalScope};
        b->scope = &initializerScope;
        DynamicArrayTemporary *globalTemps = dynamicArrayTempMark(b);
        // Default all globals before running any user initializer.
        for (Binding *binding = b->globalScope->bindings; binding; binding = binding->next) {
            Value address = binding->address;
            if (destroy) {
                b->copyTransport = 3;
                if (ownsArrays(b, address.kind)) copyValue(b, load(b, address), 0);
            } else {
                b->copyTransport = 1;
                Value initial = emptyValue(b, address.kind);
                Value owned = copyValue(b, initial, 0);
                escapeDynamicArrays(b, owned);
                rawStore(b, address, owned);
            }
            b->copyTransport = 0;
        }
        if (!destroy)
          for (size_t i = 0; i < b->program->nStatements; i++) {
            stmt *s = &b->program->statements[i];
            if (s->type != STMT_VAR) continue;
            struct stmt_var *v = s->value;
            if (!v->nInitValues) continue;
            Kind *expected = checked(sizeof(Kind) * v->nVars);
            for (size_t j = 0; j < v->nVars; j++)
                expected[j] = v->vars[j].type == UNDERSCORE ? BAD : find(b, v->vars[j].lexeme).kind;
            ValueList values = evaluateValues(b, v->initValues, v->nInitValues, expected, v->nVars, 1);
            for (size_t j = 0; j < v->nVars && j < values.count; j++)
                if (v->vars[j].type != UNDERSCORE) store(b, find(b, v->vars[j].lexeme), values.items[j]);
            free(expected); free(values.items);
          }
        cleanDynamicArrayTemps(b, globalTemps);
        fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr null)\n", boxRegion(b).operand);
        fprintf(b->body, "  ret void\n");
        const char *name = destroy ? "destroy_globals" : "initialize_globals";
        fprintf(output, "define internal void @__ocean.%s() {\nentry:\n", name);
        if (copy(b->slots, output) || copy(b->body, output)) fail(b, "cannot emit global initializer");
        fprintf(output, "}\n"); fclose(b->slots); fclose(b->body);
        fprintf(output, "@llvm.global_%s = appending global [1 x { i32, ptr, ptr }] "
                        "[{ i32, ptr, ptr } { i32 65535, ptr @__ocean.%s, ptr null }]\n",
                destroy ? "dtors" : "ctors", name);
        clearScope(&initializerScope);
        b->scope = b->globalScope;
        b->initializingGlobals = 0;
    }
}
// Generate per-concrete-type operations so dynamic any copies retain value semantics.
static void emitAnyType(Backend *b, Kind kind) {
    BufferKind *t = bufferKind(b, kind);
    Value metadata = anyMetadata(b, kind);
    int aggregate = t && (t->shape == 4 || t->shape == 5 || !t->shape || (t->definition && representationKind(b, kind) == SCALAR_ANY));
    if (!aggregate) {
        b->ffiUsed = 1;
        fprintf(b->out, "%s = private constant { ptr, ptr, ptr } { ptr %s, ptr null, ptr null }\n",
                metadata.operand, ffiType(b, representationKind(b, kind) == F32 ? FLOAT : representationKind(b, kind) == BOOL || representationKind(b, kind) == I8 || representationKind(b, kind) == I16 || representationKind(b, kind) == U8 || representationKind(b, kind) == U16 ? I32 : kind));
        return;
    }
    b->returnTransportUsed = 1;
    b->nullChecksUsed = 1;
    fprintf(b->out, "%s = private constant { ptr, ptr, ptr } { ptr null, ptr @__ocean.any.clone.%u, ptr @__ocean.any.destroy.%u }\n",
            metadata.operand, (unsigned)kind, (unsigned)kind);
    Scope scope = {0};
    b->scope = &scope;
    Value size = storageSize(b, kind);
    // Arrays have runtime extent, and aggregates owning arrays need deep storage.
    int owns = ownsArrays(b, kind);
    for (int operation = 0; operation < 3; operation++) {
        b->body = tmpfile(); b->slots = tmpfile();
        b->terminated = 0; b->copyTransport = operation == 2 ? 3 : 1;
        if (operation == 0) {
            fprintf(b->out, "define internal { ptr, i64 } @__ocean.any.box.%u(ptr %%input) {\nentry:\n", (unsigned)kind);
            Value source = load(b, value(b, kind, "%%input"));
            Value bitsSlot = slot(b, INT);
            rawStore(b, bitsSlot, value(b, INT, "0"));
            Value indirect = temp(b, BOOL);
            if (owns) fprintf(b->body, "  %s = icmp eq i1 true, true\n", indirect.operand);
            else fprintf(b->body, "  %s = icmp ugt i64 %s, 8\n", indirect.operand, size.operand);
            unsigned big = ++b->label, small = ++b->label, done = ++b->label;
            branch(b, indirect, big, small); label(b, big);
            Value copied = copyValue(b, source, 0), pointer;
            escapeDynamicArrays(b, copied);
            if (t->shape == 4) pointer = copied;
            else {
                pointer = temp(b, RAW_POINTER);
                fprintf(b->body, "  %s = call ptr @\"\\01malloc\"(i64 %s)\n", pointer.operand, size.operand);
                Value present = temp(b, BOOL);
                fprintf(b->body, "  %s = icmp ne ptr %s, null\n", present.operand, pointer.operand);
                unsigned good = ++b->label, bad = ++b->label;
                branch(b, present, good, bad); label(b, bad);
                fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
                b->terminated = 1; label(b, good);
                rawStore(b, value(b, kind, "%s", pointer.operand), copied);
            }
            Value bits = temp(b, INT);
            fprintf(b->body, "  %s = ptrtoint ptr %s to i64\n", bits.operand, pointer.operand);
            rawStore(b, bitsSlot, bits); jump(b, done);
            label(b, small);
            // This branch is unreachable for runtime-sized values.
            if (!owns) fprintf(b->body, "  store %s %s, ptr %s\n", irType(b, kind), source.operand, bitsSlot.operand);
            jump(b, done); label(b, done);
            Value tag = temp(b, SCALAR_ANY), result = temp(b, SCALAR_ANY), bitsValue = load(b, bitsSlot);
            fprintf(b->body, "  %s = insertvalue { ptr, i64 } poison, ptr %s, 0\n", tag.operand, metadata.operand);
            fprintf(b->body, "  %s = insertvalue { ptr, i64 } %s, i64 %s, 1\n", result.operand, tag.operand, bitsValue.operand);
            fprintf(b->body, "  ret { ptr, i64 } %s\n", result.operand);
        } else if (operation == 1) {
            fprintf(b->out, "define internal %s @__ocean.any.unbox.%u(i64 %%bits) {\nentry:\n", irType(b, kind), (unsigned)kind);
            Value storage = slot(b, INT); rawStore(b, storage, value(b, INT, "%%bits"));
            Value indirect = temp(b, BOOL);
            if (owns) fprintf(b->body, "  %s = icmp eq i1 true, true\n", indirect.operand);
            else fprintf(b->body, "  %s = icmp ugt i64 %s, 8\n", indirect.operand, size.operand);
            unsigned big = ++b->label, small = ++b->label;
            branch(b, indirect, big, small); label(b, big);
            Value pointer = temp(b, RAW_POINTER);
            fprintf(b->body, "  %s = inttoptr i64 %%bits to ptr\n", pointer.operand);
            Value result = t->shape == 4 ? value(b, kind, "%s", pointer.operand)
                                       : load(b, value(b, kind, "%s", pointer.operand));
            fprintf(b->body, "  ret %s %s\n", irType(b, kind), result.operand);
            b->terminated = 1; label(b, small);
            if (owns) fprintf(b->body, "  unreachable\n");
            else {
                result = load(b, value(b, kind, "%s", storage.operand));
                fprintf(b->body, "  ret %s %s\n", irType(b, kind), result.operand);
            }
        } else {
            fprintf(b->out, "define internal void @__ocean.any.destroy.%u(i64 %%bits) {\nentry:\n", (unsigned)kind);
            Value indirect = temp(b, BOOL);
            if (owns) fprintf(b->body, "  %s = icmp eq i1 true, true\n", indirect.operand);
            else fprintf(b->body, "  %s = icmp ugt i64 %s, 8\n", indirect.operand, size.operand);
            unsigned big = ++b->label, done = ++b->label;
            branch(b, indirect, big, done); label(b, big);
            Value pointer = temp(b, RAW_POINTER);
            fprintf(b->body, "  %s = inttoptr i64 %%bits to ptr\n", pointer.operand);
            Value source = t->shape == 4 ? value(b, kind, "%s", pointer.operand)
                                       : load(b, value(b, kind, "%s", pointer.operand));
            copyValue(b, source, 0);
            if (t->shape != 4) fprintf(b->body, "  call void @\"\\01free\"(ptr %s)\n", pointer.operand);
            jump(b, done); label(b, done); fprintf(b->body, "  ret void\n");
        }
        copy(b->slots, b->out); copy(b->body, b->out); fprintf(b->out, "}\n");
        fclose(b->slots); fclose(b->body);
    }
    fprintf(b->out,
        "define internal i64 @__ocean.any.clone.%u(i64 %%bits) {\nentry:\n"
        "  %%value = call %s @__ocean.any.unbox.%u(i64 %%bits)\n"
        "  %%slot = alloca %s\n"
        "  store %s %%value, ptr %%slot\n"
        "  %%box = call { ptr, i64 } @__ocean.any.box.%u(ptr %%slot)\n"
        "  %%result = extractvalue { ptr, i64 } %%box, 1\n"
        "  ret i64 %%result\n}\n",
        (unsigned)kind, irType(b, kind), (unsigned)kind, irType(b, kind), irType(b, kind), (unsigned)kind);
    b->copyTransport = 0; b->scope = NULL;
}
// Only trusted reflect wrappers can call these internal entry points. Dispatch
// uses the existing concrete any tags; no raw layout API is exposed to Ocean.
static void emitReflection(Backend *b) {
    if (!b->reflectionUsed) return;
    for (BufferKind *t = b->buffers; t; t = t->next)
        if (t->shape == 5 && t->record) anyMetadata(b, t->kind);
    FILE *previousBody = b->body, *previousSlots = b->slots;
    Scope *previousScope = b->scope;
    struct stmt_func *previousFunction = b->function;
    Value previousRegion = b->boxRegion;
    int previousTransport = b->copyTransport;
    b->function = NULL; b->copyTransport = 0;
    if (b->reflectionFieldsKind) {
        BufferKind *array = bufferKind(b, b->reflectionFieldsKind);
        BufferKind *field = array ? bufferKind(b, array->element) : NULL;
        if (!array || !array->fixedArray || array->shape != 4 || !field || field->shape != 5 || !field->record || field->count != 2 ||
            strcmp(field->record->fieldNames[0].lexeme, "Name") || strcmp(field->record->fieldNames[1].lexeme, "Value") ||
            !bufferKind(b, field->members[0]) || !bufferKind(b, field->members[0])->immutableString || field->members[1] != SCALAR_ANY) {
            fail(b, "standard reflect.Field must contain Name string and Value any, in that order");
            b->function = previousFunction; b->copyTransport = previousTransport;
            return;
        }
        Scope scope = {.parent = b->globalScope}; b->scope = &scope;
        b->body = tmpfile(); b->slots = tmpfile(); b->boxRegion = (Value){0}; b->terminated = 0;
        DynamicArrayTemporary *mark = dynamicArrayTempMark(b);
        Value input = value(b, SCALAR_ANY, "%%input"), tag = temp(b, RAW_POINTER);
        fprintf(b->body, "  %s = extractvalue { ptr, i64 } %%input, 0\n", tag.operand);
        // All concrete struct specializations are registered above, including
        // named types and structs only encountered while reflecting a field.
        for (AnyType *a = b->anyTypes; a; a = a->next) {
            BufferKind *record = bufferKind(b, a->kind);
            if (!record || record->shape != 5 || !record->record || record->record->enumStorage) continue;
            Value matches = temp(b, BOOL);
            fprintf(b->body, "  %s = icmp eq ptr %s, @__ocean.any.type.%u\n", matches.operand, tag.operand, (unsigned)a->kind);
            unsigned body = ++b->label, next = ++b->label;
            branch(b, matches, body, next); label(b, body);
            Value source = unboxMatched(b, input, a->kind);
            Value result = allocateCollection(b, b->reflectionFieldsKind, value(b, INT, "%zu", record->count));
            for (size_t i = 0; i < record->count; i++) {
                const char *name = record->record->fieldNames[i].lexeme;
                size_t length = strlen(name);
                Value nameValue = allocateCollection(b, field->members[0], value(b, INT, "%zu", length));
                Value data = arrayData(b, nameValue), text = moduleString(b, name);
                fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %zu, i1 false)\n", data.operand, text.operand, length);
                b->dataCopyUsed = 1;
                Value descriptor = emptyValue(b, field->kind);
                descriptor = insert(b, descriptor, nameValue, 0);
                descriptor = insert(b, descriptor, boxScalar(b, extract(b, source, i), 0), 1);
                store(b, elementAddress(b, result, value(b, INT, "%zu", i)), descriptor);
            }
            b->copyTransport = 1;
            Value returned = copyValue(b, result, 0);
            b->copyTransport = 0; escapeDynamicArrays(b, returned);
            cleanDynamicArrayTemps(b, mark);
            fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr null)\n", boxRegion(b).operand);
            fprintf(b->body, "  ret ptr %s\n", returned.operand); b->terminated = 1;
            label(b, next);
        }
        // Non-structs have no fields. Pointers are intentionally not followed.
        Value result = allocateCollection(b, b->reflectionFieldsKind, value(b, INT, "0"));
        b->copyTransport = 1; Value returned = copyValue(b, result, 0); b->copyTransport = 0;
        escapeDynamicArrays(b, returned); cleanDynamicArrayTemps(b, mark);
        fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr null)\n", boxRegion(b).operand);
        fprintf(b->body, "  ret ptr %s\n", returned.operand);
        fprintf(b->out, "define internal ptr @__ocean.reflect.fields({ ptr, i64 } %%input) {\nentry:\n");
        if (copy(b->slots, b->out) || copy(b->body, b->out)) fail(b, "could not emit reflection fields");
        fprintf(b->out, "}\n"); fclose(b->slots); fclose(b->body);
        clearScope(&scope);
    }
    fprintf(b->out, "define internal i1 @__ocean.reflect.is_struct({ ptr, i64 } %%input) {\nentry:\n  %%tag = extractvalue { ptr, i64 } %%input, 0\n");
    unsigned count = 0;
    for (AnyType *a = b->anyTypes; a; a = a->next) {
        BufferKind *record = bufferKind(b, a->kind);
        if (!record || record->shape != 5 || !record->record || record->record->enumStorage) continue;
        fprintf(b->out, "  %%m%u = icmp eq ptr %%tag, @__ocean.any.type.%u\n", count, (unsigned)a->kind);
        if (count) fprintf(b->out, "  %%r%u = or i1 %s%u, %%m%u\n", count, count == 1 ? "%m" : "%r", count - 1, count);
        count++;
    }
    if (count) fprintf(b->out, "  ret i1 %s%u\n}\n", count == 1 ? "%m" : "%r", count - 1);
    else fprintf(b->out, "  ret i1 false\n}\n");
    b->body = previousBody; b->slots = previousSlots; b->scope = previousScope;
    b->function = previousFunction; b->boxRegion = previousRegion; b->copyTransport = previousTransport;
}
static void emitArrayView(Backend *b) {
    if (!b->arrayViewUsed) return;
    b->body = tmpfile(); b->slots = tmpfile();
    Scope scope = {0}; b->scope = &scope;
    b->boxRegion = (Value){0}; b->copyTransport = 0; b->terminated = 0;
    Value tag = temp(b, RAW_POINTER), bits = temp(b, INT);
    fprintf(b->body, "  %s = extractvalue { ptr, i64 } %%box, 0\n", tag.operand);
    fprintf(b->body, "  %s = extractvalue { ptr, i64 } %%box, 1\n", bits.operand);
    // Boxing nested array elements can discover more concrete array types.
    for (;;) {
        AnyType *entry = NULL;
        for (AnyType *a = b->anyTypes; a; a = a->next) {
            BufferKind *t = bufferKind(b, a->kind);
            if (!a->arrayEmitted && t && (t->shape == 4 || t->fixedArray)) { entry = a; break; }
        }
        if (!entry) break;
        entry->arrayEmitted = 1;
        Kind kind = entry->kind;
        BufferKind *t = bufferKind(b, kind);
        Value matches = temp(b, BOOL);
        unsigned good = ++b->label, next = ++b->label;
        fprintf(b->body, "  %s = icmp eq ptr %s, @__ocean.any.type.%u\n", matches.operand, tag.operand, (unsigned)kind);
        branch(b, matches, good, next); label(b, good);
        Value source = temp(b, kind);
        if (!t->shape)
            fprintf(b->body, "  %s = call %s @__ocean.any.unbox.%u(i64 %s)\n", source.operand, t->ir, (unsigned)kind, bits.operand);
        else fprintf(b->body, "  %s = inttoptr i64 %s to ptr\n", source.operand, bits.operand);
        Value view = t->element == SCALAR_ANY && t->shape == 4 ? source : boxArray(b, source);
        int previous = b->copyTransport;
        b->copyTransport = 1;
        Value heap = copyValue(b, view, 0);
        escapeDynamicArrays(b, heap);
        b->copyTransport = previous;
        if (b->boxRegion.operand)
            fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr null)\n", b->boxRegion.operand);
        fprintf(b->body, "  ret ptr %s\n", heap.operand); b->terminated = 1;
        label(b, next);
    }
    fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
    b->nullChecksUsed = 1;
    fprintf(b->out, "define internal ptr @__ocean.any.array({ ptr, i64 } %%box) {\nentry:\n");
    if (copy(b->slots, b->out) || copy(b->body, b->out)) fail(b, "could not emit array view");
    fprintf(b->out, "}\n");
    fclose(b->slots); fclose(b->body); b->scope = NULL; b->boxRegion = (Value){0};
}
static void emitHomogeneousRuntime(Backend *b) {
    if (!b->homogeneousUsed) return;
    b->nullChecksUsed = 1;
    fputs(
        "define internal void @__ocean.array.check_write(ptr %array, { ptr, i64 } %value) {\n"
        "entry:\n  %bytes = load i64, ptr %array\n"
        "  %empty = icmp eq i64 %bytes, 0\n"
        "  br i1 %empty, label %done, label %check\n"
        "check:\n  %first = getelementptr { i64, [0 x { ptr, i64 }] }, ptr %array, i32 0, i32 1\n"
        "  %expected = load ptr, ptr %first\n"
        "  %actual = extractvalue { ptr, i64 } %value, 0\n"
        "  %same = icmp eq ptr %expected, %actual\n"
        "  br i1 %same, label %done, label %bad\n"
        "bad:\n  call void @llvm.trap()\n  unreachable\n"
        "done:\n  ret void\n}\n"
        "define internal void @__ocean.array.check(ptr %array) {\n"
        "entry:\n  %bytes = load i64, ptr %array\n"
        "  %count = sdiv i64 %bytes, 16\n"
        "  %data = getelementptr { i64, [0 x { ptr, i64 }] }, ptr %array, i32 0, i32 1\n"
        "  br label %loop\n"
        "loop:\n  %i = phi i64 [ 1, %entry ], [ %next, %body ]\n"
        "  %more = icmp slt i64 %i, %count\n"
        "  br i1 %more, label %body, label %done\n"
        "body:\n  %slot = getelementptr { ptr, i64 }, ptr %data, i64 %i\n"
        "  %value = load { ptr, i64 }, ptr %slot\n"
        "  call void @__ocean.array.check_write(ptr %array, { ptr, i64 } %value)\n"
        "  %next = add i64 %i, 1\n  br label %loop\n"
        "done:\n  ret void\n}\n", b->out);
}
static void emitAnyRuntime(Backend *b) {
    // Scope cleanup calls are emitted even in functions which never box a value.
    b->returnTransportUsed = 1;
    b->nullChecksUsed = 1;
    for (;;) {
        AnyType *entry = NULL;
        for (AnyType *t = b->anyTypes; t; t = t->next) if (!t->emitted) { entry = t; break; }
        if (!entry) break;
        entry->emitted = 1;
        emitAnyType(b, entry->kind);
    }
    fputs(
        "define internal { ptr, i64 } @__ocean.any.copy({ ptr, i64 } %box) {\nentry:\n"
        "  %tag = extractvalue { ptr, i64 } %box, 0\n"
        "  %slot = getelementptr { ptr, ptr, ptr }, ptr %tag, i32 0, i32 1\n"
        "  %clone = load ptr, ptr %slot\n"
        "  %owned = icmp ne ptr %clone, null\n"
        "  br i1 %owned, label %copy, label %scalar\n"
        "copy:\n"
        "  %bits = extractvalue { ptr, i64 } %box, 1\n"
        "  %newbits = call i64 %clone(i64 %bits)\n"
        "  %result = insertvalue { ptr, i64 } %box, i64 %newbits, 1\n"
        "  ret { ptr, i64 } %result\nscalar:\n  ret { ptr, i64 } %box\n}\n"
        "define internal void @__ocean.any.drop({ ptr, i64 } %box) {\nentry:\n"
        "  %tag = extractvalue { ptr, i64 } %box, 0\n"
        "  %slot = getelementptr { ptr, ptr, ptr }, ptr %tag, i32 0, i32 2\n"
        "  %drop = load ptr, ptr %slot\n"
        "  %owned = icmp ne ptr %drop, null\n"
        "  br i1 %owned, label %destroy, label %done\n"
        "destroy:\n"
        "  %bits = extractvalue { ptr, i64 } %box, 1\n"
        "  call void %drop(i64 %bits)\n"
        "  br label %done\ndone:\n  ret void\n}\n"
        "define internal void @__ocean.any.track(ptr %region, { ptr, i64 } %box) {\nentry:\n"
        "  %tag = extractvalue { ptr, i64 } %box, 0\n"
        "  %slot = getelementptr { ptr, ptr, ptr }, ptr %tag, i32 0, i32 2\n"
        "  %drop = load ptr, ptr %slot\n"
        "  %owned = icmp ne ptr %drop, null\n"
        "  br i1 %owned, label %allocate, label %done\n"
        "allocate:\n"
        "  %node = call ptr @\"\\01malloc\"(i64 ptrtoint (ptr getelementptr ({ ptr, { ptr, i64 } }, ptr null, i64 1) to i64))\n"
        "  %ok = icmp ne ptr %node, null\n"
        "  br i1 %ok, label %link, label %bad\nbad:\n  call void @llvm.trap()\n  unreachable\n"
        "link:\n"
        "  %head = load ptr, ptr %region\n"
        "  store ptr %head, ptr %node\n"
        "  %value = getelementptr { ptr, { ptr, i64 } }, ptr %node, i32 0, i32 1\n"
        "  store { ptr, i64 } %box, ptr %value\n"
        "  store ptr %node, ptr %region\n"
        "  br label %done\ndone:\n  ret void\n}\n"
        "define internal void @__ocean.any.clean(ptr %region, ptr %stop) {\nentry:\n"
        "  %head = load ptr, ptr %region\n  br label %check\ncheck:\n"
        "  %node = phi ptr [%head, %entry], [%next, %body]\n"
        "  %more = icmp ne ptr %node, %stop\n"
        "  br i1 %more, label %body, label %done\nbody:\n"
        "  %next = load ptr, ptr %node\n"
        "  %value = getelementptr { ptr, { ptr, i64 } }, ptr %node, i32 0, i32 1\n"
        "  %box = load { ptr, i64 }, ptr %value\n"
        "  call void @__ocean.any.drop({ ptr, i64 } %box)\n"
        "  call void @\"\\01free\"(ptr %node)\n"
        "  br label %check\ndone:\n  store ptr %stop, ptr %region\n  ret void\n}\n", b->out);
}

static void emitFFIRuntime(Backend *b) {
    if (!b->ffiUsed) return;
    FILE *out = b->out;
    for (Kind k = VOID; k < BUFFER; k++)
        if (b->fixedCScalars & (1u << k))
            fprintf(out, "@__ocean.c.fixed.%u = private constant { ptr, ptr, ptr } { ptr %s, ptr null, ptr null }\n", (unsigned)k, ffiType(b, k));
    const char *names[] = {"void", "uint64", "uint32", "uint16", "uint8", "sint8", "sint16", "float", "sint32", "sint64", "double", "pointer"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        fprintf(out, "@ffi_type_%s = external global i8\n", names[i]);
    fputs("declare i32 @ffi_prep_cif_var(ptr, i32, i32, i32, ptr, ptr)\n"
          "declare void @ffi_call(ptr, ptr, ptr, ptr)\n"
          "define internal void @__ocean.ffi_call(ptr %fn, i32 %fixed, i64 %count, ptr %boxes, ptr %rtype, ptr %result) {\n"
          "entry:\n", out);
    fprintf(out, "  %%cif = alloca [%zu x i8], align %zu\n", sizeof(ffi_cif), _Alignof(ffi_cif));
    fputs("  %types = alloca ptr, i64 %count, align 8\n"
          "  %values = alloca ptr, i64 %count, align 8\n"
          "  br label %check\n"
          "check:\n"
          "  %i = phi i64 [0, %entry], [%next, %scalar.body]\n"
          "  %more = icmp ult i64 %i, %count\n"
          "  br i1 %more, label %body, label %call\n"
          "body:\n"
          "  %box = getelementptr { ptr, i64 }, ptr %boxes, i64 %i\n"
          "  %descriptor = load ptr, ptr %box\n"
          "  %type = load ptr, ptr %descriptor\n"
          "  %scalar = icmp ne ptr %type, null\n"
          "  br i1 %scalar, label %scalar.body, label %bad\n"
          "scalar.body:\n"
          "  %bits = getelementptr { ptr, i64 }, ptr %box, i32 0, i32 1\n"
          "  %tp = getelementptr ptr, ptr %types, i64 %i\n"
          "  %vp = getelementptr ptr, ptr %values, i64 %i\n"
          "  store ptr %type, ptr %tp\n"
          "  store ptr %bits, ptr %vp\n"
          "  %next = add i64 %i, 1\n"
          "  br label %check\n"
          "call:\n"
          "  %n = trunc i64 %count to i32\n", out);
    fprintf(out, "  %%status = call i32 @ffi_prep_cif_var(ptr %%cif, i32 %d, i32 %%fixed, i32 %%n, ptr %%rtype, ptr %%types)\n", FFI_DEFAULT_ABI);
    fputs("  %ok = icmp eq i32 %status, 0\n"
          "  br i1 %ok, label %invoke, label %bad\n"
          "bad:\n"
          "  call void @llvm.trap()\n"
          "  unreachable\n"
          "invoke:\n"
          "  call void @ffi_call(ptr %cif, ptr %fn, ptr %result, ptr %values)\n"
          "  ret void\n"
          "}\n", out);
}

/* Copy each argv string, including its terminator, into the stream os expects. */
static void emitOSArgs(FILE *out, struct stmt_func *hook) {
    fputs("  %args.count = zext i32 %argc to i64\n"
          "  br label %args.size\nargs.size:\n"
          "  %args.i = phi i64 [0, %entry], [%args.next, %args.size.done]\n"
          "  %args.total = phi i64 [0, %entry], [%args.total.next, %args.size.done]\n"
          "  %args.more = icmp ult i64 %args.i, %args.count\n"
          "  br i1 %args.more, label %args.size.start, label %args.allocate\n"
          "args.size.start:\n"
          "  %args.slot = getelementptr ptr, ptr %argv, i64 %args.i\n"
          "  %args.string = load ptr, ptr %args.slot\n"
          "  br label %args.size.byte\nargs.size.byte:\n"
          "  %args.len = phi i64 [0, %args.size.start], [%args.len.next, %args.size.byte]\n"
          "  %args.char.ptr = getelementptr i8, ptr %args.string, i64 %args.len\n"
          "  %args.char = load i8, ptr %args.char.ptr\n"
          "  %args.len.next = add i64 %args.len, 1\n"
          "  %args.end = icmp eq i8 %args.char, 0\n"
          "  br i1 %args.end, label %args.size.done, label %args.size.byte\n"
          "args.size.done:\n"
          "  %args.total.next = add i64 %args.total, %args.len.next\n"
          "  %args.next = add i64 %args.i, 1\n"
          "  br label %args.size\nargs.allocate:\n"
          "  %args.stream = alloca i8, i64 %args.total\n"
          "  br label %args.copy\nargs.copy:\n"
          "  %args.j = phi i64 [0, %args.allocate], [%args.j.next, %args.copy.done]\n"
          "  %args.offset = phi i64 [0, %args.allocate], [%args.offset.next, %args.copy.done]\n"
          "  %args.copy.more = icmp ult i64 %args.j, %args.count\n"
          "  br i1 %args.copy.more, label %args.copy.start, label %args.ready\n"
          "args.copy.start:\n"
          "  %args.copy.slot = getelementptr ptr, ptr %argv, i64 %args.j\n"
          "  %args.copy.string = load ptr, ptr %args.copy.slot\n"
          "  br label %args.copy.byte\nargs.copy.byte:\n"
          "  %args.k = phi i64 [0, %args.copy.start], [%args.k.next, %args.copy.byte]\n"
          "  %args.from = getelementptr i8, ptr %args.copy.string, i64 %args.k\n"
          "  %args.byte = load i8, ptr %args.from\n"
          "  %args.position = add i64 %args.offset, %args.k\n"
          "  %args.to = getelementptr i8, ptr %args.stream, i64 %args.position\n"
          "  store i8 %args.byte, ptr %args.to\n"
          "  %args.k.next = add i64 %args.k, 1\n"
          "  %args.copy.end = icmp eq i8 %args.byte, 0\n"
          "  br i1 %args.copy.end, label %args.copy.done, label %args.copy.byte\n"
          "args.copy.done:\n"
          "  %args.offset.next = add i64 %args.position, 1\n"
          "  %args.j.next = add i64 %args.j, 1\n"
          "  br label %args.copy\nargs.ready:\n"
          "  call void ", out);
    functionName(out, hook);
    fputs("(ptr %args.stream, i64 %args.count)\n", out);
}

int emitLLVM(parseResult *program, FILE *output) {
    Scope globals = {0};
    Backend b = {.program = program, .out = output, .globalScope = &globals, .scope = &globals};
    fprintf(output, "; Generated by ocean-c\n"
                    "@__ocean.null = private global i8 0\n"
                    "%%__ocean.trace = type { ptr, ptr, ptr, i64, i64, ptr, i1 }\n"
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
    if (!program->isLibrary && (!main || main->isExtern || main->nTypeParams || main->nParams || main->nResults > 1))
        fail(&b, "executable requires func main() or func main() int");
    else if (!program->isLibrary && main->nResults && resultType(&b, main) != INT)
        fail(&b, "main must return int or no value");
    // Non-generic declarations have one concrete layout even before first use.
    for (size_t i = 0; i < program->nStatements; i++) {
        stmt *s = &program->statements[i];
        if (s->type == STMT_STRUCT && !((struct stmt_struct *)s->value)->nTypeParams)
            recordKind(&b, s->value, NULL, 0);
    }
    b.globalOutput = output;
    emitGlobals(&b, output);
    for (size_t i = 0; i < program->nStatements; i++) {
        stmt *s = &program->statements[i];
        if (s->type == STMT_PACKAGE || s->type == STMT_NULL || s->type == STMT_STRUCT || s->type == STMT_INTERFACE || s->type == STMT_TYPE || s->type == STMT_ENUM || s->type == STMT_VAR)
            continue;
        if (s->type != STMT_FUNC) {
            fail(&b, "unsupported declaration at file scope");
            continue;
        }
        struct stmt_func *f = s->value;
        if (f->receiverType.nTokens) continue; // Emit concrete methods when called.
        if (f->nTypeParams) continue; // Emit explicit specializations when called.
        if (f->isExtern) {
            int duplicate = 0;
            const char *name = f->externalName ? f->externalName : f->name.lexeme;
            for (size_t j = 0; j < i; j++) {
                stmt *previous = &program->statements[j];
                if (previous->type != STMT_FUNC) continue;
                struct stmt_func *other = previous->value;
                const char *otherName = other->externalName ? other->externalName : other->name.lexeme;
                if (!other->isExtern || strcmp(name, otherName)) continue;
                duplicate = 1;
                if (f->nParams != other->nParams || f->isVariadic != other->isVariadic ||
                    strcmp(irType(&b, resultType(&b, f)), irType(&b, resultType(&b, other))))
                    fail(&b, "conflicting extern declarations across packages");
                else for (size_t k = 0; k < f->nParams; k++)
                    if (strcmp(irType(&b, parameterKind(&b, f, k)), irType(&b, parameterKind(&b, other, k))))
                        fail(&b, "conflicting extern declarations across packages");
                break;
            }
            if (duplicate) continue;
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
    if (!program->isLibrary && main && !main->isExtern && !main->nParams && main->nResults <= 1) {
        b.body = tmpfile();
        b.slots = tmpfile();
        b.function = main;
        if (!b.body || !b.slots)
            fail(&b, "could not create entry point buffers");
        else {
            if (program->osSetArgs) emitOSArgs(b.body, program->osSetArgs);
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
            fprintf(output, "define i32 @main(%s) {\nentry:\n", program->osSetArgs ? "i32 %argc, ptr %argv" : "");
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
    FILE *anyRuntime = tmpfile();
    b.out = anyRuntime;
    emitReflection(&b);
    emitArrayView(&b);
    emitHomogeneousRuntime(&b);
    for (TypeConstraint *c = b.constraints; c; c = c->next) {
        fprintf(b.out, "define internal i1 @__ocean.constraint.%u(ptr %%tag) {\nentry:\n", c->id);
        if (universalConstraint(&b, c->pattern)) {
            fprintf(b.out, "  ret i1 true\n}\n");
            continue;
        }
        unsigned n = 0;
        for (AnyType *a = b.anyTypes; a; a = a->next) {
            if (!matchesConstraint(&b, c->pattern, a->kind)) continue;
            BufferKind *actual = bufferKind(&b, a->kind);
            if (c->inlineBuffer) {
                if (!actual || actual->shape != 0 || ownsArrays(&b, a->kind)) continue;
                fprintf(b.out, "  %%tag%u = icmp eq ptr %%tag, @__ocean.any.type.%u\n", n, (unsigned)a->kind);
                fprintf(b.out, "  %%size%u = icmp ule i64 ptrtoint (ptr getelementptr (%s, ptr null, i32 1) to i64), 8\n",
                        n, irType(&b, a->kind));
                fprintf(b.out, "  %%m%u = and i1 %%tag%u, %%size%u\n", n, n, n);
            } else fprintf(b.out, "  %%m%u = icmp eq ptr %%tag, @__ocean.any.type.%u\n", n, (unsigned)a->kind);
            if (n) fprintf(b.out, "  %%r%u = or i1 %s%u, %%m%u\n", n, n == 1 ? "%m" : "%r", n - 1, n);
            n++;
        }
        if (n) fprintf(b.out, "  ret i1 %s%u\n}\n", n == 1 ? "%m" : "%r", n - 1);
        else fprintf(b.out, "  ret i1 false\n}\n");
    }
    emitAnyRuntime(&b);
    b.out = output;
    for (BufferKind *t = b.buffers; t; t = t->next) {
        if (t->shape != 5 || t->definition) continue;
        validateLayout(&b, t);
        fprintf(output, "%s = type { ", t->ir);
        // A byte-size header must determine array length even for empty records.
        if (!t->count) fprintf(output, "i8");
        for (size_t i = 0; i < t->count; i++)
            fprintf(output, "%s%s", i ? ", " : "", irType(&b, t->members[i]));
        fprintf(output, " }\n");
    }
    if (copy(anyRuntime, output)) fail(&b, "could not write any runtime");
    fclose(anyRuntime);
    if (copy(functions, output)) fail(&b, "could not write module functions");
    fclose(functions);
    if (b.stringComparisonUsed)
        fprintf(output, "declare i32 @\"\\01memcmp\"(ptr, ptr, i64)\n");
    if (b.dataCopyUsed)
        fprintf(output, "declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n");
    if (b.newUsed) fprintf(output, "declare ptr @\"\\01calloc\"(i64, i64)\n");
    if (b.returnTransportUsed) {
        // Distinct IR names share the libc symbols without conflicting with user declarations.
        fprintf(output, "declare ptr @\"\\01malloc\"(i64)\n"
                        "declare void @\"\\01free\"(ptr)\n");
    }
    emitFFIRuntime(&b);
    if (b.nullChecksUsed || b.ffiUsed) fprintf(output, "declare void @llvm.trap() cold noreturn nounwind\n");
    emitBoundsRuntime(&b);
    if (fflush(output) || ferror(output))
        fail(&b, "could not write LLVM IR");
    clearScope(&globals);
    while (b.dynamicArrayTemps) {
        DynamicArrayTemporary *next = b.dynamicArrayTemps->next;
        free(b.dynamicArrayTemps); b.dynamicArrayTemps = next;
    }
    while (b.diagnostics) {
        BackendDiagnostic *next = b.diagnostics->next;
        free(b.diagnostics); b.diagnostics = next;
    }
    while (b.constraints) {
        TypeConstraint *next = b.constraints->next;
        free(b.constraints); b.constraints = next;
    }
    while (b.anyTypes) {
        AnyType *next = b.anyTypes->next;
        free(b.anyTypes); b.anyTypes = next;
    }
    while (b.declaredKinds) {
        DeclaredKind *next = b.declaredKinds->next;
        free(b.declaredKinds); b.declaredKinds = next;
    }
    while (b.buffers) {
        BufferKind *next = b.buffers->next;
        if (!b.buffers->definition) {
            free(b.buffers->arguments);
            free(b.buffers->members);
        }
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
    while (b.operands) {
        OperandText *next = b.operands->next;
        free(b.operands->text);
        free(b.operands);
        b.operands = next;
    }
    return b.errors ? 1 : 0;
}
/* Stage beside the destination so a failed compiler/linker preserves old output.
 * exec arguments are passed directly; paths never become shell commands. */
static int compileLLVMMode(const char *irPath, const char *outputPath, int object, const char *objectPath) {
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
    // Ordinary programs retain their existing link requirements. Only modules
    // carrying runtime scalar metadata need the libffi runtime.
    int needsFFI = 0;
    FILE *input = fopen(irPath, "r");
    if (input) {
        char line[1024];
        while (fgets(line, sizeof(line), input))
            if (strstr(line, "@ffi_type_")) { needsFFI = 1; break; }
        fclose(input);
    }
    pid_t child = fork();
    if (child < 0) {
        perror("could not start clang");
        unlink(staged);
        free(staged);
        return 1;
    }
    if (!child) {
        if (objectPath) {
            if (needsFFI) execlp("clang", "clang", "-Wno-override-module", objectPath, "-o", staged, OCEAN_SYSTEM_LINK_LIBRARIES "-lffi", (char *)NULL);
            else execlp("clang", "clang", "-Wno-override-module", objectPath, "-o", staged, OCEAN_SYSTEM_LINK_LIBRARIES (char *)NULL);
        } else if (object)
            execlp("clang", "clang", "-Wno-override-module", "-c", "-x", "ir", irPath, "-o", staged, (char *)NULL);
        else if (needsFFI)
            execlp("clang", "clang", "-Wno-override-module", "-x", "ir", irPath, "-o", staged, OCEAN_SYSTEM_LINK_LIBRARIES "-lffi", (char *)NULL);
        else
            execlp("clang", "clang", "-Wno-override-module", "-x", "ir", irPath, "-o", staged, OCEAN_SYSTEM_LINK_LIBRARIES (char *)NULL);
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

int compileLLVM(const char *irPath, const char *outputPath) {
    return compileLLVMMode(irPath, outputPath, 0, NULL);
}
int compileLLVMObject(const char *irPath, const char *outputPath) {
    return compileLLVMMode(irPath, outputPath, 1, NULL);
}

int linkLLVMObject(const char *objectPath, const char *irPath, const char *outputPath) {
    return compileLLVMMode(irPath, outputPath, 0, objectPath);
}

int checkBackend(parseResult *program) {
    char directory[] = "/tmp/ocean-check-XXXXXX";
    if (!mkdtemp(directory)) { perror("ocean check: temporary directory"); return 1; }
    char irPath[128], output[128];
    snprintf(irPath,sizeof(irPath),"%s/check.ll",directory);
    snprintf(output,sizeof(output),"%s/check.out",directory);
    FILE *ir=fopen(irPath,"w"); int failed=!ir;
    if(ir){failed=emitLLVM(program,ir);if(fclose(ir))failed=1;}
    if(!failed){
        failed=program->isLibrary?compileLLVMObject(irPath,output):compileLLVM(irPath,output);
        if(failed)diagnostic(program->sourceText,(token){.filename=(char *)program->sourceFilename,.line=1,.column=1},"backend","error","LLVM compilation or linking failed during check");
    }
    unlink(irPath);unlink(output);rmdir(directory);return failed;
}

int linkLLVMObjects(const char **objects, size_t count, const char *irPath, const char *output, int library) {
    char *staged=checked(strlen(output)+16);sprintf(staged,"%s.XXXXXX",output);
    int fd=mkstemp(staged);if(fd<0){perror("could not create output");free(staged);return 1;}close(fd);
    int ffi=0;FILE *ir=fopen(irPath,"r");char line[1024];if(ir){while(fgets(line,sizeof(line),ir))if(strstr(line,"@ffi_type_"))ffi=1;fclose(ir);}
    char **args=checked(sizeof(char*)*(count+12));size_t n=0;
    args[n++]="clang";args[n++]="-Wno-override-module";
    if(library){args[n++]="-r";args[n++]="-nostdlib";args[n++]="-no-pie";}
    for(size_t i=0;i<count;i++)args[n++]=(char*)objects[i];
    args[n++]="-o";args[n++]=staged;if(ffi&&!library)args[n++]="-lffi";
#if defined(_WIN32)
    if(!library)args[n++]="-lkernel32";
#endif
    args[n]=NULL;
    pid_t child=fork();if(!child){execvp(args[0],args);perror("could not execute clang");_exit(127);}
    int status=0,failed=child<0;
    if(child>0){while(waitpid(child,&status,0)<0)if(errno!=EINTR){failed=1;break;}if(!WIFEXITED(status)||WEXITSTATUS(status))failed=1;}
    if(failed)fprintf(stderr,"backend error: clang failed to link package objects\n");
    else if(rename(staged,output)){perror("could not save output");failed=1;}
    unlink(staged);free(staged);free(args);return failed;
}
