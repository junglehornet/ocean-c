// Exercise the real backend emitter for foreign targets without adding a cross-build CLI.
#include "../backend.c"
#include "../scanner.h"

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END)) return 2;
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET)) return 2;
    char *source = calloc((size_t)length + 1, 1);
    if (!source || fread(source, 1, (size_t)length, file) != (size_t)length) return 2;
    fclose(file);
    parseResult program = parseTokensWithSource(scanTokens(source, argv[1]), source);
    if (program.hasError || program.nStatements != 1 || program.statements[0].type != STMT_FUNC) return 2;
    struct stmt_func *function = program.statements[0].value;
    if (function->body.nStmts != 1 || function->body.stmts[0].type != STMT_ASM) return 2;
    struct stmt_asm *node = function->body.stmts[0].value;
    Binding *bindings = calloc(node->nOperands + 1, sizeof(*bindings));
    if (!bindings) return 2;
    Scope scope = {.bindings=node->nOperands ? bindings : NULL};
    Backend backend = {.body=stdout, .scope=&scope, .program=&program};
    puts("define i64 @sample(ptr %p, i64 %n) noinline noredzone {");
    const char *resultSlot = NULL;
    for (size_t i=0; i<node->nOperands; i++) {
        const char *name = node->operands[i].lexeme;
        int pointer = !strcmp(name, "data") || !strcmp(name, "timeout");
        bindings[i].name = name;
        bindings[i].address = value(&backend, pointer ? RAW_POINTER : INT, "%%operand%zu", i);
        bindings[i].next = i + 1 < node->nOperands ? &bindings[i+1] : NULL;
        fprintf(stdout, "%%operand%zu = alloca %s\n", i, pointer ? "ptr" : "i64");
        if (pointer) fprintf(stdout, "store ptr %%p, ptr %%operand%zu\n", i);
        else fprintf(stdout, "store i64 %s, ptr %%operand%zu\n",
                     !strcmp(name, "count") ? "%n" :
                     (!strcmp(name, "written") || !strcmp(name, "result")) ? "-4096" : "0", i);
        if (!strcmp(name, "written") || !strcmp(name, "result")) resultSlot = bindings[i].address.operand;
    }
    emitAssembly(&backend, node, node->architecture.lexeme);
    if (resultSlot) fprintf(stdout, "%%answer = load i64, ptr %s\nret i64 %%answer\n}\n", resultSlot);
    else puts("ret i64 0\n}");
    free(bindings);
    return backend.errors != 0;
}
