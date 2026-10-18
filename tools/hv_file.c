/* hv_file.c: see hv_file.h. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700       /* realpath, mkstemp, fchown */
#endif

#include "hv_file.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jpeg2000/hv_error.h"

/* The suffix mkstemp replaces. */
static const char suffix[] = ".XXXXXX";

/* Only one output is active at a time; keep its stdio buffer off the stack. */
static char output_buffer[128 * 1024];

/* ------------------------------------------------------------------------
 * The temporary file removed on a signal
 * ------------------------------------------------------------------------ */

static const int cleanup_signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGBUS};
#define NSIGNALS (sizeof cleanup_signals / sizeof *cleanup_signals)
static struct sigaction previous[NSIGNALS];
static volatile sig_atomic_t installed[NSIGNALS];         /* not for a signal ignored before */
static char *volatile pending;          /* the temporary file, while it exists */

/* Removes the temporary file, says why for SIGBUS, puts back the handler
 * that was there and raises the signal again, which that handler (or the
 * default) takes. unlink, write, sigaction and raise are
 * async-signal-safe. */
static void on_signal(int sig) {
    static const char bus[] =
        "SIGBUS (an input file changed while it was read?); the output is left as it was\n";
    size_t i;
    int saved = errno;
    if (pending != NULL)
        unlink(pending);
    pending = NULL;
    if (sig == SIGBUS) {
        ssize_t written = write(STDERR_FILENO, bus, sizeof bus - 1);
        (void)written;                  /* nothing more to do here */
    }
    for (i = 0; i < NSIGNALS; i++)
        if (installed[i]) {
            sigaction(cleanup_signals[i], &previous[i], NULL);
            installed[i] = 0;
        }
    raise(sig);
    errno = saved;
}

/* Blocks the cleanup signals, keeping the mask that was in *old. */
static void block_signals(sigset_t *old) {
    sigset_t block;
    size_t i;
    sigemptyset(&block);
    for (i = 0; i < NSIGNALS; i++)
        sigaddset(&block, cleanup_signals[i]);
    sigprocmask(SIG_BLOCK, &block, old);
}

static void watch(char *tmp) {
    struct sigaction sa;
    size_t i;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < NSIGNALS; i++)
        sigaddset(&sa.sa_mask, cleanup_signals[i]);
    pending = tmp;
    for (i = 0; i < NSIGNALS; i++) {
        /* A signal the process ignores (nohup's SIGHUP) stays ignored. */
        installed[i] = sigaction(cleanup_signals[i], NULL, &previous[i]) == 0 &&
                       previous[i].sa_handler != SIG_IGN &&
                       sigaction(cleanup_signals[i], &sa, NULL) == 0;
    }
}

static void unwatch(void) {
    size_t i;
    if (pending == NULL)
        return;
    for (i = 0; i < NSIGNALS; i++)
        if (installed[i]) {
            sigaction(cleanup_signals[i], &previous[i], NULL);
            installed[i] = 0;
        }
    pending = NULL;
}

static void clear(hv_file *f) {
    unwatch();
    free(f->tmp);
    free(f->target);
    f->tmp = f->target = NULL;
    f->file = NULL;
    f->fd = -1;
}

/* Closes and removes the temporary file, keeping errno. */
static void discard(hv_file *f) {
    int saved = errno;
    if (f->file != NULL)
        fclose(f->file);
    else if (f->fd >= 0)
        close(f->fd);
    if (f->tmp != NULL)
        unlink(f->tmp);
    clear(f);
    errno = saved;
}

/* The file that opening `path` for writing would write: path with its
 * symbolic links resolved (realpath), or, where it does not exist yet, the
 * path itself or the one its last link names, as open follows a link to a
 * file it creates. NULL with errno set: ELOOP for a link cycle. */
static char *resolve(const char *path) {
    char *p = strdup(path);
    int links;
    for (links = 0; p != NULL; links++) {
        char *real = realpath(p, NULL), *target = NULL, *next;
        const char *slash;
        struct stat st;
        size_t size = 256, dir;
        ssize_t n = 0;
        if (real != NULL || lstat(p, &st) != 0 || !S_ISLNK(st.st_mode)) {
            if (real == NULL)
                return p;                   /* a file to be created */
            free(p);
            return real;
        }
        if (links == 40) {                  /* as Linux's MAXSYMLINKS */
            free(p);
            errno = ELOOP;
            return NULL;
        }
        do {
            char *grown = realloc(target, size *= 2);
            if (grown == NULL) {
                free(target);
                free(p);
                errno = ENOMEM;
                return NULL;
            }
            target = grown;
        } while ((n = readlink(p, target, size)) >= 0 && (size_t)n == size);
        if (n < 0) {
            free(target);
            free(p);
            return NULL;
        }
        /* A relative target is relative to the link's directory. */
        slash = strrchr(p, '/');
        dir = target[0] == '/' || slash == NULL ? 0 : (size_t)(slash - p) + 1;
        if ((next = malloc(dir + (size_t)n + 1)) != NULL) {
            memcpy(next, p, dir);
            memcpy(next + dir, target, (size_t)n);
            next[dir + (size_t)n] = 0;
        } else {
            errno = ENOMEM;
        }
        free(target);
        free(p);
        p = next;
    }
    return NULL;
}

