#include "core.h"
#include "dynarray.h"
#include "token.h"
#include "stdio.h"
#include <assert.h>
#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "scanner.h"

void printTokensFromFile(char *filename, tokenlist *list) {
    printf("Warning: printTokensFromFile() called. If you are a user and this is an official build\n"
           "of the Ocean compiler then this is a bug. Please report this bug to the Ocean developers.\n\n");

    printf("--==[%s]==--\n", filename);
    for (size_t t = 0; t < list->top; t++) {
        printToken(get_tokenlist(list, t));
    }
}

#define NKEYWORDS (sizeof(keywords) / sizeof(keywords[0]))
#define KEYWORD_MAX_LEN 16

const enum tokenType keyword_types[] = {
    //INT, FLOAT, I8, I16, I32, U8, U16, U32, U64,
    /*F32, STRING,*/ TYPE, STRUCT, FUNC, IMPORT,
    ANY, INTERFACE, CONTINUE, BREAK, ENUM, IF, DEFER,
    SWITCH, FALLTHROUGH, EXTERN, FOR, GOTO, AS,
    PACKAGE, ELSE, RANGE, RETURN, CASE, DEFAULT,
};

const char *keywords[] = {
    //"int", "float", "i8", "i16", "i32", "u8", "u16", "u32", "u64",
    /*"f32", "string",*/ "type", "struct", "func", "import",
    "any", "interface", "continue", "break", "enum", "if", "defer",
    "switch", "fallthrough", "extern", "for", "goto", "as",
    "package", "else", "range", "return", "case", "default",
};

unsigned long hashes[NKEYWORDS] = {};

static unsigned long hash_str(char *str) {
    unsigned long hash = 5381;

    for (size_t i = 0; str[i] != '\0'; i++) {
        hash = ((hash << 5) + hash) ^ str[i]; /* hash * 33 ^ c */
    }

    return hash;
}

static void populate_keyword_hashes() {
    for (size_t i = 0; i < NKEYWORDS; i++) {
        char str[KEYWORD_MAX_LEN];
        strcpy(str, keywords[i]);
        hashes[i] = hash_str(str);
    }

    // check for hash collisions between keywords, if one exists, abort()
    for (size_t i = 0; i < NKEYWORDS; i++) {
        for (size_t j = i + 1; j < NKEYWORDS; j++) {
            if (hashes[i] == hashes[j]) {
                printf("Fatal compiler error: hash collision for keywords detected.\n");
                printf("This error is critical. If you are a user and you encounter this\n"
                       "error, the build you are using is broken. Download a newer version\n"
                       "of the Ocean compiler to fix this issue. If there is no newer\n"
                       "version, please report this bug to the Ocean developers immediately.\n");
                abort();
            }
        }
    }
}

static enum tokenType get_keyword_type(char *lexeme) {
    unsigned long hash = hash_str(lexeme);
    
    for (size_t i = 0; i < NKEYWORDS; i++) {
        if (hash == hashes[i]) {
            if (strcmp(lexeme, keywords[i]) == 0) return keyword_types[i];       
        }
    }
    return IDENTIFIER;
}

static char next_char(struct scannerState *state) {
    if (state->source[state->offset] == '\0') {
        printf("Fatal compiler error: tried to read past EOF.\n");
        abort();
    }
    state->column++;
    state->offset++;
    char c = state->source[state->offset];
    if (c == '\n') {
        state->column = 0;
        state->line++;
    }
    return c;
}

static char peek(struct scannerState *state) {
    return state->source[state->offset + 1];
}

static char is_digit(char c, char hex) {
    return (hex && ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) || (c >= '0' && c <= '9');
}

static char is_alphaNumeric(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c == '_') || is_digit(c, 0);
}
 
char scanning_error = 0;

static void scan_literal_string(struct scannerState *state);
static token scan_raw_string(struct scannerState *state);
static uint16_t scan_literal_char(struct scannerState *state);
static void scan_literal_number(struct scannerState *state);
static void multiline_comment(struct scannerState *state);

