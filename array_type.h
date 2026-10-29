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
static inline int builtinStorageTypeName(const char *name) {
    const char *names[] = {"int", "uint", "byte", "rune",  "i8",  "i16", "i32",  "i64",   "u8",
                           "u16", "u32",  "u64",  "float", "f32", "f64", "bool", "string", "any"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}
#endif
