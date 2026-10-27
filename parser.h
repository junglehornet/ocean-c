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
    const char *sourceText; // Borrowed source, used to embed runtime diagnostics.
    const char *sourceFilename;
};

parseResult parseTokens(tokenlist *tokens);
expr *parseBufferDimension(typeTokens dimension);
char parseTypeApplicationArguments(typeTokens span, typeTokens **arguments, size_t *count);

#endif
