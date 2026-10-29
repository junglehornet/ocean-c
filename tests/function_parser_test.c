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
    stmt result = state.tokens[0].type == EXTERN ? parseExtern(&state) : parseFunc(&state);
    assert(result.type == STMT_NULL && state.hasError && state.isSynchronizing);
    free_tokenlist(tokens);
    free(tokens);
    checks++;
}
int main(void) {
    valid("func casts(x buffer<i8>) { a := x->buffer<i8>; b := a->(buffer<i8>[]); }");
    struct stmt_func *implicit = valid("func myFunc<T>(thing T) {}");
    assert(implicit->nTypeParams == 1 && implicit->typeParamConstraints[0].nTokens == 1);
    assert(implicit->typeParamConstraints[0].tokens[0].type == ANY &&
           !strcmp(implicit->typeParamConstraints[0].tokens[0].lexeme, "any"));
    implicit = valid("func grouped<T, U,>(a T, b U) {}");
    assert(implicit->nTypeParams == 2 && implicit->typeParamConstraints[1].tokens[0].type == ANY);
    implicit = valid("func grouped<T ~int, U>(a T, b U) {}");
    assert(implicit->typeParamConstraints[0].tokens[0].type == TILDE &&
           implicit->typeParamConstraints[1].tokens[0].type == ANY);
    valid("func trailing<T, U any, V,>(value V) {}");
    valid("func arrays(a mixed any[], b any[]) mixed any[] {}");
    invalid("func mixed() {}");
    invalid("func bad(a mixed any) {}");
    invalid("func bad<T,,>() {}");
    invalid("func bad<T, T>() {}");
    invalid("func bad<T:>() {}");
    struct stmt_func *method = valid("func (n *Node<T>) append(value T) {}");
    assert(!strcmp(method->receiverName.lexeme, "n"));
    assert(method->receiverType.nTokens == 5 && method->nTypeParams == 0);
    assert(method->nReceiverTypeParams == 1 &&
           !strcmp(method->receiverTypeParamNames[0].lexeme, "T"));
    method = valid("func (p Pair<A, B>) convert<U any>(value U) A {}");
    assert(method->nReceiverTypeParams == 2 && method->nTypeParams == 1);
    valid("func (n Node[T]) get() T {}");
    valid("func (n Node<T>*) get() T {}");
    invalid("func (*Node<T> n) append(value T) {}");
    invalid("func (n *Node<T>) append<T any>(value T) {}");
    invalid("func (n Pair<T, T>) get() {}");
    invalid("func (T Node<T>) get() {}");
    invalid("func (n Node<*T>) get() {}");
    invalid("func (n Node<Box<T>>) get() {}");
    invalid("func (n ...Node) get() {}");
    invalid("func (a Node, b Node) get() {}");
    invalid("func (Node) get() {}");
    struct stmt_func *f = valid("func main() {}");
    assert(!strcmp(f->name.lexeme, "main") && !f->nParams && !f->nResults && !f->body.nStmts);
    f = valid("extern void empty(void);");
    assert(f->isExtern && !f->nParams && !f->nResults);
    f = valid("extern func() int factory(void);");
    assert(f->isExtern && !f->nParams && f->nResults == 1);
    assert(f->resultTypes[0].nTokens == 4 && f->resultTypes[0].tokens[0].type == FUNC);
    f = valid("extern i32 puts(string text);");
    assert(f->isExtern && f->nParams == 1 && f->nResults == 1);
    assert(!strcmp(f->name.lexeme, "puts") && !strcmp(f->paramNames[0].lexeme, "text"));
    assert(!strcmp(f->paramTypeNames[0].tokens[0].lexeme, "string"));
    assert(!strcmp(f->resultTypes[0].tokens[0].lexeme, "i32"));
    f = valid("extern void notify();"); assert(!f->nParams && !f->nResults);
    f = valid("extern i32 compare(string, string other);");
    assert(f->nParams == 2 && f->paramNames[0].type == EOF_TOKEN);
    f = valid("extern i32 printf(string format, ...);"); assert(f->isVariadic && f->nParams == 1);
    f = valid("extern i32 probe(string* text);"); assert(f->paramTypeNames[0].nTokens == 2);
    invalid("extern func puts(text string) i32;");
    invalid("extern i32 f(void) {}"); invalid("extern i32 f(void)");
    invalid("extern i32 f(void, int x);"); invalid("extern i32 f(void x);");
    invalid("extern i32 f(int x,);"); invalid("extern i32 f(...);");
    invalid("extern i32 f(int x, ..., int y);");
    invalid("extern i32 f(int x, int x);"); invalid("extern i32 f(int x int y);");
    f = valid("func add(a int, b int) int { return a + b; }");
    assert(f->nParams == 2 && f->nResults == 1 && f->type.nTokens == 1);
    assert(!strcmp(f->paramNames[0].lexeme, "a") && !strcmp(f->paramTypeNames[1].tokens[0].lexeme, "int"));
    assert(f->body.stmts[0].type == STMT_RETURN);
    f = valid("func grouped(a, b int, c string,) (int x, y, string z) { return 1, 2, c; }");
    assert(f->nParams == 3 && f->nResults == 3 && f->paramTypeNames[1].nTokens == 1);
    assert(!strcmp(f->resultNames[1].lexeme, "y"));
    assert(((struct stmt_return*)f->body.stmts[0].value)->nValues == 3);
    f = valid("func unnamed(int, string) (int, string,) {}");
    assert(f->paramNames[0].type == EOF_TOKEN && f->resultNames[1].type == EOF_TOKEN);
    f = valid("func blanks(_ int, _ string) {}"); assert(f->nParams == 2);
    f = valid("func sum(label string, values ...int) int { return 0 }");
    assert(f->isVariadic && f->paramTypeNames[1].nTokens == 1);
    f = valid("func variadic(...int,) {}"); assert(f->isVariadic);
    f = valid("func identity[T any](value T) T { return value; }");
    assert(f->nTypeParams == 1 && !strcmp(f->typeParamNames[0].lexeme, "T"));
    f = valid("func generic[T, U any, V ~int | ~string,](a T, b U, c V) (T, U) {}");
    assert(f->nTypeParams == 3 && f->typeParamConstraints[2].nTokens == 5);
    f = valid("func constraint[T interface { ~int | ~string; }](value T) {}"); assert(f->nTypeParams == 1);
    f = valid("func types(p *pkg.Type, xs []int, names [4]string, matrix int[3], box Box[int]) *pkg.Type {}");
    assert(f->nParams == 5 && f->resultTypes[0].nTokens == 4);
    f = valid("func maps(items map[string][]int, events chan int) {}"); assert(f->nParams == 2);
    f = valid("func callback(cb func(x int, y int) int) {}"); assert(f->paramTypeNames[0].nTokens == 9);
    f = valid("func callback(cb func() ()) {}");
    assert(f->nParams == 1 && !strcmp(f->paramNames[0].lexeme, "cb"));
    f = valid("func named() (int total) { total = 1; return; }"); assert(f->body.nStmts == 2);
    f = valid("func loop(n int) { for (i := 0; i < n; i++) { n--; } }"); assert(f->body.stmts[0].type == STMT_FOR);
    f = valid("func calls() int { io.Println(1); return next(); }"); assert(f->body.nStmts == 2);
    f = valid("func element(xs []int) int { return xs[0]; }"); assert(f->nResults == 1);
    f = valid("func pair[T any](value T) (T, T) { return value, value; }"); assert(f->nResults == 2);
    f = valid("func nested(cb func(a int) (int, string)) {}"); assert(f->nParams == 1);
    parseResult r = parseTokens(scanTokens("func a() {} func b() {}", "adjacent-functions"));
    assert(!r.hasError && r.nStatements == 2); checks++;
    invalid("func () {}"); invalid("func f {}"); invalid("func f(a int b int) {}");
    invalid("func f(a int, a int) {}"); invalid("func f([]int, b string) {}");
    invalid("func f(a ...int, b int) {}"); invalid("func f() (...int) {}");
    valid("func f[T]() {}"); invalid("func f[]() {}"); invalid("func f[T any, T any]() {}");
    invalid("func f(a int) (int a) {}"); invalid("func f(a int) int");
    invalid("func f(int a"); invalid("func f() {"); invalid("func f(a [3 int) {}");
    invalid("func f() { return 1,; }"); invalid("func f() int int {}");
    r = parseTokens(scanTokens("func broken(x int y int) {}", "public-error"));
    assert(r.hasError); checks++;
    r = parseTokens(scanTokens("func broken() { +; }", "body-error"));
    assert(r.hasError); checks++;
    printf("%zu function parser checks passed\n", checks);
}
