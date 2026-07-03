#ifndef ocean_c_scanner
#define ocean_c_scanner

#include "dynarray.h"

struct scannerState {
    long line, column;
    long thisLine, thisColumn;
    char *filename;
    charlist *lexeme;
    char *source;
    tokenlist *list;
    size_t offset;
};

tokenlist *scanTokens(char *source, char *filename);

void printTokensFromFile(char *filename, tokenlist *list);

#endif
