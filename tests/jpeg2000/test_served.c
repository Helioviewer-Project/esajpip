/* test_served: hv_served.c, included, with open, fstat, read and malloc
 * that misbehave on demand: every branch of hv_load_file (a failed open or
 * fstat, a file that is no regular file, of a negative size, over the
 * limit, out of memory, reads that are interrupted, short, cut or fail,
 * and file descriptor 0), hv_path_within and hv_is_jpx_name at their
 * edges, and what hv_check_served reports (the counts, the offsets, the
 * linked path) for the corpus's linked file and its frames copied to a
 * temporary directory, one of them missing or unreadable. No descriptor
 * is left open and nothing leaks.
 *
 *   test_served <vector directory> */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1       /* mkdtemp: Darwin hides it under _XOPEN_SOURCE */
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The misbehaviors, each once when set. */
static int open_fails, fstat_fails, fstat_negative, fstat_fifo, malloc_fails;
/* 1: every file has device 1 and inode 1; 2: inode 1 and its inode for
 * its device, so that no two files share a device. */
static int fstat_identity;
static int read_eintr, read_short, read_zero, read_fails, read_grown;
static int opened, closed, calloc_fails;
static size_t read_bytes;

static int test_open(const char *path, int flags, ...) {
    int fd;
    if (open_fails) {
        open_fails = 0;
        errno = EACCES;
        return -1;
    }
    fd = open(path, flags);
    opened += fd >= 0;
    return fd;
}

/* close and free succeed and leave errno 0, which the caller must not
 * report. */
static int test_close(int fd) {
    int status = close(fd);
    closed++;
    errno = 0;
    return status;
}

static void test_free(void *p) {
    free(p);
    errno = 0;
}

static int test_fstat(int fd, struct stat *st) {
    int status;
    if (fstat_fails) {
        fstat_fails = 0;
        errno = EIO;
        return -1;
    }
    status = fstat(fd, st);
    if (fstat_negative) {
        fstat_negative = 0;
        st->st_size = -1;
    }
    if (fstat_fifo) {
        fstat_fifo = 0;
        st->st_mode = (st->st_mode & ~(mode_t)S_IFMT) | S_IFIFO;
    }
    if (fstat_identity) {
        st->st_dev = fstat_identity == 1 ? 1 : (dev_t)st->st_ino;
        st->st_ino = 1;
    }
    return status;
}

static ssize_t test_read(int fd, void *p, size_t n) {
    if (read_eintr) {
        read_eintr--;
        errno = EINTR;
        return -1;
    }
    if (read_fails) {
        read_fails = 0;
        errno = EBADF;
        return -1;
    }
    if (read_zero) {
        read_zero = 0;
        errno = EINTR;          /* left from before: an end is no interruption */
        return 0;
    }
    if (read_grown == 2) {      /* one byte, then */
        read_grown = 1;
        return read(fd, p, 1);
    }
    if (read_grown) {           /* the file grew since fstat: all it is asked */
        read_grown = 0;
        memset(p, 'x', n);
        return (ssize_t)n;
    }
    if (read_short && n > 1)
        n = 1;
    {
        ssize_t got = read(fd, p, n);
        read_bytes += got > 0 ? (size_t)got : 0;
        return got;
    }
}

/* A request for 0 bytes fails, as C allows, and so does one above 1 GiB. */
static void *test_malloc(size_t n) {
    if (malloc_fails || n == 0 || n > ((size_t)1 << 30)) {
        malloc_fails = 0;
        errno = ENOMEM;
        return NULL;
    }
    return malloc(n);
}

static void *test_calloc(size_t n, size_t size) {
    if (calloc_fails) {
        calloc_fails = 0;
        errno = ENOMEM;
        return NULL;
    }
    return calloc(n, size);
}

#define open test_open
#define calloc test_calloc
#define close test_close
#define fstat test_fstat
#define read test_read
#define malloc test_malloc
#define free test_free
#include "jpeg2000/hv_served.c"
#undef open
#undef close
#undef fstat
#undef read
#undef malloc
#undef free

static int failures;

