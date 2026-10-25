// Run from the repository root; see tests/README.md.
#include "../parser.c"
#include "../scanner.h"
#include <assert.h>

static size_t checks;
static struct stmt_func *valid(char *source) {
    parseResult result = parseTokens(scanTokens(source, "function-test"));
    assert(!result.hasError && result.nStatements == 1);
    assert(result.statements[0].type == STMT_FUNC);
    checks++;
    return result.statements[0].value;
}
static void invalid(char *source) {
    tokenlist *tokens = scanTokens(source, "invalid-function-test");
    parserState state = {.tokens = tokens->arr, .tokensLen = tokens->top};
    stmt result = parseFunc(&state);
    assert(result.type == STMT_NULL && state.hasError && state.isSynchronizing);
    free_tokenlist(tokens);
    free(tokens);
    checks++;
}
int main(void) {
    struct stmt_func *f = valid("func main() {}");
    assert(!strcmp(f->name.lexeme, "main") && !f->nParams && !f->nResults && !f->body.nStmts);
    f = valid("func add(int a, int b) int { return a + b; }");
    assert(f->nParams == 2 && f->nResults == 1 && f->type.nTokens == 1);
    assert(!strcmp(f->paramNames[0].lexeme, "a") && !strcmp(f->paramTypeNames[1].tokens[0].lexeme, "int"));
    assert(f->body.stmts[0].type == STMT_RETURN);
    f = valid("func grouped(int a, b, string c,) (int x, y, string z) { return 1, 2, c; }");
    assert(f->nParams == 3 && f->nResults == 3 && f->paramTypeNames[1].nTokens == 1);
    assert(!strcmp(f->resultNames[1].lexeme, "y"));
    assert(((struct stmt_return*)f->body.stmts[0].value)->nValues == 3);
    f = valid("func unnamed(int, string) (int, string,) {}");
    assert(f->paramNames[0].type == EOF_TOKEN && f->resultNames[1].type == EOF_TOKEN);
    f = valid("func blanks(int _, string _) {}"); assert(f->nParams == 2);
    f = valid("func sum(string label, ...int values) int { return 0 }");
    assert(f->isVariadic && f->paramTypeNames[1].nTokens == 1);
    f = valid("func variadic(...int,) {}"); assert(f->isVariadic);
    f = valid("func identity[T any](T value) T { return value; }");
    assert(f->nTypeParams == 1 && !strcmp(f->typeParamNames[0].lexeme, "T"));
    f = valid("func generic[T, U any, V ~int | ~string,](T a, U b, V c) (T, U) {}");
    assert(f->nTypeParams == 3 && f->typeParamConstraints[2].nTokens == 5);
    f = valid("func constraint[T interface { ~int | ~string; }](T value) {}"); assert(f->nTypeParams == 1);
    f = valid("func types(*pkg.Type p, []int xs, [4]string names, int[3] matrix, Box[int] box) *pkg.Type {}");
    assert(f->nParams == 5 && f->resultTypes[0].nTokens == 4);
    f = valid("func maps(map[string][]int items, chan int events) {}"); assert(f->nParams == 2);
    f = valid("func callback(func(int x, int y) int cb) {}"); assert(f->paramTypeNames[0].nTokens == 9);
    f = valid("func callback(func() () cb) {}");
    assert(f->nParams == 1 && !strcmp(f->paramNames[0].lexeme, "cb"));
    f = valid("func named() (int total) { total = 1; return; }"); assert(f->body.nStmts == 2);
    f = valid("func loop(int n) { for (i := 0; i < n; i++) { n--; } }"); assert(f->body.stmts[0].type == STMT_FOR);
    f = valid("func calls() int { io.Println(1); return next(); }"); assert(f->body.nStmts == 2);
    f = valid("func element([]int xs) int { return xs[0]; }"); assert(f->nResults == 1);
    f = valid("func pair[T any](T value) (T, T) { return value, value; }"); assert(f->nResults == 2);
    f = valid("func nested(func(int a) (int, string) cb) {}"); assert(f->nParams == 1);
    parseResult r = parseTokens(scanTokens("func a() {} func b() {}", "adjacent-functions"));
    assert(!r.hasError && r.nStatements == 2); checks++;
    invalid("func () {}"); invalid("func f {}"); invalid("func f(int a int b) {}");
    invalid("func f(int a, int a) {}"); invalid("func f(int, string b) {}");
    invalid("func f(...int a, int b) {}"); invalid("func f() (...int) {}");
    invalid("func f[T]() {}"); invalid("func f[]() {}"); invalid("func f[T any, T any]() {}");
    invalid("func f(int a) (int a) {}"); invalid("func f(int a) int");
    invalid("func f(int a"); invalid("func f() {"); invalid("func f([3 int a) {}");
    invalid("func f() { return 1,; }"); invalid("func f() int int {}");
    r = parseTokens(scanTokens("func broken(int x int y) {}", "public-error"));
    assert(r.hasError); checks++;
    r = parseTokens(scanTokens("func broken() { +; }", "body-error"));
    assert(r.hasError); checks++;
    printf("%zu function parser checks passed\n", checks);
}
