#ifndef OCEAN_VALUE_LIST_H
#define OCEAN_VALUE_LIST_H
#include "ast.h"
static inline int isComma(expr *e) {
    return e->type == EXPR_TYPE_BINARY &&
           ((struct expr_binary *)e->underlying)->operation == EXPR_COMMA;
}
static inline size_t commaCount(expr *e) {
    if (!isComma(e))
        return 1;
    struct expr_binary *v = e->underlying;
    return commaCount(&v->left) + commaCount(&v->right);
}
static inline expr *commaItem(expr *e, size_t index) {
    if (!isComma(e))
        return e;
    struct expr_binary *v = e->underlying;
    size_t left = commaCount(&v->left);
    return index < left ? commaItem(&v->left, index) : commaItem(&v->right, index - left);
}
static inline int isDiscard(expr *e) {
    return e->type == EXPR_TYPE_IDENTIFIER &&
           ((struct expr_identifier *)e->underlying)->lexeme.type == UNDERSCORE;
}
#endif
