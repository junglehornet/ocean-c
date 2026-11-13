#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "backend.h"
#include "build_cache.h"
#include "runtime_threads_source.h"
#include "runtime_process_source.h"
#include <ctype.h>
#include "diagnostic.h"
#include "array_type.h"
#include "buffer_type.h"
#include "value_list.h"
#include <errno.h>
#include <ffi.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int backendWasm(void) {
    const char *arch = getenv("OCARCH");
    return arch && !strcmp(arch, "wasm");
}
int validateBackendTarget(void) {
    const char *os = getenv("OCOS"), *arch = getenv("OCARCH");
    if ((os && !strcmp(os, "js")) || backendWasm()) {
        if (!os || strcmp(os, "js") || !arch || strcmp(arch, "wasm")) {
            fprintf(stderr, "backend error: WebAssembly requires OCOS=js and OCARCH=wasm together\n");
            return 1;
        }
    } else if ((os && *os) || (arch && *arch)) {
        fprintf(stderr, "backend error: target overrides currently support only OCOS=js OCARCH=wasm\n");
        return 1;
    }
    return 0;
}
static void emitTarget(FILE *out) {
    if (backendWasm()) fputs("target triple = \"wasm32-unknown-unknown\"\n", out);
}

#ifdef _WIN32
// Keep the backend's private kind names separate from Win32 typedefs/macros.
#undef VOID
#define VOID OCEAN_KIND_VOID
#define INT OCEAN_KIND_INT
#define FLOAT OCEAN_KIND_FLOAT
#define BOOL OCEAN_KIND_BOOL
#define UINT OCEAN_KIND_UINT
#endif
typedef enum { VOID, INT, I32, FLOAT, BOOL, RAW_POINTER, I8, BAD, NULL_VALUE, SCALAR_ANY, I16, F32, UINT, U32, U16, U8, BUFFER } Kind;
typedef struct {
    Kind kind;
    const char *operand; // Backend-owned text; aggregate constants can be arbitrarily long.
    struct Scope *owner;
    int ownedBox; // A map bucket owns its box outside lexical temporary regions.
    int closureCell; // Shared capture storage, owned by the closure runtime.
    int throughPointer;
    int homogeneousDynamic;
    const char *homogeneousArray; // Indexed writes retain the array element identity.
    Kind elementRequirement;
    Kind backingKind; // A .data argument retains its source through a deferred call.
    const char *backingOperand;
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
    int map;
    int mapEntry;
    Kind mapKey, mapValue, mapKeyRequirement, mapValueRequirement;
    struct stmt_type *definition;
    Kind underlying;
    Kind requirement; // Element interface constraint for boxed arrays.
    struct stmt_interface *contract;
    int shape; // 0: fixed buffer, 2: result tuple, 3: raw buffer pointer, 4:
               // indirect array value; 5: user struct; 6: typed pointer
    struct stmt_struct *record; // Shape 5: concrete user struct; type arguments are compiler-only.
    Kind *arguments;
    size_t nArguments;
    struct stmt_func *callable; // Shape 17: Ocean function signature.
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
    struct stmt_func *interfaceMethod; // Non-null for a boxed dispatch adapter.
    struct stmt_func *function;
    Kind record, receiver;
    Kind *arguments;
    size_t nArguments, nBindings;
    TypeBinding *bindings;
    unsigned id;
    struct MethodInstance *next;
} MethodInstance;
typedef struct ClosureInstance {
    struct stmt_func *function;
    Scope *lexicalScope;
    Kind kind;
    Kind *captures;
    char *captureLengths;
    TypeBinding *bindings;
    size_t nBindings;
    unsigned id;
    int adapter;
    MethodInstance *method, *typeContext;
    struct ClosureInstance *next;
} ClosureInstance;
typedef struct ClosureCellType {
    Kind kind;
    struct ClosureCellType *next;
} ClosureCellType;
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
typedef struct EqualityKind {
    Kind left, right;
    int emitted;
    struct EqualityKind *next;
} EqualityKind;
typedef struct AnyType {
    Kind kind;
    struct AnyType *next;
    int emitted, arrayEmitted, reflectionPrepared, reflectable;
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
typedef struct FunctionUse {
    struct stmt_func *function;
    int emitted;
    struct FunctionUse *next;
} FunctionUse;
typedef struct NativeName {
    char *from, *to;
    struct NativeName *next;
} NativeName;
typedef struct {
    llvmPackages *units;
    FILE **packageBodies;
    hashmap *nativeNames;
    NativeName *nativeNameStorage;
    int declarationOnly;
    FunctionUse *functions;
    OperandText *operands;
    AnyType *anyTypes;
    EqualityKind *equalities;
    int boxedEqualityUsed;
    int mapHashUsed;
    TypeConstraint *constraints;
    unsigned nConstraints;
    int arrayViewUsed, homogeneousUsed;
    int reflectionUsed, reflectionVariantUsed;
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
    FILE *out, *body, *slots, *globalOutput, *deferredOutput;
    int initializingGlobals;
    Scope *scope;
    Scope *functionScope;
    Scope *globalScope;
    struct stmt_func *function;
    unsigned next, label, strings;
    unsigned breakLabel, continueLabel;
    int terminated, errors;
    int functionHasAsm;
    ClosureInstance *closures, *lastClosure, *closure;
    ClosureCellType *closureCells;
    unsigned nClosures;
    int closuresUsed;
    int threadsUsed;
    expr *deferCall;
    unsigned nDefers;
    Value deferFrame;
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
// LLVM's \01 escape suppresses target mangling while keeping runtime imports
// distinct from user FFI declarations. Mach-O therefore needs an explicit '_'.
static const char *libcSymbolForTarget(Backend *b, const char *name, int darwin) {
    return value(b, RAW_POINTER, "@\"\\01%s%s\"", darwin ? "_" : "", name).operand;
}
static const char *libcSymbol(Backend *b, const char *name) {
#ifdef __APPLE__
    return libcSymbolForTarget(b, name, !backendWasm());
#else
    return libcSymbolForTarget(b, name, 0);
#endif
}
static void nativeName(Backend *, const char *, const char *);
static const char *nativeKindIdentity(Backend *, Kind);
static void nativeDeferredIdentity(Backend *, unsigned);
static void nativeConstantIdentity(Backend *, const char *, const void *, size_t);
static void finishNativeModules(Backend *, FILE *);
static Value temp(Backend *b, Kind k) { return value(b, k, "%%v%u", ++b->next); }
static Value aggregateEquality(Backend *, Value, Value);
static Value boxedEquality(Backend *, enum binaryExprType, Value, Value);
static Value specializedClosureValue(Backend *, expr *, int);
static Kind closureKind(Backend *, struct stmt_func *);
static Value closureParts(Backend *, Value, int);
static void requireClosureCode(Backend *, Value, token);
static Value closureValue(Backend *, struct stmt_func *, MethodInstance *, int);
static Value localSlot(Backend *, Kind, const char *);
static BufferKind *closureCalleeKind(Backend *, expr *);
static Binding *lookupBinding(Backend *b, const char *name) {
    for (Scope *scope = b->scope; scope; scope = scope->parent)
        for (Binding *binding = scope->bindings; binding; binding = binding->next)
            if (binding->name && !strcmp(binding->name, name)) return binding;
    return NULL;
}
static int copy(FILE *, FILE *);
static Value scheduleIntrinsicDefer(Backend *, expr *);
static void retainDeferredPointee(Backend *, Value);
static void scheduleDefer(Backend *, struct stmt_func *, MethodInstance *, Value, Value *, size_t, Kind, size_t, Kind *, expr *, Value);
static Value moduleString(Backend *b, const char *text) {
    unsigned id = ++b->strings;
    size_t n = strlen(text);
    nativeConstantIdentity(b, value(b, RAW_POINTER, "@.str%u", id).operand, text, n + 1);
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
        if (!v->definition && !v->mapEntry && v->shape == shape && v->element == element && v->count == count &&
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
        if (v->dynamicArray && !v->map && !v->immutableString && !v->definition && v->element == element) return v->kind;
    Kind base = arrayValueKind(b, element);
    BufferKind *v = checked(sizeof(*v)); *v = *bufferKind(b, base);
    v->kind = (Kind)(BUFFER + b->nBuffers++); v->dynamicArray = 1;
    v->ir = strdup(v->ir); v->layout = strdup("{ i64, i64, ptr }");
    v->next = b->buffers; b->buffers = v;
    return v->kind;
}
static Kind mapKind(Backend *b, Kind key, Kind item, Kind kr, Kind vr) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (v->map && !v->definition && v->mapKey == key && v->mapValue == item &&
            v->mapKeyRequirement == kr && v->mapValueRequirement == vr) return v->kind;
    Kind members[] = {BOOL, key, item};
    Kind tuple = structKind(b, 2, VOID, members, 3);
    BufferKind *bucket = checked(sizeof(*bucket)); *bucket = *bufferKind(b, tuple);
    bucket->kind = (Kind)(BUFFER + b->nBuffers++); bucket->mapEntry = 1;
    bucket->members = checked(sizeof(members)); memcpy(bucket->members, members, sizeof(members));
    bucket->ir = strdup(bucket->ir); bucket->zeroOperand = NULL;
    bucket->next = b->buffers; b->buffers = bucket;
    Kind entry = bucket->kind;
    Kind base = dynamicArrayKind(b, entry);
    BufferKind *v = checked(sizeof(*v)); *v = *bufferKind(b, base);
    v->kind = (Kind)(BUFFER + b->nBuffers++); v->map = 1;
    v->mapKey = key; v->mapValue = item; v->mapKeyRequirement = kr; v->mapValueRequirement = vr;
    v->ir = strdup(v->ir); v->layout = strdup(v->layout);
    v->zeroOperand = NULL; v->emptyEmitted = 0;
    v->next = b->buffers; b->buffers = v; return v->kind;
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
        if (!v->definition && !v->dynamicArray && v->ir && v->shape == 4 && v->count == 1 && !v->requirement && v->element == SCALAR_ANY) return v->kind;
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
        if (!v->definition && !v->dynamicArray && v->ir && v->shape == 4 && v->count == (size_t)mixed && v->requirement == requirement) return v->kind;
    BufferKind *base = bufferKind(b, arrayValueKind(b, SCALAR_ANY));
    BufferKind *v = checked(sizeof(*v)); *v = *base;
    v->kind = (Kind)(BUFFER + b->nBuffers++); v->count = mixed; v->requirement = requirement;
    v->ir = strdup(base->ir); v->layout = strdup(base->layout);
    v->zeroOperand = NULL; v->emptyEmitted = 0;
    v->next = b->buffers; b->buffers = v;
    return v->kind;
}
// Mutable interface arrays share constraints with variadic views, but own a
// dynamic header and expose the ordinary mutation methods.
static Kind dynamicConstrainedArrayKind(Backend *b, Kind requirement, int mixed) {
    for (BufferKind *v = b->buffers; v; v = v->next)
        if (!v->definition && v->dynamicArray && !v->map && !v->immutableString &&
            v->element == SCALAR_ANY && v->requirement == (requirement == SCALAR_ANY ? VOID : requirement) && v->count == (size_t)mixed)
            return v->kind;
    Kind base = constrainedArrayKind(b, requirement, mixed);
    BufferKind *v = checked(sizeof(*v)); *v = *bufferKind(b, base);
    v->kind = (Kind)(BUFFER + b->nBuffers++); v->dynamicArray = 1;
    v->ir = strdup(v->ir); v->layout = strdup("{ i64, i64, ptr }");
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
static int validateOwnership(Backend *, Kind, Kind *, unsigned);
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
    if (!b->recordDepth) {
        Kind path[64];
        if (!validateLayout(b, v)) return BAD;
        if (!validateOwnership(b, v->kind, path, 0)) return v->kind;
    }
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
    MethodInstance *method = owner == b->globalScope ? NULL : b->closure ? b->closure->typeContext : b->method;
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
static size_t typeListEnd(typeTokens span, size_t start) {
    size_t nesting = 0;
    for (size_t i = start; i < span.nTokens; i++) {
        enum tokenType k = span.tokens[i].type;
        if (!nesting && k == BITWISE_OR) return i;
        if (k == LESS_THAN || k == LEFT_BRACKET || k == LEFT_PAREN || k == LEFT_CURLY_BRACE)
            nesting++;
        if ((k == GREATER_THAN || k == RIGHT_BRACKET || k == RIGHT_PAREN ||
             k == RIGHT_CURLY_BRACE) &&
            nesting)
            nesting--;
        if (k == BITSHIFT_RIGHT) nesting = nesting > 1 ? nesting - 2 : 0;
    }
    return span.nTokens;
}

static Kind constraintKind(Backend *b, typeTokens span) {
    if (span.nTokens >= 3 && span.tokens[0].type == INTERFACE) {
        struct stmt_interface *contract=parseInterfaceType(span);
        return contract ? interfacePattern(b,contract) : BAD;
    }
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
    typeTokens mapKey, mapValue;
    if (mapTypeParts(span, &mapKey, &mapValue)) return type(b, span);
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
    if (p->map || a->map) return p->map && a->map && pattern == actual;
    if (p->fixedArray && p->shape == 4)
        return a->fixedArray && matchesConstraint(b, p->element, a->element);
    if (p->shape == 4 && a->shape == 4) {
        // Concrete string and byte-array cases have distinct boxed identities.
        // Generic array constraints may still inspect a string's element type.
        if (p->immutableString != a->immutableString &&
            (p->immutableString || p->element != SCALAR_ANY)) return 0;
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
    if (t.tokens[0].type == FUNC) {
        struct stmt_func *signature = parseFunctionType(t);
        if (!signature) { fail(b, "invalid function type"); return BAD; }
        return closureKind(b, signature);
    }
    if (t.tokens[0].type == TILDE || typeListEnd(t, 0) < t.nTokens) return SCALAR_ANY;
    typeTokens mapKey, mapValue;
    if (mapTypeParts(t, &mapKey, &mapValue)) {
        Kind key = type(b, mapKey), item = type(b, mapValue);
        return mapKind(b, key, item, key == SCALAR_ANY ? constraintKind(b, mapKey) : VOID,
                       item == SCALAR_ANY ? constraintKind(b, mapValue) : VOID);
    }
    if (t.nTokens > 1 && t.tokens[t.nTokens - 1].type == STAR)
        return pointerKind(b, type(b, (typeTokens){t.nTokens - 1, t.tokens}));
    typeTokens arrayElement;
    if (arrayElementType(t, &arrayElement)) {
        Kind element = type(b, arrayElement);
        if (t.tokens[0].type == MIXED || element == SCALAR_ANY)
            return dynamicConstrainedArrayKind(b, constraintKind(b, arrayElement), t.tokens[0].type == MIXED);
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
    if (t.nTokens >= 3 && t.tokens[0].type == INTERFACE) return SCALAR_ANY;
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
static int identicalKinds(Backend *, Kind, Kind);
static int constraintSubset(Backend *b, Kind left, Kind right, unsigned depth) {
    if (depth > 32) return 0;
    BufferKind *a = bufferKind(b, left), *c = bufferKind(b, right);
    if (a && a->shape == 12 && a->contract->nConstraints)
        return constraintSubset(b, constraintKind(b, a->contract->constraints[0]), right,
                                depth + 1);
    if (c && c->shape == 12 && c->contract->nConstraints)
        return constraintSubset(b, left, constraintKind(b, c->contract->constraints[0]), depth + 1);
    if (a && a->shape == 8) {
        for (size_t i = 0; i < a->nArguments; i++)
            if (!constraintSubset(b, a->arguments[i], right, depth + 1)) return 0;
        return 1;
    }
    if (c && c->shape == 8) {
        for (size_t i = 0; i < c->nArguments; i++)
            if (constraintSubset(b, left, c->arguments[i], depth + 1)) return 1;
        return 0;
    }
    return identicalKinds(b, left, right);
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
    if (a->shape == 8 && c->shape == 8) {
        if (a->nArguments != c->nArguments) return 0;
        for (size_t i=0;i<a->nArguments;i++) {
            int found=0;
            for (size_t j=0;j<c->nArguments;j++) if (identicalKinds(b,a->arguments[i],c->arguments[j])) found=1;
            if (!found) return 0;
        }
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
            if (matchesConstraint(b, constraintKind(b, term), actual)) matches = 1;
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
        if (!provided || provided->nTypeParams != required->nTypeParams ||
            required->nParams != provided->nParams || required->nResults != provided->nResults ||
            required->isVariadic != provided->isVariadic) { accepted = 0; break; }
        TypeBinding *previous = b->typeBindings;
        TypeBinding *bindings = checked(sizeof(*bindings) * (provided->nReceiverTypeParams + 2*provided->nTypeParams + 1));
        for (size_t j = 0; j < provided->nReceiverTypeParams && j < owner->nArguments; j++) {
            bindings[j] = (TypeBinding){provided->receiverTypeParamNames[j].lexeme, owner->arguments[j], b->typeBindings};
            b->typeBindings = &bindings[j];
        }
        for (size_t j=0;j<provided->nTypeParams;j++) {
            Kind placeholder=constraintShape(b,17,VOID,j);
            size_t index=provided->nReceiverTypeParams+2*j;
            bindings[index]=(TypeBinding){provided->typeParamNames[j].lexeme,placeholder,b->typeBindings};
            b->typeBindings=&bindings[index];
            bindings[index+1]=(TypeBinding){required->typeParamNames[j].lexeme,placeholder,b->typeBindings};
            b->typeBindings=&bindings[index+1];
        }
        for (size_t j=0;accepted && j<provided->nTypeParams;j++)
            if (!(constraintSubset(b,constraintKind(b,required->typeParamConstraints[j]),constraintKind(b,provided->typeParamConstraints[j]),0) &&
                  constraintSubset(b,constraintKind(b,provided->typeParamConstraints[j]),constraintKind(b,required->typeParamConstraints[j]),0))) accepted=0;
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
static void requireFunction(Backend *b, struct stmt_func *f) {
    if (!f || f->isExtern || f->receiverType.nTokens || f->nTypeParams) return;
    for (FunctionUse *use = b->functions; use; use = use->next)
        if (use->function == f) return;
    FunctionUse *use = checked(sizeof(*use));
    *use = (FunctionUse){.function = f, .next = b->functions};
    b->functions = use;
}
static void functionName(Backend *b, FILE *out, struct stmt_func *f) {
    requireFunction(b, f);
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
// Dispatch adapters use the ordinary method ABI, including defer capture.
static MethodInstance *interfaceInstance(Backend *b, struct stmt_func *required,
                                        typeTokens *spans, size_t count) {
    Kind *arguments = checked(sizeof(Kind) * (count + 1));
    if (count != required->nTypeParams) {
        free(arguments); fail(b, "interface method type argument count mismatch"); return NULL;
    }
    for (size_t i=0;i<count;i++) arguments[i]=type(b,spans[i]);
    for (MethodInstance *m=b->methods;m;m=m->next)
        if (m->interfaceMethod==required && m->nArguments==count &&
            (!count || !memcmp(arguments,m->arguments,count*sizeof(Kind)))) {
            free(arguments); return m;
        }
    if (b->nMethods >= 256) {
        free(arguments); fail(b, "method specialization limit exceeded"); return NULL;
    }
    MethodInstance *m=checked(sizeof(*m));
    m->interfaceMethod=required;
    m->function=checked(sizeof(*m->function)); *m->function=*required;
    // A signature has no receiver syntax; borrow its name to mark the adapter ABI.
    m->function->receiverType=(typeTokens){1,&required->name};
    m->receiver=SCALAR_ANY; m->record=BAD; m->id=++b->nMethods;
    m->arguments=arguments; m->nArguments=m->nBindings=count;
    m->bindings=checked(sizeof(TypeBinding)*(count+1));
    for (size_t i=0;i<count;i++)
        m->bindings[i]=(TypeBinding){required->typeParamNames[i].lexeme,arguments[i],
                                    i+1<count ? &m->bindings[i+1] : NULL};
    if (b->lastMethod) b->lastMethod->next=m; else b->methods=m;
    b->lastMethod=m;
    return m;
}
static MethodInstance *dispatchTarget(Backend *b, MethodInstance *adapter, Kind kind) {
    struct stmt_interface contract={.nMethods=1,.methods=adapter->interfaceMethod};
    if (!matchesInterface(b,&contract,kind)) return NULL;
    TypeBinding *previous=b->typeBindings;
    b->typeBindings=adapter->nBindings ? adapter->bindings : NULL;
    typeTokens *spans=checked(sizeof(*spans)*(adapter->nArguments+1));
    for (size_t i=0;i<adapter->nArguments;i++)
        spans[i]=(typeTokens){1,&adapter->interfaceMethod->typeParamNames[i]};
    MethodInstance *target=methodInstance(b,kind,adapter->function->name.lexeme,spans,adapter->nArguments);
    free(spans); b->typeBindings=previous;
    return target;
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
    if (!identifier->externReference) return lookupBinding(b, identifier->lexeme.lexeme) ? NULL : findFunction(b, identifier->lexeme.lexeme);
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
// Mixed-width arithmetic uses 64-bit storage. Prefer signed storage when it
// can represent both operand ranges; semantics rejects unsafe signed/u64 pairs.
static Kind commonIntegerKind(Backend *b, Kind a, Kind c) {
    if (unsignedInteger(b, a) && unsignedInteger(b, c)) return UINT;
    if (!unsignedInteger(b, a) && !unsignedInteger(b, c)) return INT;
    Kind unsignedKind = unsignedInteger(b, a) ? a : c;
    return integerWidth(b, unsignedKind) < 64 ? INT : UINT;
}
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
        fprintf(b->body, "  call void @__ocean.%s.check_write(ptr %s, { ptr, i64 } %s)\n",
                address.homogeneousDynamic ? "array.dynamic" : "array", address.homogeneousArray, v.operand);
    }
    if (address.ownedBox) {
        int previous = b->copyTransport; b->copyTransport = 1;
        Value replacement = copyValue(b, v, 0);
        b->copyTransport = 3; copyValue(b, load(b, address), 0);
        b->copyTransport = previous;
        rawStore(b, address, replacement); return;
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
static void reflectionInput(Backend *b, Kind kind) {
    // An any field/element keeps its dynamic tag; there is no tag for any itself.
    if (kind == SCALAR_ANY) return;
    anyMetadata(b, kind);
    for (AnyType *a = b->anyTypes; a; a = a->next)
        if (a->kind == kind) {
            if (!a->reflectable) a->reflectionPrepared = 0;
            a->reflectable = 1;
            break;
        }
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
    // Mode 2 boxes private enum storage, which cannot itself escape to a
    // reflection call. Ordinary boxing of a payload still registers its type.
    if (promote != 2) reflectionInput(b, v.kind);
    Kind identity = v.kind;
    // Preserve Ocean identity while keeping small scalars ready for C varargs.
    if (representationKind(b, v.kind) == BOOL || representationKind(b, v.kind) == I8 || representationKind(b, v.kind) == I16 || representationKind(b, v.kind) == U8 || representationKind(b, v.kind) == U16) v = convert(b, v, I32);
    if (representationKind(b, v.kind) == F32) v = convert(b, v, FLOAT);
    if (v.kind == NULL_VALUE) v = value(b, RAW_POINTER, "@__ocean.null");
    BufferKind *t = bufferKind(b, v.kind);
    Value metadata = anyMetadata(b, identity == NULL_VALUE ? RAW_POINTER : identity);
    if (t && (t->shape == 2 || t->shape == 4 || t->shape == 5 || t->shape == 17 || !t->shape || (t->definition && representationKind(b, t->kind) == SCALAR_ANY))) {
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
    if (t && (t->shape == 2 || t->shape == 4 || t->shape == 5 || t->shape == 17 || !t->shape || (t->definition && representationKind(b, t->kind) == SCALAR_ANY))) {
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
        if (source->element != SCALAR_ANY) {
            v = boxArray(b, v);
            if (target->dynamicArray) v = collectionCopy(b, v, expected);
        }
        if (!target->count && !target->fixedArray) {
            b->homogeneousUsed = 1;
            fprintf(b->body, "  call void @__ocean.%s.check(ptr %s)\n", bufferKind(b, v.kind)->dynamicArray ? "array.dynamic" : "array", v.operand);
        }
        v.kind = expected;
        checkArrayRequirement(b, v, target->requirement);
        return v;
    }
    if (implicitVoidPointer(b, v.kind, expected))
        return value(b, expected, "%s", v.operand);
    if (v.kind == NULL_VALUE) {
        if (target && target->shape == 5 && target->record && target->record->enumStorage &&
            !strcmp(target->record->name.lexeme, OCEAN_BUILTIN_OPTIONAL_NAME)) return emptyValue(b, expected);
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
    if (!t || (t->shape && t->shape != 2 && t->shape != 5 && t->shape != 17) || index >= t->count) {
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
    fprintf(b->body, "  %s = call ptr %s(i64 32)\n",
        base.operand, libcSymbol(b, "malloc"));
    rawStore(b, base, value(b, RAW_POINTER, "inttoptr (i64 1 to ptr)"));
    fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 8\n", result.operand, base.operand);
    fprintf(b->body, "  %s = call ptr %s(i64 %s)\n",
        data.operand, libcSymbol(b, "malloc"), bytes.operand);
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
static Value mapCountAddress(Backend *b, Value map) {
    Value result = temp(b, INT);
    fprintf(b->body, "  %s = getelementptr { i64, i64, ptr }, ptr %s, i32 0, i32 1\n", result.operand, map.operand);
    return result;
}
static Value collectionCopy(Backend *b, Value source, Kind target) {
    Value count = arrayLength(b, source), result = allocateCollection(b, target, count);
    BufferKind *from = bufferKind(b, source.kind), *to = bufferKind(b, target);
    if (from->element == to->element && !ownsArrays(b, to->element)) {
        Value data = arrayData(b, source), destination = arrayData(b, result);
        Value bytes = temp(b, INT), stride = storageSize(b, to->element);
        fprintf(b->body, "  %s = mul i64 %s, %s\n", bytes.operand, count.operand, stride.operand);
        fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)\n",
                destination.operand, data.operand, bytes.operand);
        b->dataCopyUsed = 1;
        if (to->map) rawStore(b, mapCountAddress(b, result), load(b, mapCountAddress(b, source)));
        return result;
    }
    Value cursor = slot(b, INT); rawStore(b, cursor, value(b, INT, "0"));
    unsigned condition = ++b->label, body = ++b->label, end = ++b->label;
    jump(b, condition); label(b, condition);
    Value i = load(b, cursor), more = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
    branch(b, more, body, end); label(b, body);
    store(b, elementAddress(b, result, i), load(b, elementAddress(b, source, i)));
    Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
    rawStore(b, cursor, next); jump(b, condition); label(b, end);
    if (bufferKind(b, target)->map) rawStore(b, mapCountAddress(b, result), load(b, mapCountAddress(b, source)));
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
        Binding *binding = lookupBinding(b, name);
        if (binding) return binding->address.kind;
        struct stmt_func *function = findFunction(b, name);
        return function ? closureKind(b, function) : find(b, name).kind;
    }
    case EXPR_TYPE_INSTANTIATION: return specializedClosureValue(b, e, 1).kind;
    case EXPR_TYPE_FUNCTION: return closureKind(b, e->underlying);
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
                return arrayKind(b, k, *(int64_t *)((struct expr_literal *)allocation->count.underlying)->value);
            return pointerBufferKind(b, k);
        }
        return k == SCALAR_ANY ? constrainedArrayKind(b, constraintKind(b, element), 0) : dynamicArrayKind(b, k);
    }
    case EXPR_TYPE_SLICE: {
        struct expr_slice *v = e->underlying;
        BufferKind *t = bufferKind(b, expressionKind(b, &v->parent));
        return t ? ((t->shape == 4 && !t->fixedArray) ? t->kind : dynamicArrayKind(b, t->element)) : BAD;
    }
    case EXPR_TYPE_INDEX: {
        BufferKind *t =
            bufferKind(b, expressionKind(b, &((struct expr_index *)e->underlying)->parent));
        return t ? (t->map ? t->mapValue : t->element) : BAD;
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
        BufferKind *callable = closureCalleeKind(b, &v->func);
        if (callable) return callable->element;
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
            return methodResult(b, dot->interfaceMethod ? interfaceInstance(b, dot->interfaceMethod, typeArguments, count) : methodInstance(b, expressionKind(b, &dot->parent),
                ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme, typeArguments, count));
        }
        if (v->func.type == EXPR_TYPE_IDENTIFIER) {
            const char *name = ((struct expr_identifier *)v->func.underlying)->lexeme.lexeme;
            if (!strcmp(name, "compilerThreadSpawn") && b->function && b->function->compilerIntrinsic == INTRINSIC_THREAD_SPAWN) return INT;
            if (!strcmp(name, "compilerThreadReturn") && b->function && b->function->compilerIntrinsic == INTRINSIC_THREAD_RETURN) return VOID;
            if (!strcmp(name, "recover") && !findFunction(b, "recover")) return SCALAR_ANY;
            if ((!strcmp(name, "compilerReflectFields") || !strcmp(name, "compilerReflectEnumVariant")) && b->function &&
                (b->function->compilerIntrinsic == INTRINSIC_REFLECT_FIELDS || b->function->compilerIntrinsic == INTRINSIC_REFLECT_ENUM_VARIANT))
                return resultType(b, b->function);
            if ((!strcmp(name, "compilerReflectIsStruct") || !strcmp(name, "compilerReflectIsEnum")) && b->function &&
                (b->function->compilerIntrinsic == INTRINSIC_REFLECT_IS_STRUCT || b->function->compilerIntrinsic == INTRINSIC_REFLECT_IS_ENUM))
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
        if (a == SCALAR_ANY || c == SCALAR_ANY)
            return v->checkedResult.nTokens ? type(b, v->checkedResult) : SCALAR_ANY;
        return a == FLOAT || c == FLOAT ? FLOAT : a == F32 || c == F32 ? F32
               : integral(b, a) && integral(b, c) && a != c ? commonIntegerKind(b, a, c) : a;
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
    if (b->errors) return;
    if (depth >= 32) { fail(b, "nested dynamic array ownership is too deep"); return; }
    BufferKind *t = bufferKind(b, item.kind);
    if (!t) return;
    if (t->mapEntry) {
        int previous = b->copyTransport; b->copyTransport = 3;
        copyValue(b, item, depth); b->copyTransport = previous; return;
    }
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
        Value item = load(b, elementAddress(b, dynamicArray, i));
        if (representationKind(b, type->element) == SCALAR_ANY) {
            int previous = b->copyTransport; b->copyTransport = 3;
            copyValue(b, item, 0); b->copyTransport = previous;
        } else dropNestedDynamicArrays(b, item, 0);
        Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
        rawStore(b, cursor, next); jump(b, condition); label(b, done);
    }
    Value data = arrayData(b, dynamicArray);
    fprintf(b->body, "  call void %s(ptr %s)\n",
        libcSymbol(b, "free"), data.operand);
    Value base = temp(b, RAW_POINTER);
    fprintf(b->body, "  %s = getelementptr i8, ptr %s, i64 -8\n", base.operand, dynamicArray.operand);
    fprintf(b->body, "  call void %s(ptr %s)\n",
        libcSymbol(b, "free"), base.operand);
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
    result.ownedBox = t->dynamicArray && t->element == SCALAR_ANY;
    result.owner = array.owner;
    result.throughPointer = array.throughPointer;
    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", result.operand,
            irType(b, t->element), data.operand, index.operand);
    return result;
}
static Value copyValue(Backend *b, Value source, unsigned depth) {
    if (b->errors) return source;
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
            fprintf(b->body, "  %s = call ptr %s(i64 %s)\n",
        base.operand, libcSymbol(b, "malloc"), allocationSize.operand);
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
            fprintf(b->body, "  call void %s(ptr %s)\n",
        libcSymbol(b, "free"), base.operand);
        }
        if (b->copyTransport != 3) trackDynamicArrays(b, result);
        return result;
    }
    Value result = source;
    int previousTransport = b->copyTransport;
    if (t->mapEntry && b->copyTransport != 3) b->copyTransport = 1;
    for (size_t i = 0; i < t->count; i++) {
        Kind member = t->shape ? t->members[i] : t->element;
        if (ownsArrays(b, member))
            result = insert(b, result, copyValue(b, extract(b, source, i), depth + 1), i);
    }
    b->copyTransport = previousTransport;
    return result;
}
static void initializeArray(Backend *b, Value array, Value count) {
    BufferKind *t = bufferKind(b, array.kind);
    count = coerce(b, count, INT);
    // Byte collections dominate string/request preparation. Avoid an unoptimized
    // checked element loop for every zeroed byte, especially under emulation.
    if (t->element == I8 || t->element == U8) {
        Value data = arrayData(b, array);
        fprintf(b->body, "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 %s, i1 false)\n", data.operand, count.operand);
        b->dataCopyUsed = 1;
        return;
    }
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
static Value binary(Backend *, enum binaryExprType, Value, Value);
#include "backend_map.inc"
static Value constructComposite(Backend *b, expr *e, Kind kind) {
    struct expr_composite *v = e->underlying;
    BufferKind *t = bufferKind(b, kind);
    if (!t) {
        fail(b, "literal needs a supported array, buffer, or struct type");
        return value(b, BAD, "0");
    }
    if (t->map) {
        Value storage = slot(b, kind); rawStore(b, storage, emptyValue(b, kind));
        for (size_t i = 0; i < v->nEntries; i++) {
            Value key = coerce(b, expectedValue(b, &v->keys[i], t->mapKey), t->mapKey);
            Value item = expectedValue(b, &v->values[i], t->mapValue);
            Value destination = mapWriteAddress(b, storage, key);
            store(b, destination, coerce(b, item, t->mapValue));
        }
        Value result = load(b, storage); rawStore(b, storage, emptyValue(b, kind));
        trackDynamicArrays(b, result); return result;
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
static int pureIndex(expr *);
static ValueList tryIndexValues(Backend *b, struct expr_index *v) {
    Value base = materialize(b, &v->parent);
    BufferKind *t = bufferKind(b, base.kind);
    if (t->map) {
        Value parent = load(b, base);
        if (!pureIndex(&v->index)) parent = copyValue(b, parent, 0);
        Value key = coerce(b, expectedValue(b, &v->index, t->mapKey), t->mapKey), found;
        Value result = mapRead(b, parent, key, &found);
        Value *items = checked(2 * sizeof(*items)); items[0] = result; items[1] = found;
        return (ValueList){items, 2};
    }
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
    Value heap = temp(b, arrayValueKind(b, SCALAR_ANY));
    fprintf(b->body, "  %s = call ptr @__ocean.any.array({ ptr, i64 } %s)\n", heap.operand, box.operand);
    int previous = b->copyTransport;
    b->copyTransport = 2;
    Value result = copyValue(b, heap, 0);
    b->copyTransport = previous;
    result = coerce(b, result, kind);
    BufferKind *target = bufferKind(b, kind);
    if (!target->count && !target->fixedArray) {
        b->homogeneousUsed = 1;
        fprintf(b->body, "  call void @__ocean.%s.check(ptr %s)\n", bufferKind(b, result.kind)->dynamicArray ? "array.dynamic" : "array", result.operand);
    }
    checkArrayRequirement(b, result, target->requirement);
    return result;
}

static void checkElementRequirement(Backend *b, Value box, Kind requirement) {
    if (!requirement || requirement == SCALAR_ANY) return;
    Value matches = testConstraint(b, box, requirement, 0);
    BufferKind *required = bufferKind(b, requirement);
    if (required && required->shape == 12) {
        Value tag = temp(b, RAW_POINTER), bits = temp(b, INT);
        fprintf(b->body, "  %s = extractvalue { ptr, i64 } %s, 0\n", tag.operand, box.operand);
        fprintf(b->body, "  %s = extractvalue { ptr, i64 } %s, 1\n", bits.operand, box.operand);
        Value metadata = anyMetadata(b, RAW_POINTER);
        Value pointer = temp(b, BOOL), nullBits = temp(b, BOOL), absent = temp(b, BOOL), accepted = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp eq ptr %s, %s\n", pointer.operand, tag.operand, metadata.operand);
        fprintf(b->body, "  %s = icmp eq i64 %s, ptrtoint (ptr @__ocean.null to i64)\n", nullBits.operand, bits.operand);
        fprintf(b->body, "  %s = and i1 %s, %s\n", absent.operand, pointer.operand, nullBits.operand);
        fprintf(b->body, "  %s = or i1 %s, %s\n", accepted.operand, matches.operand, absent.operand);
        matches = accepted;
    }
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
        Value metadata = anyMetadata(b, RAW_POINTER);
        b->ffiUsed = 1;
        return value(b, kind, "{ ptr %s, i64 ptrtoint (ptr @__ocean.null to i64) }", metadata.operand);
    }
    if (kind == RAW_POINTER)
        return value(b, kind, "null");
    if (floating(b, kind))
        return value(b, kind, "0x0000000000000000");
    BufferKind *t = bufferKind(b, kind);
    if (t && t->shape == 17) return value(b, kind, "zeroinitializer");
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
        fprintf(b->body, "  call void @__ocean.%s.check(ptr %s)\n", bufferKind(b, array.kind)->dynamicArray ? "array.dynamic" : "array", array.operand);
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
        fprintf(b->body, "  call void @__ocean.%s.check(ptr %s)\n", bufferKind(b, array.kind)->dynamicArray ? "array.dynamic" : "array", array.operand);
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
        if (t && t->map) {
            Value map = load(b, base);
            if (b->readingIndex && !pureIndex(&v->index)) map = copyValue(b, map, 0);
            int previous = b->readingIndex; b->readingIndex = 0;
            Value key = coerce(b, expectedValue(b, &v->index, t->mapKey), t->mapKey);
            b->readingIndex = previous;
            if (previous) {
                Value result = slot(b, t->mapValue), found;
                Value item = mapRead(b, map, key, &found);
                requireMapKey(b, found, v->location);
                rawStore(b, result, item); return result;
            }
            return mapWriteAddress(b, base, key);
        }
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
        result.ownedBox = t->dynamicArray && t->element == SCALAR_ANY;
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
                if (t->element == SCALAR_ANY && !t->count) { result.homogeneousArray = pointer.operand; result.homogeneousDynamic = t->dynamicArray; }
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
    checkArrayRequirement(b, result, t->requirement);
    if (t->element == SCALAR_ANY && !t->count) {
        b->homogeneousUsed = 1;
        fprintf(b->body, "  call void @__ocean.array.dynamic.check(ptr %s)\n", result.operand);
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
        fprintf(b->body, "  call void @__ocean.%s.check(ptr %s)\n", bufferKind(b, result.kind)->dynamicArray ? "array.dynamic" : "array", result.operand);
    }
    return result;
}
static Value binary(Backend *, enum binaryExprType, Value, Value);
static int runtimeOperator(Backend *b, enum binaryExprType op, Kind a, Kind c) {
    int comparison = op >= EXPR_EQUALITY && op <= EXPR_GREATER_EQUALS;
    if ((integral(b, a) || floating(b, a)) && (integral(b, c) || floating(b, c))) {
        if (integral(b, a) && integral(b, c) && unsignedInteger(b, a) != unsignedInteger(b, c) &&
            (unsignedInteger(b, a) ? a : c) == UINT)
            return 0;
        return comparison || (op >= EXPR_ADD && op <= EXPR_DIVIDE) ||
               (!floating(b, a) && !floating(b, c) &&
                (op == EXPR_MODULO || op == EXPR_XOR || op == EXPR_BITWISE_AND ||
                 op == EXPR_AMPERSAND || op == EXPR_BITWISE_OR || op == EXPR_BITSHIFT_LEFT ||
                 op == EXPR_BITSHIFT_RIGHT));
    }
    if (op == EXPR_ADD && arrayAdditionKind(b, a, c) != BAD) return 1;
    BufferKind *left = bufferKind(b, a), *right = bufferKind(b, c);
    if (left && right && left->shape == 6 && right->shape == 6 && a == c &&
        (op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS))
        return 1;
    if (a == BOOL && c == BOOL)
        return op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS || op == EXPR_XOR;
    return a == stringKind(b) && c == a &&
           (op == EXPR_ADD || comparison);
}
typedef struct {
    Kind *items;
    size_t count;
} OperatorKinds;
static void operatorKinds(Backend *b, typeTokens span, OperatorKinds *result, unsigned depth) {
    if (depth > 32) {
        fail(b, "recursive operator type list");
        return;
    }
    for (size_t start = 0; start < span.nTokens;) {
        size_t end = typeListEnd(span, start);
        typeTokens term = {end - start, span.tokens + start};
        if (term.nTokens && term.tokens[0].type == TILDE) {
            term.tokens++;
            term.nTokens--;
        }
        struct stmt_interface *contract =
            term.nTokens == 1 ? findInterface(b, term.tokens[0].lexeme)
            : term.nTokens && term.tokens[0].type == INTERFACE ? parseInterfaceType(term)
                                                               : NULL;
        Scope *owner = NULL;
        struct stmt_type *alias =
            term.nTokens == 1 ? findTypeDeclaration(b, term.tokens[0].lexeme, &owner) : NULL;
        if (contract && contract->nConstraints)
            operatorKinds(b, contract->constraints[0], result, depth + 1);
        else if (alias && alias->isAlias) {
            Scope *previous = b->scope;
            b->scope = owner;
            operatorKinds(b, alias->underlying, result, depth + 1);
            b->scope = previous;
        } else {
            Kind kind = type(b, term);
            int found = 0;
            for (size_t i = 0; i < result->count; i++)
                if (result->items[i] == kind) found = 1;
            if (!found) {
                result->items = realloc(result->items, (result->count + 1) * sizeof(Kind));
                if (!result->items) {
                    fprintf(stderr, "out of memory\n");
                    exit(1);
                }
                result->items[result->count++] = kind;
            }
        }
        start = end + 1;
    }
}
// Dispatch follows the declared finite alternatives, never arbitrary any values.
static Value boxedBinary(Backend *b, enum binaryExprType op, Value a, Value c, typeTokens leftTypes,
                         typeTokens rightTypes) {
    if ((op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS) &&
        ((a.kind == SCALAR_ANY && c.kind == NULL_VALUE) ||
         (c.kind == SCALAR_ANY && a.kind == NULL_VALUE))) {
        Value box = a.kind == SCALAR_ANY ? a : c;
        Value tag = temp(b, RAW_POINTER), bits = temp(b, INT);
        fprintf(b->body, "  %s = extractvalue { ptr, i64 } %s, 0\n", tag.operand, box.operand);
        fprintf(b->body, "  %s = extractvalue { ptr, i64 } %s, 1\n", bits.operand, box.operand);
        Value pointerTag = anyMetadata(b, RAW_POINTER), isPointer = temp(b, BOOL), isNull = temp(b, BOOL), result = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp eq ptr %s, %s\n", isPointer.operand, tag.operand, pointerTag.operand);
        fprintf(b->body, "  %s = icmp eq i64 %s, ptrtoint (ptr @__ocean.null to i64)\n", isNull.operand, bits.operand);
        fprintf(b->body, "  %s = and i1 %s, %s\n", result.operand, isPointer.operand, isNull.operand);
        if (op == EXPR_NOT_EQUALS) {
            Value inverted = temp(b, BOOL);
            fprintf(b->body, "  %s = xor i1 %s, true\n", inverted.operand, result.operand);
            return inverted;
        }
        return result;
    }
    if (op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS) return boxedEquality(b, op, a, c);
    Kind fallback[] = {INT, I8, I16, I32, UINT, U8, U16, U32, FLOAT, F32, BOOL, stringKind(b)};
    OperatorKinds left = {0}, right = {0};
    if (a.kind == SCALAR_ANY && leftTypes.nTokens) operatorKinds(b, leftTypes, &left, 0);
    if (c.kind == SCALAR_ANY && rightTypes.nTokens) operatorKinds(b, rightTypes, &right, 0);
    Kind *leftKinds = left.count ? left.items : a.kind == SCALAR_ANY ? fallback : &a.kind;
    Kind *rightKinds = right.count ? right.items : c.kind == SCALAR_ANY ? fallback : &c.kind;
    size_t leftCount = left.count             ? left.count
                       : a.kind == SCALAR_ANY ? sizeof(fallback) / sizeof(*fallback)
                                              : 1;
    size_t rightCount = right.count            ? right.count
                        : c.kind == SCALAR_ANY ? sizeof(fallback) / sizeof(*fallback)
                                               : 1;
    int comparison = op >= EXPR_EQUALITY && op <= EXPR_GREATER_EQUALS;
    Value result = slot(b, comparison ? BOOL : SCALAR_ANY);
    unsigned end = ++b->label;
    for (size_t i = 0; i < leftCount; i++) {
        for (size_t j = 0; j < rightCount; j++) {
            if (!runtimeOperator(b, op, leftKinds[i], rightKinds[j])) continue;
            Value yes = value(b, BOOL, "true");
            if (a.kind == SCALAR_ANY)
                yes = testConstraint(b, a, constraintShape(b, 14, leftKinds[i], 0), 0);
            if (c.kind == SCALAR_ANY) {
                Value match = testConstraint(b, c, constraintShape(b, 14, rightKinds[j], 0), 0),
                      both = temp(b, BOOL);
                fprintf(b->body, "  %s = and i1 %s, %s\n", both.operand, yes.operand,
                        match.operand);
                yes = both;
            }
            unsigned selected = ++b->label, next = ++b->label;
            branch(b, yes, selected, next);
            label(b, selected);
            Value left = a.kind == SCALAR_ANY ? unboxMatched(b, a, leftKinds[i]) : a;
            Value right = c.kind == SCALAR_ANY ? unboxMatched(b, c, rightKinds[j]) : c;
            Value computed = binary(b, op, left, right);
            rawStore(b, result, comparison ? computed : boxScalar(b, computed, 1));
            jump(b, end);
            label(b, next);
        }
    }
    fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
    b->nullChecksUsed = 1;
    b->terminated = 1;
    free(left.items);
    free(right.items);
    label(b, end);
    return load(b, result);
}
static Value boxedUnary(Backend *b, enum unaryExprType op, Value input, typeTokens types) {
    Kind fallback[] = {INT, I8, I16, I32, UINT, U8, U16, U32, FLOAT, F32, BOOL};
    OperatorKinds choices = {0};
    if (types.nTokens) operatorKinds(b, types, &choices, 0);
    Kind *kinds = choices.count ? choices.items : fallback;
    size_t count = choices.count ? choices.count : sizeof(fallback) / sizeof(*fallback);
    Value result = slot(b, SCALAR_ANY);
    unsigned end = ++b->label;
    for (size_t i = 0; i < count; i++) {
        Kind kind = kinds[i];
        if ((op == UNARY_BITWISE_NOT && !integral(b, kind)) ||
            (op == UNARY_LOGICAL_NOT && kind != BOOL) || (op != UNARY_LOGICAL_NOT && kind == BOOL))
            continue;
        Value match = testConstraint(b, input, constraintShape(b, 14, kind, 0), 0);
        unsigned yes = ++b->label, next = ++b->label;
        branch(b, match, yes, next);
        label(b, yes);
        Value concrete = unboxMatched(b, input, kind), computed;
        if (op == UNARY_NEGATE)
            computed = binary(b, EXPR_SUBTRACT,
                              value(b, kind, floating(b, kind) ? "0.000000e+00" : "0"), concrete);
        else if (op == UNARY_BITWISE_NOT || op == UNARY_LOGICAL_NOT) {
            computed = temp(b, kind);
            fprintf(b->body, "  %s = xor %s %s, %s\n", computed.operand, irType(b, kind),
                    concrete.operand, kind == BOOL ? "true" : "-1");
        } else {
            int decrement = op == UNARY_POSTFIX_DECREMENT || op == UNARY_PREFIX_DECREMENT;
            computed = binary(b, decrement ? EXPR_SUBTRACT : EXPR_ADD, concrete,
                              value(b, kind, floating(b, kind) ? "1.000000e+00" : "1"));
        }
        rawStore(b, result, boxScalar(b, computed, 1));
        jump(b, end);
        label(b, next);
    }
    fprintf(b->body, "  call void @llvm.trap()\n  unreachable\n");
    b->nullChecksUsed = 1;
    b->terminated = 1;
    free(choices.items);
    label(b, end);
    return load(b, result);
}

#include "backend_equality.inc"
static Value binary(Backend *b, enum binaryExprType op, Value a, Value c) {
    if (representationKind(b, a.kind) == SCALAR_ANY) a.kind = SCALAR_ANY;
    if (representationKind(b, c.kind) == SCALAR_ANY) c.kind = SCALAR_ANY;
    if (op == EXPR_ADD) {
        Kind result = arrayAdditionKind(b, a.kind, c.kind);
        if (result != BAD) return addArrayValues(b, a, c, result);
    }
    if (a.kind == SCALAR_ANY || c.kind == SCALAR_ANY) return boxedBinary(b,op,a,c,(typeTokens){0},(typeTokens){0});
    if (a.kind == NULL_VALUE || c.kind == NULL_VALUE) {
        if (op != EXPR_EQUALITY && op != EXPR_NOT_EQUALS) {
            fail(b, "null supports only equality comparisons");
            return value(b, BAD, "0");
        }
        if (a.kind == NULL_VALUE && c.kind == NULL_VALUE)
            return value(b, BOOL, "%d", op == EXPR_EQUALITY);
        Value other = a.kind == NULL_VALUE ? c : a;
        BufferKind *optional = bufferKind(b, other.kind);
        if (optional && optional->shape == 5 && optional->record && optional->record->enumStorage &&
            !strcmp(optional->record->name.lexeme, OCEAN_BUILTIN_OPTIONAL_NAME))
            return binary(b, op, extract(b, other, 0), value(b, INT, "0"));
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
    if (array && array->shape == 4 && array->element == I8 && rightArray && rightArray->shape == 4 && rightArray->element == I8 &&
        ((op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS) ||
         (array->immutableString && rightArray->immutableString && op >= EXPR_LESS_THAN && op <= EXPR_GREATER_EQUALS)))
        return stringComparison(b, op, a, c);
    if (op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS) {
        if (a.kind == RAW_POINTER || (array && array->shape == 3)) {
            if (a.kind != c.kind) return value(b, BOOL, "%d", op == EXPR_NOT_EQUALS);
            Value result = temp(b, BOOL);
            fprintf(b->body, "  %s = icmp %s ptr %s, %s\n", result.operand,
                    op == EXPR_EQUALITY ? "eq" : "ne", a.operand, c.operand);
            return result;
        }
        if ((array && (array->shape == 4 || array->shape == 5 || array->shape == 2 || array->shape == 17 || !array->shape)) ||
            (rightArray && (rightArray->shape == 4 || rightArray->shape == 5 || rightArray->shape == 2 || rightArray->shape == 17 || !rightArray->shape))) {
            Value result = aggregateEquality(b, a, c);
            if (op == EXPR_NOT_EQUALS) {
                Value inverted = temp(b, BOOL);
                fprintf(b->body, "  %s = xor i1 %s, true\n", inverted.operand, result.operand); result = inverted;
            }
            return result;
        }
    }
    if (integral(b, a.kind) && integral(b, c.kind) && a.kind != c.kind) {
        Kind common = commonIntegerKind(b, a.kind, c.kind);
        a = convert(b, a, common);
        c = convert(b, c, common);
    }
    if ((floating(b, a.kind) || floating(b, c.kind)) && a.kind != c.kind &&
        (floating(b, a.kind) || integral(b, a.kind)) && (floating(b, c.kind) || integral(b, c.kind))) {
        Kind common = a.kind == FLOAT || c.kind == FLOAT ? FLOAT : F32;
        a = convert(b, a, common); c = convert(b, c, common);
    }
    if (a.kind != c.kind && (op == EXPR_EQUALITY || op == EXPR_NOT_EQUALS))
        return value(b, BOOL, "%d", op == EXPR_NOT_EQUALS);
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
        result = insert(b, result, boxScalar(b, coerce(b, payload, kind), 2), 1);
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
        fprintf(b->body, "  %s = call ptr %s(i64 1, i64 %s)\n",
        result.operand, libcSymbol(b, "calloc"), bytes.operand);
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
            if (v->unsignedLiteral) return value(b, UINT, "%" PRIu64, *(uint64_t *)v->value);
            return value(b, INT, "%" PRId64, *(int64_t *)v->value);
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
            nativeConstantIdentity(b, value(b, RAW_POINTER, "@.bytes%u", id).operand, v->value, v->stringLength);
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
    case EXPR_TYPE_INSTANTIATION: return specializedClosureValue(b, e, 0);
    case EXPR_TYPE_FUNCTION: return closureValue(b, e->underlying, NULL, 0);
    case EXPR_TYPE_IDENTIFIER: {
        const char *name = ((struct expr_identifier *)e->underlying)->lexeme.lexeme;
        if (!strcmp(name, "true") || !strcmp(name, "false"))
            return value(b, BOOL, "%d", !strcmp(name, "true"));
        Binding *binding = lookupBinding(b, name);
        if (binding) return load(b, binding->address);
        struct stmt_func *function = findFunction(b, name);
        if (function) return closureValue(b, function, NULL, 1);
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
    case EXPR_TYPE_SLICE: {
        struct expr_slice *v = e->underlying;
        Value source = expression(b, &v->parent);
        // Snapshot only when evaluating bounds could mutate the receiver.
        if (!pureIndex(&v->start) || !pureIndex(&v->end)) source = copyValue(b, source, 0);
        BufferKind *t = bufferKind(b, source.kind);
        int knownLength = t->shape != 3 || t->fixedArray || source.length;
        if (!knownLength && !v->end.type) {
            fail(b, "slicing a buffer pointer requires an explicit end bound");
            return value(b, BAD, "0");
        }
        Value length = knownLength ? arrayLength(b, source) : value(b, INT, "0");
        Value start = v->start.type ? coerce(b, expression(b, &v->start), INT) : value(b, INT, "0");
        Value end = v->end.type ? coerce(b, expression(b, &v->end), INT) : length;
        Value nonnegative = temp(b, BOOL), ordered = temp(b, BOOL), valid = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp sge i64 %s, 0\n", nonnegative.operand, start.operand);
        fprintf(b->body, "  %s = icmp sge i64 %s, %s\n", ordered.operand, end.operand, start.operand);
        fprintf(b->body, "  %s = and i1 %s, %s\n", valid.operand, nonnegative.operand, ordered.operand);
        if (knownLength) {
            Value fits = temp(b, BOOL), both = temp(b, BOOL);
            fprintf(b->body, "  %s = icmp sle i64 %s, %s\n", fits.operand, end.operand, length.operand);
            fprintf(b->body, "  %s = and i1 %s, %s\n", both.operand, valid.operand, fits.operand);
            valid = both;
        }
        unsigned good = ++b->label, bad = ++b->label;
        branch(b, valid, good, bad); label(b, bad);
        b->boundsUsed = 1;
        token location = diagnosticLocation(b, v->location);
        Value filename = moduleString(b, location.filename);
        char *text = sourceSnippet(b, location); Value snippet = moduleString(b, text); free(text);
        fprintf(b->body, "  call void @__ocean.bounds_fail(i64 %s, i64 %s, ptr %s, i64 %ld, i64 %ld, ptr %s)\n  unreachable\n",
                end.operand, length.operand, filename.operand, location.line, location.column, snippet.operand);
        b->terminated = 1; label(b, good);
        Value count = temp(b, INT);
        fprintf(b->body, "  %s = sub i64 %s, %s\n", count.operand, end.operand, start.operand);
        Value result = allocateCollection(b, expressionKind(b, e), count);
        Value cursor = slot(b, INT); rawStore(b, cursor, value(b, INT, "0"));
        unsigned condition = ++b->label, body = ++b->label, done = ++b->label;
        jump(b, condition); label(b, condition);
        Value i = load(b, cursor), more = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp slt i64 %s, %s\n", more.operand, i.operand, count.operand);
        branch(b, more, body, done); label(b, body);
        Value offset = temp(b, INT);
        fprintf(b->body, "  %s = add i64 %s, %s\n", offset.operand, start.operand, i.operand);
        store(b, elementAddress(b, result, i), load(b, elementAddress(b, source, offset)));
        Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
        rawStore(b, cursor, next); jump(b, condition); label(b, done);
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
                Value data = arrayData(b, parent);
                if (b->deferCall && !t->immutableString && v->parent.type == EXPR_TYPE_IDENTIFIER) {
                    Value storage = address(b, &v->parent);
                    retainDeferredPointee(b, value(b, pointerKind(b, storage.kind), "%s", storage.operand));
                } else {
                    data.backingKind = parent.kind; data.backingOperand = parent.operand;
                }
                return data;
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
        BufferKind *indirectKind = closureCalleeKind(b, &v->func);
        Value indirect = value(b, BAD, "");
        if (indirectKind) {
            indirect = expression(b, &v->func);
            f = indirectKind->callable;
        } else if (dot) {
            name = ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme;
            Kind actual = expressionKind(b, &dot->parent);
            BufferKind *dynamicArray = bufferKind(b, actual);
            BufferKind *arrayReceiver = dynamicArray;
            while (arrayReceiver && arrayReceiver->shape == 6) arrayReceiver = bufferKind(b, arrayReceiver->element);
            if (b->deferCall == e && arrayReceiver && arrayReceiver->shape == 4 &&
                (!strcmp(name, "array") || !strcmp(name, "remove") || !strcmp(name, "pop") ||
                 !strcmp(name, "append") || !strcmp(name, "prepend") || !strcmp(name, "add")))
                return scheduleIntrinsicDefer(b, e);
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
            method = dot->interfaceMethod ? interfaceInstance(b, dot->interfaceMethod, typeArguments, count)
                                          : methodInstance(b, actual, name, typeArguments, count);
            if (!method) return value(b, BAD, "0");
            f = method->function;
            BufferKind *actualType = bufferKind(b, actual);
            BufferKind *wantedType = bufferKind(b, method->receiver);
            if (method->interfaceMethod) {
                receiver = coerce(b, expression(b, &dot->parent), SCALAR_ANY);
            } else if (wantedType && wantedType->shape == 6 && !wantedType->definition && !(actualType && actualType->shape == 6 && !actualType->definition)) {
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
        if (!strcmp(name, "compilerThreadSpawn") || !strcmp(name, "compilerThreadReturn")) {
            int spawn = !strcmp(name, "compilerThreadSpawn");
            if (!b->function || b->function->compilerIntrinsic != (spawn ? INTRINSIC_THREAD_SPAWN : INTRINSIC_THREAD_RETURN) ||
                v->nParams != (size_t)spawn || v->spread) {
                fail(b, "thread intrinsics are restricted to the standard library threads package"); return value(b, BAD, "0");
            }
            b->threadsUsed = b->closuresUsed = 1;
            if (!spawn) {
                fprintf(b->body, "  call void @ocean_runtime_thread_return()\n  unreachable\n");
                b->terminated = 1; return value(b, VOID, "");
            }
            Value callback = expression(b, &v->params[0]);
            Value code = closureParts(b, callback, 0), environment = closureParts(b, callback, 1);
            requireClosureCode(b, code, v->location);
            Value result = temp(b, INT);
            fprintf(b->body, "  %s = call i64 @ocean_threads_spawn(ptr %s, ptr %s)\n", result.operand, code.operand, environment.operand);
            return result;
        }
        if (!strcmp(name, "compilerReflectFields") || !strcmp(name, "compilerReflectIsStruct") ||
            !strcmp(name, "compilerReflectIsEnum") || !strcmp(name, "compilerReflectEnumVariant")) {
            int fields = !strcmp(name, "compilerReflectFields"), isEnum = !strcmp(name, "compilerReflectIsEnum"),
                variant = !strcmp(name, "compilerReflectEnumVariant");
            if (!b->function || b->function->compilerIntrinsic != (fields ? INTRINSIC_REFLECT_FIELDS : isEnum ? INTRINSIC_REFLECT_IS_ENUM : variant ? INTRINSIC_REFLECT_ENUM_VARIANT : INTRINSIC_REFLECT_IS_STRUCT) ||
                v->nParams != 1 || v->spread) {
                fail(b, "reflection intrinsics are restricted to the standard library reflect package");
                return value(b, BAD, "0");
            }
            Kind kind = (fields || variant) ? resultType(b, b->function) : BOOL;
            Value source = coerce(b, expression(b, &v->params[0]), SCALAR_ANY), result = temp(b, kind);
            b->reflectionUsed = 1;
            if (fields) b->reflectionFieldsKind = kind;
            if (variant) b->reflectionVariantUsed = 1;
            fprintf(b->body, "  %s = call %s @__ocean.reflect.%s({ ptr, i64 } %s)\n", result.operand,
                    irType(b, kind), fields ? "fields" : isEnum ? "is_enum" : variant ? "enum_variant" : "is_struct", source.operand);
            if (fields || variant) {
                int previous = b->copyTransport; b->copyTransport = 2;
                result = copyValue(b, result, 0); b->copyTransport = previous;
            }
            return result;
        }
        if (!f && b->deferCall == e && (!strcmp(name, "recover") || !strcmp(name, "free") || !strcmp(name, "len") || !strcmp(name, "delete")))
            return scheduleIntrinsicDefer(b, e);
        if (!f && !strcmp(name, "recover")) {
            Value header = temp(b, stringKind(b)), present = temp(b, BOOL), result = slot(b, SCALAR_ANY);
            fprintf(b->body, "  %s = call ptr @__ocean.recover()\n", header.operand);
            fprintf(b->body, "  %s = icmp ne ptr %s, null\n", present.operand, header.operand);
            unsigned yes = ++b->label, no = ++b->label, done = ++b->label;
            branch(b, present, yes, no); label(b, yes);
            rawStore(b, result, coerce(b, header, SCALAR_ANY)); jump(b, done);
            label(b, no); rawStore(b, result, coerce(b, value(b, NULL_VALUE, "null"), SCALAR_ANY));
            jump(b, done); label(b, done); return load(b, result);
        }
        if (!f && !strcmp(name, "delete")) {
            Value binding = address(b, &v->params[0]);
            BufferKind *map = bufferKind(b, binding.kind);
            Value key = coerce(b, expectedValue(b, &v->params[1], map->mapKey), map->mapKey);
            mapDelete(b, load(b, binding), key);
            return value(b, VOID, "");
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
                fprintf(b->body, "  call void %s(ptr %s)\n",
        libcSymbol(b, "free"), item.operand);
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
            if (t->map) return load(b, mapCountAddress(b, array));
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
            parameterKinds[i] = indirectKind
                ? f->isVariadic && i + 1 == f->nParams ? bufferKind(b, indirectKind->arguments[i])->element : indirectKind->arguments[i]
                : f->isVariadic && i + 1 == f->nParams ? type(b, f->paramTypeNames[i]) : parameterKind(b, f, i);
        Kind variadicArrayKind = f->isVariadic && !f->isExtern && f->nParams
                                     ? indirectKind ? indirectKind->arguments[f->nParams-1] : parameterKind(b, f, f->nParams - 1) : BAD;
        Kind resultKind = indirectKind ? indirectKind->element : resultType(b, f);
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
        if (dynamicC && b->deferCall == e) {
            scheduleDefer(b, NULL, NULL, value(b, BAD, ""), evaluated.items,
                          evaluated.count, VOID, 0, NULL, e, value(b, BAD, ""));
            free(evaluated.items); free(parameterKinds);
            return value(b, VOID, "");
        }
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
            fprintf(b->body, "  call void @__ocean.ffi_call(ptr "); functionName(b, b->body, f);
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
        if (b->deferCall == e) {
            scheduleDefer(b, f, method, receiver, args, nArgs, resultKind, fixed, parameterKinds, NULL, indirect);
            free(args); free(parameterKinds);
            return value(b, VOID, "");
        }
        Value code = value(b, RAW_POINTER, "null"), environment = code;
        if (indirectKind) { code = closureParts(b, indirect, 0); environment = closureParts(b, indirect, 1); requireClosureCode(b, code, v->location); }
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
        if (indirectKind) fprintf(b->body, "%s", code.operand);
        else if (method) methodName(b->body, method);
        else functionName(b, b->body, f);
        fprintf(b->body, "(");
        if (indirectKind) fprintf(b->body, "ptr %s", environment.operand);
        if (method && f->receiverType.nTokens)
            fprintf(b->body, "%s %s", irType(b, receiver.kind), receiver.operand);
        for (size_t i = 0; i < nArgs; i++)
            fprintf(b->body, "%s%s %s", (i || indirectKind || (method && f->receiverType.nTokens)) ? ", " : "",
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
                enum binaryExprType op=ops[v->operation - EXPR_ADD_EQUALS];
                if ((old.kind==SCALAR_ANY || r.kind==SCALAR_ANY) &&
                    (v->operandTypes[0].nTokens || v->operandTypes[1].nTokens))
                    r=boxedBinary(b,op,old,r,v->operandTypes[0],v->operandTypes[1]);
                else r = binary(b,op,old,r);
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
            if (a.kind == SCALAR_ANY) a = unboxValue(b,a,BOOL);
            Value dest = slot(b, BOOL);
            store(b, dest, a);
            unsigned rhs = ++b->label, end = ++b->label;
            if (v->operation == EXPR_LOGICAL_AND)
                branch(b, a, rhs, end);
            else
                branch(b, a, end, rhs);
            label(b, rhs);
            Value c = expression(b, &v->right);
            if (c.kind == SCALAR_ANY) c = unboxValue(b,c,BOOL);
            store(b, dest, c);
            jump(b, end);
            label(b, end);
            return load(b, dest);
        }
        if ((v->operation == EXPR_EQUALITY || v->operation == EXPR_NOT_EQUALS) && ownsArrays(b, a.kind) &&
            v->right.type != EXPR_TYPE_IDENTIFIER && v->right.type != EXPR_TYPE_LITERAL)
            a = copyValue(b, a, 0);
        if (v->operation == EXPR_ADD &&
            arrayAdditionKind(b, a.kind, expressionKind(b, &v->right)) != BAD)
            a = copyValue(b, a, 0); // Snapshot before RHS side effects.
        Value c = expression(b, &v->right);
        if (v->operation == EXPR_COMMA)
            return c;
        Value computed = (a.kind==SCALAR_ANY || c.kind==SCALAR_ANY) &&
            (v->operandTypes[0].nTokens || v->operandTypes[1].nTokens)
            ? boxedBinary(b,v->operation,a,c,v->operandTypes[0],v->operandTypes[1])
            : binary(b, v->operation, a, c);
        return computed.kind == SCALAR_ANY && v->checkedResult.nTokens
            ? convert(b,computed,type(b,v->checkedResult)) : computed;
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
            if (old.kind == SCALAR_ANY) {
                Value updated = boxedUnary(b,v->operation,old,v->operandTypes); store(b,addr,updated);
                return v->operation == UNARY_POSTFIX_INCREMENT || v->operation == UNARY_POSTFIX_DECREMENT ? old : updated;
            }
            Value one = floating(b, old.kind) ? value(b, old.kind, "0x3FF0000000000000") : value(b, old.kind, "1");
            Value r = binary(b, decrement ? EXPR_SUBTRACT : EXPR_ADD, old, one);
            store(b, addr, r);
            return v->operation == UNARY_POSTFIX_INCREMENT ||
                           v->operation == UNARY_POSTFIX_DECREMENT
                       ? old
                       : r;
        }
        Value a = expression(b, &v->child);
        if (a.kind == SCALAR_ANY) {
            if (v->operation == UNARY_LOGICAL_NOT) {
                Value concrete=unboxValue(b,a,BOOL), result=temp(b,BOOL);
                fprintf(b->body,"  %s = xor i1 %s, true\n",result.operand,concrete.operand); return result;
            }
            return boxedUnary(b,v->operation,a,v->operandTypes);
        }
        Value r = temp(b, a.kind);
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
    case EXPR_TYPE_SLICE:
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
    if (s->type == STMT_DEFER) return 1;
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
    if (!b->terminated) {
        if (b->function && v == &b->function->body)
            fprintf(b->body, "  call void @__ocean.leave(ptr %s)\n", b->deferFrame.operand);
        restoreStack(b, &scope);
    }
    b->scope = scope.parent;
    clearScope(&scope);
}
static void block(Backend *b, struct stmt_block *v) { blockLimit(b, v, v->nStmts); }
#include "backend_closure.inc"
#include "backend_defer.inc"
#include "backend_threads.inc"
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
    } else if (!strcmp(architecture, "riscv64")) {
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

// Selectors follow the configured target, defaulting to the native host.
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
    if (backendWasm()) { architecture = "wasm"; platform = "js"; }
    if (strcmp(v->architecture.lexeme, architecture) ||
        (v->platform.type != EOF_TOKEN && strcmp(v->platform.lexeme, platform))) return;
    emitAssembly(b, v, architecture);
}

static void statementImpl(Backend *b, stmt *s) {
    if (!s || (b->terminated && s->type != STMT_LABEL))
        return;
    switch (s->type) {
    case STMT_DEFER: {
        expr *previous = b->deferCall;
        b->deferCall = ((struct stmt_defer *)s->value)->call;
        expression(b, b->deferCall);
        b->deferCall = previous;
        break;
    }
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
        if (b->threadsUsed) fprintf(b->body, "  call void @ocean_threads_checkpoint()\n");
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
                Value binding = localSlot(b, kind, v->vars[i].lexeme);
                bind(b, v->vars[i].lexeme, binding);
                b->scope->bindings->autoRelease = !binding.closureCell;
                if (binding.closureCell) store(b, binding, storage);
                else { escapeDynamicArrays(b, storage); rawStore(b, binding, storage); }
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
                Value binding = localSlot(b, pointerKind, v->vars[i].lexeme);
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
            Value addr = localSlot(b, k, v->vars[i].lexeme);
            if (v->nInitValues && i < values.count) addr.length = initial[i].length;
            bind(b, v->vars[i].lexeme, addr);
            b->scope->bindings->autoRelease = !addr.closureCell && containsDynamicArrays(b, k);
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
        for (size_t i = 0; i < b->function->nResults; i++)
            if (b->function->resultNames[i].type != EOF_TOKEN)
                store(b, find(b, b->function->resultNames[i].lexeme),
                      b->function->nResults == 1 ? r : extract(b, r, i));
        fprintf(b->body, "  call void @__ocean.leave(ptr %s)\n", b->deferFrame.operand);
        for (size_t i = 0; i < b->function->nResults; i++)
            if (b->function->resultNames[i].type != EOF_TOKEN) {
                Value item = load(b, find(b, b->function->resultNames[i].lexeme));
                r = b->function->nResults == 1 ? item : insert(b, r, item, i);
            }
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
            if (rangeType->map) range = copyValue(b, range, 0);
            index = slot(b, INT);
            store(b, index, value(b, INT, "0"));
        }
        jump(b, v->deferCondition ? body : condition);
        label(b, condition);
        if (b->threadsUsed) fprintf(b->body, "  call void @ocean_threads_checkpoint()\n");
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
        if (v->range && rangeType->map) {
            Value bucket = elementAddress(b, range, load(b, index));
            Value occupied = load(b, mapField(b, bucket, 0, BOOL));
            unsigned present = ++b->label;
            branch(b, occupied, present, update); label(b, present);
        }
        Scope bodyScope = {.parent = b->scope, .jumpRegion = v->body};
        b->scope = &bodyScope;
        saveStack(b, &bodyScope, v->range || statementAllocatesStack(v->body));
        if (v->range && v->bindings[0].type != UNDERSCORE) {
            // The visible binding is a per-iteration value, not the loop counter.
            Kind keyKind = rangeType->map ? rangeType->mapKey : INT;
            Value binding = localSlot(b, keyKind, v->bindings[0].lexeme);
            Value key = load(b, index);
            if (rangeType->map) key = load(b, mapField(b, elementAddress(b, range, key), 1, keyKind));
            store(b, binding, key);
            bind(b, v->bindings[0].lexeme, binding);
            b->scope->bindings->autoRelease = !binding.closureCell;
        }
        if (v->range && v->nBindings == 2 && v->bindings[1].type != UNDERSCORE) {
            Kind itemKind = rangeType->map ? rangeType->mapValue : rangeType->element;
            Value item = localSlot(b, itemKind, v->bindings[1].lexeme);
            Value i = load(b, index);
            checkArrayIndex(b, range, i, v->range->location);
            Value entry = elementAddress(b, range, i);
            if (rangeType->map) entry = mapField(b, entry, 2, itemKind);
            store(b, item, load(b, entry));
            bind(b, v->bindings[1].lexeme, item);
            b->scope->bindings->autoRelease = !item.closureCell;
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
                Value address = localSlot(b, kind, v->binding.lexeme);
                store(b, address, bound);
                bind(b, v->binding.lexeme, address);
                b->scope->bindings->autoRelease = !address.closureCell && containsDynamicArrays(b, kind);
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
    fprintf(out, "%s %s ", f->isExtern ? "declare" : b->declarationOnly ? "declare" : b->units ? "define hidden" : b->closure || ownsArrays(b, result) ? "define internal" : "define",
            irType(b, result));
    if (b->closure) fprintf(out, "@__ocean.closure.%u", b->closure->id);
    else if (b->method) methodName(out, b->method);
    else functionName(b, out, f);
    fprintf(out, "(");
    int hasReceiver = b->method && f->receiverType.nTokens;
    if (b->closure) fputs("ptr %environment", out);
    if (hasReceiver) fprintf(out, "%s %%receiver", irType(b, b->method->receiver));
    for (size_t i = 0; i < f->nParams; i++)
        fprintf(out, "%s%s %%arg%zu", (i || hasReceiver || b->closure) ? ", " : "",
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
    const char *printName = "dprintf";
#if defined(_WIN32)
    if (!backendWasm()) {
        printName = "__ocean.dprintf";
        // Windows va_list is a pointer on both x64 and ARM64. Keep diagnostics
        // on stderr without requiring the POSIX dprintf entry point.
        fputs("declare ptr @__acrt_iob_func(i32)\n"
              "declare i32 @vfprintf(ptr, ptr, ptr)\n"
              "declare void @llvm.va_start(ptr)\n"
              "declare void @llvm.va_end(ptr)\n"
              "define internal i32 @__ocean.dprintf(i32 %fd, ptr %format, ...) {\n"
              "  %args = alloca ptr, align 8\n"
              "  call void @llvm.va_start(ptr %args)\n"
              "  %list = load ptr, ptr %args\n"
              "  %stream = call ptr @__acrt_iob_func(i32 %fd)\n"
              "  %result = call i32 @vfprintf(ptr %stream, ptr %format, ptr %list)\n"
              "  call void @llvm.va_end(ptr %args)\n"
              "  ret i32 %result\n}\n", b->out);
    } else
#endif
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
        "  call i32 (i32, ptr, ...) @%s(i32 2, ptr %s, ptr %%snippet)\n"
        "  call i32 (i32, ptr, ...) @%s(i32 2, ptr %s)\n"
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
        "  call i32 (i32, ptr, ...) @%s(i32 2, ptr %s, ptr %%name, ptr %%trace.file, i64 "
        "%%trace.line, i64 %%trace.column)\n"
        "  %%parent.slot = getelementptr %%__ocean.trace, ptr %%frame, i32 0, i32 0\n"
        "  %%previous = load ptr, ptr %%parent.slot\n"
        "  br label %%trace\n"
        "done:\n"
        "  call void @%s(i32 1)\n"
        "  unreachable\n"
        "}\n",
        printName, snippetFormat.operand, printName, traceTitle.operand, printName, frameFormat.operand, b->threadsUsed ? "ocean_threads_panic_exit" : "exit");
    fprintf(b->out,
        "define internal void @__ocean.panic(ptr %%m, i64 %%n, ptr %%f, i64 %%l, i64 %%c, ptr %%s) noreturn {\n"
        "  call void @__ocean.unwind(ptr %%m, i64 %%n, i1 false)\n"
        "  call void @__ocean.panic_report(ptr %%m, i64 %%n, ptr %%f, i64 %%l, i64 %%c, ptr %%s)\n  unreachable\n}\n");
    Value panicPrefix = moduleString(b, "Panic: ");
    Value panicLocation = moduleString(b, "\n  --> %s:%lld:%lld\n");
    fprintf(b->out, "declare i64 %s(i32, ptr, i64)\n"
            "define internal void @__ocean.panic_report(ptr %%message, i64 %%size, ptr %%file, i64 %%line, "
            "i64 %%column, ptr %%snippet) noreturn {\nentry:\n"
            "  call i64 %s(i32 2, ptr %s, i64 7)\n"
            "  call i64 %s(i32 2, ptr %%message, i64 %%size)\n"
            "  call i32 (i32, ptr, ...) @%s(i32 2, ptr %s, ptr %%file, i64 %%line, i64 %%column)\n"
            "  call void @__ocean.runtime_diagnostic(ptr %%file, i64 %%line, i64 %%column, ptr %%snippet)\n"
            "  unreachable\n}\n"
            "define internal void @__ocean.runtime_error(ptr %%format, i64 %%index, i64 %%length, "
            "ptr %%file, i64 %%line, i64 %%column, ptr %%snippet) noreturn {\nentry:\n"
            "  call i32 (i32, ptr, ...) @%s(i32 2, ptr %%format, i64 %%index, i64 %%length, "
            "ptr %%file, i64 %%line, i64 %%column)\n"
            "  call void @__ocean.runtime_diagnostic(ptr %%file, i64 %%line, i64 %%column, ptr %%snippet)\n"
            "  unreachable\n}\n",
        libcSymbol(b, "write"), libcSymbol(b, "write"), panicPrefix.operand, libcSymbol(b, "write"), printName, panicLocation.operand, printName);
    fprintf(b->out,
            "define internal void @__ocean.return_panic(ptr %%message, i64 %%size, ptr %%file, "
            "i64 %%line, i64 %%column, ptr %%snippet) noreturn {\nentry:\n"
            "  call void @__ocean.unwind(ptr %%message, i64 %%size, i1 true)\n"
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
            "  call void @__ocean.panic_report(ptr %%message, i64 %%size, ptr %%caller.file, "
            "i64 %%caller.line, i64 %%caller.column, ptr %%caller.snippet)\n"
            "  unreachable\n"
            "fallback:\n"
            "  call void @__ocean.panic_report(ptr %%message, i64 %%size, ptr %%file, "
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
// Owning recursion needs runtime copy/drop helpers. Reject it before inlining
// ownership operations, whose per-call depth used to reset at collection stores.
static int validateOwnership(Backend *b, Kind kind, Kind *path, unsigned depth) {
    BufferKind *t = bufferKind(b, kind);
    if (!t || t->shape == 6 || t->shape == 3 || t->shape == 17) return 1;
    for (unsigned i = 0; i < depth; i++) if (path[i] == kind) {
        fail(b, "recursive array-value layouts are unsupported; use explicit pointers for recursive ownership");
        return 0;
    }
    if (depth >= 64) { fail(b, "array-value ownership nesting is too deep"); return 0; }
    path[depth++] = kind;
    if (t->definition) return validateOwnership(b, t->underlying, path, depth);
    if (t->shape == 4 || !t->shape) return validateOwnership(b, t->element, path, depth);
    if (t->shape == 2 || t->shape == 5)
        for (size_t i = 0; i < t->count; i++)
            if (!validateOwnership(b, t->members[i], path, depth)) return 0;
    return 1;
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
    Scope scope = {.parent = b->closure && b->closure->lexicalScope ? b->closure->lexicalScope : b->globalScope};
    b->scope = b->functionScope = &scope;
    b->function = f;
    b->terminated = 0;
    b->functionTemps = dynamicArrayTempMark(b);
    if (b->closure && !b->closure->adapter) {
        for (size_t i = 0; i < f->nCaptures; i++) {
            Value field = temp(b, RAW_POINTER), cell = temp(b, RAW_POINTER);
            fprintf(b->body, "  %s = getelementptr { ptr, i64 }, ptr %%environment, i64 %zu, i32 0\n", field.operand, i);
            fprintf(b->body, "  %s = load ptr, ptr %s\n", cell.operand, field.operand);
            Value address = value(b, b->closure->captures[i], "%s", cell.operand);
            address.throughPointer = address.closureCell = 1;
            if (b->closure->captureLengths[i]) {
                Value lengthField = temp(b, RAW_POINTER), length = temp(b, INT);
                fprintf(b->body, "  %s = getelementptr { ptr, i64 }, ptr %%environment, i64 %zu, i32 1\n", lengthField.operand, i);
                fprintf(b->body, "  %s = load i64, ptr %s\n", length.operand, lengthField.operand);
                address.length = length.operand;
            }
            bind(b, f->captures[i].lexeme, address);
        }
    }
    if (b->method && f->receiverType.nTokens) {
        Value addr = localSlot(b, b->method->receiver, f->receiverName.lexeme);
        bind(b, f->receiverName.lexeme, addr);
        b->scope->bindings->autoRelease = !addr.closureCell;
        store(b, addr, value(b, b->method->receiver, "%%receiver"));
    }
    b->terminated = 0;
    b->breakScope = NULL;
    b->breakLabel = b->continueLabel = 0;
    for (size_t j = 0; j < f->nParams; j++) {
        Kind k = parameterKind(b, f, j);
        Value addr = localSlot(b, k, f->paramNames[j].lexeme);
        bind(b, f->paramNames[j].lexeme ? f->paramNames[j].lexeme : "$unused", addr);
        b->scope->bindings->autoRelease = !addr.closureCell;
        store(b, addr, value(b, k, "%%arg%zu", j));
        if (k==SCALAR_ANY) checkElementRequirement(b,load(b,addr),constraintKind(b,f->paramTypeNames[j]));
    }
    for (size_t j = 0; j < f->nResults; j++)
        if (f->resultNames[j].type != EOF_TOKEN) {
            Kind k = type(b, f->resultTypes[j]);
            Value addr = localSlot(b, k, f->resultNames[j].lexeme);
            bind(b, f->resultNames[j].lexeme, addr);
            b->scope->bindings->autoRelease = !addr.closureCell;
            Value initial = emptyValue(b, k);
            store(b, addr, initial);
        }
    b->deferFrame = temp(b, RAW_POINTER);
    fprintf(b->slots, "  %s = alloca %%__ocean.defer_frame, align 16\n", b->deferFrame.operand);
    fprintf(b->body, "  call void @__ocean.enter(ptr %s)\n", b->deferFrame.operand);
    stmt functionBody = {.type = STMT_BLOCK, .value = &f->body};
    int hasDefer = containsDefer(&functionBody);
    unsigned recoverLabel = 0;
    if (hasDefer) {
        Value environment = temp(b, RAW_POINTER), buffer = temp(b, RAW_POINTER), resumed = temp(b, I32), recovering = temp(b, BOOL);
        fprintf(b->slots, "  %s = alloca [%zu x i8], align %zu\n", buffer.operand, sizeof(jmp_buf), (size_t)_Alignof(jmp_buf));
        fprintf(b->body, "  %s = getelementptr %%__ocean.defer_frame, ptr %s, i32 0, i32 3\n", environment.operand, b->deferFrame.operand);
        fprintf(b->body, "  store ptr %s, ptr %s\n", buffer.operand, environment.operand);
        fprintf(b->body, "  %s = call i32 %s(ptr %s%s)\n",
        resumed.operand, libcSymbol(b, deferSetjmpName()), buffer.operand, deferSetjmpArguments());
        fprintf(b->body, "  %s = icmp ne i32 %s, 0\n", recovering.operand, resumed.operand);
        recoverLabel = ++b->label;
        unsigned startLabel = ++b->label;
        branch(b, recovering, recoverLabel, startLabel); label(b, startLabel);
    }
    if (b->threadsUsed) fprintf(b->body, "  call void @ocean_threads_checkpoint()\n");
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
    if (hasDefer) {
        label(b, recoverLabel);
        fprintf(b->body, "  call void @__ocean.finish_recovery(ptr %s)\n", b->deferFrame.operand);
        Kind recoveredKind = resultType(b, f);
        Value recoveredResult = emptyValue(b, recoveredKind);
        if (f->nResults == 1 && f->resultNames[0].type != EOF_TOKEN)
            recoveredResult = loadRecoveredResult(b, find(b, f->resultNames[0].lexeme));
        else if (f->nResults > 1) {
            for (size_t i = 0; i < f->nResults; i++)
                if (f->resultNames[i].type != EOF_TOKEN)
                    recoveredResult = insert(b, recoveredResult, loadRecoveredResult(b, find(b, f->resultNames[i].lexeme)), i);
        }
        if (ownsArrays(b, recoveredKind)) {
            int previous = b->copyTransport; b->copyTransport = 1;
            recoveredResult = copyValue(b, recoveredResult, 0); b->copyTransport = previous;
            escapeDynamicArrays(b, recoveredResult);
        }
        releaseScopeValues(b);
        cleanDynamicArrayTemps(b, b->functionTemps);
        fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr null)\n", boxRegion(b).operand);
        if (recoveredKind == VOID) fprintf(b->body, "  ret void\n");
        else fprintf(b->body, "  ret %s %s\n", irType(b, recoveredKind), recoveredResult.operand);
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
    b->function = NULL; b->functionScope = NULL;
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
    case EXPR_TYPE_SLICE: return globalKindReady(b, &((struct expr_slice *)e->underlying)->parent);
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
// Prepare to a fixed point: dispatched implementations can box new concrete types
// or call further interface methods. Do not freeze adapters until reachability settles.
static int prepareInterfaceDispatch(Backend *b) {
    unsigned before=b->nMethods;
    for (MethodInstance *m=b->methods;m;m=m->next) if (m->interfaceMethod)
        for (AnyType *a=b->anyTypes;a;a=a->next) dispatchTarget(b,m,a->kind);
    return before!=b->nMethods;
}
static void emitInterfaceDispatch(Backend *b, FILE *out) {
    for (MethodInstance *m=b->methods;m;m=m->next) {
        if (!m->interfaceMethod) continue;
        b->typeBindings=m->nBindings ? m->bindings : NULL;
        struct stmt_func *f=m->function;
        Kind result=resultType(b,f);
        fprintf(out,"define %s ",irType(b,result)); methodName(out,m);
        fprintf(out,"({ ptr, i64 } %%receiver");
        for (size_t i=0;i<f->nParams;i++)
            fprintf(out,", %s %%arg%zu",irType(b,parameterKind(b,f,i)),i);
        fputs(") {\nentry:\n  %tag = extractvalue { ptr, i64 } %receiver, 0\n  %bits = extractvalue { ptr, i64 } %receiver, 1\n  %storage = alloca i64\n  store i64 %bits, ptr %storage\n",out);
        unsigned index=0;
        for (AnyType *a=b->anyTypes;a;a=a->next) {
            MethodInstance *target=dispatchTarget(b,m,a->kind);
            if (!target) continue;
            unsigned n=++index;
            fprintf(out,"  %%test%u = icmp eq ptr %%tag, @__ocean.any.type.%u\n  br i1 %%test%u, label %%call%u, label %%next%u\ncall%u:\n",n,(unsigned)a->kind,n,n,n,n);
            BufferKind *t=bufferKind(b,a->kind);
            int aggregate=t && (t->shape==4 || t->shape==5 || t->shape==17 || !t->shape || (t->definition && representationKind(b,a->kind)==SCALAR_ANY));
            if (aggregate)
                fprintf(out,"  %%value%u = call %s @__ocean.any.unbox.%u(i64 %%bits)\n",n,irType(b,a->kind),(unsigned)a->kind);
            else if (representationKind(b,a->kind)==F32)
                fprintf(out,"  %%wide%u = load double, ptr %%storage\n  %%value%u = fptrunc double %%wide%u to float\n",n,n,n);
            else fprintf(out,"  %%value%u = load %s, ptr %%storage\n",n,irType(b,a->kind));
            const char *receiver=value(b,a->kind,"%%value%u",n).operand;
            if (a->kind!=target->receiver) {
                // A boxed pointer also exposes its value-receiver methods.
                fprintf(out,"  %%nonnull%u = icmp ne ptr %s, null\n  br i1 %%nonnull%u, label %%load%u, label %%null%u\nnull%u:\n  call void @llvm.trap()\n  unreachable\nload%u:\n",n,receiver,n,n,n,n,n);
                fprintf(out,"  %%loaded%u = load %s, ptr %s\n",n,irType(b,target->receiver),receiver);
                receiver=value(b,target->receiver,"%%loaded%u",n).operand;
            }
            if (result!=VOID) fprintf(out,"  %%result%u = ",n); else fputs("  ",out);
            fprintf(out,"call %s ",irType(b,result)); methodName(out,target);
            fprintf(out,"(%s %s",irType(b,target->receiver),receiver);
            for (size_t i=0;i<f->nParams;i++)
                fprintf(out,", %s %%arg%zu",irType(b,parameterKind(b,f,i)),i);
            fputs(")\n",out);
            if (result==VOID) fputs("  ret void\n",out);
            else fprintf(out,"  ret %s %%result%u\n",irType(b,result),n);
            fprintf(out,"next%u:\n",n);
        }
        Value message = moduleString(b, "method call on null interface");
        Value filename = moduleString(b, "<interface dispatch>");
        Value snippet = moduleString(b, "");
        fprintf(out, "  call void @__ocean.panic(ptr %s, i64 29, ptr %s, i64 0, i64 0, ptr %s)\n  unreachable\n}\n", message.operand, filename.operand, snippet.operand);
        b->boundsUsed=1;
        b->nullChecksUsed=1;
    }
    b->typeBindings=NULL;
}
// Generate per-concrete-type operations so dynamic any copies retain value semantics.
static void emitAnyType(Backend *b, Kind kind) {
    BufferKind *t = bufferKind(b, kind);
    Value metadata = anyMetadata(b, kind);
    int aggregate = t && (t->shape == 2 || t->shape == 4 || t->shape == 5 || t->shape == 17 || !t->shape || (t->definition && representationKind(b, kind) == SCALAR_ANY));
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
                fprintf(b->body, "  %s = call ptr %s(i64 %s)\n",
        pointer.operand, libcSymbol(b, "malloc"), size.operand);
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
            if (t->shape != 4) fprintf(b->body, "  call void %s(ptr %s)\n",
        libcSymbol(b, "free"), pointer.operand);
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
static void prepareReflectionTypes(Backend *b) {
    // A reflected field or array element may itself contain a struct. Discover
    // this closure before emitting dispatch, without registering unused types.
    if (!b->reflectionUsed && !b->arrayViewUsed) return;
    // Library callers are outside this source graph. Keep their locally known
    // layouts available rather than assuming only local boxing sites can call.
    if (b->program->isLibrary && b->reflectionUsed)
        for (BufferKind *t = b->buffers; t; t = t->next)
            if (t->shape == 5 && t->record)
                reflectionInput(b, t->kind);
    for (;;) {
        AnyType *a = b->anyTypes;
        while (a && a->reflectionPrepared) a = a->next;
        if (!a) break;
        a->reflectionPrepared = 1;
        BufferKind *t = bufferKind(b, a->kind);
        if (!t) continue;
        if (b->reflectionUsed && a->reflectable && t->shape == 5 && t->record && !t->record->enumStorage)
            for (size_t i = 0; i < t->count; i++) reflectionInput(b, t->members[i]);
        if (b->reflectionUsed && a->reflectable && t->record && t->record->enumeration) {
            struct stmt_enum *decl = t->record->enumeration;
            for (size_t i = 0; i < decl->nMembers; i++) {
                if (!decl->forms[i]) continue;
                Kind payload = enumPayloadKind(b, decl, i, t->kind);
                if (decl->forms[i] == 3) reflectionInput(b, payload);
                else {
                    BufferKind *record = bufferKind(b, payload);
                    for (size_t j = 0; j < record->count; j++) reflectionInput(b, record->members[j]);
                }
            }
        }
        if ((t->shape == 4 || t->fixedArray) && !t->immutableString)
            reflectionInput(b, t->element);
    }
}
static void emitReflection(Backend *b) {
    if (!b->reflectionUsed) return;
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
        // Only types reachable through boxed values and their fields are registered.
        for (AnyType *a = b->anyTypes; a; a = a->next) {
            BufferKind *record = bufferKind(b, a->kind);
            if (!a->reflectable || !record || record->shape != 5 || !record->record) continue;
            struct stmt_enum *decl = record->record->enumeration;
            for (size_t member = 0; member < (decl ? decl->nMembers : 1); member++) {
                Value matches = temp(b, BOOL);
                fprintf(b->body, "  %s = icmp eq ptr %s, @__ocean.any.type.%u\n", matches.operand, tag.operand, (unsigned)a->kind);
                unsigned body = ++b->label, next = ++b->label;
                branch(b, matches, body, next); label(b, body);
                Value source = unboxMatched(b, input, a->kind);
                BufferKind *payloadRecord = record;
                int form = decl ? decl->forms[member] : 2;
                if (decl) {
                    Value valid = binary(b, EXPR_EQUALITY, extract(b, source, 0), value(b, INT, "%zu", member));
                    unsigned active = ++b->label;
                    branch(b, valid, active, next); label(b, active);
                    if (form) {
                        Kind payload = enumPayloadKind(b, decl, member, a->kind);
                        Value boxed = extract(b, source, 1);
                        source = payload == SCALAR_ANY ? boxed : unboxMatched(b, boxed, payload);
                        payloadRecord = bufferKind(b, payload);
                    }
                }
                size_t fields = !form ? 0 : form == 3 ? 1 : payloadRecord->count;
                Value result = allocateCollection(b, b->reflectionFieldsKind, value(b, INT, "%zu", fields));
                for (size_t i = 0; i < fields; i++) {
                    const char *name = form == 2 ? payloadRecord->record->fieldNames[i].lexeme : "";
                    size_t length = strlen(name);
                    Value nameValue = allocateCollection(b, field->members[0], value(b, INT, "%zu", length));
                    Value data = arrayData(b, nameValue), text = moduleString(b, name);
                    fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %zu, i1 false)\n", data.operand, text.operand, length);
                    b->dataCopyUsed = 1;
                    Value descriptor = emptyValue(b, field->kind);
                    descriptor = insert(b, descriptor, nameValue, 0);
                    descriptor = insert(b, descriptor, boxScalar(b, form == 3 ? source : extract(b, source, i), 0), 1);
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
        }
        // Other inputs have no fields. Pointers are intentionally not followed.
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
    // Qualified variant names are owned strings, just like reflected field names.
    if (b->reflectionVariantUsed) {
        Scope scope = {.parent = b->globalScope}; b->scope = &scope;
        b->body = tmpfile(); b->slots = tmpfile(); b->boxRegion = (Value){0}; b->terminated = 0;
        DynamicArrayTemporary *mark = dynamicArrayTempMark(b);
        Value input = value(b, SCALAR_ANY, "%%input"), tag = temp(b, RAW_POINTER);
        fprintf(b->body, "  %s = extractvalue { ptr, i64 } %%input, 0\n", tag.operand);
        for (AnyType *a = b->anyTypes; a; a = a->next) {
            BufferKind *record = bufferKind(b, a->kind);
            struct stmt_enum *decl = record && record->record ? record->record->enumeration : NULL;
            if (!a->reflectable || !decl) continue;
            for (size_t member = 0; member < decl->nMembers; member++) {
                Value matches = temp(b, BOOL);
                fprintf(b->body, "  %s = icmp eq ptr %s, @__ocean.any.type.%u\n", matches.operand, tag.operand, (unsigned)a->kind);
                unsigned body = ++b->label, next = ++b->label, active = ++b->label;
                branch(b, matches, body, next); label(b, body);
                Value source = unboxMatched(b, input, a->kind);
                Value valid = binary(b, EXPR_EQUALITY, extract(b, source, 0), value(b, INT, "%zu", member));
                branch(b, valid, active, next); label(b, active);
                const char *owner = decl->displayName ? decl->displayName : decl->name.lexeme;
                if (decl->isBuiltin) owner = !strcmp(owner, OCEAN_BUILTIN_OPTIONAL_NAME) ? "Optional" : "Result";
                size_t length = strlen(owner) + strlen(decl->names[member].lexeme) + 1;
                char *name = checked(length + 1);
                snprintf(name, length + 1, "%s.%s", owner, decl->names[member].lexeme);
                Value result = allocateCollection(b, stringKind(b), value(b, INT, "%zu", length));
                Value data = arrayData(b, result), text = moduleString(b, name); free(name);
                fprintf(b->body, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %zu, i1 false)\n", data.operand, text.operand, length);
                b->dataCopyUsed = 1;
                b->copyTransport = 1; Value returned = copyValue(b, result, 0); b->copyTransport = 0;
                escapeDynamicArrays(b, returned); cleanDynamicArrayTemps(b, mark);
                fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr null)\n", boxRegion(b).operand);
                fprintf(b->body, "  ret ptr %s\n", returned.operand); b->terminated = 1;
                label(b, next);
            }
        }
        Value result = allocateCollection(b, stringKind(b), value(b, INT, "0"));
        b->copyTransport = 1; Value returned = copyValue(b, result, 0); b->copyTransport = 0;
        escapeDynamicArrays(b, returned); cleanDynamicArrayTemps(b, mark);
        fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr null)\n", boxRegion(b).operand);
        fprintf(b->body, "  ret ptr %s\n", returned.operand);
        fprintf(b->out, "define internal ptr @__ocean.reflect.enum_variant({ ptr, i64 } %%input) {\nentry:\n");
        if (copy(b->slots, b->out) || copy(b->body, b->out)) fail(b, "could not emit enum variant reflection");
        fprintf(b->out, "}\n"); fclose(b->slots); fclose(b->body); clearScope(&scope);
    }
    for (int enumeration = 0; enumeration < 2; enumeration++) {
        fprintf(b->out, "define internal i1 @__ocean.reflect.%s({ ptr, i64 } %%input) {\nentry:\n  %%tag = extractvalue { ptr, i64 } %%input, 0\n", enumeration ? "is_enum" : "is_struct");
        unsigned count = 0;
        for (AnyType *a = b->anyTypes; a; a = a->next) {
            BufferKind *record = bufferKind(b, a->kind);
            if (!a->reflectable || !record || record->shape != 5 || !record->record || (!!record->record->enumStorage != enumeration)) continue;
            fprintf(b->out, "  %%m%u = icmp eq ptr %%tag, @__ocean.any.type.%u\n", count, (unsigned)a->kind);
            if (count) fprintf(b->out, "  %%r%u = or i1 %s%u, %%m%u\n", count, count == 1 ? "%m" : "%r", count - 1, count);
            count++;
        }
        if (count) fprintf(b->out, "  ret i1 %s%u\n}\n", count == 1 ? "%m" : "%r", count - 1);
        else fprintf(b->out, "  ret i1 false\n}\n");
    }
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
        if (t->dynamicArray && t->element == SCALAR_ANY)
            view = collectionCopy(b, source, arrayValueKind(b, SCALAR_ANY));
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
    fputs(
        "define internal void @__ocean.array.dynamic.check_write(ptr %array, { ptr, i64 } %value) {\n"
        "entry:\n  %bytes = load i64, ptr %array\n"
        "  %empty = icmp eq i64 %bytes, 0\n"
        "  br i1 %empty, label %done, label %check\n"
        "check:\n  %field = getelementptr { i64, i64, ptr }, ptr %array, i32 0, i32 2\n"
        "  %first = load ptr, ptr %field\n"
        "  %expected = load ptr, ptr %first\n"
        "  %actual = extractvalue { ptr, i64 } %value, 0\n"
        "  %same = icmp eq ptr %expected, %actual\n"
        "  br i1 %same, label %done, label %bad\n"
        "bad:\n  call void @llvm.trap()\n  unreachable\n"
        "done:\n  ret void\n}\n"
        "define internal void @__ocean.array.dynamic.check(ptr %array) {\n"
        "entry:\n  %bytes = load i64, ptr %array\n"
        "  %count = add i64 %bytes, 0\n"
        "  %field = getelementptr { i64, i64, ptr }, ptr %array, i32 0, i32 2\n"
        "  %data = load ptr, ptr %field\n"
        "  br label %loop\n"
        "loop:\n  %i = phi i64 [ 1, %entry ], [ %next, %body ]\n"
        "  %more = icmp slt i64 %i, %count\n"
        "  br i1 %more, label %body, label %done\n"
        "body:\n  %slot = getelementptr { ptr, i64 }, ptr %data, i64 %i\n"
        "  %value = load { ptr, i64 }, ptr %slot\n"
        "  call void @__ocean.array.dynamic.check_write(ptr %array, { ptr, i64 } %value)\n"
        "  %next = add i64 %i, 1\n  br label %loop\n"
        "done:\n  ret void\n}\n", b->out);
}
static void emitAnyRuntime(Backend *b) {
    // Scope cleanup calls are emitted even in functions which never box a value.
    b->returnTransportUsed = 1;
    b->nullChecksUsed = 1;
    for (;;) {
        for (;;) {
            AnyType *entry = NULL;
            for (AnyType *type = b->anyTypes; type; type = type->next) if (!type->emitted) { entry = type; break; }
            if (!entry) break;
            entry->emitted = 1; emitAnyType(b, entry->kind);
        }
        prepareEqualityHelpers(b);
        AnyType *pending = b->anyTypes;
        while (pending && pending->emitted) pending = pending->next;
        if (!pending) break;
    }
    emitBoxedEquality(b);
    emitMapHash(b);
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
        "allocate:\n", b->out);
    fprintf(b->out, "  %%node = call ptr %s(i64 ptrtoint (ptr getelementptr ({ ptr, { ptr, i64 } }, ptr null, i64 1) to i64))\n", libcSymbol(b, "malloc"));
    fputs(
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
        "  call void @__ocean.any.drop({ ptr, i64 } %box)\n", b->out);
    fprintf(b->out, "  call void %s(ptr %%node)\n", libcSymbol(b, "free"));
    fputs(
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
static void emitOSArgs(Backend *b, FILE *out, struct stmt_func *hook) {
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
    functionName(b, out, hook);
    fputs("(ptr %args.stream, i64 %args.count)\n", out);
}

#include "backend_modules.inc"

static int emitLLVMMode(parseResult *program, FILE *output, llvmPackages *units) {
    if (validateBackendTarget()) return 1;
    if (!units) emitTarget(output);
    Scope globals = {0};
    Backend b = {.program = program, .out = output, .globalScope = &globals, .scope = &globals, .units = units};
    for (size_t i = 0; i < program->nStatements; i++) {
        if (program->statements[i].type != STMT_FUNC) continue;
        struct stmt_func *f = program->statements[i].value;
        const char *name = f->externalName ? f->externalName : f->name.lexeme;
        if (f->compilerIntrinsic == INTRINSIC_THREAD_SPAWN || f->compilerIntrinsic == INTRINSIC_THREAD_RETURN ||
            (f->isExtern && !strncmp(name, "ocean_threads_", 14))) b.threadsUsed = b.closuresUsed = 1;
    }
    if (b.threadsUsed && backendWasm()) { fail(&b, "threads and sync are unavailable on WebAssembly"); return 1; }
    if (units) {
        b.nativeNames = hashmap_create();
        if (!b.nativeNames) { fail(&b, "could not create native symbol table"); return 1; }
        b.packageBodies = checked(sizeof(FILE *) * units->count);
        for (size_t i = 1; i < units->count; i++) {
            b.packageBodies[i] = tmpfile();
            if (!b.packageBodies[i]) {
                for (size_t j = 1; j < i; j++) fclose(b.packageBodies[j]);
                free(b.packageBodies); hashmap_free(b.nativeNames);
                fail(&b, "could not create package function buffer"); return 1;
            }
        }
    }
    fprintf(output, "; Generated by ocean-c\n"
                    "@__ocean.null = private global i8 0\n"
                    "%%__ocean.trace = type { ptr, ptr, ptr, i64, i64, ptr, i1 }\n"
                    "%%__ocean.defer_frame = type { ptr, ptr, ptr, ptr, ptr }\n"
                    "%%__ocean.recovery = type { ptr, ptr, i1, { i64, i64, ptr }, ptr }\n"
                    "@__ocean.trace_head = internal thread_local global ptr null\n"
                    "declare ptr @llvm.stacksave.p0()\n"
                    "declare void @llvm.stackrestore.p0(ptr)\n");
    FILE *functions = tmpfile();
    if (!functions) {
        fail(&b, "could not create module function buffer");
        return 1;
    }
    b.deferredOutput = functions;
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
    if (b.errors) { fclose(functions); goto cleanup; }
    b.globalOutput = output;
    emitGlobals(&b, output);
    if (!program->isLibrary) {
        requireFunction(&b, main);
        requireFunction(&b, program->osSetArgs);
    }
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
            if (result && result->shape == 17) fail(&b, "Ocean function values cannot cross the native C ABI");
            else if (result && result->shape == 5)
                fail(&b, "C structs passed or returned by value require ABI lowering; use pointers");
            for (size_t j = 0; j < f->nParams; j++) {
                BufferKind *param = bufferKind(&b, type(&b, f->paramTypeNames[j]));
                if (param && param->shape == 17) fail(&b, "Ocean function values cannot cross the native C ABI");
                else if (param && param->shape == 5)
                    fail(&b, "C structs passed or returned by value require ABI lowering; use pointers");
            }
            signature(&b, f, output);
            fprintf(output, "\n");
            continue;
        }
        if (program->isLibrary || units) requireFunction(&b, f);
    }
    MethodInstance *emittedMethod = NULL;
    ClosureInstance *emittedClosure = NULL;
    for (;;) {
        MethodInstance *method = emittedMethod ? emittedMethod->next : b.methods;
        ClosureInstance *closure = emittedClosure ? emittedClosure->next : b.closures;
        FunctionUse *pending = b.functions;
        while (pending && pending->emitted) pending = pending->next;
        if (pending) {
            pending->emitted = 1; // Mark before lowering recursive calls.
            b.closure = NULL; b.method = NULL; b.typeBindings = NULL;
            FILE *destination = output;
            if (units) {
                size_t owner = 0;
                for (size_t i = 0; i < program->nSources; i++)
                    if (pending->function->name.filename && !strcmp(pending->function->name.filename, program->sources[i].filename))
                        owner = program->sources[i].packageId;
                for (size_t i = 0; i < program->nPackages; i++)
                    if (program->packages[i].id == owner) destination = b.packageBodies[i + 1];
            }
            emitFunction(&b, pending->function, destination);
        } else if (method) {
            b.closure = NULL; b.method = method;
            b.typeBindings = method->nBindings ? method->bindings : NULL;
            if (!method->interfaceMethod) emitFunction(&b, method->function, output);
            emittedMethod = method;
        } else if (closure) {
            b.closure = closure; b.method = NULL;
            b.typeBindings = closure->nBindings ? closure->bindings : NULL;
            if (closure->adapter) emitClosureAdapter(&b, closure, output);
            else emitFunction(&b, closure->function, output);
            emittedClosure = closure;
        } else {
            prepareReflectionTypes(&b);
            if (!prepareInterfaceDispatch(&b)) break;
        }
    }
    emitInterfaceDispatch(&b, output);
    b.closure = NULL;
    emitClosureCellDrops(&b, output);
    b.method = NULL;
    b.typeBindings = NULL;
    if (!program->isLibrary && main && !main->isExtern && !main->nParams && main->nResults <= 1) {
        b.body = tmpfile();
        b.slots = tmpfile();
        b.function = main;
        if (!b.body || !b.slots)
            fail(&b, "could not create entry point buffers");
        else {
            if (program->osSetArgs) emitOSArgs(&b, b.body, program->osSetArgs);
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
    prepareReflectionTypes(&b);
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
        fprintf(output, "declare i32 %s(ptr, ptr, i64)\n",
        libcSymbol(&b, "memcmp"));
    if (b.dataCopyUsed)
        fprintf(output, "declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n"
                        "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)\n");
    if (b.newUsed) fprintf(output, "declare ptr %s(i64, i64)\n",
        libcSymbol(&b, "calloc"));
    {
        // Deferred captures and boxed values use libc allocation. Distinct IR
        // names avoid conflicts with user declarations of the same symbols.
        fprintf(output, "declare ptr %s(i64)\n"
                        "declare void %s(ptr)\n",
        libcSymbol(&b, "malloc"), libcSymbol(&b, "free"));
    }
    emitFFIRuntime(&b);
    if (b.nullChecksUsed || b.ffiUsed) fprintf(output, "declare void @llvm.trap() cold noreturn nounwind\n");
    emitClosureRuntime(&b);
    emitThreadRuntime(&b);
    emitDeferRuntime(&b);
    emitBoundsRuntime(&b);
    if (fflush(output) || ferror(output))
        fail(&b, "could not write LLVM IR");
    if (units && !b.errors) finishNativeModules(&b, output);
cleanup:
    if (units) {
        for (size_t i = 1; i < units->count; i++) fclose(b.packageBodies[i]);
        free(b.packageBodies);
        hashmap_free(b.nativeNames);
        while (b.nativeNameStorage) {
            NativeName *next = b.nativeNameStorage->next;
            free(b.nativeNameStorage->from); free(b.nativeNameStorage->to); free(b.nativeNameStorage); b.nativeNameStorage = next;
        }
    }
    clearScope(&globals);
    while (b.dynamicArrayTemps) {
        DynamicArrayTemporary *next = b.dynamicArrayTemps->next;
        free(b.dynamicArrayTemps); b.dynamicArrayTemps = next;
    }
    while (b.equalities) { EqualityKind *next = b.equalities->next; free(b.equalities); b.equalities = next; }
    while (b.diagnostics) {
        BackendDiagnostic *next = b.diagnostics->next;
        free(b.diagnostics); b.diagnostics = next;
    }
    while (b.constraints) {
        TypeConstraint *next = b.constraints->next;
        free(b.constraints); b.constraints = next;
    }
    while (b.functions) {
        FunctionUse *next = b.functions->next;
        free(b.functions); b.functions = next;
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
        if (b.buffers->shape == 17 && !b.buffers->definition) free(b.buffers->callable);
        free(b.buffers->layout);
        free(b.buffers->ir);
        free(b.buffers);
        b.buffers = next;
    }
    while (b.closures) {
        ClosureInstance *next = b.closures->next;
        Scope *scope = b.closures->lexicalScope;
        while (scope && scope != b.globalScope) { Scope *parent = scope->parent; clearScope(scope); free(scope); scope = parent; }
        free(b.closures->captures); free(b.closures->captureLengths); free(b.closures->bindings); free(b.closures); b.closures = next;
    }
    while (b.closureCells) {
        ClosureCellType *next = b.closureCells->next;
        free(b.closureCells); b.closureCells = next;
    }
    while (b.methods) {
        MethodInstance *next = b.methods->next;
        if (b.methods->interfaceMethod) free(b.methods->function);
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
int emitLLVM(parseResult *program, FILE *output) {
    return emitLLVMMode(program, output, NULL);
}
int emitLLVMPackages(parseResult *program, llvmPackages *units) {
    FILE *support = tmpfile();
    if (!support) { perror("package support buffer"); return 1; }
    int failed = emitLLVMMode(program, support, units);
    fclose(support); return failed;
}
/* Stage beside the destination so a failed compiler/linker preserves old output.
 * exec arguments are passed directly; paths never become shell commands. */
// Build the embedded platform runtime only for programs with native threading.
// The runtime travels in the compiler executable; no installation path lookup
// or system-wide runtime library is needed.
static char *compileThreadRuntime(void) {
    if (backendWasm()) { fprintf(stderr, "backend error: native threads are unavailable on WebAssembly\n"); return NULL; }
    char source[4096]; oceanTemporary(source, sizeof(source), "ocean-thread-runtime-XXXXXX");
    int fd = mkstemp(source);
    if (fd < 0) { perror("thread runtime source"); return NULL; }
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(source); return NULL; }
    int failed = fputs(oceanThreadRuntimeSource, file) == EOF;
    if (fclose(file)) failed = 1;
    char *object = checked(strlen(source) + 3); sprintf(object, "%s.o", source);
    char *args[] = {"clang", "-std=c11", "-O2", "-ffunction-sections", "-fdata-sections", "-x", "c", "-c", source, "-o", object, NULL};
    if (!failed) failed = oceanRun(args);
    unlink(source);
    if (failed) { unlink(object); free(object); return NULL; }
    return object;
}

static char *compileProcessRuntime(void) {
    if (backendWasm()) { fprintf(stderr, "backend error: native processes are unavailable on WebAssembly\n"); return NULL; }
    char source[4096]; oceanTemporary(source, sizeof(source), "ocean-process-runtime-XXXXXX");
    int fd = mkstemp(source);
    if (fd < 0) { perror("process runtime source"); return NULL; }
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(source); return NULL; }
    int failed = fputs(oceanProcessRuntimeSource, file) == EOF;
    if (fclose(file)) failed = 1;
    char *object = checked(strlen(source) + 3); sprintf(object, "%s.o", source);
    char *args[] = {"clang", "-std=c11", "-O2", "-ffunction-sections", "-fdata-sections", "-x", "c", "-c", source, "-o", object, NULL};
    if (!failed) failed = oceanRun(args);
    unlink(source);
    if (failed) { unlink(object); free(object); return NULL; }
    return object;
}

/* Discover the full dependency closure when statically linking libcurl. */
static char **staticCurlFlags(size_t *count) {
    *count = 0;
#ifdef _WIN32
    FILE *pipe = _popen("pkg-config --static --libs libcurl", "r");
#else
    FILE *pipe = popen("pkg-config --static --libs libcurl", "r");
#endif
    if (!pipe) return NULL;
    char buffer[32768]; size_t used = fread(buffer, 1, sizeof(buffer)-1, pipe); buffer[used] = 0;
#ifdef _WIN32
    int status = _pclose(pipe);
#else
    int status = pclose(pipe);
#endif
    if (status || !used || used == sizeof(buffer)-1) { fputs("backend error: static HTTP linking requires pkg-config and libcurl's static development dependencies\n", stderr); return NULL; }
    char **result = checked(512 * sizeof(*result));
    char *at = buffer;
    while (*at) {
        while (isspace((unsigned char)*at)) at++;
        if (!*at) break;
        if (*count == 511) goto invalid;
        char token[32768], *out = token; char quote = 0;
        while (*at && (quote || !isspace((unsigned char)*at))) {
            if ((*at == '\'' || *at == '"') && (!quote || quote == *at)) { quote = quote ? 0 : *at; at++; }
            else if (*at == '\\' && at[1] && quote != '\'') { at++; *out++ = *at++; }
            else *out++ = *at++;
        }
        if (quote) goto invalid;
        *out = 0; result[(*count)++] = strdup(token);
    }
    if (*count) return result;
invalid:
    for (size_t i = 0; i < *count; i++) free(result[i]); free(result); return NULL;
}
static void freeCurlFlags(char **flags, size_t count) { for (size_t i = 0; i < count; i++) free(flags[i]); free(flags); }

#ifdef _WIN32
// COFF has no relocatable link. Keep runtime and generated objects together in
// an indexed archive, flattening previous archives when packaging libraries.
static int archiveWindowsObjects(const char *output, const char **objects, size_t count) {
    char **args = checked((count + 5) * sizeof(*args));
    args[0] = "llvm-ar"; args[1] = "qcLs"; args[2] = (char *)output;
    for (size_t i = 0; i < count; i++) args[i + 3] = (char *)objects[i];
    args[count + 3] = NULL;
    // mkstemp reserves the output path; llvm-ar needs an absent or valid archive.
    unlink(output);
    int failed = oceanRun(args); free(args);
    if (failed) fputs("backend error: llvm-ar failed to package COFF objects\n", stderr);
    return failed;
}
#endif

static int compileLLVMMode(const char *irPath, const char *outputPath, int object, const char *objectPath, int staticLink) {
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
    int needsFFI = 0, needsThreads = 0, needsCurl = 0, needsProcess = 0, ownsProcessRuntime = 0, ownsThreadRuntime = 0;
    FILE *input = fopen(irPath, "r");
    if (input) {
        char line[1024];
        while (fgets(line, sizeof(line), input)) {
            if (strstr(line, "@ffi_type_")) needsFFI = 1;
            if (!strncmp(line, "declare ", 8) &&
                (strstr(line, "@\"curl_") || strstr(line, "@curl_"))) needsCurl = 1;
            if (strstr(line, "ocean_process_")) needsProcess = 1;
            if (strstr(line, "define hidden void @ocean_runtime_process_marker")) { ownsProcessRuntime = 1; needsProcess = 1; }
            if (strstr(line, "@ocean_threads_")) needsThreads = 1;
            if (strstr(line, "define hidden void @ocean_runtime_thread_invoke")) ownsThreadRuntime = 1;
        }
        fclose(input);
    }
    char *threadObject = needsThreads && (!object || ownsThreadRuntime) ? compileThreadRuntime() : NULL;
    if (needsThreads && (!object || ownsThreadRuntime) && !threadObject) { unlink(staged); free(staged); return 1; }
    char *processObject = needsProcess && (!object || ownsProcessRuntime) ? compileProcessRuntime() : NULL;
    if (needsProcess && (!object || ownsProcessRuntime) && !processObject) { if (threadObject) { unlink(threadObject); free(threadObject); } unlink(staged); free(staged); return 1; }
    int coffObject = 0;
#ifdef _WIN32
    coffObject = object && !backendWasm();
#endif
    size_t curlCount = 0; char **curlFlags = needsCurl && staticLink && !object ? staticCurlFlags(&curlCount) : NULL;
    if (needsCurl && staticLink && !object && !curlFlags) { if (processObject) { unlink(processObject); free(processObject); } if (threadObject) { unlink(threadObject); free(threadObject); } unlink(staged); free(staged); return 1; }
        char **args = checked((48 + curlCount) * sizeof(*args)); size_t n = 0;
        args[n++] = "clang"; args[n++] = "-Wno-override-module";
        if (backendWasm()) {
            args[n++] = "--target=wasm32-unknown-unknown";
            // Package support can contain very large reflection dispatchers.
            // Keep their compilation bounded; user/package code needs register
            // coloring to stay within JavaScript's per-function local limit.
            const char *base = strrchr(irPath, '/'); base = base ? base + 1 : irPath;
            if (strcmp(base, "support.ll")) { args[n++] = "-O1"; args[n++] = "-fno-inline"; }
            if (!object) { args[n++] = "-nostdlib"; args[n++] = "-Wl,--no-entry"; args[n++] = "-Wl,--export=main"; args[n++] = "-Wl,--allow-undefined"; }
        }
        args[n++] = "-ffunction-sections"; args[n++] = "-fdata-sections";
        if (!object) {
#if defined(__APPLE__)
            args[n++] = backendWasm() ? "-Wl,--gc-sections" : "-Wl,-dead_strip";
#else
            args[n++] = "-Wl,--gc-sections";
#endif
        }
        if (staticLink && !object) args[n++] = "-static";
        if (objectPath) args[n++] = (char *)objectPath;
        else {
            if (object) {
                if (!coffObject && (threadObject || processObject)) {
                    args[n++] = "-r"; args[n++] = "-nostdlib";
                } else args[n++] = "-c";
            }
            args[n++] = "-x"; args[n++] = "ir"; args[n++] = (char *)irPath;
        }
        if (threadObject && !coffObject) { args[n++] = "-x"; args[n++] = "none"; args[n++] = threadObject; }
        if (processObject && !coffObject) { args[n++] = "-x"; args[n++] = "none"; args[n++] = processObject; }
        args[n++] = "-o"; args[n++] = staged;
        if (!object) {
#if defined(_WIN32)
            if (!backendWasm()) { args[n++] = "-lkernel32"; args[n++] = "-lshell32"; args[n++] = "-Wl,--stack,8388608"; }
#endif
            if (needsFFI && !backendWasm()) args[n++] = "-lffi";
            if (needsCurl && !backendWasm()) args[n++] = "-lcurl";
#if !defined(_WIN32)
            if (needsThreads && !backendWasm()) args[n++] = "-pthread";
#endif
        }
        for (size_t i = 0; i < curlCount; i++) args[n++] = curlFlags[i];
        args[n] = NULL;
    int failed = oceanRun(args);
#ifdef _WIN32
    if (!failed && coffObject && (threadObject || processObject)) {
        char *archive = checked(strlen(staged) + 3); sprintf(archive, "%s.a", staged);
        const char *objects[3] = {staged}; size_t count = 1;
        if (threadObject) objects[count++] = threadObject;
        if (processObject) objects[count++] = processObject;
        failed = archiveWindowsObjects(archive, objects, count);
        if (!failed) { unlink(staged); failed = rename(archive, staged) != 0; }
        unlink(archive); free(archive);
    }
#endif
    free(args); freeCurlFlags(curlFlags, curlCount);
    if (processObject) { unlink(processObject); free(processObject); }
    if (failed == 127) fputs("could not execute clang (install clang and add it to PATH)\n", stderr);
    if (failed) {
        fprintf(stderr, "backend error: clang failed to compile or link LLVM IR\n");
        failed = 1;
    } else if (rename(staged, outputPath)) {
        perror("could not save executable");
        failed = 1;
    }
    unlink(staged);
    free(staged);
    if (threadObject) { unlink(threadObject); free(threadObject); }
    return failed;
}

int compileLLVM(const char *irPath, const char *outputPath) {
    return compileLLVMExecutable(irPath, outputPath, 0);
}
int compileLLVMExecutable(const char *irPath, const char *outputPath, int staticLink) {
    return compileLLVMMode(irPath, outputPath, 0, NULL, staticLink);
}
int compileLLVMObject(const char *irPath, const char *outputPath) {
    return compileLLVMMode(irPath, outputPath, 1, NULL, 0);
}

int linkLLVMObject(const char *objectPath, const char *irPath, const char *outputPath) {
    return compileLLVMMode(irPath, outputPath, 0, objectPath, 0);
}

int checkBackend(parseResult *program) {
    llvmPackages units = {0};
    if (createLLVMPackages(program, &units)) { perror("ocean check: temporary directory"); return 1; }
    int failed = emitLLVMPackages(program, &units);
    char **objects = checked(sizeof(char *) * units.count);
    for (size_t i = 0; !failed && i < units.count; i++) {
        objects[i] = checked(strlen(units.paths[i]) + 3);
        sprintf(objects[i], "%s.o", units.paths[i]);
        failed = compileLLVMObject(units.paths[i], objects[i]);
    }
    char *output = checked(strlen(units.directory) + 16);
    sprintf(output, "%s/check.out", units.directory);
    if (!failed) failed = linkLLVMObjects((const char **)objects, units.count, units.needsFFI, units.needsThreads, units.needsCurl, units.needsProcess, output, program->isLibrary, 0);
    if (failed) diagnostic(program->sourceText, (token){.filename=(char *)program->sourceFilename,.line=1,.column=1},
                           "backend", "error", "LLVM compilation or linking failed during check");
    for (size_t i = 0; i < units.count; i++) if (objects[i]) { unlink(objects[i]); free(objects[i]); }
    free(objects); unlink(output); free(output); freeLLVMPackages(&units); return failed;
}

int linkLLVMObjects(const char **objects, size_t count, int ffi, int threads, int curl, int processes, const char *output, int library, int staticLink) {
#ifdef _WIN32
    if (library && !backendWasm()) {
        char *staged = checked(strlen(output) + 16); sprintf(staged, "%s.XXXXXX", output);
        int fd = mkstemp(staged);
        if (fd < 0) { perror("could not create library output"); free(staged); return 1; }
        close(fd);
        int failed = archiveWindowsObjects(staged, objects, count);
        if (!failed && rename(staged, output)) { perror("could not save library"); failed = 1; }
        unlink(staged); free(staged); return failed;
    }
#endif
    char *staged=checked(strlen(output)+16);sprintf(staged,"%s.XXXXXX",output);
    int fd=mkstemp(staged);if(fd<0){perror("could not create output");free(staged);return 1;}close(fd);
    (void)processes; /* The support package object owns the process ABI. */
    char *processObject = NULL;
    size_t curlCount = 0; char **curlFlags = curl && staticLink && !library ? staticCurlFlags(&curlCount) : NULL;
    if (curl && staticLink && !library && !curlFlags) { if (processObject) { unlink(processObject); free(processObject); } unlink(staged); free(staged); return 1; }
    char **args=checked(sizeof(char*)*(count+32+curlCount));size_t n=0;
    args[n++]="clang";args[n++]="-Wno-override-module";
    if (backendWasm()) {
        args[n++]="--target=wasm32-unknown-unknown";
        if (!library) { args[n++]="-nostdlib"; args[n++]="-Wl,--no-entry"; args[n++]="-Wl,--export=main"; args[n++]="-Wl,--allow-undefined"; }
    }
    if (!library) {
#if defined(__APPLE__)
        args[n++] = backendWasm() ? "-Wl,--gc-sections" : "-Wl,-dead_strip";
#else
        args[n++] = "-Wl,--gc-sections";
#endif
    }
    if(staticLink&&!library)args[n++]="-static";
    if(library){args[n++]="-r";args[n++]="-nostdlib";}
    for(size_t i=0;i<count;i++)args[n++]=(char*)objects[i];
    if (processObject) args[n++] = processObject;
    args[n++]="-o";args[n++]=staged;if(ffi&&!library&&!backendWasm())args[n++]="-lffi";
    if(curl&&!library&&!backendWasm())args[n++]="-lcurl";
#if defined(_WIN32)
    if(!library&&!backendWasm()){args[n++]="-lkernel32";args[n++]="-lshell32";args[n++]="-Wl,--stack,8388608";}
#endif
#if !defined(_WIN32)
    if (threads && !library && !backendWasm()) args[n++]="-pthread";
#endif
    for (size_t i = 0; i < curlCount; i++) args[n++] = curlFlags[i];
    args[n]=NULL;
    int failed = oceanRun(args);
    freeCurlFlags(curlFlags, curlCount);
    if (processObject) { unlink(processObject); free(processObject); }
    if(failed)fprintf(stderr,"backend error: clang failed to link package objects\n");
    else if(rename(staged,output)){perror("could not save output");failed=1;}
    unlink(staged);free(staged);free(args);return failed;
}
