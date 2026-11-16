#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Model an absent status sampled immediately before the controller publishes
 * it and exits. Only a real split-compiler waiter is affected, once per process.
 * No certificate, status, output or process lifetime is changed by this hook. */
static FILE *(*native_open)(const char *, const char *);
static int injected;

static int waiter(void) {
    char command[8192];
    FILE *in = native_open("/proc/self/cmdline", "rb");
    if (!in) return 0;
    size_t n = fread(command, 1, sizeof command - 1, in);
    fclose(in); command[n] = 0;
    char *arg = memchr(command, 0, n);
    return arg && (!strcmp(arg + 1, "bmi-await") || !strcmp(arg + 1, "bmi-compile"));
}

static int first_field(const char *path, char *value, size_t capacity) {
    FILE *in = native_open(path, "rb");
    if (!in) return 0;
    char line[128]; size_t n = 0;
    int ok = fgets(line, sizeof line, in) && fgets(line, sizeof line, in)
        && fgets(line, sizeof line, in) && sscanf(line, "%zu", &n) == 1
        && n > 0 && n < capacity && fread(value, 1, n, in) == n;
    fclose(in);
    if (ok) value[n] = 0;
    return ok;
}

static int ended(long pid) {
    char path[64], line[4096];
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    FILE *in = native_open(path, "rb");
    if (!in) return errno == ENOENT;
    int ok = fgets(line, sizeof line, in) != NULL;
    fclose(in);
    char *end = ok ? strrchr(line, ')') : NULL;
    return end && end[1] == ' ' && (end[2] == 'Z' || end[2] == 'X');
}

FILE *fopen64(const char *path, const char *mode) {
    if (!native_open) native_open = dlsym(RTLD_NEXT, "fopen64");
    FILE *result = native_open(path, mode);
    size_t n = strlen(path);
    if (!result || injected || mode[0] != 'r' || n < 3
        || strcmp(path + n - 3, ".rc") || !waiter()) return result;
    char pointer[4096], directory[4096], controller[4096], pid_text[64];
    if (n + sizeof ".transaction" >= sizeof pointer) return result;
    memcpy(pointer, path, n - 3); strcpy(pointer + n - 3, ".transaction");
    if (!first_field(pointer, directory, sizeof directory)) return result;
    int length = snprintf(controller, sizeof controller, "%s/controller", directory);
    if (length < 0 || (size_t)length >= sizeof controller
        || !first_field(controller, pid_text, sizeof pid_text)) return result;
    long pid = strtol(pid_text, NULL, 10);
    if (pid <= 1) return result;
    for (int tries = 0; tries < 600 && !ended(pid); ++tries)
        nanosleep(&(struct timespec){ .tv_nsec = 5000000 }, NULL);
    if (!ended(pid)) return result;
    /* Make the waiter's next bounded controller observation due. */
    nanosleep(&(struct timespec){ .tv_nsec = 80000000 }, NULL);
    injected = 1;
    fclose(result);
    char marker[4096];
    if (snprintf(marker, sizeof marker, "%s.completion-race", path) >= (int)sizeof marker) abort();
    FILE *receipt = native_open(marker, "wb");
    if (!receipt) abort();
    fputs("status exists; controller ended; one earlier absent observation replayed\n", receipt);
    if (fclose(receipt)) abort();
    errno = ENOENT;
    return NULL;
}

FILE *fopen(const char *path, const char *mode) { return fopen64(path, mode); }
