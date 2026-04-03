#ifndef ocean_c_token
#define ocean_c_token

enum tokenType {
    // keywords and types
    INT, FLOAT, I8, I16, I32, F32, STRING,
    TYPE, STRUCT, INDEX, FUNC, IMPORT, ANY, INTERFACE,

    // symbols
    LEFT_CURLY_BRACE, RIGHT_CURLY_BRACE, LEFT_PAREN, RIGHT_PAREN,
    LEFT_BRACKET, RIGHT_BRACKET, UNDERSCORE, DOUBLE_QUOTES, SEMICOLON,
    BACKSLASH, BACKTICK, COMMA, DOT, EQUALS, COLON,

    // operators
    PLUS, MINUS, STAR, SLASH, MODULO, XOR, AMPERSAND,
    TILDE, EXCLAMATION, EQUALS_EQUALS, NOT_EQUALS,
    LESS_THAN, GREATER_THAN, LESS_EQUALS, GREATER_EQUALS,

    // literals
    LITERAL_INT, LITERAL_I8, LITERAL_I16, LITERAL_I32,
    LITERAL_FLOAT, LITERAL_F32, LITERAL_STRING,

    // ident token
    IDENTIFIER,

    // eof token
    EOF,
};

typedef struct {
    enum tokenType type;
    char *lexeme;
    long line, column;
    char *filename;
} token;

#define NULL_TOKEN token{.type = EOF, .lexeme = NULL, .line = 0, .column = 0, .filename = NULL}

#endif
