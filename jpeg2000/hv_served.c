/* hv_served.c: see hv_served.h. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700       /* realpath, O_CLOEXEC */
#endif

#include "hv_served.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The regular file at path, opened without blocking, and its status in
 * *st: its descriptor, or -1 with errno set (EINVAL: no regular file). */
static int open_regular(const char *path, struct stat *st) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC), saved;
    if (fd < 0)
        return -1;
    if (fstat(fd, st) != 0)
        goto fail;
    if (!S_ISREG(st->st_mode) || st->st_size < 0) {
        errno = EINVAL;
        goto fail;
    }
    return fd;

fail:
    saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

/* hv_load_file on fd, which open_regular opened with the status st; fd is
 * closed. */
static uint8_t *load_open(int fd, const struct stat *st, size_t max, size_t *size) {
    uint8_t *buf = NULL;
    size_t n, got = 0;
    int saved;

    if ((uintmax_t)st->st_size > max) {
        errno = EFBIG;
        goto fail;
    }
    n = (size_t)st->st_size;
    if ((buf = malloc(n ? n : 1)) == NULL)
        goto fail;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            if (r == 0)
                errno = EIO;            /* the file shrank while read */
            goto fail;
        }
        got += (size_t)r;
    }
    close(fd);
    *size = n;
    return buf;

fail:
    saved = errno;
    free(buf);
    close(fd);
    errno = saved;
    return NULL;
}

uint8_t *hv_load_file(const char *path, size_t max, size_t *size) {
    struct stat st;
    int fd = open_regular(path, &st);
    return fd < 0 ? NULL : load_open(fd, &st, max, size);
}

int hv_path_within(const char *path, const char *root, char **resolved) {
    char *p, *r;
    size_t n;
    int inside, saved;

    if (resolved != NULL)
        *resolved = NULL;
    if ((p = realpath(path, NULL)) == NULL)
        return -1;
    if ((r = realpath(root, NULL)) == NULL) {
        saved = errno;
        free(p);
        errno = saved;
        return -1;
    }
    /* Below root: its path and a '/', or, for root "/", any other path. */
    n = strlen(r);
    inside = n == 1 ? p[1] != 0 : strncmp(p, r, n) == 0 && p[n] == '/';
    free(r);
    if (inside && resolved != NULL)
        *resolved = p;
    else
        free(p);
    return inside;
}

int hv_is_jpx_name(const char *path) {
    static const char suffix[] = ".jpx";
    size_t n = strlen(path), k = sizeof suffix - 1, i;
    if (n < k)
        return 0;
    for (i = 0; i < k; i++)
        if (tolower((unsigned char)path[n - k + i]) != suffix[i])
            return 0;
    return 1;
}

/* A linked file that passed its checks, as the file system knows it, and
 * the fragment it passed with. */
typedef struct {
    uintmax_t dev, ino, size;
    uint64_t offset, length;
    int used;                   /* in a slot of passed_set: taken */
} passed_link;

/* The links that passed, by file: an open-addressed hash set, at most half
 * full. A linked file holds one codestream (hv_served_link), so the links
 * that pass with one file all give the same fragment: each file is read
 * and checked once, however many links name it. */
typedef struct {
    passed_link *slots;
    size_t cap, n;
} passed_set;

/* Unsigned arithmetic that wraps on purpose, which Clang's
 * -fsanitize=integer would report. */
#if defined(__clang__)
#define WRAPS __attribute__((no_sanitize("unsigned-integer-overflow", "unsigned-shift-base")))
#else
#define WRAPS
#endif

/* A multiplicative hash of the device and inode, modulo 2^64. */
WRAPS static size_t slot_of(const passed_set *set, const passed_link *l) {
    uint64_t h = (uint64_t)l->dev * 0x9E3779B97F4A7C15u ^ (uint64_t)l->ino;
    size_t i;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9u;
    h ^= h >> 29;
    for (i = (size_t)h & (set->cap - 1); set->slots[i].used; i = (i + 1) & (set->cap - 1))
        if (set->slots[i].dev == l->dev && set->slots[i].ino == l->ino &&
            set->slots[i].size == l->size && set->slots[i].offset == l->offset &&
            set->slots[i].length == l->length)
            break;
    return i;
}

