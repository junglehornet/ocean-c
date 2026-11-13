#ifndef OCEAN_LSP_H
#define OCEAN_LSP_H
/* Serve Language Server Protocol messages over stdin/stdout. */
int oceanLsp(void);
#ifdef _WIN32
int oceanLspWorker(const char *, const char *);
#endif
#endif
