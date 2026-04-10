#ifndef ocean_c_dynarray
#define ocean_c_dynarray

#include <stddef.h>
#include <memory.h>

#include "token.h"

#define GROW_DYNAMIC_ARRAY(list, type) \
    list->top >= list->len ? (list->len *= 2, (type *)realloc(list->arr, list->len * sizeof(type))) : list->arr

#define SHRINK_DYNAMIC_ARRAY(list, type) \
    list->top <= (list->len / 2) ? (list->len /= 2, (type *)realloc(list->arr, list->len * sizeof(type))) : list->arr

#define DEFINE_DYNAMIC_ARRAY_TYPE(type) \
    typedef struct { \
        type *arr;\
        long top;\
        long len;\
    } type##list; \
    \
    void init_##type##list(type##list *list);\
    type pop_##type##list(type##list *list);\
    void push_##type##list(type##list *list, type item);\
    void insert_##type##list(type##list *list, type item, size_t index);\
    type get_##type##list(type##list *list, size_t index);\
    void free_##type##list(type##list *list);\
    type *to_##type##array(type##list *list);\

#define IMPLEMENT_DYNAMIC_ARRAY_TYPE(type, default) \
    static int isValid_##type##list(type##list *list) {\
        if (list->arr == NULL) {\
            return 0;\
        }\
        \
        if (list->len < 0) {\
            return 0;\
        }\
        \
        if (list->top < 0) {\
            return 0;\
        }\
        \
        if (list->top > list->len) {\
            return 0;\
        }\
        return 1;\
    }\
    \
    static type error_##type(const char *message) {\
        printf("\033[31m%s\033[0m\n", message);\
        if (ABORT_ON_ERROR) abort();\
        return default;\
    }\
    \
    void init_##type##list(type##list *list) {\
        if (list == NULL) {\
            error_##type("Error: tried to init a " #type " list with an invalid pointer");\
        }\
        if (list->arr != NULL) {\
            free_##type##list(list);\
        }\
        list->arr = (type *)calloc(8, sizeof(type));\
        list->top = 0;\
        list->len = 8;\
    }\
    \
    type pop_##type##list(type##list *list) {\
        if (!isValid_##type##list(list)) {\
            return error_##type("Error: tried to pop an invalid " #type " list");\
        }\
        if (list->top == 0) {\
            return error_##type("Error: tried to pop an empty " #type " list");\
        }\
        type value = list->arr[--list->top];\
        list->top--;\
        list->arr = SHRINK_DYNAMIC_ARRAY(list, type);\
        return value;\
    }\
    \
    void push_##type##list(type##list *list, type item) {\
        if (!isValid_##type##list(list)) {\
            error_##type("Error: tried to push to an invalid " #type " list");\
            return;\
        }\
        list->arr = GROW_DYNAMIC_ARRAY(list, type);\
        list->arr[list->top++] = item;\
    }\
    \
    void insert_##type##list(type##list *list, type item, size_t index) {\
        if (!isValid_##type##list(list)) {\
            error_##type("Error: tried to insert to an empty " #type " list");\
            return;\
        }\
        \
        if (index > list->top || index < 0) {\
            error_##type("Error: tried to insert an item of type " #type " at an invalid index");\
            return;\
        }\
        list->top++;\
        list->arr = GROW_DYNAMIC_ARRAY(list, type);\
        type previous = item;\
        type temp;\
        for (size_t i = index; index < list->top; index++) {\
            temp = list->arr[index];\
            list->arr[index] = previous;\
            previous = temp;\
        }\
    }\
    \
    type get_##type##list(type##list *list, size_t index) {\
        if (!isValid_##type##list(list)) {\
            return error_##type("Error: tried to get item from an invalid " #type " list");\
        }\
        \
        if (index >= list->top || index < 0) {\
            return error_##type("Error: tried to get an item of type " #type " at an invalid index");\
        }\
        return list->arr[index];\
    }\
    \
    void free_##type##list(type##list *list) {\
        free(list->arr);\
        list->arr = NULL;\
        list->top = -1;\
        list->len = -1;\
    }\
    \
    type *to_##type##array(type##list *list) {\
        type *arr = malloc(sizeof(type) * list->top);\
        if (!arr) {\
            printf("Fatal compiler error: could not allocate memory for to_" #type "array().");\
            exit(1);\
        }\
        memcpy(arr, list->arr, list->top);\
        return arr;\
    }

DEFINE_DYNAMIC_ARRAY_TYPE(token)
DEFINE_DYNAMIC_ARRAY_TYPE(char)
    
#endif
