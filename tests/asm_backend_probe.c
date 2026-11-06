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
    const char *names[] = {"data", "count", "written"};
    Binding bindings[3] = {0};
    Scope scope = {.bindings=bindings};
    Backend backend = {.body=stdout, .scope=&scope, .program=&program};
    for (size_t i=0; i<3; i++) {
        bindings[i].name = names[i];
        bindings[i].address = value(&backend, i == 0 ? RAW_POINTER : INT, "%%%s", names[i]);
        bindings[i].next = i < 2 ? &bindings[i+1] : NULL;
    }
    puts("define i64 @sample(ptr %p, i64 %n) noinline noredzone {");
    puts("%data = alloca ptr\n%count = alloca i64\n%written = alloca i64");
    puts("store ptr %p, ptr %data\nstore i64 %n, ptr %count\nstore i64 -4096, ptr %written");
    emitAssembly(&backend, node, node->architecture.lexeme);
    puts("%result = load i64, ptr %written\nret i64 %result\n}");
    return backend.errors != 0;
}
