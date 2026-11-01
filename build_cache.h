#ifndef OCEAN_BUILD_CACHE_H
#define OCEAN_BUILD_CACHE_H
#include "parser.h"

typedef struct buildCache buildCache;
/* Cache failures are misses, never build failures. */
buildCache *openBuildCache(const char *compiler, parseResult *program, const char *packageDirectory);
void closeBuildCache(buildCache *cache);
const char *buildCacheIRKey(buildCache *cache);
int buildCacheObjectKey(buildCache *cache, const char *irPath, char key[65]);
int readBuildCache(buildCache *cache, const char *key, const char *destination);
void storeBuildCache(buildCache *cache, const char *key, const char *source);
int cleanBuildCache(void);
#endif