tokenlist *scanTokens(char *source, char *filename) {
    scanning_error = 0;
    populate_keyword_hashes();

    tokenlist *list = (tokenlist*)malloc(sizeof(tokenlist));
    if (!list) {
        printf("Error: Out of memory - Could not malloc tokenlist");
        exit(1);
    }
    *list = (tokenlist){0};
    init_tokenlist(list);

    struct scannerState state;
    state.line = 1;
    state.column = 1;
    state.offset = 0;
    state.source = source;
    state.filename = filename;
    charlist lexeme = {0};
    state.lexeme = &lexeme;
    state.list = list;

    for (char currentChar = *state.source;; currentChar = next_char(&state)) {
        state.thisLine = state.line;
        state.thisColumn = state.column;
        switch (currentChar) {
            case '\0':
                push_tokenlist(list, (token){.type = EOF_TOKEN, .lexeme = "EOF", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                goto end_loop;
            case '{':
                push_tokenlist(list, (token){.type = LEFT_CURLY_BRACE, .lexeme = "{", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            case '}':
                push_tokenlist(list, (token){.type = RIGHT_CURLY_BRACE, .lexeme = "}", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            case '(':
                push_tokenlist(list, (token){.type = LEFT_PAREN, .lexeme = "(", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            case ')':
                push_tokenlist(list, (token){.type = RIGHT_PAREN, .lexeme = ")", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            case '[':
                push_tokenlist(list, (token){.type = LEFT_BRACKET, .lexeme = "[", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            case ']':
                push_tokenlist(list, (token){.type = RIGHT_BRACKET, .lexeme = "]", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            case '_':
                push_tokenlist(list, (token){.type = UNDERSCORE, .lexeme = "_", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            case '\"':
                scan_literal_string(&state);
                break;
            case ';':
                push_tokenlist(list, (token){.type = SEMICOLON, .lexeme = ";", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            case '`':
                push_tokenlist(list, scan_raw_string(&state));
                break;
            case ',':
                push_tokenlist(list, (token){.type = COMMA, .lexeme = ",", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            case '.':
                push_tokenlist(list, (token){.type = DOT, .lexeme = ".", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            #define COMPOUNDABLE_SIMPLE_OPERATOR(operator, tokenType) case operator[0]:\
                if (peek(&state) == '=') {\
                    next_char(&state);\
                    push_tokenlist(list, (token){.type = tokenType##_EQUALS, .lexeme = operator "=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});\
                } else {\
                    push_tokenlist(list, (token){.type = tokenType, .lexeme = operator, .line = state.thisLine, .column = state.thisColumn, .filename = filename});\
                }\
                break;
            COMPOUNDABLE_SIMPLE_OPERATOR("=", EQUALS)
            COMPOUNDABLE_SIMPLE_OPERATOR("*", STAR)
            COMPOUNDABLE_SIMPLE_OPERATOR("%", MODULO)
            COMPOUNDABLE_SIMPLE_OPERATOR("^", XOR)
            COMPOUNDABLE_SIMPLE_OPERATOR("~", TILDE)
            case ':':
                push_tokenlist(list, (token){.type = COLON, .lexeme = ":", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
            #undef COMPOUNDABLE_SIMPLE_OPERATOR
            case '+':
                if (peek(&state) == '=') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = PLUS_EQUALS, .lexeme = "+=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else if (peek(&state) == '+') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = PLUS_PLUS, .lexeme = "++", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else {
                    push_tokenlist(list, (token){.type = PLUS, .lexeme = "+", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
                break;
            case '-':
                if (peek(&state) == '>') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = ARROW, .lexeme = "->", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else if (peek(&state) == '=') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = MINUS_EQUALS, .lexeme = "-=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else if (peek(&state) == '-') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = MINUS_MINUS, .lexeme = "--", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else {
                    push_tokenlist(list, (token){.type = MINUS, .lexeme = "-", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
                break;
            case '|':
                if (peek(&state) == '=') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = BITWISE_OR_EQUALS, .lexeme = "|=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else if (peek(&state) == '|') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = LOGICAL_OR, .lexeme = "||", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else {
                    push_tokenlist(list, (token){.type = BITWISE_OR, .lexeme = "|", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
                break;
            case '/':
                if (peek(&state) == '=') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = SLASH_EQUALS, .lexeme = "/=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else if (peek(&state) == '/') {
                    for (;peek(&state) != '\n' && peek(&state) != '\0'; next_char(&state));
                } else if (peek(&state) == '*') {
                    multiline_comment(&state);
                } else {
                    push_tokenlist(list, (token){.type = SLASH, .lexeme = "/", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
                break;
            case '<':
                if (peek(&state) == '=') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = LESS_EQUALS, .lexeme = "<=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else if (peek(&state) == '<') {
                    next_char(&state);
                    if (peek(&state) == '=') {
                        next_char(&state);
                        push_tokenlist(list, (token){.type = BITSHIFT_LEFT_EQUALS, .lexeme = "<<=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                    } else {
                        push_tokenlist(list, (token){.type = BITSHIFT_LEFT, .lexeme = "<<", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                    }
                } else {
                    push_tokenlist(list, (token){.type = LESS_THAN, .lexeme = "<", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
                break;
            case '>':
                if (peek(&state) == '=') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = GREATER_EQUALS, .lexeme = ">=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else if (peek(&state) == '>') {
                    next_char(&state);
                    if (peek(&state) == '=') {
                        next_char(&state);
                        push_tokenlist(list, (token){.type = BITSHIFT_RIGHT_EQUALS, .lexeme = ">>=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                    } else {
                        push_tokenlist(list, (token){.type = BITSHIFT_RIGHT, .lexeme = ">>", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                    }
                } else {
                    push_tokenlist(list, (token){.type = GREATER_THAN, .lexeme = ">", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
                break;
            case '!':
                if (peek(&state) == '=') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = NOT_EQUALS, .lexeme = "!=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else {
                    push_tokenlist(list, (token){.type = EXCLAMATION, .lexeme = "!", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
                break;
            case '&':
                if (peek(&state) == '=') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = AMPERSAND_EQUALS, .lexeme = "&=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else if (peek(&state) == '&') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = LOGICAL_AND, .lexeme = "&&", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else {
                    push_tokenlist(list, (token){.type = AMPERSAND, .lexeme = "&", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
                break;
            case '\'': {
                size_t start = state.offset;
                char first = next_char(&state);
                if (first == '\0' || first == '\n' || first == '\'') {
                    printf("[%s:%ld:%ld]: Invalid character literal\n", filename, state.thisLine, state.thisColumn);
                    scanning_error = 1;
                    break;
                }
                uint16_t c = scan_literal_char(&state);
                if (scanning_error) break;
                if (next_char(&state) != '\'') {
                    printf("[%s:%ld:%ld]: Expected closing quote after character literal\n", filename, state.thisLine, state.thisColumn);
                    scanning_error = 1;
                    break;
                }
                size_t length = state.offset - start + 1;
                char *lexeme = malloc(length + 1);
                assert(lexeme);
                memcpy(lexeme, source + start, length);
                lexeme[length] = '\0';
                push_tokenlist(list, (token){.type = LITERAL_INT, .lexeme = lexeme,
                    .numValue.integer = c, .line = state.thisLine,
                    .column = state.thisColumn, .filename = filename});
                break;
            }
            case '?':
                push_tokenlist(list, (token){.type = QUESTION_MARK, .lexeme = "?", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                // ignore whitespace
                break;
            default:
                if (is_digit(currentChar, 0)) {
                    scan_literal_number(&state);
                } else {
                    charlist chars = {0};
                    init_charlist(&chars);
                    if (!is_alphaNumeric(currentChar)) {
                        printf("[%s:%ld:%ld]: Unexpected or invalid character: '%c'. Note: Only the characters A-Z, a-z, 0-9, and underscores are allowed in identifiers.\n", state.filename, state.line, state.column, currentChar);
                        free_charlist(&chars);
                        scanning_error = 1;
                        break;
                    }
                    push_charlist(&chars, currentChar);
                    while (is_alphaNumeric(peek(&state))) push_charlist(&chars, next_char(&state));
                    char *chararr = to_chararray(&chars);
                    char *lexeme = malloc(chars.top + 1);
                    memcpy(lexeme, chararr, chars.top);
                    lexeme[chars.top] = '\0';
                    free(chararr);
                    free_charlist(&chars);
                    push_tokenlist(list, (token){.type = get_keyword_type(lexeme), .lexeme = lexeme, .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
        }
        if (scanning_error) goto end_loop;
    }
    
    end_loop:
    
    if (scanning_error) {
        printf("One or more errors encountered during scanning, quitting.\n");
        exit(1);
    }

    free_charlist(state.lexeme);
    return list;
}

// the decimal value of the first digit char (0) in ASCII
// by subtracting this from a char we are able to get its digit representation
// ex. '3' is 51 in ASCII, 51 - 48 = 3
#define CHAR_OFFSET 48

// similar to CHAR_OFFSET, however HEX_OFFSET actually points
// to 10 before A. This allows us to subtract HEX_OFFSET from
// a alpha hex digit (A-F) to get its binary representation
#define HEX_OFFSET 55

static char char_to_decimal(char c) {
    if (c >= '0' && c <= '9') {
        return c - CHAR_OFFSET;
    }

    if (c >= 'a' && c <= 'z') c -= 32;

    if (c >= 'A' && c <= 'Z') {
        return c - HEX_OFFSET;
    }

    // caller should use is_digit() to verify that char is a digit,
    // as char_to_decimal() does not error on invalid digits
    return 0;
}

// scan a number at least minn characters long and up to maxn characters long with base b
// if hex is 1, scan hex digits too, otherwise error on hex digits
static uint16_t scan_numn(struct scannerState *state, char minn, char maxn, char base, char hex) {
    unsigned value = 0;
    size_t count = 0;
    do {
        char c = state->source[state->offset];
        if (!is_digit(c, hex) || char_to_decimal(c) >= base) break;
        value = value * base + char_to_decimal(c);
        count++;
        if (count == (size_t)maxn || !is_digit(peek(state), hex) ||
            char_to_decimal(peek(state)) >= base) break;
        next_char(state);
    } while (1);
    if (count < (size_t)minn || value > 255) {
        printf("[%s:%ld:%ld]: Invalid escape sequence\n", state->filename, state->line, state->column);
        scanning_error = 1;
        return UINT16_MAX;
    }
    return (uint16_t)value;
}

static void scan_literal_string(struct scannerState *state) {
    init_charlist(state->lexeme);

    for (char currentChar = next_char(state);;currentChar = next_char(state)) {
        if (currentChar == '\0') {
            printf("[%s:%ld:%ld] Error: End of file inside string literal\n", state->filename, state->line, state->column);
            scanning_error = 1;
            return;
        }
        if (currentChar == '\n') {
            printf("[%s:%ld:%ld] Error: Unescaped newline inside string literal\n", state->filename, state->line, state->column);
            scanning_error = 1;
            return;
        }
        if (currentChar == '"') {
            goto end_loop;
        }

        uint16_t c = scan_literal_char(state);
        if (c <= 255) {
            push_charlist(state->lexeme, c);
        }
    }

    end_loop:
    ;
    push_charlist(state->lexeme, '\0');
    token string = {0};
    string.column = state->thisColumn;
    string.line = state->thisLine;
    string.filename = state->filename;
    string.lexeme = to_chararray(state->lexeme);
    string.type = LITERAL_STRING;
    push_tokenlist(state->list, string);
    init_charlist(state->lexeme);
}

static token scan_raw_string(struct scannerState *state) {
    init_charlist(state->lexeme);

    for (char currentChar = next_char(state); currentChar != '`'; currentChar = next_char(state)) {
        if (currentChar == '\0') {
            printf("[%s:%ld:%ld]: Unterminated raw string\n", state->filename, state->thisLine, state->thisColumn);
            scanning_error = 1;
            return (token){.type = EOF_TOKEN};
        }
        push_charlist(state->lexeme, currentChar);
    }

    push_charlist(state->lexeme, '\0');
    token string = {0};
    string.column = state->thisColumn;
    string.line = state->thisLine;
    string.filename = state->filename;
    string.lexeme = to_chararray(state->lexeme);
    string.type = LITERAL_STRING;
    init_charlist(state->lexeme);
    return string;
}

static void scan_literal_number(struct scannerState *state) {
    long line = state->line, column = state->column;
    size_t start = state->offset;
    int base = 10;
    if (state->source[state->offset] == '0') {
        switch (peek(state)) {
            case 'x': case 'X': base = 16; break;
            case 'b': case 'B': base = 2; break;
            case 'o': case 'O': base = 8; break;
        }
        if (base != 10) { next_char(state); next_char(state); }
    }
    long integer = 0;
    size_t digits = 0;
    while (is_digit(state->source[state->offset], 1)) {
        int digit = char_to_decimal(state->source[state->offset]);
        if (digit >= base || integer > (LONG_MAX - digit) / base) {
            printf("[%s:%ld:%ld]: invalid numeric digit or integer overflow\n", state->filename, line, column);
            scanning_error = 1;
            return;
        }
        integer = integer * base + digit;
        digits++;
        if (!is_digit(peek(state), 1)) break;
        next_char(state);
    }
    if (!digits) {
        printf("[%s:%ld:%ld]: Expected numeric digit after base prefix\n", state->filename, line, column);
        scanning_error = 1;
        return;
    }
    double value = (double)integer;
    char floating = 0;
    if (peek(state) == '.' && is_digit(state->source[state->offset + 2], base == 16)) {
        floating = 1;
        next_char(state);
        next_char(state);
        double divisor = base;
        do {
            int digit = char_to_decimal(state->source[state->offset]);
            if (digit >= base) {
                printf("[%s:%ld:%ld]: invalid numeric digit\n", state->filename, line, column);
                scanning_error = 1;
                return;
            }
            value += digit / divisor;
            divisor *= base;
            if (!is_digit(peek(state), 1)) break;
            next_char(state);
        } while (1);
    }
    size_t length = state->offset - start + 1;
    char *lexeme = malloc(length + 1);
    assert(lexeme);
    memcpy(lexeme, state->source + start, length);
    lexeme[length] = '\0';
    token result = {.type = floating ? LITERAL_FLOAT : LITERAL_INT, .lexeme = lexeme,
        .line = line, .column = column, .filename = state->filename};
    if (floating) result.numValue.floating = value;
    else result.numValue.integer = integer;
    push_tokenlist(state->list, result);
}

static uint16_t scan_literal_char(struct scannerState *state) {
    char currentChar = state->source[state->offset];
    
    switch (currentChar) {
        case '\n':
            printf("[%s:%ld:%ld] Error: newline encountered while scanning char\n", state->filename, state->line, state->column);
            scanning_error = 1;
            return -1;
        case '\\':
            currentChar = next_char(state);
            #define SIMPLE_ESCAPE_SEQUENCE(c, r) case c: \
                return r;\

            switch (currentChar) {
                SIMPLE_ESCAPE_SEQUENCE('a', '\a')
                SIMPLE_ESCAPE_SEQUENCE('b', '\b')
                SIMPLE_ESCAPE_SEQUENCE('e', '\e')
                SIMPLE_ESCAPE_SEQUENCE('f', '\f')
                SIMPLE_ESCAPE_SEQUENCE('n', '\n')
                SIMPLE_ESCAPE_SEQUENCE('r', '\r')
                SIMPLE_ESCAPE_SEQUENCE('t', '\t')
                SIMPLE_ESCAPE_SEQUENCE('v', '\v')
                SIMPLE_ESCAPE_SEQUENCE('\\', '\\')
                SIMPLE_ESCAPE_SEQUENCE('\'', '\'')
                SIMPLE_ESCAPE_SEQUENCE('"', '"')
                SIMPLE_ESCAPE_SEQUENCE('?', '?')
                SIMPLE_ESCAPE_SEQUENCE('\n', '\n')
                case 'x':
                    next_char(state);
                    return scan_numn(state, 2, 2, 16, 1);
                case 'u':
                    printf("[%s:%ld:%ld] Warning: ocean-c does not support unicode yet and \\u is ignored.\n", state->filename, state->line, state->column);
                    return 'u';
                case 'U':
                    printf("[%s:%ld:%ld] Warning: ocean-c does not support unicode yet and \\U is ignored.\n", state->filename, state->line, state->column);
                    return 'U';
                default:
                    return scan_numn(state, 1, 3, 8, 0);
            }
        default:
            return currentChar;
    }
}

static void multiline_comment(struct scannerState *state) {
    long depth = 0;
    for (char currentChar = state->source[state->offset];; currentChar = next_char(state)) {
        switch (currentChar) {
            case '\0':
                if (scanning_error) {
                    printf("Syntax errors encountered during scanning, quitting.\n");
                    exit(1);
                } else {
                    printf("[%s:%ld:%ld]: Unterminated block comment\n", state->filename, state->thisLine, state->thisColumn);
                    scanning_error = 1;
                    return;
                }
            case '\n':
                break;
            case '/':
                if (peek(state) == '*') {
                    next_char(state);
                    depth++;
                }
                break;
            case '*':
                if (peek(state) == '/') {
                    next_char(state);
                    if (--depth == 0) {
                        return;
                    }
                }
                break;
        }
    }
}