static void check(int ok, const char *what, const char *detail) {
    if (!ok) {
        printf("FAIL %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
        failures++;
    }
}

static int write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    int ok = f != NULL && fwrite(p, 1, n, f) == n;
    if (f != NULL && fclose(f) != 0)
        ok = 0;
    return ok;
}

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long size;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0 || (p = malloc((size_t)size + 1)) == NULL) {
        if (f != NULL)
            fclose(f);
        return NULL;
    }
    *n = fread(p, 1, (size_t)size, f);
    fclose(f);
    return p;
}

/* hv_load_file of `path` with the misbehavior set: the contents, or NULL
 * and errno as expected. */
static void load(const char *what, const char *path, size_t max, const uint8_t *want,
                 size_t want_size, int want_errno) {
    size_t size = 12345;
    uint8_t *p;
    int before = opened - closed;
    errno = 0;
    p = hv_load_file(path, max, &size);
    if (want_errno) {
        char detail[64];
        snprintf(detail, sizeof detail, "errno %d, expected %d", errno, want_errno);
        check(p == NULL && errno == want_errno && size == 12345, what, detail);
    } else {
        check(p != NULL && size == want_size && (want_size == 0 || memcmp(p, want, size) == 0),
              what, NULL);
    }
    check(opened - closed == before, what, "a descriptor left open");
    free(p);
}

static void check_load(const char *tmp) {
    char file[128], empty[128], missing[128];
    static const uint8_t text[] = "0123456789abcdef";
    snprintf(file, sizeof file, "%s/f", tmp);
    snprintf(empty, sizeof empty, "%s/e", tmp);
    snprintf(missing, sizeof missing, "%s/m", tmp);
    if (!write_file(file, text, 16) || !write_file(empty, "", 0)) {
        check(0, "test files", strerror(errno));
        return;
    }
    load("a file", file, SIZE_MAX, text, 16, 0);
    load("a file at its limit", file, 16, text, 16, 0);
    load("a file over its limit", file, 15, NULL, 0, EFBIG);
    load("an empty file", empty, 0, text, 0, 0);
    load("a missing file", missing, SIZE_MAX, NULL, 0, ENOENT);
    open_fails = 1;
    load("open fails", file, SIZE_MAX, NULL, 0, EACCES);
    fstat_fails = 1;
    load("fstat fails", file, SIZE_MAX, NULL, 0, EIO);
    fstat_negative = 1;
    load("a negative size", file, SIZE_MAX, NULL, 0, EINVAL);
    fstat_fifo = 1;
    load("no regular file", file, SIZE_MAX, NULL, 0, EINVAL);
    malloc_fails = 1;
    load("out of memory", file, SIZE_MAX, NULL, 0, ENOMEM);
    read_eintr = 2;
    load("reads interrupted", file, SIZE_MAX, text, 16, 0);
    read_short = 1;
    load("reads of one byte", file, SIZE_MAX, text, 16, 0);
    read_short = 0;
    read_zero = 1;
    load("a file cut while read", file, SIZE_MAX, NULL, 0, EIO);
    read_fails = 1;
    load("a read that fails", file, SIZE_MAX, NULL, 0, EBADF);
    read_grown = 2;
    load("a file grown while read", file, SIZE_MAX, (const uint8_t *)"0xxxxxxxxxxxxxxx", 16, 0);
    /* Descriptor 0, with standard input closed. */
    {
        int in = dup(0);
        if (in >= 0 && close(0) == 0) {
            load("descriptor 0", file, SIZE_MAX, text, 16, 0);
            dup2(in, 0);
            close(in);
        }
    }
    unlink(file);
    unlink(empty);
}

