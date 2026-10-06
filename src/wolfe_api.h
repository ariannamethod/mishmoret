#ifndef MISHMERET_WOLFE_API_H
#define MISHMERET_WOLFE_API_H
#include <stddef.h>

/* Public embedding ABI from vendor/wolfe/wolfe.c. */
typedef struct Wolfe Wolfe;
enum { WOLFE_OK = 0, WOLFE_ERROR = 1, WOLFE_BUFFER_TOO_SMALL = 2 };
enum { WOLFE_NEURAL = 0, WOLFE_FIELD = 1, WOLFE_KEYWORD = 2 };
enum { WOLFE_REASONING_OFF = 0, WOLFE_REASONING_FULL = 1, WOLFE_REASONING_COMPACT = 2 };
Wolfe *wolfe_load(const char *, const char *, const char *, char *, size_t);
int wolfe_call(Wolfe *, const char *, int, int, char *, size_t, size_t *);
const char *wolfe_error(const Wolfe *);
void wolfe_free(Wolfe *);
#endif