int hv_file_create(hv_file *f, const char *path, int mode, char *error, size_t error_size) {
    struct stat st;
    sigset_t old;
    mode_t mask;
    size_t n;
    int replaced, saved;

    f->path = path;
    f->target = f->tmp = NULL;
    f->file = NULL;
    f->fd = -1;
    /* The handlers know one temporary file (pending): a second would make
     * the first's handler its own previous one. */
    if (pending != NULL)
        return hv_fail(error, error_size, "cannot create %s: another output file is open", path);
    if (path[0] == 0)
        errno = ENOENT;                     /* as open gives */
    if (path[0] == 0 || (f->target = resolve(path)) == NULL)
        return hv_fail(error, error_size, "cannot create %s: %s", path, strerror(errno));
    replaced = stat(f->target, &st) == 0 && S_ISREG(st.st_mode);
    if (mode == HV_FILE_KEEP_MODE) {
        mask = umask(0);
        umask(mask);
        mode = (int)(replaced ? st.st_mode & 0777 : 0666 & ~mask);
    }
    n = strlen(f->target);
    if ((f->tmp = malloc(n + sizeof suffix)) == NULL) {
        clear(f);
        return hv_fail(error, error_size, "out of memory");
    }
    memcpy(f->tmp, f->target, n);
    memcpy(f->tmp + n, suffix, sizeof suffix);
    /* No cleanup signal between creating the temporary file and watching
     * it: one would leave the file behind. */
    block_signals(&old);
    if ((f->fd = mkstemp(f->tmp)) >= 0)
        watch(f->tmp);
    saved = errno;
    sigprocmask(SIG_SETMASK, &old, NULL);
    if (f->fd < 0) {
        hv_fail(error, error_size, "cannot create %s: %s", path, strerror(saved));
        clear(f);               /* nothing was created */
        return -1;
    }
    /* The replaced file's owner and group, or its group alone, as far as
     * the process may give them; before fchmod, as a change of owner may
     * clear mode bits. */
    if (replaced && fchown(f->fd, st.st_uid, st.st_gid) != 0) {
        int group = fchown(f->fd, (uid_t)-1, st.st_gid);
        (void)group;            /* if not, the process's, as a new file's */
    }
    if (fchmod(f->fd, (mode_t)(mode & 0777)) != 0 || (f->file = fdopen(f->fd, "wb")) == NULL) {
        discard(f);
        return hv_fail(error, error_size, "cannot write %s: %s", path, strerror(errno));
    }
    if (setvbuf(f->file, output_buffer, _IOFBF, sizeof output_buffer) != 0) {
        discard(f);
        return hv_fail(error, error_size, "cannot buffer %s", path);
    }
    return 0;
}

int hv_file_write(hv_file *f, const void *bytes, size_t size, char *error, size_t error_size) {
    if (size != 0 && fwrite(bytes, 1, size, f->file) != size)
        return hv_fail(error, error_size, "cannot write %s: %s", f->path, strerror(errno));
    return 0;
}

int hv_file_commit(hv_file *f, char *error, size_t error_size) {
    int status = 0, saved = 0;
    if (f->file != NULL && fflush(f->file) != 0)
        status = -1;
    if (status != 0)
        saved = errno;
    if ((f->file != NULL ? fclose(f->file) : close(f->fd)) != 0 && status == 0) {
        status = -1;
        saved = errno;
    }
    f->file = NULL;
    f->fd = -1;
    if (status == 0 && rename(f->tmp, f->target) != 0) {
        status = -1;
        saved = errno;
    }
    if (status != 0) {
        errno = saved;
        discard(f);
        return hv_fail(error, error_size, "cannot write %s: %s", f->path, strerror(saved));
    }
    clear(f);
    return 0;
}

void hv_file_abort(hv_file *f) {
    discard(f);
}
