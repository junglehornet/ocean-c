#ifndef ocean_c_token
#define ocean_c_token
#include <stddef.h>
#include <stdint.h>
#include "unicode.h"

enum tokenType {
    // keywords and types
    /*INT, FLOAT, I8, I16, I32, U8, U16, U32, U64,
    F32, STRING,*/ TYPE, STRUCT, FUNC, IMPORT,
    ANY, MIXED, INTERFACE, CONTINUE, BREAK, ENUM, IF, DEFER,
    SWITCH, FALLTHROUGH, EXTERN, FOR, GOTO, AS,
    PACKAGE, ELSE, RANGE, RETURN, CASE, DEFAULT, NULL_KEYWORD, PANIC,

    // symbols
    LEFT_CURLY_BRACE, RIGHT_CURLY_BRACE, LEFT_PAREN, RIGHT_PAREN,
    LEFT_BRACKET, RIGHT_BRACKET, UNDERSCORE, SEMICOLON,
    BACKTICK, COMMA, DOT, COLON, QUESTION_MARK, ARROW,

    // operators
    PLUS, MINUS, STAR, SLASH, MODULO, XOR, AMPERSAND,
    TILDE, EXCLAMATION, EQUALS_EQUALS, NOT_EQUALS,
    LESS_THAN, GREATER_THAN, LESS_EQUALS, GREATER_EQUALS,
    BITSHIFT_LEFT, BITSHIFT_RIGHT, BITWISE_OR, LOGICAL_OR,
    LOGICAL_AND,

    // assignment operators
    TOKEN_SPECIAL_ASSIGN_START, // denotes start of assignment operators
    EQUALS, PLUS_EQUALS, MINUS_EQUALS, STAR_EQUALS, SLASH_EQUALS,
    MODULO_EQUALS, XOR_EQUALS, TILDE_EQUALS, AMPERSAND_EQUALS, BITWISE_OR_EQUALS,
    BITSHIFT_LEFT_EQUALS, BITSHIFT_RIGHT_EQUALS,
    TOKEN_SPECIAL_ASSIGN_END, // denotes end of assignment operators (for purposes of expression parsing)
    // also assignment operators, but carry a different operator precedence so they are not between 1024 and 2048
    PLUS_PLUS, MINUS_MINUS,

    // literals
    LITERAL_INT, LITERAL_FLOAT, LITERAL_STRING,

    // ident token
    IDENTIFIER,

    // eof token
    // this is named EOF_TOKEN because EOF is a macro in stdio.h
    EOF_TOKEN,
};

typedef struct {
    char *lexeme;
    char unsignedLiteral; // Integer magnitude exceeds LONG_MAX, but fits uint64_t.
    size_t stringLength; // Decoded bytes, excluding the scanner storage terminator.
    union {
        long integer;
        uint64_t unsignedInteger;
        double floating;
    } numValue;
    long line, column;
    char *filename;
    enum tokenType type;
} token;

#define NULL_TOKEN token{.type = EOF_TOKEN, .lexeme = NULL, .line = 0, .column = 0, .filename = NULL}

char *getTokenTypeStr(enum tokenType t);

void printToken(token tok);

/* Identifiers currently use the ASCII alphabet supported by the scanner. */
static inline int exportedName(const char *name) {
    uint32_t c; return name && unicodeDecode(name, &c) && unicodeUpper(c);
}

#endif