static void check_paths(const char *tmp) {
    char a[128], ab[128], sub[128], x[128];
    char *resolved = (char *)"unset";
    snprintf(a, sizeof a, "%s/a", tmp);
    snprintf(ab, sizeof ab, "%s/ab", tmp);
    snprintf(sub, sizeof sub, "%s/a/x", tmp);
    snprintf(x, sizeof x, "%s/ab/x", tmp);
    if (mkdir(a, 0700) != 0 || mkdir(ab, 0700) != 0 || !write_file(sub, "", 0) ||
        !write_file(x, "", 0)) {
        check(0, "test paths", strerror(errno));
        return;
    }
    check(hv_path_within(x, a, &resolved) == 0 && resolved == NULL,
          "a sibling whose name starts with the root's", NULL);
    check(hv_path_within(sub, a, &resolved) == 1 && resolved != NULL &&
          strcmp(resolved + strlen(resolved) - 4, "/a/x") == 0, "a file in the root", NULL);
    free(resolved);
    check(hv_path_within(sub, "/", &resolved) == 1 && resolved != NULL, "under /", NULL);
    free(resolved);
    check(hv_path_within("/", "/", &resolved) == 0 && resolved == NULL, "/ itself", NULL);
    errno = 0;
    check(hv_path_within(sub, x, NULL) == 0, "a file as the root", NULL);
    errno = 0;
    {
        char none[160];
        snprintf(none, sizeof none, "%s/none", tmp);
        check(hv_path_within(sub, none, &resolved) == -1 && errno == ENOENT && resolved == NULL,
              "a missing root", NULL);
    }
    unlink(sub);
    unlink(x);
    rmdir(a);
    rmdir(ab);

    check(hv_is_jpx_name(".jpx") && hv_is_jpx_name("a.JpX") && !hv_is_jpx_name("jpx") &&
          !hv_is_jpx_name("a.jpy") && !hv_is_jpx_name("a.jpxx") && !hv_is_jpx_name("") &&
          !hv_is_jpx_name("ajpx") &&
          !hv_is_jpx_name("b.jp2"), "hv_is_jpx_name", NULL);
}

/* hv_check_served on the corpus's linked file, copied with its frames. */
static void show(const char *what, const char *error, const hv_served *r) {
    if (getenv("SHOW"))
        printf("%s: %s at %zu linked %zu embedded %zu at_linked %d path %s\n", what,
               error ? error : "-", r->at, r->linked, r->embedded, r->at_linked, r->linked_path);
}

/* A linked JPX file of n codestreams, codestream k in frame file
 * target[k] of `files` (f00.jp2, f01.jp2, ...), each at the offset and
 * length of the base's frame, but offset[k] and length[k] where they are
 * given and nonzero: the
 * base's signature, ftyp, rreq and jp2h, n of its jpch, n ftbl, the dtbl.
 * *n_out bytes, malloc'd. */
static uint8_t *repeated(const uint8_t *base, int n, const int *target, const uint64_t *offset,
                         const uint32_t *length, int files, size_t *n_out) {
    size_t cap = 104 + (size_t)n * (30 + 32) + 10 + (size_t)files * 32, at = 0, dtbl;
    uint8_t *b = malloc(cap);
    int k;
#define PUT32(v)                                                                   \
    (b[at] = (uint8_t)((v) >> 24), b[at + 1] = (uint8_t)((v) >> 16),               \
     b[at + 2] = (uint8_t)((v) >> 8), b[at + 3] = (uint8_t)(v), at += 4)
    memcpy(b, base, 104);                       /* jP, ftyp, rreq, jp2h */
    at = 104;
    for (k = 0; k < n; k++, at += 30)
        memcpy(b + at, base + 104, 30);         /* jpch */
    for (k = 0; k < n; k++) {
        uint64_t off = offset != NULL && offset[k] ? offset[k] : 0x55;
        PUT32(32);
        memcpy(b + at, "ftbl", 4), at += 4;
        PUT32(24);
        memcpy(b + at, "flst", 4), at += 4;
        b[at++] = 0, b[at++] = 1;               /* NF */
        PUT32(0);
        PUT32((uint32_t)off);
        PUT32(length != NULL && length[k] ? length[k] : 0x58);
        b[at++] = 0, b[at++] = (uint8_t)(target[k] + 1);  /* DR */
    }
    dtbl = at;
    PUT32(0);
    memcpy(b + at, "dtbl", 4), at += 4;
    b[at++] = 0, b[at++] = (uint8_t)files;
    for (k = 0; k < files; k++) {
        char loc[32];
        size_t len = (size_t)snprintf(loc, sizeof loc, "file://./f%02d.jp2", k) + 1;
        PUT32((uint32_t)(12 + len));
        memcpy(b + at, "url ", 4), at += 4;
        PUT32(0);
        memcpy(b + at, loc, len), at += len;
    }
    k = (int)(at - dtbl);
    at = dtbl;
    PUT32((uint32_t)k);
    at = dtbl + (size_t)k;
#undef PUT32
    *n_out = at;
    return b;
}

