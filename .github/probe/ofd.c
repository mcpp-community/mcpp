/* [temp] probe 885: the errno of a contended non-waiting F_OFD_SETLK on macOS.
   Eight processes open (O_CREAT) and try-lock four files in turn, as
   mcpp's compiler slots do, and count every errno they see. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define F_OFD_SETLK 90
int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    for (int p = 0; p < 8; ++p) {
        if (fork() == 0) {
            int counts[128] = {0};
            for (int round = 0; round < 4000; ++round) {
                for (int i = 0; i < 4; ++i) {
                    char path[512];
                    snprintf(path, sizeof path, "%s/slot-%d.lock", dir, i);
                    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
                    if (fd < 0) { counts[errno < 128 ? errno : 127]++; continue; }
                    struct flock fl = {0};
                    fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
                    int r = fcntl(fd, F_OFD_SETLK, &fl);
                    if (r == 0) { usleep(50); fl.l_type = F_UNLCK; fcntl(fd, F_OFD_SETLK, &fl); counts[0]++; close(fd); break; }
                    counts[errno < 128 ? errno : 127]++;
                    close(fd);
                }
            }
            for (int e = 0; e < 128; ++e)
                if (counts[e]) printf("child %d: %s (%d): %d\n", p, e ? strerror(e) : "acquired", e, counts[e]);
            fflush(stdout);
            _exit(0);
        }
    }
    while (wait(NULL) > 0) {}
    return 0;
}
