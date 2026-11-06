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
#include "diagnostic.h"

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
    ANY, MIXED, INTERFACE, CONTINUE, BREAK, ENUM, IF, DEFER,
    SWITCH, FALLTHROUGH, EXTERN, FOR, GOTO, AS,
    PACKAGE, ELSE, RANGE, RETURN, CASE, DEFAULT, NULL_KEYWORD, PANIC, MATCH, ASM,
};

const char *keywords[] = {
    //"int", "float", "i8", "i16", "i32", "u8", "u16", "u32", "u64",
    /*"f32", "string",*/ "type", "struct", "func", "import",
    "any", "mixed", "interface", "continue", "break", "enum", "if", "defer",
    "switch", "fallthrough", "extern", "for", "goto", "as",
    "package", "else", "range", "return", "case", "default", "null", "panic", "match", "asm",
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
    if (state->source[state->offset] == '\n') {
        state->column = 1;
        state->line++;
    } else state->column++;
    state->offset++;
    char c = state->source[state->offset];
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
static uint32_t scan_literal_char(struct scannerState *state);
static void scan_literal_number(struct scannerState *state);
static void multiline_comment(struct scannerState *state);

// Assembly is opaque to the Ocean lexer, including foreign-target blocks.
static int asmHeader(tokenlist *list) {
    size_t n = list->top;
    if (n >= 2 && get_tokenlist(list, n-2).type == ASM &&
        get_tokenlist(list, n-1).type == IDENTIFIER) return 1;
    return n >= 4 && get_tokenlist(list, n-4).type == ASM &&
        get_tokenlist(list, n-3).type == IDENTIFIER &&
        get_tokenlist(list, n-2).type == SLASH &&
        get_tokenlist(list, n-1).type == IDENTIFIER;
}

static void scanAssembly(struct scannerState *state) {
    size_t start = state->offset + 1;
    int depth = 1, quote = 0, lineComment = 0, blockComment = 0;
    size_t n = state->list->top;
    token arch = get_tokenlist(state->list, n - (get_tokenlist(state->list, n-2).type == SLASH ? 3 : 1));
    int hashComment = strcmp(arch.lexeme, "aarch64") != 0;
    while (next_char(state)) {
        char c = state->source[state->offset], next = peek(state);
        if (lineComment) { if (c == '\n') lineComment = 0; continue; }
        if (blockComment) {
            if (c == '*' && next == '/') { next_char(state); blockComment = 0; }
            continue;
        }
        if (quote) {
            if (c == '\\' && next) next_char(state);
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '/' && next == '/') { lineComment = 1; next_char(state); }
        else if (c == '/' && next == '*') { blockComment = 1; next_char(state); }
        else if (c == '#' && hashComment) lineComment = 1;
        else if (c == '"' || c == '\'') quote = c;
        else if (c == '{') depth++;
        else if (c == '}' && !--depth) {
            size_t length = state->offset - start;
            char *body = malloc(length + 1);
            assert(body);
            memcpy(body, state->source + start, length); body[length] = 0;
            push_tokenlist(state->list, (token){.type=ASM_BODY, .lexeme=body,
                .line=state->thisLine, .column=state->thisColumn, .filename=state->filename});
            return;
        }
    }
    diagnostic(state->source, (token){.filename=state->filename,
        .line=state->thisLine, .column=state->thisColumn}, "scanner", "error", "Unterminated asm block");
    scanning_error = 1;
}

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
                if (asmHeader(list)) {
                    scanAssembly(&state);
                    if (!state.source[state.offset]) goto end_loop;
                    break;
                }
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
            case '=':
                if (peek(&state) == '>') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = FAT_ARROW, .lexeme = "=>", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else if (peek(&state) == '=') {
                    next_char(&state);
                    push_tokenlist(list, (token){.type = EQUALS_EQUALS, .lexeme = "==", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                } else push_tokenlist(list, (token){.type = EQUALS, .lexeme = "=", .line = state.thisLine, .column = state.thisColumn, .filename = filename});
                break;
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
                    diagnostic(state.source, (token){.filename = filename, .line = state.thisLine, .column = state.thisColumn}, "scanner", "error", "Invalid character literal");
                    scanning_error = 1;
                    break;
                }
                uint32_t c = scan_literal_char(&state);
                if (scanning_error) break;
                if (next_char(&state) != '\'') {
                    diagnostic(state.source, (token){.filename = filename, .line = state.thisLine, .column = state.thisColumn}, "scanner", "error", "Expected closing quote after character literal");
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
                    uint32_t code; size_t bytes = unicodeDecode(state.source + state.offset, &code);
                    if (!bytes || !unicodeStart(code)) {
                        diagnostic(state.source, (token){.filename=state.filename,.line=state.line,.column=state.column}, "scanner", "error", "Unexpected or invalid character or UTF-8 encoding in identifier");
                        free_charlist(&chars); scanning_error = 1; break;
                    }
                    for (;;) {
                        for (size_t i=0;i<bytes;i++) { push_charlist(&chars,state.source[state.offset]); if(i+1<bytes)next_char(&state); }
                        bytes=unicodeDecode(state.source+state.offset+1,&code);
                        if(!bytes || !unicodeContinue(code))break;
                        next_char(&state);
                    }
                    char *chararr = to_chararray(&chars);
                    char *lexeme = malloc(chars.top + 1);
                    memcpy(lexeme, chararr, chars.top);
                    lexeme[chars.top] = '\0';
                    free(chararr);
                    free_charlist(&chars);
                    push_tokenlist(list, (token){.type = !strcmp(lexeme,"_") ? UNDERSCORE : get_keyword_type(lexeme), .lexeme = lexeme, .line = state.thisLine, .column = state.thisColumn, .filename = filename});
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
        diagnostic(state->source, (token){.filename = state->filename, .line = state->line, .column = state->column}, "scanner", "error", "Invalid escape sequence");
        scanning_error = 1;
        return UINT16_MAX;
    }
    return (uint16_t)value;
}

static void scan_literal_string(struct scannerState *state) {
    init_charlist(state->lexeme);

    for (char currentChar = next_char(state);;currentChar = next_char(state)) {
        if (currentChar == '\0') {
            diagnostic(state->source, (token){.filename = state->filename, .line = state->line, .column = state->column}, "scanner", "error", "End of file inside string literal");
            scanning_error = 1;
            return;
        }
        if (currentChar == '\n') {
            diagnostic(state->source, (token){.filename = state->filename, .line = state->line, .column = state->column}, "scanner", "error", "Unescaped newline inside string literal");
            scanning_error = 1;
            return;
        }
        if (currentChar == '"') {
            goto end_loop;
        }

        int utf8 = (unsigned char)currentChar >= 128 ||
                   (currentChar == '\\' && (peek(state) == 'u' || peek(state) == 'U'));
        uint32_t c = scan_literal_char(state);
        if (scanning_error) return;
        if (!utf8 || c < 128) push_charlist(state->lexeme, c);
        else {
            if (c < 0x800) push_charlist(state->lexeme, 0xc0 | (c >> 6));
            else if (c < 0x10000) { push_charlist(state->lexeme, 0xe0 | (c >> 12)); push_charlist(state->lexeme,0x80 | ((c>>6)&63)); }
            else { push_charlist(state->lexeme,0xf0 | (c>>18));push_charlist(state->lexeme,0x80 | ((c>>12)&63));push_charlist(state->lexeme,0x80 | ((c>>6)&63)); }
            push_charlist(state->lexeme,0x80 | (c&63));
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
    string.stringLength = state->lexeme->top - 1;
    push_tokenlist(state->list, string);
    init_charlist(state->lexeme);
}

static token scan_raw_string(struct scannerState *state) {
    init_charlist(state->lexeme);

    for (char currentChar = next_char(state); currentChar != '`'; currentChar = next_char(state)) {
        if (currentChar == '\0') {
            diagnostic(state->source, (token){.filename = state->filename, .line = state->thisLine, .column = state->thisColumn}, "scanner", "error", "Unterminated raw string");
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
    string.stringLength = state->lexeme->top - 1;
    init_charlist(state->lexeme);
    return string;
}

static void scan_literal_number(struct scannerState *state) {
    size_t start=state->offset, at=start; unsigned base=10; int floating=0;
    const char *s=state->source;
    if(s[at]=='0') { switch(s[at+1]) {case 'x':case 'X':base=16;break;case 'b':case 'B':base=2;break;case 'o':case 'O':base=8;break;} if(base!=10)at+=2; }
    size_t digits=at; uint64_t integer=0; long double mantissa=0; int overflow=0;
    while(is_digit(s[at],base==16)) {
        unsigned digit=char_to_decimal(s[at]);
        if(digit>=base)goto invalid;
        if(integer>(UINT64_MAX-digit)/base)overflow=1;
        integer=integer*base+digit;mantissa=mantissa*base+digit;at++;
    }
    if(at==digits)goto invalid;
    long double value=mantissa;
    if(s[at]=='.' && is_digit(s[at+1],base==16)) {
        floating=1;at++;long double divisor=base;
        while(is_digit(s[at],base==16)) {unsigned digit=char_to_decimal(s[at]);if(digit>=base)goto invalid;value+=digit/divisor;divisor*=base;at++;}
    }
    if(base==10 && (s[at]=='e'||s[at]=='E')) {
        floating=1;at++;int negative=0;if(s[at]=='+'||s[at]=='-'){negative=s[at]=='-';at++;}
        if(!is_digit(s[at],0))goto invalid;
        unsigned exponent=0;while(is_digit(s[at],0)){if(exponent<100000)exponent=exponent*10+(s[at]-'0');at++;}
        if (value != 0) value*=powl(10,negative?-(long double)exponent:(long double)exponent);
    }
    if(is_alphaNumeric(s[at]))goto invalid;
    if((overflow && !floating) || (floating && !isfinite((double)value)))goto invalid;
    token result={.type=floating?LITERAL_FLOAT:LITERAL_INT,.line=state->thisLine,.column=state->thisColumn,.filename=state->filename};
    size_t n=at-start;result.lexeme=malloc(n+1);assert(result.lexeme);memcpy(result.lexeme,s+start,n);result.lexeme[n]=0;
    if(floating)result.numValue.floating=(double)value;
    else {result.unsignedLiteral=integer>LONG_MAX;result.numValue.unsignedInteger=integer;}
    while(state->offset+1<at)next_char(state);
    push_tokenlist(state->list,result);return;
invalid:
    diagnostic(state->source,(token){.filename=state->filename,.line=state->thisLine,.column=state->thisColumn},"scanner","error","invalid numeric digit, exponent, or integer overflow (maximum UINT64_MAX)");scanning_error=1;
}

static uint32_t scan_literal_char(struct scannerState *state) {
    char currentChar = state->source[state->offset];
    
    switch (currentChar) {
        case '\n':
            diagnostic(state->source, (token){.filename = state->filename, .line = state->line, .column = state->column}, "scanner", "error", "newline encountered while scanning char");
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
                case 'u': case 'U': {
                    unsigned count = currentChar == 'u' ? 4 : 8;
                    // currentChar is updated to the escaped letter above.
                    uint32_t value = 0;
                    for (unsigned i=0;i<count;i++) {
                        char digit = next_char(state);
                        if (!is_digit(digit,1)) { scanning_error=1; break; }
                        value = (value<<4) | (unsigned)char_to_decimal(digit);
                    }
                    if (scanning_error || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) {
                        diagnostic(state->source,(token){.filename=state->filename,.line=state->line,.column=state->column},"scanner","error","Invalid Unicode escape sequence");
                        scanning_error=1; return UINT32_MAX;
                    }
                    return value;
                }
                default:
                    return scan_numn(state, 1, 3, 8, 0);
            }
        default: {
            uint32_t code; size_t n=unicodeDecode(state->source+state->offset,&code);
            if(!n){diagnostic(state->source,(token){.filename=state->filename,.line=state->line,.column=state->column},"scanner","error","Invalid UTF-8 encoding");scanning_error=1;return UINT32_MAX;}
            for(size_t i=1;i<n;i++)next_char(state);
            return code;
        }
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
                    diagnostic(state->source, (token){.filename = state->filename, .line = state->thisLine, .column = state->thisColumn}, "scanner", "error", "Unterminated block comment");
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
