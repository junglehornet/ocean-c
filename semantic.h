#ifndef OCEAN_SEMANTIC_H
#define OCEAN_SEMANTIC_H
#include "parser.h"

/* Analyze a parsed file or merged package graph, reporting diagnostics to stderr. Ambiguous bracket
 * expressions are rewritten to index nodes when their argument names a value.
 * The AST and its token storage must remain alive throughout this call. */
typedef struct semanticResult {
    size_t nErrors;
} semanticResult;
semanticResult analyzeSemantics(parseResult *program);
#endif