/* The set of the files that passed, directly: 100 files added one by one,
 * each found after, none before, in a set at most half full that doubles
 * from 16 slots; each field of a file tells it apart. */
static void check_passed_set(void) {
    passed_set set = {0};
    passed_link l = {0, 0, 0, 0, 0, 0}, other;
    size_t i, k, cap;
    int ok = 1, field;
    for (i = 0; i < 100 && ok; i++) {
        l.dev = i % 3;
        l.ino = i;
        l.size = 173;
        l.offset = 85;
        l.length = 88;
        ok = !passed(&set, &l) && add_passed(&set, &l) == 0 && passed(&set, &l);
        for (cap = 16; cap < 2 * (i + 1); cap *= 2)
            ;
        ok = ok && set.n == i + 1 && set.cap == cap;
        for (k = 0; k <= i && ok; k++) {
            other = l;
            other.dev = k % 3;
            other.ino = k;
            ok = passed(&set, &other);
        }
    }
    check(ok, "the files that passed: found, and the set at most half full", NULL);
    for (field = 0; field < 5; field++) {
        other = l;
        other.dev += field == 0 ? 1000 : 0;
        other.ino += field == 1 ? 1000 : 0;
        other.size += field == 2 ? 1000 : 0;
        other.offset += field == 3 ? 1000 : 0;
        other.length += field == 4 ? 1000 : 0;
        check(!passed(&set, &other), "a file that differs in one field", NULL);
    }
    free(set.slots);
}

/* Links that name one file again: each file is read once, however many
 * links name it and in whatever order; a link to a file that passed, with
 * another fragment, is checked again (and fails); out of memory for the
 * files that passed. Twenty files, two links each, to grow the set. */
static void check_repeated_links(const char *tmp, const uint8_t *base, const uint8_t *frame,
                                 size_t frame_size) {
    enum { FILES = 20, LINKS = 2 * FILES };
    int target[LINKS], k;
    uint64_t offset[LINKS] = {0};
    char path[FILES][256], jpx[256];
    uint8_t *b;
    size_t n;
    hv_served r;
    const char *error;

    for (k = 0; k < FILES; k++) {
        snprintf(path[k], sizeof path[k], "%s/f%02d.jp2", tmp, k);
        write_file(path[k], frame, frame_size);
    }
    snprintf(jpx, sizeof jpx, "%s/r.jpx", tmp);
    for (k = 0; k < LINKS; k++)
        target[k] = k % FILES;                  /* 0, 1, ..., 19, 0, 1, ... */
    check_passed_set();
    b = repeated(base, LINKS, target, NULL, NULL, FILES, &n);
    read_bytes = 0;
    k = opened;
    close(0);                   /* the first linked file is opened as 0 */
    error = hv_check_served(jpx, b, n, 1, tmp, &r);
    check(error == NULL && r.linked == LINKS && read_bytes == FILES * frame_size &&
          opened - k == LINKS, "each linked file read once", error);
    read_bytes = 0;
    error = hv_check_served(jpx, b, n, 1, NULL, &r);
    check(error == NULL && r.linked == LINKS && read_bytes == FILES * frame_size,
          "each linked file read once, no root", error);
    free(b);

    /* The last link names file 19 again, at another offset. */
    offset[LINKS - 1] = 0x54;
    b = repeated(base, LINKS, target, offset, NULL, FILES, &n);
    read_bytes = 0;
    error = hv_check_served(jpx, b, n, 1, tmp, &r);
    check(error != NULL && strcmp(error, "flst.source-extent") == 0 &&
          r.linked == LINKS - 1 && r.at_linked && read_bytes == (FILES + 1) * frame_size,
          "a file that passed, with another fragment", error);
    free(b);

    /* ... with another length. */
    {
        uint32_t length[LINKS] = {0};
        length[LINKS - 1] = 0x57;
        b = repeated(base, LINKS, target, NULL, length, FILES, &n);
        read_bytes = 0;
        error = hv_check_served(jpx, b, n, 1, tmp, &r);
        check(error != NULL && strcmp(error, "flst.source-extent") == 0 &&
              r.linked == LINKS - 1 && read_bytes == (FILES + 1) * frame_size,
              "a file that passed, with another length", error);
        free(b);
    }

    /* A file is the same by its device, inode and size: files 0 and 1
     * with one device and inode (as a file replaced by another) but two
     * sizes, file 1 the frame and a free box, are both read; and so are
     * two of one size on two devices with one inode. Two links, one each. */
    {
        static const uint8_t free_box[8] = {0, 0, 0, 8, 'f', 'r', 'e', 'e'};
        uint8_t *longer = malloc(frame_size + 8);
        int two[2] = {0, 1};
        memcpy(longer, frame, frame_size);
        memcpy(longer + frame_size, free_box, 8);
        b = repeated(base, 2, two, NULL, NULL, 2, &n);
        for (k = 1; k <= 2; k++) {
            size_t extra = k == 1 ? 8 : 0;
            write_file(path[1], longer, frame_size + extra);
            fstat_identity = k;
            read_bytes = 0;
            error = hv_check_served(jpx, b, n, 1, tmp, &r);
            fstat_identity = 0;
            check(error == NULL && r.linked == 2 && read_bytes == 2 * frame_size + extra,
                  k == 1 ? "one device and inode, two sizes" : "one inode, two devices", error);
        }
        free(longer);
        free(b);
        write_file(path[1], frame, frame_size);
    }

    b = repeated(base, LINKS, target, NULL, NULL, FILES, &n);
    calloc_fails = 1;
    error = hv_check_served(jpx, b, n, 1, tmp, &r);
    calloc_fails = 0;
    check(error != NULL && strcmp(error, "out of memory") == 0 && r.linked == 0,
          "out of memory for the files that passed", error);
    free(b);
    for (k = 0; k < FILES; k++)
        unlink(path[k]);
}

