#include "core.h"
#include "dynarray.h"
#include "token.h"
#include "stdio.h"
#include <assert.h>
#include <math.h>
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
    /*F32, STRING,*/ TYPE, STRUCT, INDEX, FUNC, IMPORT,
    ANY, INTERFACE, CONTINUE, BREAK, ENUM, IF, DO,
    SWITCH, FALLTHROUGH, EXTERN, FOR, GOTO, AS,
    PACKAGE, WHILE, ELSE,
};

const char *keywords[] = {
    //"int", "float", "i8", "i16", "i32", "u8", "u16", "u32", "u64",
    /*"f32", "string",*/ "type", "struct", "index", "func", "import",
    "any", "interface", "continue", "break", "enum", "if", "do",
    "switch", "fallthrough", "extern", "for", "goto", "as",
    "package", "while", "else",
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
        return next_char(state);
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
    populate_keyword_hashes();

    tokenlist *list = (tokenlist*)malloc(sizeof(tokenlist));
    if (!list) {
        printf("Error: Out of memory - Could not malloc tokenlist");
        exit(1);
    }
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
                if (peek(&state) == '=') {
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
                if (peek(&state) == '|') {
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
                if (peek(&state) == '&') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = LOGICAL_AND, .lexeme = "&&", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else {
                    push_tokenlist(list, (token){.type = AMPERSAND, .lexeme = "&", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
                break;
            case '\'':
                // parse char and then push as int literal
                ;
                size_t quote_offset = state.offset;
                uint16_t c = scan_literal_char(&state);
                next_char(&state);
                char *lexemebuf = (char*)malloc((state.offset - quote_offset + 1) * sizeof(char));
                memcpy(lexemebuf, state.source + quote_offset, state.offset - quote_offset);
                lexemebuf[state.offset - quote_offset + 1] = '\0';
                if (c > 255) {
                    push_tokenlist(list, (token){.type = LITERAL_INT, .lexeme = lexemebuf, .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else {
                    push_tokenlist(list, (token){.type = LITERAL_INT, .lexeme = lexemebuf, .numValue.integer=c, .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
                break;
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
                        continue;
                    }
                    push_charlist(&chars, currentChar);
                    while (is_alphaNumeric(peek(&state))) push_charlist(&chars, next_char(&state));
                    char *chararr = to_chararray(&chars);
                    char *lexeme = malloc(chars.top + 1);
                    memcpy(lexeme, chararr, chars.top);
                    lexeme[chars.top] = '\0';
                    free(chararr);
                    push_tokenlist(list, (token){.type = get_keyword_type(lexeme), .lexeme = lexeme, .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                }
        }
    }
    
    end_loop:
    
    if (scanning_error) {
        printf("One or more errors encountered during scanning, quitting.\n");
        exit(1);
    }

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
static uint16_t scan_numn(struct scannerState *state, char minn, char maxn, char b, char hex) {
    char n = 0;
    long value = 0;
    char sequence[maxn];
    char has_error;

    for (char currentChar = state->source[state->offset];;currentChar = next_char(state)) {
        if (currentChar == '\0') {
            break;
        }
        
        if (!is_digit(currentChar, 1)) {
            if (n >= minn) {
                break;
            } else {
                has_error = 1;
            }
        }

        if (n >= maxn) {
            break;
        }
        
        sequence[n++] = currentChar;
        if ((currentChar >= 'A' && currentChar <= 'F') || (currentChar >= 'a' && currentChar <= 'f')) {
            if (!hex) has_error = 1;
        }
    }

    if (has_error) {
        scanning_error = 1;
        return -1;
    }

    for (char *i = sequence; i < sequence + n; i++) {
            if (is_digit(*i, 0)) {
                value = (*i - HEX_OFFSET) * powl(b, n - (i - sequence));
            } else if (is_digit(*i, 1)) {
                value += *i - CHAR_OFFSET * powl(b, n - (i - sequence));
            } else {
                printf("Fatal compiler error scanning number: 1\n");
                abort();
            }
    }

    return value;
}

static void scan_literal_string(struct scannerState *state) {
    init_charlist(state->lexeme);
    push_charlist(state->lexeme, '"');

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
            push_charlist(state->lexeme, '"');
            goto end_loop;
        }

        uint16_t c = scan_literal_char(state);
        if (c <= 255) {
            push_charlist(state->lexeme, c);
        }
    }

    end_loop:
    ;
    token string;
    string.column = state->column;
    string.line = state->line;
    string.filename = state->filename;
    string.lexeme = to_chararray(state->lexeme);
    string.type = LITERAL_STRING;
    push_tokenlist(state->list, string);
    init_charlist(state->lexeme);
}

static token scan_raw_string(struct scannerState *state) {
    init_charlist(state->lexeme);

    for (char currentChar = next_char(state); currentChar != '`'; currentChar = next_char(state)) {
        push_charlist(state->lexeme, currentChar);
    }

    token string;
    string.column = state->column;
    string.line = state->line;
    string.filename = state->filename;
    string.lexeme = to_chararray(state->lexeme);
    string.type = LITERAL_STRING;
    init_charlist(state->lexeme);
    return string;
}

static void scan_literal_number(struct scannerState *state) {
    char currentChar = state->source[state->offset];
    const long line = state->line;
    const long column = state->column;
    init_charlist(state->lexeme);
    char n = 0;
    char base = 10;
    // check for (and consume) 0x, 0b, 0o denoting alternate numerical format
    if (currentChar == '0') {
        switch (peek(state)) {
            case 'x':
            case 'X':
                push_charlist(state->lexeme, currentChar);
                push_charlist(state->lexeme, next_char(state));
                currentChar = next_char(state);
                base = 16;
                break;
            case 'b':
            case 'B':
                push_charlist(state->lexeme, currentChar);
                push_charlist(state->lexeme, next_char(state));
                currentChar = next_char(state);
                base = 2;
                break;
            case 'o':
            case 'O':
                push_charlist(state->lexeme, currentChar);
                push_charlist(state->lexeme, next_char(state));
                currentChar = next_char(state);
                base = 8;
                break;
            default:
                //n++;
        }
    }

    // collect chars into array
    charlist num = {0};
    char err = 0;
    char floating = 0;
    
    init_charlist(&num);
    while (is_digit(currentChar, 1)) {
        n++;
        if (char_to_decimal(currentChar) >= base) {
            printf("[%s:%ld:%ld] Error: digit %c is invalid for a number with base %hhu\n", state->filename, state->line, state->column, currentChar, base);
            err = 1;
            scanning_error = 1;
            break;
        }
        push_charlist(&num, currentChar);
        push_charlist(state->lexeme, currentChar);        
        if (is_digit(peek(state), 1)) currentChar = next_char(state);
        else break;
    }
    
    if (peek(state) == '.') {
        next_char(state);
        floating = 1;
        push_charlist(&num, currentChar);

        do {
            if (char_to_decimal(currentChar) >= base) {
                printf("[%s:%ld:%ld] Error: digit %c is invalid for a number with base %hhu\n", state->filename, state->line, state->column, currentChar, base);
                err = 1;
                scanning_error = 1;
                break;
            }
            push_charlist(&num, currentChar);
            push_charlist(state->lexeme, currentChar);        
            currentChar = next_char(state);
        } while (is_digit(peek(state), 1));
    }
    
    if (err) {
        push_tokenlist(state->list, (token){.type = LITERAL_INT, .lexeme = to_chararray(state->lexeme), .numValue.integer = 0, .line = state->line, .column = state->column, .filename = state->filename});
        return;
    }

    // parse out number from chars
    double value = 0;

    for (size_t i = 0; i < num.top; i++) {
        if (get_charlist(&num, i) == '.') continue;
        n--;
        value += char_to_decimal(get_charlist(&num, i)) * powl(base, n);
    }

    if (floating) {
        push_tokenlist(state->list, (token){.type = LITERAL_FLOAT, .lexeme = to_chararray(state->lexeme), .numValue.floating = value, .line = line, .column = column, .filename = state->filename});
    } else {    
        push_tokenlist(state->list, (token){.type = LITERAL_INT, .lexeme = to_chararray(state->lexeme), .numValue.integer = (long) value, .line = line, .column = column, .filename = state->filename});
    }
    init_charlist(state->lexeme);
    return;
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
                    ;
                    size_t xoffset = state->offset;
                    next_char(state);
                    uint16_t num = scan_numn(state, 2, 2, 16, 1);
                    if (num > 255) {
                        char errbuf[state->offset - xoffset + 1];
                        memcpy(errbuf, state->source + xoffset, state->offset - xoffset);
                        errbuf[state->offset - xoffset + 1] = '\0';
                        printf("[%s:%ld:%ld] Error: invalid escape sequence '\\%s'\n", state->filename, state->line, state->column, errbuf);
                        return -1;
                    }
                    return num;
                case 'u':
                    printf("[%s:%ld:%ld] Warning: ocean-c does not support unicode yet and \\u is ignored.\n", state->filename, state->line, state->column);
                    return 'u';
                case 'U':
                    printf("[%s:%ld:%ld] Warning: ocean-c does not support unicode yet and \\U is ignored.\n", state->filename, state->line, state->column);
                    return 'U';
                default:
                    ;
                    size_t firstoffset = state->offset;
                    next_char(state);
                    uint16_t octal = scan_numn(state, 1, 3, 8, 0);
                    if (octal > 255) {
                        char errbuf[state->offset - firstoffset + 1];
                        memcpy(errbuf, state->source + firstoffset,  state->offset - firstoffset);
                        errbuf[state->offset - firstoffset + 1] = '\0';
                        printf("[%s:%ld:%ld] Error: invalid escape sequence '\\%s'\n", state->filename, state->line, state->column, errbuf);
                        return -1;
                    }
                    return octal;
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
                    state->offset--;
                    state->column--;
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
