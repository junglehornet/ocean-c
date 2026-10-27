#include "../parser.c"
#include "../scanner.h"
#include <assert.h>

static parseResult parse(char *source) {
    tokenlist *tokens = scanTokens(source, "parser-edges");
    parseResult result = parseTokens(tokens);
    free(tokens);
    return result;
}
static parseResult valid(char *source, size_t count) {
    parseResult result = parse(source);
    if (result.hasError) fprintf(stderr, "Unexpected error: %s\n", source);
    assert(!result.hasError && result.nStatements == count);
    return result;
}
static void invalid(char *source) { assert(parse(source).hasError); }
static expr expression(char *source) {
    parseResult result = valid(source, 1);
    assert(result.statements[0].type == STMT_EXPR);
    return *(expr *)result.statements[0].value;
}
int main(void) {
    struct { char *source; enum unaryExprType op; } unary[] = {
        {"&x;", UNARY_ADDRESS_OF}, {"*x;", UNARY_DEREFERENCE}, {"!x;", UNARY_LOGICAL_NOT},
        {"~x;", UNARY_BITWISE_NOT}, {"-x;", UNARY_NEGATE}, {"++x;", UNARY_PREFIX_INCREMENT},
        {"--x;", UNARY_PREFIX_DECREMENT}, {"x++;", UNARY_POSTFIX_INCREMENT},
        {"x--;", UNARY_POSTFIX_DECREMENT}, {"sizeof(x);", UNARY_SIZEOF}
    };
    for (size_t i = 0; i < sizeof(unary)/sizeof(*unary); i++) {
        expr e = expression(unary[i].source);
        assert(e.type == EXPR_TYPE_UNARY);
        struct expr_unary *node = e.underlying;
        assert(node->operation == unary[i].op && node->child.type == EXPR_TYPE_IDENTIFIER);
    }
    struct { char *source; enum binaryExprType op; } binary[] = {
        {"a ^ b;", EXPR_XOR}, {"a & b;", EXPR_BITWISE_AND}, {"a <= b;", EXPR_LESS_EQUALS},
        {"a >= b;", EXPR_GREATER_EQUALS}, {"a == b;", EXPR_EQUALITY}, {"a != b;", EXPR_NOT_EQUALS},
        {"a += b;", EXPR_ADD_EQUALS}, {"a -= b;", EXPR_SUBTRACT_EQUALS},
        {"a *= b;", EXPR_MULTIPLY_EQUALS}, {"a /= b;", EXPR_DIVIDE_EQUALS},
        {"a %= b;", EXPR_MODULO_EQUALS}, {"a ^= b;", EXPR_XOR_EQUALS},
        {"a <<= b;", EXPR_BITSHIFT_LEFT_EQUALS}, {"a >>= b;", EXPR_BITSHIFT_RIGHT_EQUALS},
        {"a &= b;", EXPR_BITWISE_AND_EQUALS}, {"a |= b;", EXPR_BITWISE_OR_EQUALS}
    };
    for (size_t i = 0; i < sizeof(binary)/sizeof(*binary); i++) {
        expr e = expression(binary[i].source);
        assert(e.type == EXPR_TYPE_BINARY);
        struct expr_binary *node = e.underlying;
        assert(node->operation == binary[i].op && node->left.type == EXPR_TYPE_IDENTIFIER &&
               node->right.type == EXPR_TYPE_IDENTIFIER);
    }
    expr e = expression("1.25;");
    assert(e.type == EXPR_TYPE_LITERAL && ((struct expr_literal*)e.underlying)->type == LITERAL_TYPE_FLOAT &&
           *(double*)((struct expr_literal*)e.underlying)->value == 1.25);
    e = expression("a ? b : c ? d : e;");
    assert(e.type == EXPR_TYPE_TERNARY && ((struct expr_ternary*)e.underlying)->ifFalse.type == EXPR_TYPE_TERNARY);
    parseResult result = valid("x, y: int = 1, 2;", 1);
    struct stmt_var *vars = result.statements[0].value;
    assert(vars->nVars == 2 && vars->nInitValues == 2 && vars->type.nTokens == 1);
    result = valid("if (x := 1; x > 0) x++; else if (y) y--; else z++;", 1);
    struct stmt_if *branch = result.statements[0].value;
    assert(branch->preStmt->type == STMT_VAR && branch->condition->type == EXPR_TYPE_BINARY &&
           branch->ifTrue->type == STMT_EXPR && branch->ifFalse->type == STMT_IF);
    branch = branch->ifFalse->value;
    assert(!branch->preStmt && branch->ifFalse->type == STMT_EXPR);
    result = valid("for (;;) {} for (x) {} for (defer x) {} "
                   "for (i := 0; defer i < n; i++) {} "
                   "for (item := range items) {}", 5);
    struct stmt_for *loop = result.statements[0].value;
    assert(!loop->initializer && !loop->condition && !loop->update && !loop->deferCondition);
    loop = result.statements[1].value;
    assert(loop->condition && !loop->deferCondition);
    loop = result.statements[2].value;
    assert(loop->condition && loop->deferCondition);
    loop = result.statements[3].value;
    assert(loop->initializer && loop->condition && loop->update && loop->deferCondition);
    loop = result.statements[4].value;
    assert(loop->nBindings == 1 && loop->range && !strcmp(loop->bindings[0].lexeme, "item"));
    result = valid("func f() { outer: for (;;) { break outer; continue outer; } "
                   "goto done; done: return; }", 1);
    struct stmt_func *function = result.statements[0].value;
    struct stmt_label *label = function->body.stmts[0].value;
    loop = label->body->value;
    struct stmt_block *body = loop->body->value;
    assert(body->stmts[0].type == STMT_BREAK && body->stmts[1].type == STMT_CONTINUE &&
           !strcmp(((struct stmt_branch*)body->stmts[0].value)->label.lexeme, "outer"));
    assert(!strcmp(((struct stmt_branch*)function->body.stmts[1].value)->label.lexeme, "done"));
    result = valid("switch { case 1: case 2: break; default: }", 1);
    struct stmt_switch *sw = result.statements[0].value;
    assert(sw->nCases == 3 && !sw->cases[0].body.nStmts && !sw->cases[2].body.nStmts);
    e = expression("Point{};");
    assert(e.type == EXPR_TYPE_COMPOSITE && !((struct expr_composite*)e.underlying)->nEntries);
    valid("type A [-1 + !N]int; type B [~N]int; type C [+N]int;", 3);
    invalid("sizeof(x;"); invalid("&;"); invalid("*;"); invalid("!;"); invalid("~;");
    invalid("++;"); invalid("--;"); invalid("a ? b;"); invalid("a ? : b;");
    invalid("if (x := 1; ) {}"); invalid("if (x) {} else +;");
    invalid("for (a, b, c := range xs) {}"); invalid("for (item := range) {}");
    invalid("for (defer) {}"); invalid("defer f();");
    invalid("func (p Point) M() (int p) {}");
    invalid("import \"io\""); invalid("import (\"io\", \"math\"");
    invalid("type A [pkg.]int;");
    assert(parseTokens(NULL).hasError);
    // Exercise recursion rejection with a real unary expression, not a
    // synthetic parser-state value that merely mirrors the guard.
    char *deep = malloc(PARSER_MAX_RECURSION_DEPTH * 2 + 3);
    memset(deep, '!', PARSER_MAX_RECURSION_DEPTH * 2);
    strcpy(deep + PARSER_MAX_RECURSION_DEPTH * 2, "x;");
    invalid(deep); free(deep);
    puts("parser expression, statement, AST, and error checks passed");
}
