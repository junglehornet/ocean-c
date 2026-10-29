#ifndef OCEAN_PACKAGE_H
#define OCEAN_PACKAGE_H
#include "parser.h"
typedef struct packageBuild packageBuild;
/* Loads, namespaces, and merges a package and its dependency graph. */
int validModulePath(const char *path);
packageBuild *loadPackage(const char *location);
/* Borrowed absolute paths and editor buffers, valid until freePackage. */
typedef struct packageSource { const char *path, *text; } packageSource;
packageBuild *loadPackageWithSources(const char *location, const packageSource *sources, size_t count);
const char *packageDirectory(packageBuild *build);
parseResult *packageProgram(packageBuild *build);
void freePackage(packageBuild *build);
#endif
