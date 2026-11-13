#ifndef OCEAN_PLATFORM_H
#define OCEAN_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include "windows/compat.h"
#else
#include <sys/types.h>
#include <sys/wait.h>
#endif
/* Spawn without a shell. Descriptor mappings are inherited only by the child. */
typedef struct oceanDescriptor { int source, destination; } oceanDescriptor;
pid_t oceanSpawn(char *const argv[], const oceanDescriptor *descriptors, size_t count);
int oceanRun(char *const argv[]);
void oceanTemporary(char *buffer, size_t capacity, const char *name);
char *oceanExecutable(void);
#endif
