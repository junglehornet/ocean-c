#include "dynarray.h"
#include "scanner.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Error: no input files included.\n");
        return 1;
    }

    for (size_t i = 1; i < argc; i++) {
        FILE *sourcefile = fopen(argv[i], "rb");
        if (!sourcefile) {
            printf("Error: file not found: %s\n", argv[i]);
            exit(1);
        }

        if (fseek(sourcefile, 0, SEEK_END) != 0) {
            printf("Error: could not read input file: %s\n", argv[i]);
            fclose(sourcefile);
            return 1;
        }
        long length = ftell(sourcefile);
        if (length < 0 || fseek(sourcefile, 0, SEEK_SET) != 0) {
            printf("Error: could not read input file: %s\n", argv[i]);
            fclose(sourcefile);
            return 1;
        }
        size_t fsize = (size_t)length;

        char *source = malloc(fsize + 1);
        if (!source) {
            printf("Error: Out of memory - could not malloc source code buffer\n");
            exit(1);
        }
        if (fread(source, 1, fsize, sourcefile) != fsize || ferror(sourcefile)) {
            printf("Error: could not read input file: %s\n", argv[i]);
            free(source);
            fclose(sourcefile);
            return 1;
        }
        fclose(sourcefile);
        source[fsize] = 0;
        
        tokenlist *list = scanTokens(source, argv[i]);
        printTokensFromFile(argv[i], list);
        printf("\n\n");
        parseResult res = parseTokens(list);
        free(list);
        free(source);
        if (res.hasError) return 1;
    }
}
