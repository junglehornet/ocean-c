#ifndef OCEAN_SEMANTIC_H
#define OCEAN_SEMANTIC_H
#include "parser.h"

/* Analyze a parsed file or merged package graph, reporting diagnostics to stderr. Ambiguous bracket
 * expressions are rewritten to index nodes when their argument names a value.
 * The AST and its token storage must remain alive throughout this call. */
typedef struct semanticResult {
    size_t nErrors;
} semanticResult;
typedef struct {
    token declaration, typeDeclaration, owner;
    char *name, *type;
    size_t packageId, begin, end;
    int global, member, kind;
} semanticSymbol;
typedef struct {
    token use;
    size_t symbol;
    char *type;
} semanticReference;
typedef struct {
    token use;
    size_t *symbols;
    size_t count;
    int collection, immutable;
} semanticMembers;
typedef struct {
    semanticSymbol *symbols;
    size_t nSymbols;
    semanticReference *references;
    size_t nReferences;
    semanticMembers *members;
    size_t nMembers;
} semanticIndex;
semanticResult analyzeSemantics(parseResult *program);
semanticResult indexSemantics(parseResult *program, semanticIndex *index);
void freeSemanticIndex(semanticIndex *index);
#endif
