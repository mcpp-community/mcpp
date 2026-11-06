/* [temp] probe 885: contenders against OFD locks whose holders end by exiting
   (no unlock, no close), as mcpp's compiler executors do. 4000 processes in
   waves of 16 each take the first free of four slots, hold it 200 us and exit;
   every errno other than EAGAIN is counted. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define F_OFD_SETLK 90
static int child(const char* dir) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
        for (int i = 0; i < 4; ++i) {
            char path[512];
            snprintf(path, sizeof path, "%s/slot-%d.lock", dir, i);
            int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
            if (fd < 0) return 100 + (errno & 0x7f);
            struct flock fl = {0};
            fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
            if (fcntl(fd, F_OFD_SETLK, &fl) == 0) { usleep(200); _exit(0); }
            if (errno != EAGAIN) return errno & 0x7f;
            close(fd);
        }
        usleep(20);
    }
    return 99;
}
int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    int counts[256] = {0};
    for (int wave = 0; wave < 250; ++wave) {
        pid_t pids[16];
        for (int k = 0; k < 16; ++k) if ((pids[k] = fork()) == 0) _exit(child(dir));
        for (int k = 0; k < 16; ++k) { int st = 0; waitpid(pids[k], &st, 0); counts[WEXITSTATUS(st)]++; }
    }
    for (int e = 0; e < 256; ++e)
        if (counts[e]) printf("exit-holder contender: %d (%s): %d\n", e, e == 0 ? "acquired" : e >= 100 ? "open failed" : strerror(e), counts[e]);
    return 0;
}
