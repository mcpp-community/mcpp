#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Refuses opening an existing record for reading, as Windows refuses a name a
 * rename has just installed: MCPP_TEST_REFUSE_NAME names the file and
 * MCPP_TEST_REFUSE_COUNT how many opens are refused (-1: every one). Each
 * refusal appends a line to `<name>-refused`. */
static FILE *(*native_open)(const char *, const char *);
static long refused;

FILE *fopen64(const char *path, const char *mode) {
    if (!native_open) native_open = dlsym(RTLD_NEXT, "fopen64");
    const char *name = getenv("MCPP_TEST_REFUSE_NAME");
    const char *count = getenv("MCPP_TEST_REFUSE_COUNT");
    const long limit = count ? strtol(count, NULL, 10) : 0;
    if (name && strcmp(path, name) == 0 && mode[0] == 'r' && (limit < 0 || refused < limit)) {
        ++refused;
        char marker[512];
        snprintf(marker, sizeof marker, "%s-refused", name);
        FILE *m = native_open(marker, "ab");
        if (m) { fputs("refused\n", m); fclose(m); }
        errno = EACCES;
        return NULL;
    }
    return native_open(path, mode);
}

FILE *fopen(const char *path, const char *mode) { return fopen64(path, mode); }
