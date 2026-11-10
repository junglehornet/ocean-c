#ifndef OCEAN_BUILD_CACHE_H
#define OCEAN_BUILD_CACHE_H
#include "parser.h"

typedef struct buildCache buildCache;
/* Cache failures are misses, never build failures. */
buildCache *openBuildCache(const char *compiler, parseResult *program, const char *packageDirectory);
void closeBuildCache(buildCache *cache);
const char *buildCacheIRKey(buildCache *cache);
/* Native action identities use source/dependency inputs, never LLVM contents.
 * packageId zero identifies whole-program support; metadata is a separate action. */
int buildCachePackageKey(buildCache *cache, parseResult *program, size_t packageId, int metadata, char key[65]);
void buildHashBytes(const void *data, size_t size, char hash[65]);
int readBuildCache(buildCache *cache, const char *key, const char *destination);
void storeBuildCache(buildCache *cache, const char *key, const char *source);
int cleanBuildCache(void);
#endif
