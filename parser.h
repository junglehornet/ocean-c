#ifndef ocean_c_parser
#define ocean_c_parser

#include <stddef.h>
#include "dynarray.h"
#include "ast.h"
#include "builtin_result.h"
#include <string.h>

#include "c-hashmap/map.h"

typedef struct parseResult parseResult;
typedef struct scope scope;

struct scope {
    scope *parent;
    hashmap *variables;
};


typedef struct sourceFile {
    const char *filename, *text;
    size_t packageId; // Zero outside a merged package graph.
} sourceFile;

struct parseResult {
    size_t nStatements;
    size_t statementsCapacity;
    char builtinsPrepared;
    stmt *statements;

    char *packageName;

    char hasError;
    const char *sourceText; // Borrowed source, used to embed runtime diagnostics.
    const char *sourceFilename;
    size_t nSources;
    sourceFile *sources;
    int isLibrary;
    struct stmt_func *osSetArgs; // Trusted standard-library startup hook, borrowed from the AST.
};

static inline const char *programSource(parseResult *program, const char *filename) {
    if (filename && !strcmp(filename, OCEAN_BUILTIN_RESULT_FILE)) return oceanBuiltinResultSource();
    if (filename)
        for (size_t i = 0; i < program->nSources; i++) {
            const char *a = program->sources[i].filename, *b = filename;
            while (*a && *a == *b) { a++; b++; }
            if (!*a && !*b) return program->sources[i].text;
        }
    return program->sourceText;
}

parseResult parseTokens(tokenlist *tokens);
parseResult parseTokensWithSource(tokenlist *tokens, const char *source);
struct stmt_interface *parseInterfaceType(typeTokens span);
struct stmt_func *parseFunctionType(typeTokens span);
expr *parseBufferDimension(typeTokens dimension);
char parseTypeApplicationArguments(typeTokens span, typeTokens **arguments, size_t *count);

#endif
