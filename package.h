#ifndef OCEAN_PACKAGE_H
#define OCEAN_PACKAGE_H
#include "parser.h"
typedef struct packageBuild packageBuild;
/* Loads, namespaces, and merges a package and its dependency graph. */
int validModulePath(const char *path);
packageBuild *loadPackage(const char *location);
/* Build a synthetic executable containing only the target package's tests. */
packageBuild *loadTestPackage(const char *location, const char *harnessDirectory);
/* Borrowed absolute paths and editor buffers, valid until freePackage. */
typedef struct packageSource { const char *path, *text; } packageSource;
packageBuild *loadPackageWithSources(const char *location, const packageSource *sources, size_t count);
const char *packageDirectory(packageBuild *build);
/* Editor queries may inspect successfully parsed sources after binding errors. */
parseResult *packagePartialProgram(packageBuild *build);
parseResult *packageProgram(packageBuild *build);
typedef void (*packageImportVisitor)(const char *file, const char *alias, size_t packageId, void *context);
void visitPackageImports(packageBuild *build, packageImportVisitor visitor, void *context);
void freePackage(packageBuild *build);
#endif
