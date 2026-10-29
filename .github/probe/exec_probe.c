// #857 probe: each step of openkal-linux 0.18.2's start of a program
// (src/process.cpp:684-712), by raw system call, with its errno. Prints and
// never asserts.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static void say(const char* step, long r) {
    if (r < 0) printf("%-44s -> -1 errno=%d (%s)\n", step, errno, strerror(errno));
    else printf("%-44s -> %ld\n", step, r);
    fflush(stdout);
}

typedef long (*starter)(void*);
struct ctx { int base, exe; const char* abs; const char* name; char** argv; char** envp; };

static long do_execveat_empty(void* p) { struct ctx* c = p; return syscall(SYS_execveat, c->exe, "", c->argv, c->envp, AT_EMPTY_PATH); }
static long do_execveat_rel(void* p) { struct ctx* c = p; return syscall(SYS_execveat, c->base, c->name, c->argv, c->envp, 0); }
static long do_execve_abs(void* p) { struct ctx* c = p; return syscall(SYS_execve, c->abs, c->argv, c->envp); }
static long do_execveat_empty_noclo(void* p) {
    struct ctx* c = p; fcntl(c->exe, F_SETFD, 0);
    return syscall(SYS_execveat, c->exe, "", c->argv, c->envp, AT_EMPTY_PATH);
}

// Runs `f` in a child; the child writes its errno into a pipe when the exec fails.
static void in_child(const char* step, starter f, struct ctx* c) {
    int pfd[2]; if (pipe2(pfd, O_CLOEXEC) != 0) { say("pipe2", -1); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        int devnull = open("/dev/null", O_WRONLY); dup2(devnull, 1); dup2(devnull, 2);
        f(c);
        int e = errno; (void)!write(pfd[1], &e, sizeof e); _exit(127);
    }
    close(pfd[1]);
    int e = 0; ssize_t n = read(pfd[0], &e, sizeof e); close(pfd[0]);
    int st = 0; waitpid(pid, &st, 0);
    if (n == sizeof e) printf("%-44s -> exec failed errno=%d (%s)\n", step, e, strerror(e));
    else printf("%-44s -> exec ok, exit status %d\n", step, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    fflush(stdout);
}

int main(int argc, char** argv, char** envp) {
    if (argc < 3) { fprintf(stderr, "usage: %s <dir> <name> [args...]\n", argv[0]); return 2; }
    struct ctx c = { .name = argv[2], .argv = argv + 2, .envp = envp };
    char abs[4096]; snprintf(abs, sizeof abs, "%s/%s", argv[1], argv[2]); c.abs = abs;
    printf("== target %s\n", abs);

    c.base = open(argv[1], O_PATH | O_DIRECTORY | O_CLOEXEC); say("open(dir, O_PATH|O_DIRECTORY)", c.base);
    c.exe = openat(c.base, argv[2], O_PATH | O_CLOEXEC); say("openat(base, name, O_PATH|O_CLOEXEC)", c.exe);

    char link[64], named[4096]; snprintf(link, sizeof link, "/proc/self/fd/%d", c.base);
    ssize_t n = readlink(link, named, sizeof named - 1); say("readlink(/proc/self/fd/<base>)", n);
    if (n >= 0) { named[n] = 0; printf("   named = %s\n", named); }

    struct stat a, b;
    say("fstatat(exe, \"\", AT_EMPTY_PATH)", fstatat(c.exe, "", &a, AT_EMPTY_PATH));
    say("stat(absolute name)", stat(abs, &b));
    printf("   same_file = %d\n", a.st_dev == b.st_dev && a.st_ino == b.st_ino);
    say("access(absolute, X_OK)", access(abs, X_OK));

    in_child("execveat(exe, \"\", AT_EMPTY_PATH)", do_execveat_empty, &c);
    in_child("execveat(exe, \"\", AT_EMPTY_PATH) without CLOEXEC", do_execveat_empty_noclo, &c);
    in_child("execveat(base, name, 0)", do_execveat_rel, &c);
    in_child("execve(absolute name)", do_execve_abs, &c);
    return 0;
}
