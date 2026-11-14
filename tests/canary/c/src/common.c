/* The checks every row of the matrix runs. Each prints `ok <what>` or `FAIL <what>`;
 * the status is the number of failures. */
#define _POSIX_C_SOURCE 200112L
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "canary.h"

static int failures;
static void check(int held, const char *what)
{
    printf("%s %s\n", held ? "ok  " : "FAIL", what);
    if (!held) ++failures;
}

static sigjmp_buf jump;
static void leave(int depth)
{
    if (depth == 0) siglongjmp(jump, 7);
    leave(depth - 1);
}

int canary_run(const char *role, int argc, char **argv)
{
    char line[64];
    FILE *f;
    int jumped;

    if (argc > 1 && strcmp(argv[1], "--child") == 0) return 3;
    printf("canary-%s: %d argument(s)\n", role, argc);

    /* V9: a non-local jump out of nested calls. */
    jumped = sigsetjmp(jump, 1);
    if (jumped == 0) leave(3);
    check(jumped == 7, "sigsetjmp and siglongjmp");

    /* A file written and read back. */
    f = fopen("canary.tmp", "w");
    check(f != NULL, "a file is created");
    if (f != NULL) {
        fputs("canary\n", f);
        fclose(f);
        f = fopen("canary.tmp", "r");
        check(f != NULL && fgets(line, sizeof line, f) != NULL && strcmp(line, "canary\n") == 0,
              "a file reads back what was written");
        if (f != NULL) fclose(f);
        remove("canary.tmp");
    }

    /* The arguments arrive as given. */
    check(argc < 2 || strcmp(argv[argc - 1], "last") == 0, "the arguments arrive");
    return failures;
}
