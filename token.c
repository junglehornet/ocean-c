#include "token.h"
#include "stdio.h"

char *getTokenTypeStr(enum tokenType t) {
    switch (t) {
    case INT:
        return "INT";
    case FLOAT:
        return "FLOAT";
    case I8:
        return "I8";
    case I16:
        return "I16";
    case I32:
        return "I32";
    case F32:
        return "F32";
    case STRING:
        return "STRING";
    case TYPE:
        return "TYPE";
    case STRUCT:
        return "STRUCT";
    case INDEX:
        return "INDEX";
    case FUNC:
        return "FUNC";
    case IMPORT:
        return "IMPORT";
    case ANY:
        return "ANY";
    case INTERFACE:
        return "INTERFACE";
    case CONTINUE:
        return "CONTINUE";
    case BREAK:
        return "BREAK";
    case ENUM:
        return "ENUM";
    case IF:
        return "IF";
    case DO:
        return "DO";
    case SWITCH:
        return "SWITCH";
    case FALLTHROUGH:
        return "FALLTHROUGH";
    case EXTERN:
        return "EXTERN";
    case FOR:
        return "FOR";
    case GOTO:
        return "GOTO";
    case LEFT_CURLY_BRACE:
        return "LEFT_CURLY_BRACE";
    case RIGHT_CURLY_BRACE:
        return "RIGHT_CURLY_BRACE";
    case LEFT_PAREN:
        return "LEFT_PAREN";
    case RIGHT_PAREN:
        return "RIGHT_PAREN";
    case LEFT_BRACKET:
        return "LEFT_BRACKET";
    case RIGHT_BRACKET:
        return "RIGHT_BRACKET";
    case UNDERSCORE:
        return "UNDERSCORE";
    case SEMICOLON:
        return "SEMICOLON";
    case BACKTICK:
        return "BACKTICK";
    case COMMA:
        return "COMMA";
    case DOT:
        return "DOT";
    case EQUALS:
        return "EQUALS";
    case COLON:
        return "COLON";
    case COLON_EQUALS:
        return "COLON_EQUALS";
    case PLUS:
        return "PLUS";
    case MINUS:
        return "MINUS";
    case STAR:
        return "STAR";
    case SLASH:
        return "SLASH";
    case MODULO:
        return "MODULO";
    case XOR:
        return "XOR";
    case AMPERSAND:
        return "AMPERSAND";
    case TILDE:
        return "TILDE";
    case EXCLAMATION:
        return "EXCLAMATION";
    case EQUALS_EQUALS:
        return "EQUALS_EQUALS";
    case NOT_EQUALS:
        return "NOT_EQUALS";
    case LESS_THAN:
        return "LESS_THAN";
    case GREATER_THAN:
        return "GREATER_THAN";
    case LESS_EQUALS:
        return "LESS_EQUALS";
    case GREATER_EQUALS:
        return "GREATER_EQUALS";
    case BITSHIFT_LEFT:
        return "BITSHIFT_LEFT_EQUALS";
    case BITSHIFT_RIGHT:
        return "BITSHIFT_RIGHT_EQUALS";
    case PLUS_EQUALS:
        return "PLUS_EQUALS_EQUALS";
    case MINUS_EQUALS:
        return "MINUS_EQUALS_EQUALS";
    case STAR_EQUALS:
        return "STAR_EQUALS_EQUALS";
    case SLASH_EQUALS:
        return "SLASH_EQUALS_EQUALS";
    case MODULO_EQUALS:
        return "MODULO_EQUALS";
    case XOR_EQUALS:
        return "XOR_EQUALS";
    case TILDE_EQUALS:
        return "TILDE_EQUALS";
    case BITSHIFT_LEFT_EQUALS:
        return "BITSHIFT_LEFT_EQUALS";
    case BITSHIFT_RIGHT_EQUALS:
        return "BITSHIFT_RIGHT_EQUALS";
    case LITERAL_INT:
        return "LITERAL_INT";
    case LITERAL_FLOAT:
        return "LITERAL_FLOAT";
    case LITERAL_STRING:
        return "LITERAL_STRING";
    case IDENTIFIER:
        return "IDENTIFIER";
    case EOF_TOKEN:
        return "EOF";
    default:
        return "???";
    }
}

void printToken(token tok) {
    switch (tok.type) {
        case LITERAL_STRING:
            printf("(LITERAL_STRING %s) ", tok.lexeme);
            break;
        case LITERAL_INT:
            printf("(LITERAL_INT \"%s\" %ld) ", tok.lexeme, tok.numValue.integer);
            break;
        case LITERAL_FLOAT:
            printf("(LITERAL_FLOAT \"%s\" %lf) ", tok.lexeme, tok.numValue.floating);
            break;
        case IDENTIFIER:
            printf("(IDENTIFIER %s) ", tok.lexeme);
            break;
        default:
            printf("%s ", getTokenTypeStr(tok.type));
            break;
    }
}
