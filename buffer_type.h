#ifndef OCEAN_BUFFER_TYPE_H
#define OCEAN_BUFFER_TYPE_H

#include "ast.h"
#include <string.h>

static inline int isBufferType(typeTokens t) {
    return t.nTokens && t.tokens[0].type == IDENTIFIER && !strcmp(t.tokens[0].lexeme, "buffer");
}

/* Split buffer<T>, buffer<T>[], and buffer<T>[expression]. */
static inline int bufferTypeParts(typeTokens t, typeTokens *element, typeTokens *dimension) {
    if (!isBufferType(t) || t.nTokens < 4 || t.tokens[1].type != LESS_THAN)
        return 0;
    size_t close = t.nTokens - 1;
    *dimension = (typeTokens){0};
    if (t.tokens[close].type == RIGHT_BRACKET) {
        size_t end = close, depth = 1;
        while (close && depth) {
            close--;
            if (t.tokens[close].type == RIGHT_BRACKET)
                depth++;
            if (t.tokens[close].type == LEFT_BRACKET)
                depth--;
        }
        if (depth || !close)
            return 0;
        *dimension = (typeTokens){end - close - 1, t.tokens + close + 1};
        close--;
    }
    if (t.tokens[close].type != GREATER_THAN || close < 3)
        return 0;
    *element = (typeTokens){close - 2, t.tokens + 2};
    size_t depth = 0;
    for (size_t i = 0; i < element->nTokens; i++) {
        enum tokenType kind = element->tokens[i].type;
        if (kind == LESS_THAN || kind == LEFT_BRACKET || kind == LEFT_PAREN ||
            kind == LEFT_CURLY_BRACE)
            depth++;
        else if (kind == GREATER_THAN || kind == RIGHT_BRACKET || kind == RIGHT_PAREN ||
                 kind == RIGHT_CURLY_BRACE) {
            if (!depth)
                return 0;
            depth--;
        } else if (kind == COMMA && !depth)
            return 0;
    }
    return !depth;
}

/* Unsized = 0, fixed = 1, runtime expression = 2, invalid literal = -1. */
static inline int bufferDimensionKind(typeTokens dimension) {
    if (!dimension.nTokens)
        return 0;
    if (dimension.nTokens == 1 && dimension.tokens[0].type == LITERAL_INT)
        return dimension.tokens[0].numValue.integer > 0 ? 1 : -1;
    if (dimension.nTokens == 2 && dimension.tokens[0].type == MINUS &&
        dimension.tokens[1].type == LITERAL_INT)
        return -1;
    return 2;
}
#endif
