#ifndef ocean_c_token
#define ocean_c_token

enum tokenType {
    // keywords and types
    INT, FLOAT, I8, I16, I32, F32, STRING,
    TYPE, STRUCT, INDEX, FUNC, IMPORT, ANY,
    INTERFACE, CONTINUE, BREAK, ENUM, IF, DO,
    SWITCH, FALLTHROUGH, EXTERN, FOR, GOTO,

    // symbols
    LEFT_CURLY_BRACE, RIGHT_CURLY_BRACE, LEFT_PAREN, RIGHT_PAREN,
    LEFT_BRACKET, RIGHT_BRACKET, UNDERSCORE, SEMICOLON,
    BACKTICK, COMMA, DOT, EQUALS, COLON, COLON_EQUALS,

    // operators
    PLUS, MINUS, STAR, SLASH, MODULO, XOR, AMPERSAND,
    TILDE, EXCLAMATION, EQUALS_EQUALS, NOT_EQUALS,
    LESS_THAN, GREATER_THAN, LESS_EQUALS, GREATER_EQUALS,
    BITSHIFT_LEFT, BITSHIFT_RIGHT,

    // compound assignment operators
    PLUS_EQUALS, MINUS_EQUALS, STAR_EQUALS, SLASH_EQUALS,
    MODULO_EQUALS, XOR_EQUALS, TILDE_EQUALS,
    BITSHIFT_LEFT_EQUALS, BITSHIFT_RIGHT_EQUALS,

    // literals
    LITERAL_INT, LITERAL_FLOAT, LITERAL_STRING,

    // ident token
    IDENTIFIER,

    // eof token
    // is is named EOF_TOKEN because EOF is a macro in stdio.h
    EOF_TOKEN,
};

typedef struct {
    enum tokenType type;
    char *lexeme;
    union {
        long integer;
        double floating;
    } numValue;
    long line, column;
    char *filename;
} token;

#define NULL_TOKEN token{.type = EOF, .lexeme = NULL, .line = 0, .column = 0, .filename = NULL}

char *getTokenTypeStr(enum tokenType t);

void printToken(token tok);

#endif
