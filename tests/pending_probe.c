// A narrow adapter for pending syntax/scanner/printer contracts. Return 10 only
// for a checked, unmet contract; crashes and setup failures are ordinary errors.
#define _POSIX_C_SOURCE 200809L
#include "../parser.h"
#include "../scanner.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __unix__
#include <sys/resource.h>
#endif

static int missing(const char *reason) { printf("CONTRACT_MISSING: %s\n", reason); return 10; }
static char *read_source(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) exit(2);
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET)) exit(2);
    char *source = calloc((size_t)length + 1, 1);
    if (!source || fread(source, 1, (size_t)length, file) != (size_t)length) exit(2);
    fclose(file);
    return source;
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef __unix__
    struct rlimit no_core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &no_core)) return 2;
#endif
    char *source = read_source(argv[2]);
    tokenlist *tokens = scanTokens(source, argv[2]);
    if (!strcmp(argv[1], "scanner")) {
        token first = tokens->arr[0];
        char ok = 0;
        if (!strcmp(argv[3], "omega")) ok = first.type == LITERAL_STRING && !strcmp(first.lexeme, "\xce\xa9");
        else if (!strcmp(argv[3], "wave")) ok = first.type == LITERAL_STRING && !strcmp(first.lexeme, "\xf0\x9f\x8c\x8a");
        else if (!strcmp(argv[3], "unicode_identifier")) ok = first.type == IDENTIFIER && !strcmp(first.lexeme, "\xce\xb1");
        else if (!strcmp(argv[3], "exponent")) ok = first.type == LITERAL_FLOAT && first.numValue.floating == 125.0;
        else if (!strcmp(argv[3], "reject")) ok = 0; // Returning from scanning violates rejection.
        else return 2;
        free_tokenlist(tokens); free(tokens); free(source);
        return ok ? 0 : missing("scanner value/token contract not satisfied");
    }
    parseResult result = parseTokens(tokens);
    free(tokens); free(source);
    if (result.hasError) return missing("parser rejected the acceptance fixture");
    if (!result.nStatements) return missing("acceptance fixture produced no statements");
    stmt first = result.statements[0];
    expr *expression = first.type == STMT_EXPR ? first.value : NULL;
    if (first.type == STMT_VAR) {
        struct stmt_var *var = first.value;
        if (var->nInitValues == 1) expression = &var->initValues[0];
    }
    if (!strcmp(argv[1], "debug")) {
        if (!expression) return missing("fixture did not yield an expression");
        printAst(*expression);
        return 0;
    }
    return 2;
}