static int passed(const passed_set *set, const passed_link *l) {
    return set->cap != 0 && set->slots[slot_of(set, l)].used;
}

/* l added to set: 0, or -1 when out of memory. */
static int add_passed(passed_set *set, const passed_link *l) {
    size_t i;
    if (2 * (set->n + 1) > set->cap) {
        passed_set grown;
        grown.cap = set->cap ? 2 * set->cap : 16;
        grown.n = set->n;
        if ((grown.slots = calloc(grown.cap, sizeof *grown.slots)) == NULL)
            return -1;
        for (i = 0; i < set->cap; i++)
            if (set->slots[i].used)
                grown.slots[slot_of(&grown, &set->slots[i])] = set->slots[i];
        free(set->slots);
        *set = grown;
    }
    i = slot_of(set, l);
    set->slots[i] = *l;
    set->slots[i].used = 1;
    set->n++;
    return 0;
}

/* One linked codestream: its file, found and read as the server does,
 * inside root when there is one; not read again if it passed before. */
static const char *check_linked(const char *path, const char *root, const uint8_t *buf,
                                const hv_served_source *link, passed_set *set, hv_served *r) {
    const char *error;
    struct stat st;
    passed_link key;
    uint8_t *linked;
    size_t size = 0;
    int fd;

    r->at = (size_t)(link->loc - buf);
    if ((error = hv_served_path(link, path, r->linked_path, sizeof r->linked_path)) != NULL)
        return error;
    if (root != NULL) {
        char *resolved;
        switch (hv_path_within(r->linked_path, root, &resolved)) {
        case 1:
            break;
        case 0:
            return "linked file outside the root directory";
        default:
            return "url.missing-companion";
        }
        fd = open_regular(resolved, &st);
        free(resolved);
    } else {
        fd = open_regular(r->linked_path, &st);
    }
    if (fd < 0)
        return "url.missing-companion";
    key.dev = (uintmax_t)st.st_dev;
    key.ino = (uintmax_t)st.st_ino;
    key.size = (uintmax_t)st.st_size;
    key.offset = link->offset;
    key.length = link->length;
    if (passed(set, &key)) {
        close(fd);
        return NULL;
    }
    linked = load_open(fd, &st, INT_MAX, &size);
    if (linked == NULL && errno == EFBIG) {
        r->at = 0;
        r->at_linked = 1;
        return "file.size-limit";
    }
    if (linked == NULL)
        return "url.missing-companion";
    error = hv_served_link(linked, size, link, &r->at);
    r->at_linked = error != NULL;
    free(linked);
    if (error == NULL && add_passed(set, &key) != 0)
        return "out of memory";
    return error;
}

const char *hv_check_served(const char *path, const uint8_t *buf, size_t size, int jpx,
                            const char *root, hv_served *r) {
    const char *error;
    hv_served_sources file;
    hv_box jp2c;
    passed_set set = {0};
    size_t i;

    r->embedded = r->linked = 0;
    r->at = 0;
    r->at_linked = 0;
    r->linked_path[0] = 0;
    if (root != NULL) {
        char *resolved = realpath(root, NULL);
        if (resolved == NULL)
            return "root directory cannot be resolved";
        free(resolved);
    }
    if (!jpx) {
        if ((error = hv_served_jp2(buf, size, &jp2c, &r->at)) == NULL &&
            (error = hv_codestream_check(buf, jp2c.payload, jp2c.end, HV_READ_JPIP, &r->at)) ==
                NULL)
            r->embedded = 1;
        return error;
    }
    if ((error = hv_served_jpx(buf, size, &file, &r->at)) != NULL)
        return error;
    for (i = 0; i < file.count && error == NULL; i++) {
        if (file.jp2c != NULL) {
            error = hv_codestream_check(buf, file.jp2c[i].payload, file.jp2c[i].end, HV_READ_JPIP,
                                        &r->at);
            r->embedded += error == NULL;
        } else {
            error = check_linked(path, root, buf, &file.links[i], &set, r);
            r->linked += error == NULL;
        }
    }
    free(set.slots);
    if (error == NULL) {        /* nothing failed: no offset, no linked file */
        r->at = 0;
        r->linked_path[0] = 0;
    }
    hv_served_sources_free(&file);
    return error;
}
