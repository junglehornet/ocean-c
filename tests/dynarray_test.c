#define _POSIX_C_SOURCE 200809L
#include "../core.h"
#include "../dynarray.h"
#include "test_support.h"
#include <signal.h>

// Exercise the public contract for every generated list type, including
// growth, insertion order, snapshot independence, shrinking, and reuse.
#define CHECK_LIST(type, make, equal) do { \
    type##list list = {0}; \
    init_##type##list(&list); \
    for (size_t i = 0; i < 40; i++) assert(push_##type##list(&list, make(i)) == i); \
    insert_##type##list(&list, make(90), 0); \
    insert_##type##list(&list, make(91), 21); \
    insert_##type##list(&list, make(92), listlen(&list)); \
    assert(listlen(&list) == 43); \
    type *snapshot = to_##type##array(&list); \
    for (size_t i = 0; i < 43; i++) { \
        size_t value = i == 0 ? 90 : i == 21 ? 91 : i == 42 ? 92 : i < 21 ? i - 1 : i - 2; \
        assert(equal(get_##type##list(&list, i), make(value))); \
        assert(equal(snapshot[i], make(value))); \
    } \
    for (size_t i = 43; i > 0; i--) { \
        assert(equal(pop_##type##list(&list), snapshot[i - 1])); \
        assert(listlen(&list) == i - 1); \
    } \
    push_##type##list(&list, make(7)); \
    assert(equal(snapshot[0], make(90))); \
    free(snapshot); \
    init_##type##list(&list); \
    assert(listlen(&list) == 0); \
    free_##type##list(&list); \
    assert(!list.arr); \
    init_##type##list(&list); \
    push_##type##list(&list, make(8)); \
    assert(equal(get_##type##list(&list, 0), make(8))); \
    free_##type##list(&list); \
} while (0)

#define CHAR(i) ((char)(i))
#define EQ(a,b) ((a) == (b))
#define TOKEN(i) ((token){.type = IDENTIFIER, .numValue.integer = (i)})
#define TOKEN_EQ(a,b) ((a).numValue.integer == (b).numValue.integer)
#define STMT(i) ((stmt){.type = (enum stmtType)(i)})
#define STMT_EQ(a,b) ((a).type == (b).type)
#define EXPR(i) ((expr){.type = (enum exprType)(i)})
#define EXPR_EQ(a,b) ((a).type == (b).type)
static char values[100][4];
#define STR(i) values[i]

static int badOperation;
static void invalid_list(void) {
    charlist list = {0};
    if (badOperation == 0) { init_charlist(NULL); return; }
    if (badOperation == 1) { push_charlist(&list, 'x'); return; }
    init_charlist(&list);
    switch (badOperation) {
        case 2: pop_charlist(&list); break;
        case 3: get_charlist(&list, 0); break;
        case 4: insert_charlist(&list, 'x', 1); break;
        case 5: list.top = list.len + 1; push_charlist(&list, 'x'); break;
        case 6: list.len = -1; get_charlist(&list, 0); break;
        case 7: list.top = -1; pop_charlist(&list); break;
        case 8: free_charlist(&list); insert_charlist(&list, 'x', 0); break;
    }
}
static int badType;
static void invalid_typed_list(void) {
#define EMPTY_POP(type) do { type##list list = {0}; init_##type##list(&list); pop_##type##list(&list); } while (0)
    switch (badType) {
        case 0: EMPTY_POP(token); break;
        case 1: EMPTY_POP(stmt); break;
        case 2: EMPTY_POP(expr); break;
        case 3: EMPTY_POP(str); break;
    }
#undef EMPTY_POP
}
int main(void) {
    CHECK_LIST(char, CHAR, EQ);
    CHECK_LIST(token, TOKEN, TOKEN_EQ);
    CHECK_LIST(stmt, STMT, STMT_EQ);
    CHECK_LIST(expr, EXPR, EXPR_EQ);
    CHECK_LIST(str, STR, EQ);
    for (badOperation = 0; badOperation <= 8; badOperation++) {
        int status;
        char *output = capture_child(invalid_list, &status);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        assert(strstr(output, "Error:"));
        free(output);
    }
    for (badType = 0; badType < 4; badType++) {
        int status;
        char *output = capture_child(invalid_typed_list, &status);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        assert(strstr(output, "empty"));
        free(output);
    }
    puts("dynamic array lifecycle and error checks passed");
}
