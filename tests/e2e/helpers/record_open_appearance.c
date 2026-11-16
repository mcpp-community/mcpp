#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Simulate an atomic publish between the reader's first open and its
 * existence probe. The real completed record remains present throughout. */
static FILE *(*native_open)(const char *, const char *);
static int injected;

FILE *fopen64(const char *path, const char *mode) {
    if (!native_open) native_open = dlsym(RTLD_NEXT, "fopen64");
    if (!injected && strcmp(path, "record") == 0 && mode[0] == 'r') {
        injected = 1;
        FILE *marker = native_open("record-open-interposed", "wb");
        if (marker) fclose(marker);
        errno = ENOENT;
        return NULL;
    }
    return native_open(path, mode);
}

FILE *fopen(const char *path, const char *mode) { return fopen64(path, mode); }
