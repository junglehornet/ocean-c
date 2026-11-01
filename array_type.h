#ifndef OCEAN_ARRAY_TYPE_H
#define OCEAN_ARRAY_TYPE_H
#include "buffer_type.h"

/* A buffer<T>[] remains a raw buffer; buffer<T>[N][] is an array of buffers. */
static inline int arrayElementType(typeTokens t, typeTokens *element) {
    if (t.nTokens && t.tokens[0].type == MIXED) { t.tokens++; t.nTokens--; }
    // A leading pointer applies to the complete array type: *T[] is a pointer
    // to an array, while T*[] is an array whose elements are pointers.
    if (t.nTokens && t.tokens[0].type == STAR)
        return 0;
    if (t.nTokens < 3 || t.tokens[t.nTokens - 2].type != LEFT_BRACKET ||
        t.tokens[t.nTokens - 1].type != RIGHT_BRACKET)
        return 0;
    typeTokens ignored, dimension;
    if (bufferTypeParts(t, &ignored, &dimension))
        return 0;
    *element = (typeTokens){t.nTokens - 2, t.tokens};
    return 1;
}
/* Split the outer sized suffix, preserving nested dimensions and generic types. */
static inline int sizedArrayTypeParts(typeTokens t, typeTokens *element, typeTokens *dimension) {
    if (t.nTokens < 4 || t.tokens[0].type == STAR ||
        t.tokens[t.nTokens - 1].type != RIGHT_BRACKET) return 0;
    typeTokens ignored;
    if (bufferTypeParts(t, &ignored, dimension)) return 0;
    size_t close = t.nTokens - 1, depth = 1, end = close;
    while (close && depth) {
        close--;
        if (t.tokens[close].type == RIGHT_BRACKET) depth++;
        if (t.tokens[close].type == LEFT_BRACKET) depth--;
    }
    if (depth || !close || end == close + 1) return 0;
    *element = (typeTokens){close, t.tokens};
    *dimension = (typeTokens){end - close - 1, t.tokens + close + 1};
    return 1;
}
static inline int arrayHasRuntimeDimension(typeTokens t) {
    typeTokens element, dimension;
    if (!sizedArrayTypeParts(t, &element, &dimension)) return 0;
    return bufferDimensionKind(dimension) == 2 || arrayHasRuntimeDimension(element);
}
/* array<T> erases only the length of an immutable-size array. */
static inline int unsizedArrayTypeParts(typeTokens t, typeTokens *element) {
    if (t.nTokens < 4 || t.tokens[0].type != IDENTIFIER ||
        strcmp(t.tokens[0].lexeme, "array") || t.tokens[1].type != LESS_THAN ||
        t.tokens[t.nTokens - 1].type != GREATER_THAN) return 0;
    size_t depth = 0;
    for (size_t i = 2; i + 1 < t.nTokens; i++) {
        enum tokenType k = t.tokens[i].type;
        if (k == LESS_THAN) depth++;
        else if (k == GREATER_THAN) { if (!depth) return 0; depth--; }
        else if (k == COMMA && !depth) return 0;
    }
    if (depth) return 0;
    *element = (typeTokens){t.nTokens - 3, t.tokens + 2};
    return 1;
}
static inline int builtinStorageTypeName(const char *name) {
    const char *names[] = {"int", "uint", "byte", "rune",  "i8",  "i16", "i32",  "i64",   "u8",
                           "u16", "u32",  "u64",  "float", "f32", "f64", "bool", "string", "any"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}
#endif
