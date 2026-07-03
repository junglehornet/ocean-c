#include "dynarray.h"
#include "scanner.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Error: no input files included.\n");
        exit(0);
    }

    for (size_t i = 1; i < argc; i++) {
        FILE *sourcefile = fopen(argv[i], "rb");
        if (!sourcefile) {
            printf("Error: file not found: %s\n", argv[i]);
            exit(1);
        }

        fseek(sourcefile, 0, SEEK_END);
        size_t fsize = ftell(sourcefile);
        fseek(sourcefile, 0, SEEK_SET);

        char *source = malloc(fsize + 1);
        if (!source) {
            printf("Error: Out of memory - could not malloc source code buffer\n");
            exit(1);
        }
        fread(source, fsize, 1, sourcefile);
        source[fsize] = 0;
        
        tokenlist *list = scanTokens(source, argv[i]);
        printTokensFromFile(argv[i], list);
        printf("\n\n");
        parseResult res = parseTokens(list);
        //printAst(*(expr*)res.statements[0].value);
    }
}
