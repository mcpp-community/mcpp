/* [temp] probe 885: the FIRST F_OFD_SETLK of a process on a lock another
   process holds. A holder keeps slot-0 locked; 2000 short-lived processes
   each open it and try once; their errnos are counted. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define F_OFD_SETLK 90
static int try_once(const char* path) {
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) return 1000 + errno;
    struct flock fl = {0};
    fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
    int r = fcntl(fd, F_OFD_SETLK, &fl);
    int e = r == 0 ? 0 : errno;
    close(fd);
    return e;
}
int main(int argc, char** argv) {
    char path[512];
    snprintf(path, sizeof path, "%s/slot-0.lock", argc > 1 ? argv[1] : ".");
    int holder = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    struct flock fl = {0};
    fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
    if (fcntl(holder, F_OFD_SETLK, &fl) != 0) { perror("holder"); return 1; }
    int counts[2000] = {0};
    for (int i = 0; i < 2000; ++i) {
        pid_t p = fork();
        if (p == 0) _exit(try_once(path) & 0xff);
        int st = 0; waitpid(p, &st, 0);
        counts[WEXITSTATUS(st)]++;
    }
    for (int e = 0; e < 256; ++e) if (counts[e]) printf("first attempt errno %d (%s): %d\n", e, strerror(e), counts[e]);
    return 0;
}
