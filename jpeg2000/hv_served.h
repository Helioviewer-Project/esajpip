/* hv_served.h: the served profile's checks of a whole file on disk, the
 * files a JPX file links to included, as the server reads it
 * (JPIP_PROFILE.md). For the programs that check files: hv_walk -P and
 * test/test_profile. They read each file whole into memory; a server that
 * maps its files uses hv_served_jp2, hv_served_jpx, hv_served_path,
 * hv_path_within and hv_served_link on the mapping instead. */
#ifndef HV_SERVED_H
#define HV_SERVED_H

#include <stddef.h>
#include <stdint.h>

#include "hv_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The file rules of the served profile (JPIP_PROFILE.md; the profile layer
 * of ../spec/jp2-boxes.asn1) for a JP2 file: at most INT_MAX bytes; the
 * signature box, then a file type box with the jp2 brand and a jp2
 * compatibility entry; top-level boxes framed as hv_boxes_next requires;
 * exactly one codestream box. Other boxes' contents are not checked, and a
 * raw codestream fails. NULL on success, with *jp2c the codestream box;
 * otherwise the rule that fails (the manifest's name where it has one) and
 * *at its offset. Check the codestream with hv_codestream_open and
 * HV_READ_JPIP. */
const char *hv_served_jp2(const uint8_t *buf, size_t size, hv_box *jp2c, size_t *at);

/* A linked codestream: the fragment of its flst box, and the LOC of the url
 * box its DR names. */
typedef struct {
    uint64_t offset, length;    /* the codestream in the linked file */
    uint64_t dr;                /* its data reference, 1 to NDR */
    const uint8_t *loc;         /* the URL, in the caller's buffer */
    size_t loc_size;            /* without its NUL */
} hv_served_source;

/* The codestreams of a JPX file, in box order: embedded (jp2c boxes) or
 * linked (links), never both. */
typedef struct {
    size_t count;
    hv_box *jp2c;               /* count boxes, or NULL */
    hv_served_source *links;             /* count links, or NULL */
} hv_served_sources;

/* The file rules of the served profile for a JPX file (JPIP_PROFILE.md; the
 * profile layer of ../spec/jp2-boxes.asn1): at most INT_MAX bytes; the
 * signature, then a file type box with the jpx brand and a jpx
 * compatibility entry; top-level boxes framed as hv_boxes_next requires,
 * and the children of jpch, ftbl and dtbl; the placement and count rules of
 * hv_rules.c; one fragment per ftbl, linking to a .jp2 file through a
 * version-0 file:// URL (hv_rule_url). Other boxes, asoc included, are not
 * read. NULL on success, with *jpx the codestreams (hv_served_sources_free); otherwise
 * the rule that
 * fails and *at its offset. Check each embedded codestream with
 * hv_codestream_check and HV_READ_JPIP, and each linked file with
 * hv_served_link. */
const char *hv_served_jpx(const uint8_t *buf, size_t size, hv_served_sources *jpx, size_t *at);
void hv_served_sources_free(hv_served_sources *jpx);

/* The file a link names, as the server resolves it: LOC without "file://",
 * percent-decoded (hv_url_path), and, unless the decoded path is absolute,
 * relative to the directory of jpx_path (with jpx_path NULL, left relative:
 * to the current directory). NULL, with the path in out; otherwise why not
 * (url.length, url.file-scheme, url.percent-encoding, or out too small),
 * with out empty when out_size is nonzero. */
const char *hv_served_path(const hv_served_source *link, const char *jpx_path, char *out,
                         size_t out_size);

/* A linked file, held in buf, against its link: hv_served_jp2, its
 * codestream with HV_READ_JPIP, and the fragment exactly that codestream
 * (flst.source-extent). NULL, or the rule that fails and *at its offset. */
const char *hv_served_link(const uint8_t *buf, size_t size, const hv_served_source *link, size_t *at);

/* The contents of the regular file at path, malloc'd (one byte for an
 * empty file), and *size; NULL, with errno set, if it cannot be read, is
 * no regular file (EINVAL) or is larger than max bytes (EFBIG, before
 * anything is allocated). It is opened without blocking, so a FIFO or a
 * device fails at once instead of hanging or never ending. The readers
 * take at most INT_MAX bytes (file.size-limit). */
uint8_t *hv_load_file(const char *path, size_t max, size_t *size);

/* Where the file at path lies against the directory root, both with their
 * symbolic links, "." and ".." resolved (realpath): 1 inside it, at any
 * depth; 0 outside; -1 if either cannot be resolved (errno set: ENOENT
 * for a missing file). *resolved (unless NULL) gets path resolved,
 * malloc'd, when inside. A link's path can name any file, "../" and
 * absolute paths included, so a server confines links to its document
 * root with this. */
int hv_path_within(const char *path, const char *root, char **resolved);

/* Nonzero when path ends in ".jpx", in any case. The server picks the
 * format by the extension: ReadJPX for .jpx, ReadJP2 for .jp2. */
int hv_is_jpx_name(const char *path);

/* What hv_check_served checked, and where it failed. */
typedef struct {
    size_t embedded;            /* codestreams checked in the file itself */
    size_t linked;              /* codestreams checked in linked files */
    size_t at;                  /* the offset of the failure */
    int at_linked;              /* at is in linked_path, not in the file */
    char linked_path[4096];     /* the linked file that failed, or "" */
} hv_served;

/* The served profile's checks of the file at `path`, held in buf:
 * hv_served_jp2, or (jpx) hv_served_jpx; then each codestream with
 * HV_READ_JPIP, embedded (hv_codestream_check) or linked (hv_served_path from
 * path, hv_load_file, hv_served_link). With `root` (else NULL), a linked
 * file must lie inside that directory (hv_path_within), and is read
 * through its resolved path; a root that cannot be resolved fails with
 * "root directory cannot be resolved". NULL, with r->at 0 and
 * r->linked_path ""; otherwise the rule that fails, or the reader's
 * error, with r->at its offset. A link whose path hv_served_path refuses
 * fails with its reason, one outside root with "linked file outside the
 * root directory", and one whose file (r->linked_path) cannot be read, or
 * is no regular file, with url.missing-companion, all at the offset of
 * the link's LOC; one larger than INT_MAX bytes fails with
 * file.size-limit at offset 0 of that file, before it is read; a linked
 * file that fails its checks is named in r->linked_path, and r->at_linked
 * is set: r->at is in that file. r->embedded and r->linked count the
 * codestreams that passed. A linked file is read and checked once, however
 * many links name it: the same file (device and inode) of the same size,
 * with the same fragment, passes again unread. Otherwise a JPX file of a
 * few bytes per link could make it read one large file over and over; a
 * server that checks the links on its mappings needs the same care. */
const char *hv_check_served(const char *path, const uint8_t *buf, size_t size, int jpx,
                            const char *root, hv_served *r);

#ifdef __cplusplus
}
#endif

#endif
