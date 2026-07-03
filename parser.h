#ifndef ocean_c_parser
#define ocean_c_parser

#include <stddef.h>
#include "dynarray.h"
#include "ast.h"

#include "c-hashmap/map.h"

typedef struct parseResult parseResult;
typedef struct scope scope;

struct scope {
    scope *parent;
    hashmap *variables;
};


struct parseResult {
    size_t nStatements;
    stmt *statements;

    char *packageName;

    char hasError;
};

parseResult parseTokens(tokenlist *tokens);

#endif
