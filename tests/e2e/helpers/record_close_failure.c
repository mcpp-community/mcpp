#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Fail only the final close of the real runtime-record temporary. The native
 * close still happens, so the caller must reject publication rather than rely
 * on a later destructor or a leaked stream. */
int fclose(FILE *stream) {
    static int (*original)(FILE *);
    if (!original) original = dlsym(RTLD_NEXT, "fclose");
    char descriptor[64], name[4096];
    const int fd = fileno(stream);
    snprintf(descriptor, sizeof descriptor, "/proc/self/fd/%d", fd);
    const ssize_t size = readlink(descriptor, name, sizeof name - 1);
    if (size >= 0) name[size] = 0;
    const int selected = size >= 0 && strstr(name, "/.mcpp-record-") != NULL;
    const int result = original(stream);
    if (!selected) return result;
    const int marker = open("record-close-refused", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (marker >= 0) { (void)write(marker, "injected", 8); close(marker); }
    errno = EIO;
    return EOF;
}
