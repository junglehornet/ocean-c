#ifndef ocean_c_scanner
#define ocean_c_scanner

#include "dynarray.h"

struct scannerState {
    long line, column;
    char *filename;
    charlist *lexeme;
    char *source;
    tokenlist *list;
    size_t offset;
};

tokenlist *scanTokens(char *source, char *filename);

#endif