static void check_served(const char *dir, const char *tmp) {
    static const char *const names[] = {"jpx-linked.jpx", "jpx-linked-frame1.jp2",
                                        "jpx-linked-frame2.jp2", "jpx-embedded.jpx", "jp2.jp2"};
    char path[4][256], jpx[256];
    uint8_t *buf, *data[5];
    size_t size[5], i;
    hv_served r;
    const char *error;

    for (i = 0; i < 5; i++) {
        char from[4096];
        snprintf(from, sizeof from, "%s/%s", dir, names[i]);
        if ((data[i] = read_all(from, &size[i])) == NULL) {
            check(0, "cannot read", from);
            return;
        }
    }
    for (i = 0; i < 3; i++) {
        snprintf(path[i], sizeof path[i], "%s/%s", tmp, names[i]);
        write_file(path[i], data[i], size[i]);
    }
    snprintf(jpx, sizeof jpx, "%s", path[0]);
    buf = data[0];

    memset(&r, 0x5A, sizeof r);
    error = hv_check_served(jpx, buf, size[0], 1, tmp, &r);
    show("x", error, &r);
    check(error == NULL && r.embedded == 0 && r.linked == 2 && r.at == 0 && !r.at_linked &&
          r.linked_path[0] == 0, "linked, inside the root", error);
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served(jpx, buf, size[0], 1, NULL, &r);
    show("x", error, &r);
    check(error == NULL && r.linked == 2 && r.linked_path[0] == 0, "linked, no root", error);

    /* The second frame missing: its link's LOC, and its path. */
    unlink(path[2]);
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served(jpx, buf, size[0], 1, NULL, &r);
    show("x", error, &r);
    check(error != NULL && strcmp(error, "url.missing-companion") == 0 && r.linked == 1 &&
          !r.at_linked && r.at == 293 && strstr(r.linked_path, "jpx-linked-frame2.jp2") != NULL,
          "a frame missing", error);
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served(jpx, buf, size[0], 1, tmp, &r);
    show("x", error, &r);
    check(error != NULL && strcmp(error, "url.missing-companion") == 0 && r.linked == 1 &&
          r.at == 293, "a frame missing, with the root", error);
    /* ... there but a frame of another size: the frame's offset. */
    write_file(path[2], data[1], size[1] - 1);
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served(jpx, buf, size[0], 1, NULL, &r);
    show("x", error, &r);
    check(error != NULL && r.linked == 1 && r.at_linked &&
          strstr(r.linked_path, "jpx-linked-frame2.jp2") != NULL, "a frame cut", error);
    write_file(path[2], data[2], size[2]);
    /* ... unreadable in a way the reader cannot resolve. */
    open_fails = 1;
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served(jpx, buf, size[0], 1, NULL, &r);
    show("x", error, &r);
    open_fails = 0;
    check(error != NULL && strcmp(error, "url.missing-companion") == 0 && r.linked == 0 &&
          r.at == 250, "the first frame unreadable", error);

    /* A linked path too long for linked_path: the JPX file's directory of
     * 4,090 bytes. */
    {
        char *longer = malloc(4200);
        memset(longer, 'd', 4100);
        longer[0] = '/';
        strcpy(longer + 4090, "/f.jpx");
        memset(&r, 0x5A, sizeof r);
        error = hv_check_served(longer, buf, size[0], 1, NULL, &r);
        check(error != NULL && strcmp(error, "linked path longer than the caller's buffer") == 0 &&
              r.linked == 0 && r.at == 250, "a linked path too long", error);
        free(longer);
    }

    /* A frame of INT_MAX bytes (sparse): read, which here runs out of
     * memory. */
    if (truncate(path[1], INT_MAX) == 0) {
        memset(&r, 0x5A, sizeof r);
        error = hv_check_served(jpx, buf, size[0], 1, NULL, &r);
        check(error != NULL && strcmp(error, "url.missing-companion") == 0 && r.at == 250,
              "a frame of INT_MAX bytes", error);
        error = hv_check_served(jpx, buf, size[0], 1, tmp, &r);
        check(error != NULL && strcmp(error, "url.missing-companion") == 0 && r.at == 250,
              "a frame of INT_MAX bytes, with the root", error);
        write_file(path[1], data[1], size[1]);
    }

    /* Embedded and JP2: the counts. */
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served("e.jpx", data[3], size[3], 1, NULL, &r);
    check(error == NULL && r.embedded == 2 && r.linked == 0 && r.at == 0 && !r.at_linked,
          "embedded", error);
    data[3][size[3] - 1] = 0;                   /* the second codestream's EOC */
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served("e.jpx", data[3], size[3], 1, NULL, &r);
    show("x", error, &r);
    check(error != NULL && r.embedded == 1 && r.at == size[3] - 2, "embedded, the second cut",
          error);
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served("j.jp2", data[4], size[4], 0, NULL, &r);
    show("x", error, &r);
    check(error == NULL && r.embedded == 1 && r.linked == 0 && r.at == 0 &&
          r.linked_path[0] == 0, "JP2", error);
    data[4][size[4] - 1] = 0;
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served("j.jp2", data[4], size[4], 0, NULL, &r);
    show("x", error, &r);
    check(error != NULL && r.embedded == 0 && r.at == size[4] - 2, "JP2 cut", error);
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served("j.jp2", data[4], 11, 0, NULL, &r);
    show("x", error, &r);
    check(error != NULL && strcmp(error, "file.signature") == 0 && r.embedded == 0 && r.at == 0,
          "JP2 without its signature", error);
    memset(&r, 0x5A, sizeof r);
    error = hv_check_served("e.jpx", data[4], 11, 1, NULL, &r);
    show("x", error, &r);
    check(error != NULL && strcmp(error, "file.signature") == 0 && r.at == 0,
          "JPX without its signature", error);

    check_repeated_links(tmp, data[0], data[1], size[1]);
    for (i = 0; i < 3; i++)
        unlink(path[i]);
    for (i = 0; i < 5; i++)
        free(data[i]);
}

int main(int argc, char **argv) {
    char tmp[] = "/tmp/test_served.XXXXXX";
    if (argc != 2) {
        fprintf(stderr, "usage: test_served <vector directory>\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mkdtemp(tmp) == NULL) {
        printf("FAIL mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    check_load(tmp);
    check_paths(tmp);
    check_served(argv[1], tmp);
    check(opened == closed, "every descriptor closed", NULL);
    rmdir(tmp);
    if (failures == 0)
        printf("all checks passed\n");
    return failures != 0;
}
