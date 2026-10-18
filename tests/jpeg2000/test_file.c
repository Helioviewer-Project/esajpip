/* test_file: hv_file, which hv_transcode and hv_merge replace their output
 * with: the contents and permissions committed, nothing left by an abort,
 * a symbolic link's target replaced, and the temporary file removed when
 * a signal ends the process, whose handlers are put back after: SIGBUS
 * too, from a mapped input truncated while it is read.
 *
 *   test_file */
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1       /* mkdtemp: Darwin hides it under _XOPEN_SOURCE */
#endif

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tools/hv_file.h"

static int failures;

static void check(int ok, const char *what, const char *detail) {
    if (!ok) {
        printf("FAIL %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
        failures++;
    }
}

/* The entries of dir besides "." and "..". */
static int entries(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL)
        n += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
    closedir(d);
    return n;
}

/* The contents of path are text. */
static int holds(const char *path, const char *text) {
    char buf[64];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return n == strlen(text) && memcmp(buf, text, n) == 0;
}

static void handler(int sig) { (void)sig; }

int main(void) {
    char dir[] = "/tmp/test_file.XXXXXX", out[64], target[64], link[64], error[512];
    struct sigaction sa, now;
    struct stat st;
    hv_file f;
    pid_t child;
    int status;

    if (mkdtemp(dir) == NULL) {
        printf("FAIL mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    snprintf(out, sizeof out, "%s/out", dir);
    snprintf(target, sizeof target, "%s/target", dir);
    snprintf(link, sizeof link, "%s/link", dir);

    /* Committed: the contents, the permissions, nothing else. */
    check(hv_file_create(&f, out, 0640, error, sizeof error) == 0 &&
          hv_file_write(&f, "new", 3, error, sizeof error) == 0 &&
          hv_file_commit(&f, error, sizeof error) == 0, "commit", error);
    check(holds(out, "new") && stat(out, &st) == 0 && (st.st_mode & 0777) == 0640 &&
          entries(dir) == 1, "the committed file", NULL);

    /* Aborted: the output as it was. */
    check(hv_file_create(&f, out, HV_FILE_KEEP_MODE, error, sizeof error) == 0 &&
          hv_file_write(&f, "other", 5, error, sizeof error) == 0, "create", error);
    hv_file_abort(&f);
    check(holds(out, "new") && entries(dir) == 1, "an aborted file", NULL);

    /* Replaced: its owner and group kept, another user's when run as root
     * (uid and gid 1), else the process's own. */
    {
        struct stat before;
        if (geteuid() == 0 && chown(out, 1, 1) != 0)
            check(0, "chown", strerror(errno));
        check(stat(out, &before) == 0 &&
              hv_file_create(&f, out, HV_FILE_KEEP_MODE, error, sizeof error) == 0 &&
              hv_file_write(&f, "new", 3, error, sizeof error) == 0 &&
              hv_file_commit(&f, error, sizeof error) == 0, "replace", error);
        check(holds(out, "new") && stat(out, &st) == 0 && st.st_uid == before.st_uid &&
              st.st_gid == before.st_gid && (st.st_mode & 0777) == 0640 && entries(dir) == 1,
              "the replaced file's owner, group and mode", NULL);
    }

    /* Through a symbolic link: its target replaced, the link kept. */
    check(symlink("target", link) == 0 &&
          hv_file_create(&f, link, 0600, error, sizeof error) == 0 &&
          hv_file_write(&f, "linked", 6, error, sizeof error) == 0 &&
          hv_file_commit(&f, error, sizeof error) == 0, "commit through a link", error);
    check(holds(target, "linked") && lstat(link, &st) == 0 && S_ISLNK(st.st_mode) &&
          entries(dir) == 3, "the link's target", NULL);

    /* The handlers: ours while the temporary file exists, the previous one
     * (here a custom one) after. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    check(hv_file_create(&f, out, 0600, error, sizeof error) == 0, "create", error);
    check(sigaction(SIGTERM, NULL, &now) == 0 && now.sa_handler != handler,
          "a handler while writing", NULL);
    hv_file_abort(&f);
    check(sigaction(SIGTERM, NULL, &now) == 0 && now.sa_handler == handler,
          "the previous handler after", NULL);
    signal(SIGINT, handler);
    check(hv_file_create(&f, out, 0600, error, sizeof error) == 0, "create before returning signal", error);
    raise(SIGTERM);
    check(sigaction(SIGINT, NULL, &now) == 0 && now.sa_handler == handler &&
          sigaction(SIGTERM, NULL, &now) == 0 && now.sa_handler == handler,
          "all handlers restored after a signal returns", NULL);
    hv_file_abort(&f);
    check(hv_file_create(&f, out, 0600, error, sizeof error) == 0, "create after returning signal", error);
    hv_file_abort(&f);
    raise(SIGINT);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    /* An ignored signal stays ignored. */
    signal(SIGHUP, SIG_IGN);
    check(hv_file_create(&f, out, 0600, error, sizeof error) == 0, "create", error);
    check(sigaction(SIGHUP, NULL, &now) == 0 && now.sa_handler == SIG_IGN,
          "an ignored signal while writing", NULL);
    hv_file_abort(&f);
    signal(SIGHUP, SIG_DFL);

    /* One hv_file at a time: a second create fails and changes nothing,
     * the first's handler stays and its abort puts the default back; a
     * create leaves the signal mask as it was. */
    {
        static const int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGBUS};
        hv_file g;
        sigset_t before, after;
        char second[80];
        size_t i;
        int n = entries(dir), same = 1;
        snprintf(second, sizeof second, "%s/second", dir);
        sigprocmask(SIG_SETMASK, NULL, &before);
        check(hv_file_create(&f, out, 0600, error, sizeof error) == 0, "create", error);
        sigprocmask(SIG_SETMASK, NULL, &after);
        for (i = 0; i < sizeof sigs / sizeof *sigs; i++)
            same &= sigismember(&before, sigs[i]) == sigismember(&after, sigs[i]);
        check(same, "the signal mask after a create", NULL);
        check(hv_file_create(&g, second, 0600, error, sizeof error) != 0 &&
                  strstr(error, "another output file is open") != NULL &&
                  access(second, F_OK) != 0 && entries(dir) == n + 1,
              "a second hv_file while one is open", error);
        check(sigaction(SIGTERM, NULL, &now) == 0 && now.sa_handler != SIG_DFL &&
                  now.sa_handler != SIG_IGN,
              "the first's handler after a second create", NULL);
        hv_file_abort(&f);
        check(sigaction(SIGTERM, NULL, &now) == 0 && now.sa_handler == SIG_DFL &&
                  entries(dir) == n,
              "the default handler and no temporary file after the abort", NULL);
        check(hv_file_create(&g, second, 0600, error, sizeof error) == 0, "create after abort",
              error);
        hv_file_abort(&g);
    }

    /* A signal while writing: the process ends by it, and only the files
     * from before are left. */
    fflush(stdout);
    if ((child = fork()) == 0) {
        if (hv_file_create(&f, out, 0600, error, sizeof error) != 0 ||
            hv_file_write(&f, "partial", 7, error, sizeof error) != 0 || fflush(f.file) != 0)
            _exit(3);
        raise(SIGTERM);
        _exit(4);
    }
    check(child > 0 && waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
          WTERMSIG(status) == SIGTERM, "the process ends by the signal", NULL);
    check(holds(out, "new") && entries(dir) == 3, "the temporary file removed on a signal",
          NULL);

    /* A mapped input truncated by another process: reading its lost bytes
     * raises SIGBUS, which ends the process the same way. */
    fflush(stdout);
    if ((child = fork()) == 0) {
        char input[64];
        static const char page[4096] = {1};
        const volatile char *p;
        int fd;
        snprintf(input, sizeof input, "%s/input", dir);
        if ((fd = open(input, O_RDWR | O_CREAT, 0600)) < 0 ||
            write(fd, page, sizeof page) != (ssize_t)sizeof page ||
            (p = mmap(NULL, sizeof page, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
            _exit(3);
        if (hv_file_create(&f, out, 0600, error, sizeof error) != 0 ||
            hv_file_write(&f, "partial", 7, error, sizeof error) != 0 || fflush(f.file) != 0)
            _exit(3);
        if (ftruncate(fd, 0) != 0 || unlink(input) != 0)
            _exit(3);
        if (p[0] == 1)                  /* SIGBUS */
            _exit(4);
        _exit(5);
    }
    /* By SIGBUS, or by the handler that was there before (a sanitizer's
     * reports it and exits), never past the read. */
    check(child > 0 && waitpid(child, &status, 0) == child &&
          (WIFSIGNALED(status) ? WTERMSIG(status) == SIGBUS || WTERMSIG(status) == SIGABRT
                               : WIFEXITED(status) && WEXITSTATUS(status) != 0 &&
                                     WEXITSTATUS(status) < 3),
          "the process ends at the lost bytes", NULL);
    check(holds(out, "new") && entries(dir) == 3, "the temporary file removed on SIGBUS",
          NULL);

    unlink(out);
    unlink(link);
    unlink(target);
    rmdir(dir);
    printf(failures ? "%d failures\n" : "all checks passed\n", failures);
    return failures != 0;
}
