/* test_reader: the reader's framing, offsets and errors, byte by byte,
 * where the corpus does not reach: every branch that rejects an input, with
 * its message and offset, and the items of hv_codestream_next against a
 * walk of the markers written here. The inputs are built here or edited
 * from the corpus bases (jp2.jp2, jpx-embedded.jpx, jpx-linked.jpx), each
 * in a buffer of exactly its size, so that a read past it is an
 * AddressSanitizer error. Every proper prefix of each base, and of every
 * codestream here, is read too. hv_reader.c is included, with malloc,
 * calloc and realloc that fail on demand, so that each allocation runs out
 * of memory once.
 *
 *   test_reader <vector directory> */
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "jpeg2000/hv_served.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The allocation hv_reader.c makes when fail_at counts down to 0 fails;
 * -1: none does. */
static int fail_at = -1;
/* Count non-null frees only while checking a reader's cleanup. */
static int nfrees = -1;
static void test_free(void *p) {
    if (nfrees >= 0 && p != NULL) nfrees++;
    free(p);
}

static int fails(void) { return fail_at >= 0 && fail_at-- == 0; }
static void *test_malloc(size_t n) { return fails() ? NULL : malloc(n); }
/* The element counts of the callocs, while ncallocs is not negative. */
static size_t callocs[8];
static int ncallocs = -1;
static void *test_calloc(size_t n, size_t size) {
    if (ncallocs >= 0 && ncallocs < 8)
        callocs[ncallocs++] = n;
    return fails() ? NULL : calloc(n, size);
}
static void *test_realloc(void *p, size_t n) { return fails() ? NULL : realloc(p, n); }

#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#include "jpeg2000/hv_reader.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

static int failures;
static const char *dir;

static void check(int ok, const char *what, const char *detail) {
    if (!ok) {
        printf("FAIL %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
        failures++;
    }
}

/* An error and its offset, against those expected (NULL: none, and *at not
 * written). */
static void expect(const char *what, const char *error, size_t at, const char *want,
                   size_t want_at) {
    char detail[512];
    if (want == NULL) {
        snprintf(detail, sizeof detail, "\"%s\" at %zu", error ? error : "(none)", at);
        check(error == NULL && at == (size_t)-1, what, detail);
        return;
    }
    snprintf(detail, sizeof detail, "\"%s\" at %zu, expected \"%s\" at %zu",
             error ? error : "(none)", at, want, want_at);
    check(error != NULL && strcmp(error, want) == 0 && at == want_at, what, detail);
}

/* ------------------------------------------------------------------------
 * Bytes
 * ------------------------------------------------------------------------ */

typedef struct {
    uint8_t *d;
    size_t n, cap;
} bytes;

static void put(bytes *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        if ((b->d = realloc(b->d, b->cap)) == NULL) {
            printf("FAIL out of memory\n");
            exit(1);
        }
    }
    if (n)
        memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void u8(bytes *b, unsigned v) { uint8_t c = (uint8_t)v; put(b, &c, 1); }
static void u16(bytes *b, unsigned v) { u8(b, v >> 8); u8(b, v); }
static void u32(bytes *b, uint32_t v) { u16(b, v >> 16); u16(b, v & 0xFFFF); }
static void u64(bytes *b, uint64_t v) { u32(b, (uint32_t)(v >> 32)); u32(b, (uint32_t)v); }
static void set32(bytes *b, size_t at, uint32_t v) {
    b->d[at] = (uint8_t)(v >> 24);
    b->d[at + 1] = (uint8_t)(v >> 16);
    b->d[at + 2] = (uint8_t)(v >> 8);
    b->d[at + 3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void clear(bytes *b) { b->n = 0; }
static void release(bytes *b) { free(b->d); memset(b, 0, sizeof *b); }

/* A box: LBox, TBox, payload. */
static size_t begin(bytes *b, const char *type) {
    size_t at = b->n;
    u32(b, 0);
    put(b, type, 4);
    return at;
}
static void end(bytes *b, size_t at) { set32(b, at, (uint32_t)(b->n - at)); }
static void box(bytes *b, const char *type, const void *payload, size_t n) {
    size_t at = begin(b, type);
    put(b, payload, n);
    end(b, at);
}

/* A copy of exactly the bytes, for AddressSanitizer. */
static uint8_t *exact(const bytes *b) {
    uint8_t *p = malloc(b->n ? b->n : 1);
    if (b->n)
        memcpy(p, b->d, b->n);
    return p;
}

static bytes load(const char *name) {
    char path[4096];
    bytes b = {NULL, 0, 0};
    uint8_t chunk[4096];
    size_t n;
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if ((f = fopen(path, "rb")) == NULL) {
        printf("FAIL cannot read %s\n", path);
        exit(1);
    }
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        put(&b, chunk, n);
    fclose(f);
    return b;
}

/* The top-level box of `type` (its k-th), whole, from a file whose boxes
 * have short headers. */
static bytes part(const bytes *file, const char *type, int k) {
    bytes out = {NULL, 0, 0};
    size_t p = 0;
    while (p + 8 <= file->n) {
        uint32_t l = get32(file->d + p);
        if (memcmp(file->d + p + 4, type, 4) == 0 && k-- == 0) {
            put(&out, file->d + p, l);
            return out;
        }
        p += l;
    }
    printf("FAIL no %s box\n", type);
    exit(1);
}
static void add(bytes *b, const bytes *part) { put(b, part->d, part->n); }

/* ------------------------------------------------------------------------
 * The file checks, each on exactly the bytes
 * ------------------------------------------------------------------------ */

static const char *jp2(const bytes *b, size_t *at) {
    hv_box jp2c;
    uint8_t *p = exact(b);
    const char *error;
    *at = (size_t)-1;
    error = hv_served_jp2(p, b->n, &jp2c, at);
    free(p);
    return error;
}
static const char *jpx(const bytes *b, size_t *at) {
    hv_served_sources j;
    uint8_t *p = exact(b);
    const char *error;
    *at = (size_t)-1;
    error = hv_served_jpx(p, b->n, &j, at);
    if (error == NULL)
        hv_served_sources_free(&j);
    check(j.jp2c == NULL || error == NULL, "hv_served_jpx frees on failure", error);
    free(p);
    return error;
}
static const char *jp2h(const bytes *b, size_t *at) {
    uint8_t *p = exact(b);
    const char *error;
    *at = (size_t)-1;
    error = hv_check_jp2h(p, b->n, at);
    free(p);
    return error;
}
static const char *jpx_headers(const bytes *b, size_t *at) {
    uint8_t *p = exact(b);
    const char *error;
    *at = (size_t)-1;
    error = hv_check_jpx_headers(p, b->n, at);
    free(p);
    return error;
}

/* The bases and their parts. */
static bytes JP2, JPX, LINKED, FRAME1;
static bytes SIG, FTYP_JP2, FTYP_JPX, RREQ, RREQ_LINKED, JP2H, JPX_JP2H, JPCH, JP2C, IHDR,
    COLR;

static void load_bases(void) {
    bytes h;
    JP2 = load("jp2.jp2");
    JPX = load("jpx-embedded.jpx");
    LINKED = load("jpx-linked.jpx");
    FRAME1 = load("jpx-linked-frame1.jp2");
    SIG = part(&JP2, "jP  ", 0);
    FTYP_JP2 = part(&JP2, "ftyp", 0);
    FTYP_JPX = part(&JPX, "ftyp", 0);
    RREQ = part(&JPX, "rreq", 0);
    RREQ_LINKED = part(&LINKED, "rreq", 0);
    JP2H = part(&JP2, "jp2h", 0);
    JPX_JP2H = part(&JPX, "jp2h", 0);
    JPCH = part(&JPX, "jpch", 0);
    JP2C = part(&JP2, "jp2c", 0);
    /* jp2h's children: ihdr (22 bytes) and colr (15) */
    h = JP2H;
    memset(&IHDR, 0, sizeof IHDR);
    memset(&COLR, 0, sizeof COLR);
    put(&IHDR, h.d + 8, 22);
    put(&COLR, h.d + 30, 15);
}

/* A JP2 file: the signature, ftyp, a jp2h holding `children` (n bytes of
 * boxes), and the base's codestream. */
static bytes jp2_with(const void *children, size_t n) {
    bytes b = {NULL, 0, 0};
    size_t at;
    add(&b, &SIG);
    add(&b, &FTYP_JP2);
    at = begin(&b, "jp2h");
    put(&b, children, n);
    end(&b, at);
    add(&b, &JP2C);
    return b;
}

/* ihdr and colr, then `more` */
static bytes jp2_plus(const void *more, size_t n) {
    bytes c = {NULL, 0, 0}, b;
    add(&c, &IHDR);
    add(&c, &COLR);
    put(&c, more, n);
    b = jp2_with(c.d, c.n);
    release(&c);
    return b;
}

/* ------------------------------------------------------------------------
 * Boxes
 * ------------------------------------------------------------------------ */

static void check_boxes(void) {
    bytes b = {NULL, 0, 0};
    uint8_t *p;
    hv_boxes it, in;
    hv_box x, y;
    const char *error = NULL;
    size_t at = 0;

    /* LBox 1 with XLBox, a box of 16 + 2 bytes, then LBox 0 to the end. */
    u32(&b, 1);
    put(&b, "free", 4);
    u64(&b, 18);
    u16(&b, 0xABCD);
    u32(&b, 0);
    put(&b, "free", 4);
    u16(&b, 0x1234);
    p = exact(&b);
    hv_boxes_file(&it, p, b.n);
    check(hv_boxes_next(&it, &x, &error, &at) == 1 && x.type == 0x66726565 && x.start == 0 &&
          x.payload == 16 && x.end == 18 && !x.to_end, "a box with XLBox", NULL);
    check(hv_boxes_next(&it, &x, &error, &at) == 1 && x.start == 18 && x.payload == 26 &&
          x.end == 28 && x.to_end, "a last box with LBox 0", NULL);
    check(hv_boxes_next(&it, &x, &error, &at) == 0 && hv_boxes_next(&it, &x, &error, &at) == 0,
          "no box after LBox 0", NULL);
    free(p);

    /* XLBox past the container, by one byte. */
    b.d[15] = 29;
    p = exact(&b);
    hv_boxes_file(&it, p, b.n);
    error = NULL;
    at = 99;
    check(hv_boxes_next(&it, &x, &error, &at) == -1 && error != NULL &&
          strcmp(error, "box overruns its container") == 0 && at == 0, "XLBox past the end",
          error);
    free(p);
    b.d[15] = 18;

    /* A superbox with LBox 0 child: only in a box that runs to the end. */
    clear(&b);
    u32(&b, 16);
    put(&b, "asoc", 4);
    u32(&b, 0);
    put(&b, "free", 4);
    u32(&b, 0);
    put(&b, "asoc", 4);
    u32(&b, 0);
    put(&b, "free", 4);
    p = exact(&b);
    hv_boxes_file(&it, p, b.n);
    check(hv_boxes_next(&it, &x, &error, &at) == 1 && !x.to_end, "a superbox", NULL);
    hv_boxes_children(&in, p, &x);
    error = NULL;
    check(hv_boxes_next(&in, &y, &error, &at) == -1 && error != NULL &&
          strcmp(error, "LBox = 0 inside a box that does not run to the end of the file") == 0 &&
          at == 8, "LBox 0 in a box that ends before the file", error);
    check(hv_boxes_next(&it, &x, &error, &at) == 1 && x.to_end && x.start == 16,
          "a superbox with LBox 0", NULL);
    hv_boxes_children(&in, p, &x);
    check(hv_boxes_next(&in, &y, &error, &at) == 1 && y.to_end && y.start == 24 && y.end == 32,
          "LBox 0 in a box that runs to the end", NULL);
    free(p);

    /* A header cut short: 7 bytes. */
    clear(&b);
    put(&b, "\0\0\0\x10" "abc", 7);
    p = exact(&b);
    hv_boxes_file(&it, p, b.n);
    error = NULL;
    check(hv_boxes_next(&it, &x, &error, &at) == -1 && error != NULL &&
          strcmp(error, "invalid or truncated box header") == 0, "a short header", error);
    free(p);
    check(hv_is_superbox(HV_BOX_J2CX) == 1 && hv_is_superbox(HV_BOX_JP2C) == 0,
          "hv_is_superbox", NULL);
    release(&b);
}

/* ------------------------------------------------------------------------
 * The start of a file
 * ------------------------------------------------------------------------ */

static void check_file_starts(void) {
    bytes b = {NULL, 0, 0};
    const char *error;
    size_t at;

    /* The signature box: TBox right with LBox 13, TBox wrong with LBox 12,
     * and its contents cut after 2 bytes. */
    add(&b, &SIG);
    add(&b, &FTYP_JP2);
    add(&b, &JP2H);
    add(&b, &JP2C);
    b.d[3] = 13;
    error = jp2(&b, &at);
    expect("signature LBox 13", error, at, "file.signature", 0);
    b.d[3] = 12;
    b.d[4] = 'J';
    error = jp2(&b, &at);
    expect("signature TBox JP", error, at, "file.signature", 0);
    b.d[4] = 'j';
    b.n = 10;
    error = jp2(&b, &at);
    expect("signature cut short", error, at, "file.signature", 0);
    error = jp2h(&b, &at);
    expect("signature cut short (header checks)", error, at, "file.signature", 0);

    /* Two boxes, no codestream; the header checks read ftyp. */
    clear(&b);
    add(&b, &SIG);
    add(&b, &FTYP_JP2);
    error = jp2(&b, &at);
    expect("two boxes", error, at, "jp2.one-codestream", b.n);
    {
        uint8_t *p = exact(&b);
        at = 7;
        hv_ftyp ftyp;
        check(check_file_start(p, b.n, 0, &ftyp, &at) == NULL && at == 12,
              "check_file_start on two boxes", NULL);
        free(p);
    }
    b.n = 12;
    error = jp2(&b, &at);
    expect("one box", error, at, "file.two-boxes", 12);
    error = jp2h(&b, &at);
    expect("one box (header checks)", error, at, "file.two-boxes", 12);

    /* The second box overruns the file. */
    clear(&b);
    add(&b, &SIG);
    add(&b, &FTYP_JP2);
    b.n -= 4;
    error = jp2h(&b, &at);
    expect("ftyp past the end (header checks)", error, at, "box overruns its container", 12);
    {
        uint8_t *p = exact(&b);
        hv_ftyp ftyp;
        at = 0;
        error = check_file_start(p, b.n, 0, &ftyp, &at);
        expect("ftyp past the end (the file's start)", error, at, "box overruns its container",
               12);
        free(p);
    }

    /* The size limit: INT_MAX bytes are read (here the first 16, which
     * fail the signature), INT_MAX + 1 are not. */
    {
        static const uint8_t zeros[16];
        hv_box jp2c;
        at = 0;
        error = hv_served_jp2(zeros, INT_MAX, &jp2c, &at);
        expect("INT_MAX bytes", error, at, "file.signature", 0);
        error = hv_served_jp2(zeros, (size_t)INT_MAX + 1, &jp2c, &at);
        expect("INT_MAX + 1 bytes", error, at, "file.size-limit", 0);
    }

    /* ftyp: 4 bytes (no MinV), 8 (no entry), 10 (half an entry), and an
     * entry that is not the brand, ftyp last in the file. */
    {
        static const struct {
            size_t n;
            const char *want;
        } cut[] = {{4, "ftyp: not BR, MinV and one or more compatibility entries"},
                   {8, "ftyp: not BR, MinV and one or more compatibility entries"},
                   {10, "ftyp: not BR, MinV and one or more compatibility entries"}};
        size_t i;
        for (i = 0; i < sizeof cut / sizeof *cut; i++) {
            clear(&b);
            add(&b, &SIG);
            box(&b, "ftyp", "jp2 \0\0\0\0jp2 ", cut[i].n);
            add(&b, &JP2H);
            add(&b, &JP2C);
            error = jp2(&b, &at);
            expect("a short ftyp", error, at, cut[i].want, 12);
        }
        clear(&b);
        add(&b, &SIG);
        box(&b, "ftyp", "jp2 \0\0\0\0jpx ", 12);
        error = jp2(&b, &at);
        expect("ftyp without the brand, last", error, at, "file.ftyp-compatibility", 12);
    }
    release(&b);
}

/* ------------------------------------------------------------------------
 * JPX files
 * ------------------------------------------------------------------------ */

/* A url box, VERS and FLAG 0, of loc with its NUL. */
static void url(bytes *b, const char *loc) {
    size_t at = begin(b, "url ");
    u32(b, 0);
    put(b, loc, strlen(loc) + 1);
    end(b, at);
}

/* An ftbl holding an flst of one fragment. */
static void ftbl(bytes *b, uint64_t off, uint32_t len, uint16_t dr) {
    size_t at = begin(b, "ftbl"), f = begin(b, "flst");
    u16(b, 1);
    u64(b, off);
    u32(b, len);
    u16(b, dr);
    end(b, f);
    end(b, at);
}

/* A linked file with n codestreams, all of the frame's codestream at 85
 * (jpx-linked-frame1.jp2), each through its own url. */
static bytes linked(int n) {
    bytes b = {NULL, 0, 0};
    size_t at;
    int k;
    add(&b, &SIG);
    add(&b, &FTYP_JPX);
    for (k = 0; k < n; k++)
        add(&b, &JPCH);
    for (k = 0; k < n; k++)
        ftbl(&b, 85, 88, (uint16_t)(k + 1));
    at = begin(&b, "dtbl");
    u16(&b, (unsigned)n);
    for (k = 0; k < n; k++)
        url(&b, k % 2 ? "file:///b.jp2" : "file://c%20d.jp2");
    end(&b, at);
    return b;
}

/* General JP2 access interprets framing and compatibility, not payloads. */
static void check_jp2_access(void) {
    bytes b = {0};
    hv_box cs, untouched = {0};
    const char *error;
    size_t at, i;
    uint8_t *p;

    add(&b, &SIG);
    box(&b, "ftyp", "test\0\0\0\x07jp2 ", 12);
    /* Deliberately malformed bodies remain opaque. */
    box(&b, "jp2h", "x", 1);
    box(&b, "jp2c", "bytes", 5);
    p = exact(&b);
    at = (size_t)-1;
    error = hv_read_jp2(p, b.n, &cs, &at);
    expect("general compatible brand and opaque bodies", error, at, NULL, 0);
    check(cs.end - cs.payload == 5 && memcmp(p + cs.payload, "bytes", 5) == 0,
          "general access borrows original codestream", NULL);
    at = (size_t)-1;
    error = hv_served_jp2(p, b.n, &cs, &at);
    expect("served brand retained", error, at, "file.ftyp-brand", 12);
    free(p);
    for (i = 0; i < b.n; i++) {
        bytes prefix = b;
        prefix.n = i;
        p = exact(&prefix);
        cs = untouched;
        at = (size_t)-1;
        error = hv_read_jp2(p, i, &cs, &at);
        check(error != NULL && at <= i, "general access proper prefix", error);
        check(memcmp(&cs, &untouched, sizeof cs) == 0,
              "failed access leaves codestream unchanged", NULL);
        free(p);
    }
    /* Duplicate codestreams are outside this single-codestream accessor. */
    box(&b, "jp2c", NULL, 0);
    p = exact(&b);
    at = (size_t)-1;
    error = hv_read_jp2(p, b.n, &cs, &at);
    expect("general access duplicate codestream", error, at, "jp2.one-codestream", b.n);
    free(p);
    /* Compatibility remains an interpreted requirement. */
    set32(&b, 28, HV_BRAND_JPX);
    p = exact(&b);
    at = (size_t)-1;
    error = hv_read_jp2(p, b.n, &cs, &at);
    expect("general access missing JP2 compatibility", error, at,
           "file.ftyp-compatibility", 12);
    free(p);
    release(&b);
}

/* General JPX discovery preserves box order and keeps fragment/reference
 * bodies opaque. The serving representation is deliberately not used. */
static void check_container_access(void) {
    bytes b = {0};
    hv_boxes it;
    hv_ftyp type;
    hv_box item;
    const char *error;
    uint8_t *p;
    size_t at, f, list, refs, i, n, prefix, starts[8], ends[8];
    const uint32_t types[] = {HV_BOX_JP2C, HV_BOX_FTBL, HV_BOX_JP2C,
                              HV_BOX_FTBL, HV_BOX_DTBL, HV_BOX_ASOC, HV_BOX_J2CX};

    add(&b, &SIG);
    add(&b, &FTYP_JPX);
    starts[0] = b.n; add(&b, &JP2C); ends[0] = b.n;
    starts[1] = f = begin(&b, "ftbl");
    list = begin(&b, "flst");
    u16(&b, 2);
    u64(&b, 120); u32(&b, 10); u16(&b, 0);
    u64(&b, 200); u32(&b, 20); u16(&b, 1);
    end(&b, list); end(&b, f); ends[1] = b.n;
    starts[2] = b.n; add(&b, &JP2C); ends[2] = b.n;
    /* An uninterpretable FTBL body is accessible as an opaque extent too. */
    starts[3] = b.n; box(&b, "ftbl", "x", 1); ends[3] = b.n;
    starts[4] = refs = begin(&b, "dtbl");
    u16(&b, 1); url(&b, "relative.j2k"); end(&b, refs); ends[4] = b.n;
    starts[5] = b.n; box(&b, "asoc", "x", 1); ends[5] = b.n;
    starts[6] = b.n; box(&b, "j2cx", "x", 1); ends[6] = b.n;
    p = exact(&b);
    at = (size_t)-1;
    fail_at = 0;
    error = hv_container_open(p, b.n, &it, &type, &at);
    fail_at = -1;
    expect("general JPX opening without allocation", error, at, NULL, 0);
    check(type.brand == HV_BRAND_JPX && type.jpx && it.pos == SIG.n + FTYP_JPX.n,
          "general JPX type and first content offset", NULL);
    for (n = 0; n < sizeof types / sizeof *types; n++) {
        check(hv_boxes_next(&it, &item, &error, &at) == 1 && item.type == types[n] &&
              item.start == starts[n] && item.end == ends[n],
              "general JPX ordered opaque extent", NULL);
    }
    check(hv_boxes_next(&it, &item, &error, &at) == 0,
          "general JPX end of content", NULL);
    check(memcmp(p, b.d, b.n) == 0, "general discovery preserves input bytes", NULL);
    free(p);

    /* Every exact-sized prefix: opening checks only the prefix it reads;
     * later truncation is reported by the iterator at the partial box. */
    for (prefix = 0; prefix < b.n; prefix++) {
        bytes cut = b;
        cut.n = prefix;
        p = exact(&cut);
        memset(&it, 0, sizeof it);
        memset(&type, 0, sizeof type);
        at = (size_t)-1;
        error = hv_container_open(p, prefix, &it, &type, &at);
        if (prefix < starts[0]) {
            check(error != NULL && at <= prefix, "truncated container opening", error);
            check(it.buf == NULL && it.pos == 0 && it.end == 0 && type.brand == 0,
                  "failed container outputs unchanged", NULL);
        } else {
            expect("complete container prefix opening", error, at, NULL, 0);
            for (i = 0; i < sizeof types / sizeof *types && ends[i] <= prefix; i++)
                check(hv_boxes_next(&it, &item, &error, &at) == 1 &&
                      item.start == starts[i] && item.end == ends[i],
                      "complete boxes of prefix", NULL);
            if (starts[i] == prefix)
                check(hv_boxes_next(&it, &item, &error, &at) == 0,
                      "prefix at content boundary", NULL);
            else
                check(hv_boxes_next(&it, &item, &error, &at) == -1 && at == starts[i],
                      "truncated later box offset", NULL);
        }
        free(p);
    }
    /* File Type compatibility is still checked, independently of box bodies. */
    set32(&b, SIG.n + 16, HV_BRAND_JP2);
    p = exact(&b); at = (size_t)-1;
    error = hv_container_open(p, b.n, &it, &type, &at);
    expect("JPX without JPX compatibility", error, at, "file.ftyp-compatibility", 12);
    free(p);
    release(&b);

    /* JP2-compatible alternate brand and ignored MinV use the same entry. */
    add(&b, &SIG);
    box(&b, "ftyp", "test\0\0\0\x07jp2 ", 12);
    p = exact(&b); at = (size_t)-1;
    error = hv_container_open(p, b.n, &it, &type, &at);
    expect("general JP2-compatible container opening", error, at, NULL, 0);
    check(!hv_ftyp_is_jpx(&type) && type.jp2 && type.minor == 7 && it.pos == b.n,
          "general JP2 file type", NULL);
    free(p);
    release(&b);
}

/* Framed input boxes only; payload errors are independent of box framing. */
static hv_box first_box(const bytes *b, const uint8_t *p) {
    hv_boxes it;
    hv_box box = {0};
    const char *error;
    size_t at;
    hv_boxes_file(&it, p, b->n);
    check(hv_boxes_next(&it, &box, &error, &at) == 1, "fixture box framing", NULL);
    return box;
}

static void check_fragment_access(void) {
    bytes b = {0}, payload = {0};
    hv_box table;
    hv_fragments list;
    Fragment entry;
    uint8_t *p;
    const char *error;
    size_t at, f, j, k;
    const unsigned counts[] = {0, 1, 2, 65535};

    for (k = 0; k < sizeof counts / sizeof *counts; k++) {
        clear(&b);
        f = begin(&b, "ftbl");
        j = begin(&b, "flst");
        u16(&b, counts[k]);
        for (size_t n = 0; n < counts[k]; n++) {
            u64(&b, n == 0 ? 12 : UINT64_MAX);
            u32(&b, n == 0 ? 0 : UINT32_MAX);
            u16(&b, n == 0 ? 0 : UINT16_MAX);
        }
        end(&b, j); end(&b, f);
        p = exact(&b); table = first_box(&b, p); at = (size_t)-1;
        fail_at = 0;
        error = hv_fragments_open(p, &table, &list, &at);
        fail_at = -1;
        expect("fragment counts through NF maximum", error, at, NULL, 0);
        check(list.count == counts[k] && list.buf == p && list.start == 18,
              "borrowed fragment list", NULL);
        for (size_t n = 0; n < list.count; n++) {
            at = (size_t)-1;
            error = hv_fragment_read(&list, n, &entry, &at);
            expect("fragment field decoding", error, at, NULL, 0);
            check(entry.off == (n == 0 ? 12 : UINT64_MAX) &&
                  entry.len == (n == 0 ? 0 : UINT32_MAX) &&
                  entry.dr == (n == 0 ? 0 : UINT16_MAX),
                  "fragment values without resolution or extent arithmetic", NULL);
        }
        memset(&entry, 0, sizeof entry); at = (size_t)-1;
        error = hv_fragment_read(&list, SIZE_MAX, &entry, &at);
        expect("fragment index before arithmetic", error, at, "fragment index out of range", 18);
        check(entry.off == 0 && entry.len == 0 && entry.dr == 0,
              "failed fragment output unchanged", NULL);
        if (list.count > 0) {
            /* OFF below 12 is a format constraint, not server policy. */
            memset(p + list.start, 0, 8); at = (size_t)-1;
            error = hv_fragment_read(&list, 0, &entry, &at);
            expect("fragment OFF format bound", error, at,
                   "flst: fragment offset (OFF) below 12", 18);
        }
        free(p);
    }
    /* NF=2 with every partial payload, plus a trailing byte. */
    u16(&payload, 2);
    u64(&payload, 12); u32(&payload, 1); u16(&payload, 0);
    u64(&payload, 20); u32(&payload, 1); u16(&payload, 1);
    u8(&payload, 0);
    for (k = 0; k <= payload.n; k++) {
        clear(&b); f = begin(&b, "ftbl");
        box(&b, "flst", payload.d, k); end(&b, f);
        p = exact(&b); table = first_box(&b, p); at = (size_t)-1;
        list.buf = NULL; list.start = list.count = 0;
        error = hv_fragments_open(p, &table, &list, &at);
        expect("fragment list bounded NF/extent", error, at,
               k == 30 ? NULL : k < 2 ? "flst: shorter than NF" : "flst.nf-count", 8);
        if (error != NULL)
            check(list.buf == NULL && list.count == 0 && list.start == 0,
                  "failed fragment list output unchanged", NULL);
        free(p);
    }
    /* Empty/duplicate FLST children and malformed later child framing. */
    for (k = 0; k < 3; k++) {
        clear(&b); f = begin(&b, "ftbl");
        if (k > 0) box(&b, "flst", "\0\0", 2);
        if (k == 1) box(&b, "flst", "\0\0", 2);
        if (k == 2) u8(&b, 0);
        end(&b, f);
        p = exact(&b); table = first_box(&b, p); at = (size_t)-1;
        error = hv_fragments_open(p, &table, &list, &at);
        expect("fragment table child structure", error, at,
               k == 2 ? "invalid or truncated box header" : "ftbl.one-flst", k == 2 ? 18 : 0);
        free(p);
    }
    release(&payload); release(&b);
}

static void check_reference_access(void) {
    bytes b = {0};
    hv_box table, entry;
    hv_boxes it;
    const char *error;
    const uint8_t *loc;
    uint8_t *p;
    size_t at, count, size, root;
    const char *locations[] = {"relative.j2k", "file:///other.dat", "https://example.org/a.jp2", ""};
    size_t k;

    root = begin(&b, "dtbl"); u16(&b, 4);
    for (k = 0; k < 4; k++) url(&b, locations[k]);
    end(&b, root); p = exact(&b); table = first_box(&b, p); at = (size_t)-1;
    fail_at = 0;
    error = hv_references_open(p, &table, &it, &count, &at);
    fail_at = -1;
    expect("data references without serving URL policy", error, at, NULL, 0);
    check(count == 4 && it.buf == p && it.pos == 10, "borrowed reference table", NULL);
    for (k = 0; k < count; k++) {
        check(hv_boxes_next(&it, &entry, &error, &at) == 1, "reference entry order", NULL);
        at = (size_t)-1;
        error = hv_url_read(p, &entry, &loc, &size, &at);
        expect("URL structural reading", error, at, NULL, 0);
        check(size == strlen(locations[k]) && loc == p + entry.payload + 4 &&
              memcmp(loc, locations[k], size) == 0, "original URL bytes", NULL);
    }
    free(p);
    /* Count and type errors do not leak a partially initialized iterator. */
    for (k = 0; k < 4; k++) {
        clear(&b); root = begin(&b, "dtbl");
        if (k != 0) u16(&b, k == 3 ? 0 : 1);
        if (k == 2) box(&b, "xml ", NULL, 0);
        if (k == 3) url(&b, "a");
        end(&b, root); p = exact(&b); table = first_box(&b, p);
        memset(&it, 0, sizeof it); count = SIZE_MAX; at = (size_t)-1;
        error = hv_references_open(p, &table, &it, &count, &at);
        expect("reference structure errors", error, at,
               k == 0 ? "dtbl: shorter than NDR" : k == 2 ? "dtbl.non-url" : "dtbl.ndr-count",
               k == 2 ? 10 : 0);
        check(it.buf == NULL && it.pos == 0 && count == SIZE_MAX,
              "failed reference outputs unchanged", NULL);
        free(p);
    }
    /* Exactly framed URL boxes with malformed contents. */
    for (k = 0; k < 5; k++) {
        static const uint8_t bodies[][7] = {
            {0,0,0}, {1,0,0,0,0}, {0,0,0,0,'a'},
            {0,0,0,0,0,'a',0}, {0,0,0,1,0}
        };
        static const size_t lengths[] = {3,5,5,7,5};
        clear(&b); box(&b, "url ", bodies[k], lengths[k]);
        p = exact(&b); entry = first_box(&b, p);
        loc = NULL; size = SIZE_MAX; at = (size_t)-1;
        error = hv_url_read(p, &entry, &loc, &size, &at);
        expect("URL content errors", error, at,
               k == 0 ? "url: shorter than VERS and FLAG" :
               k == 1 || k == 4 ? "url.version-flags" : "url.terminator", 0);
        check(loc == NULL && size == SIZE_MAX, "failed URL outputs unchanged", NULL);
        free(p);
    }
    release(&b);
}

/* A real sparse mapping: the XLBox ends above INT_MAX and the codestream
 * follows it. Only headers and the small original codestream are touched. */
static void check_large_jp2_access(void) {
    bytes head = {0};
    size_t stream_at = (size_t)INT_MAX + 128;
    size_t size = stream_at + JP2C.n, at;
    uint8_t *mapped;
    hv_box cs;
    const char *error;
    FILE *f = tmpfile();
    if (f == NULL) {
        check(0, "large access temporary file", NULL);
        return;
    }
    if (ftruncate(fileno(f), (off_t)size) != 0) {
        check(0, "large access sparse extent", NULL);
        fclose(f);
        return;
    }
    mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(f), 0);
    if (mapped == MAP_FAILED) {
        check(0, "large access mapping", NULL);
        fclose(f);
        return;
    }
    add(&head, &SIG);
    add(&head, &FTYP_JP2);
    u32(&head, 1);
    put(&head, "free", 4);
    u64(&head, stream_at - SIG.n - FTYP_JP2.n);
    memcpy(mapped, head.d, head.n);
    memcpy(mapped + stream_at, JP2C.d, JP2C.n);
    at = (size_t)-1;
    error = hv_read_jp2(mapped, size, &cs, &at);
    expect("general access above INT_MAX", error, at, NULL, 0);
    check(error == NULL && cs.start == stream_at && cs.end == size &&
          memcmp(mapped + cs.payload, JP2C.d + 8, JP2C.n - 8) == 0,
          "large offset original bytes", NULL);
    at = (size_t)-1;
    error = hv_served_jp2(mapped, size, &cs, &at);
    expect("served large-file limit retained", error, at, "file.size-limit", 0);
    at = (size_t)-1;
    error = hv_check_jp2h(mapped, size, &at);
    expect("header validation limit retained", error, at, "file.size-limit", 0);
    at = (size_t)-1;
    error = hv_read_jp2(mapped, size - 1, &cs, &at);
    expect("large offset codestream overrun", error, at, "box overruns its container", stream_at);
    /* The same large box extents through general JPX access. */
    {
        hv_boxes it;
        hv_ftyp type;
        memcpy(mapped + SIG.n + 8, "jpx ", 4);
        memcpy(mapped + SIG.n + 16, "jpx ", 4);
        at = (size_t)-1;
        error = hv_container_open(mapped, size, &it, &type, &at);
        expect("general JPX opening above INT_MAX", error, at, NULL, 0);
        check(hv_ftyp_is_jpx(&type) &&
              hv_boxes_next(&it, &cs, &error, &at) == 1 && cs.type == HV_BOX_FREE &&
              cs.end == stream_at && hv_boxes_next(&it, &cs, &error, &at) == 1 &&
              cs.type == HV_BOX_JP2C && cs.start == stream_at && cs.end == size &&
              hv_boxes_next(&it, &cs, &error, &at) == 0,
              "general JPX large ordered extents", NULL);
    }
    check(munmap(mapped, size) == 0, "large access unmap", NULL);
    fclose(f);
    release(&head);
}

static void check_jpx_files(void) {
    bytes b = {NULL, 0, 0};
    const char *error;
    size_t at;
    hv_served_sources j;
    uint8_t *p;
    int k, i;

    /* Many codestreams, embedded and linked: the arrays grow. */
    add(&b, &SIG);
    add(&b, &FTYP_JPX);
    for (k = 0; k < 40; k++)
        add(&b, &JPCH);
    for (k = 0; k < 40; k++)
        add(&b, &JP2C);
    p = exact(&b);
    check(hv_served_jpx(p, b.n, &j, &at) == NULL && j.count == 40 && j.links == NULL &&
          j.jp2c[39].start == b.n - JP2C.n, "40 embedded codestreams", NULL);
    hv_served_sources_free(&j);
    check(j.jp2c == NULL && j.links == NULL && j.count == 0, "hv_served_sources_free clears", NULL);
    hv_served_sources_free(&j);
    /* Out of memory at each growth. */
    for (i = 0; i < 3; i++) {
        fail_at = i;
        at = 0;
        error = hv_served_jpx(p, b.n, &j, &at);
        fail_at = -1;
        /* the array grows at the 1st, 17th and 33rd codestream */
        expect("40 embedded codestreams, out of memory", error, at, "out of memory",
               12 + 20 + 40 * JPCH.n + (size_t)(16 * i) * JP2C.n);
        check(j.jp2c == NULL && j.count == 0, "freed after out of memory", NULL);
    }
    free(p);
    release(&b);

    b = linked(40);
    p = exact(&b);
    error = hv_served_jpx(p, b.n, &j, &at);
    check(error == NULL && j.count == 40 && j.jp2c == NULL && j.links[39].dr == 40 &&
          j.links[39].offset == 85 && j.links[39].length == 88 && j.links[0].loc_size == 16 &&
          memcmp(j.links[0].loc, "file://c%20d.jp2", 16) == 0 && j.links[1].loc_size == 13,
          "40 linked codestreams", error);
    if (error == NULL)
        hv_served_sources_free(&j);
    for (i = 0; i < 9; i++) {
        fail_at = i;
        error = hv_served_jpx(p, b.n, &j, &at);
        fail_at = -1;
        check(error != NULL && strcmp(error, "out of memory") == 0 && j.links == NULL,
              "40 linked codestreams, out of memory", error);
    }
    fail_at = 9;
    error = hv_served_jpx(p, b.n, &j, &at);
    fail_at = -1;
    check(error == NULL, "40 linked codestreams, 9 allocations", error);
    if (error == NULL)
        hv_served_sources_free(&j);
    free(p);
    release(&b);

    /* One linked codestream. */
    b = linked(1);
    p = exact(&b);
    error = hv_served_jpx(p, b.n, &j, &at);
    check(error == NULL && j.count == 1 && j.links[0].dr == 1 && j.links[0].loc_size == 16 &&
          j.links[0].loc == p + b.n - 17, "one linked codestream", error);
    if (error == NULL)
        hv_served_sources_free(&j);
    free(p);
    release(&b);

    /* An flst of 2 fragments; a url of VERS 1; a url of VERS and FLAG
     * alone, and of less; a dtbl of one byte. linked(1): sig, ftyp, jpch
     * (at 32), ftbl (62) holding flst (70), dtbl (94) holding url (104). */
    b = linked(1);
    check(memcmp(b.d + 74, "flst", 4) == 0 && memcmp(b.d + 108, "url ", 4) == 0, "linked(1)",
          NULL);
    b.d[79] = 2;                                /* NF */
    error = jpx(&b, &at);
    expect("flst NF 2", error, at, "flst.one-fragment", 70);
    release(&b);
    b = linked(1);
    b.d[112] = 1;                               /* VERS */
    error = jpx(&b, &at);
    expect("url VERS 1", error, at, "url.version-flags", 104);
    release(&b);
    for (k = 0; k < 2; k++) {
        size_t d;
        clear(&b);
        add(&b, &SIG);
        add(&b, &FTYP_JPX);
        add(&b, &JPCH);
        ftbl(&b, 85, 88, 1);
        d = begin(&b, "dtbl");
        u16(&b, 1);
        box(&b, "url ", "\0\0\0\0", k ? 4 : 3);
        end(&b, d);
        error = jpx(&b, &at);
        expect(k ? "url without LOC" : "url of 3 bytes", error, at,
               k ? "url.terminator" : "url: shorter than VERS and FLAG", d + 10);
    }
    clear(&b);
    add(&b, &SIG);
    add(&b, &FTYP_JPX);
    add(&b, &JPCH);
    ftbl(&b, 85, 88, 1);
    box(&b, "dtbl", "\0", 1);
    add(&b, &JPCH);
    error = jpx(&b, &at);
    expect("dtbl of one byte", error, at, "dtbl: shorter than NDR", b.n - JPCH.n - 9);
    error = jpx_headers(&b, &at);
    check(error != NULL, "dtbl of one byte (header checks)", error);

    /* flst: NF alone, one byte of it last in the file, half a fragment,
     * OFF below 12; a dtbl holding an xml box. */
    {
        static const struct {
            const char *what, *payload;
            size_t n;
            int last;
            const char *want;
        } flst[] = {
            {"flst of NF", "\x00\x01", 2, 0, "flst.one-fragment"},
            {"flst of one byte, last", "\x00", 1, 1, "flst.one-fragment"},
            {"flst with half a fragment", "\x00\x01\0\0\0\0\0\0\0\x55", 10, 0,
             "flst.one-fragment"},
            {"flst with OFF 5", "\x00\x01\0\0\0\0\0\0\0\x05\0\0\0\x58\x00\x01", 16, 0,
             "flst: fragment offset (OFF) below 12"},
        };
        for (k = 0; k < (int)(sizeof flst / sizeof *flst); k++) {
            size_t f;
            clear(&b);
            add(&b, &SIG);
            add(&b, &FTYP_JPX);
            add(&b, &JPCH);
            if (!flst[k].last) {
                size_t d = begin(&b, "dtbl");
                u16(&b, 1);
                url(&b, "file:///f.jp2");
                end(&b, d);
            }
            f = begin(&b, "ftbl");
            box(&b, "flst", flst[k].payload, flst[k].n);
            end(&b, f);
            error = jpx(&b, &at);
            expect(flst[k].what, error, at, flst[k].want, f + 8);
        }
        clear(&b);
        add(&b, &SIG);
        add(&b, &FTYP_JPX);
        add(&b, &JPCH);
        ftbl(&b, 85, 88, 1);
        {
            size_t d = begin(&b, "dtbl");
            u16(&b, 1);
            box(&b, "xml ", "<a/>", 4);
            end(&b, d);
            error = jpx(&b, &at);
            expect("an xml box in dtbl", error, at, "dtbl.non-url", d + 10);
        }
    }

    /* A url past its dtbl. */
    release(&b);
    b = linked(1);
    b.d[107] = 30;                              /* the url's LBox */
    error = jpx(&b, &at);
    expect("a url past its dtbl", error, at, "box overruns its container", 104);
    release(&b);

    /* Two boxes. */
    clear(&b);
    add(&b, &SIG);
    add(&b, &FTYP_JPX);
    error = jpx(&b, &at);
    expect("two boxes", error, at, "jpx.no-jpch", b.n);
    b.n = 12;
    error = jpx(&b, &at);
    expect("one box", error, at, "file.two-boxes", 12);
    release(&b);
}

static void check_link_paths(void) {
    hv_served_source link;
    char out[64];
    const char *error;

    memset(&link, 0, sizeof link);
    link.loc = (const uint8_t *)"file://a%20b.jp2";
    link.loc_size = 16;
    check(hv_served_path(&link, "/x/y.jpx", out, 0) != NULL &&
          strcmp(hv_served_path(&link, "/x/y.jpx", out, 0), "no room for the linked path") == 0,
          "no room", NULL);
    strcpy(out, "?");
    error = hv_served_path(&link, "/x/y.jpx", out, 1);
    check(error != NULL && strcmp(error, "linked path longer than the caller's buffer") == 0 &&
          out[0] == 0, "room for the NUL alone", error);
    error = hv_served_path(&link, "y.jpx", out, sizeof out);
    check(error == NULL && strcmp(out, "a b.jp2") == 0, "no directory in the JPX path", error);
    /* "/x/" and "a b.jp2" with its NUL: 11 bytes */
    error = hv_served_path(&link, "/x/y.jpx", out, 11);
    check(error == NULL && strcmp(out, "/x/a b.jp2") == 0, "a joined path that just fits",
          error);
    strcpy(out, "?");
    error = hv_served_path(&link, "/x/y.jpx", out, 10);
    check(error != NULL && strcmp(error, "linked path longer than the caller's buffer") == 0 &&
          out[0] == 0, "a joined path one byte too long", error);
    link.loc = (const uint8_t *)"file://a%zz.jp2";
    link.loc_size = 15;
    strcpy(out, "?");
    error = hv_served_path(&link, NULL, out, sizeof out);
    check(error != NULL && strcmp(error, "url.percent-encoding") == 0 && out[0] == 0,
          "a bad escape", error);
    /* A full output buffer wins over an invalid escape beyond that prefix. */
    link.loc = (const uint8_t *)"file://ab%zz.jp2";
    link.loc_size = strlen((const char *)link.loc);
    error = hv_served_path(&link, NULL, out, 2);
    check(error != NULL && strcmp(error, "linked path longer than the caller's buffer") == 0 &&
          out[0] == 0, "buffer fills before invalid URL escape", error);
    error = hv_served_path(&link, NULL, out, 3);
    check(error != NULL && strcmp(error, "url.percent-encoding") == 0 && out[0] == 0,
          "invalid URL escape at the buffer boundary", error);
}

static void check_links(void) {
    hv_served_source link;
    const char *error;
    size_t at = 99;
    bytes b = {NULL, 0, 0};
    uint8_t *p;

    memset(&link, 0, sizeof link);
    link.offset = 85;
    link.length = 88;
    add(&b, &FRAME1);
    b.d[5] = 'X';                               /* TBox of the signature */
    p = exact(&b);
    error = hv_served_link(p, b.n, &link, &at);
    expect("a linked file without the signature", error, at, "file.signature", 0);
    free(p);
    release(&b);
}

/* ------------------------------------------------------------------------
 * Header boxes
 * ------------------------------------------------------------------------ */

static void check_jp2_headers(void) {
    bytes b;
    const char *error;
    size_t at, h = 32;          /* jp2h */
    int i;
    static const struct {
        const char *what, *boxes;
        size_t n, offset;       /* the error's, from jp2h's first child */
        const char *want;
    } bad[] = {
        {"an empty bpcc", "\0\0\0\x08" "bpcc", 8, 37, "bpcc: empty"},
        {"a reserved bit depth", "\0\0\0\x0A" "bpcc\x07\x30", 10, 37 + 9, "bpcc: reserved bit depth"},
        {"a reserved first bit depth", "\0\0\0\x0A" "bpcc\x30\x07", 10, 37 + 8,
         "bpcc: reserved bit depth"},
        {"a cdef of 1 and 2/3 entries", "\0\0\0\x14" "cdef\x00\x02\0\0\0\0\0\x01\0\0\0\0", 20,
         37, "cdef: shorter than its N entries"},
        {"a colr of 2 bytes", "\0\0\0\x0A" "colr\x01\x00", 10, 37, "colr: shorter than its fields"},
        {"a pclr of 2 bytes", "\0\0\0\x0A" "pclr\x00\x01", 10, 37,
         "pclr: no NE and NPC, or one out of range"},
        {"a pclr without its bit depths", "\0\0\0\x0B" "pclr\x00\x01\x01", 11, 37,
         "pclr: shorter than its NPC bit depths"},
        {"a pclr without its second bit depth", "\0\0\0\x0C" "pclr\x00\x01\x02\x07", 12, 37,
         "pclr: shorter than its NPC bit depths"},
        {"a pclr without entries", "\0\0\0\x0C" "pclr\x00\x01\x01\x07", 12, 37,
         "pclr.entries-length"},
        {"a pclr with a reserved bit depth", "\0\0\0\x0D" "pclr\x00\x01\x01\x30\x00", 13, 37,
         "pclr: reserved bit depth"},
        {"an empty cmap", "\0\0\0\x08" "cmap", 8, 37, "cmap: not a whole number of entries"},
        {"a cdef of one byte", "\0\0\0\x09" "cdef\x00", 9, 37, "cdef: no N, or N is 0"},
        {"a cdef short of its entries", "\0\0\0\x10" "cdef\x00\x02\x00\x00\x00\x00\x00\x01", 16,
         37, "cdef: shorter than its N entries"},
        {"a res child past res", "\0\0\0\x14" "res \0\0\0\x13" "resc\0\0\0\0", 20, 37 + 8,
         "box.framing"},
        {"a child past jp2h", "\0\0\0\x10" "free\0\0\0\0", 8, 37, "box.framing"},
    };

    for (i = 0; i < (int)(sizeof bad / sizeof *bad); i++) {
        b = jp2_plus(bad[i].boxes, bad[i].n);
        if (strcmp(bad[i].what, "a child past jp2h") == 0)
            b.d[h + 8 + 22 + 15 + 3] = 0x40;    /* the free box's LBox: 64 */
        error = jp2h(&b, &at);
        expect(bad[i].what, error, at, bad[i].want, h + 8 + bad[i].offset);
        release(&b);
    }

    /* Each cdef allocation out of memory. */
    b = jp2_plus("\0\0\0\x10" "cdef\x00\x01\x00\x00\x00\x00\x00\x00", 16);
    {
        uint8_t *p = exact(&b);
        fail_at = 0;
        at = 0;
        error = hv_check_jp2h(p, b.n, &at);
        fail_at = -1;
        expect("cdef out of memory", error, at, "out of memory", h + 8 + 37 + 0);
        free(p);
    }
    release(&b);

    /* A cgrp in a JP2 file: a box T.800 does not define, skipped (I.8),
     * whatever it holds. */
    b = jp2_plus("\0\0\0\x13" "cgrp" "\0\0\0\x0B" "colr\x01\x00\x00", 19);
    error = jp2h(&b, &at);
    expect("a cgrp's colr of 3 bytes", error, at, NULL, 0);
    release(&b);
    b = jp2_plus("\0\0\0\x13" "cgrp" "\0\0\0\x0C" "colr\x01\x00\x00", 19);
    error = jp2h(&b, &at);
    expect("a cgrp's child past it", error, at, NULL, 0);
    release(&b);

    /* The top level cut: jp2c past the end. */
    b = jp2_plus(NULL, 0);
    b.n--;
    error = jp2h(&b, &at);
    expect("jp2c past the end", error, at, "box overruns its container", b.n + 1 - JP2C.n);
    release(&b);

    /* ihdr.ipr at jp2h, and nothing written on success. */
    b = jp2_plus(NULL, 0);
    b.d[h + 8 + 8 + 13] = 1;                    /* IPR */
    error = jp2h(&b, &at);
    expect("IPR without an IPR box", error, at, "ihdr.ipr", h);
    b.d[h + 8 + 8 + 13] = 0;
    error = jp2h(&b, &at);
    expect("a valid jp2h", error, at, NULL, 0);
    release(&b);

    /* read_header and check_tree on their own, without the passes
     * that run before them in the checks: a box's framing error is theirs
     * too, not the end of the boxes. */
    {
        bytes x = {NULL, 0, 0};
        hv_boxes it;
        hv_box box;
        hv_header hh;
        hv_box_tree tree;
        uint8_t *p;
        size_t start = begin(&x, "jp2h");
        u32(&x, 64);                            /* a child past its box */
        put(&x, "free", 4);
        end(&x, start);
        p = exact(&x);
        hv_boxes_file(&it, p, x.n);
        if (hv_boxes_next(&it, &box, &error, &at) != 1) {
            check(0, "read_header: the jp2h box", "does not frame");
        } else {
            error = read_header(p, &box, HV_BOX_JP2H, 0, 0, 0, &hh, &at);
            expect("read_header: a child past its box", error, at, "box overruns its container",
                   8);
        }
        at = 0;
        error = check_tree(p + 8, 8, 0, &tree, &at);
        expect("check_tree: a box past the file", error, at, "box overruns its container", 0);
        free(p);
        release(&x);
    }
}

/* A JPX file: sig, ftyp, rreq, jp2h, then `rest`. */
static bytes jpx_with(const bytes *rest) {
    bytes b = {NULL, 0, 0};
    add(&b, &SIG);
    add(&b, &FTYP_JPX);
    add(&b, &RREQ);
    add(&b, &JPX_JP2H);
    add(&b, rest);
    return b;
}

static void check_jpx_header_boxes(void) {
    bytes r = {NULL, 0, 0}, b;
    const char *error;
    size_t at, x;
    int k;

    /* The base, and nothing written on success. */
    error = jpx_headers(&JPX, &at);
    expect("jpx-embedded.jpx", error, at, NULL, 0);

    /* A jplh last in the file, holding a bpcc (read to its last byte). */
    add(&r, &JPCH);
    add(&r, &JP2C);
    x = r.n;
    box(&r, "jplh", "\0\0\0\x09" "bpcc\x07", 9);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("a jplh last", error, at, NULL, 0);
    release(&b);

    /* No jpch: jp2h is every codestream's header (and the xml box last,
     * which holds what reads as an empty bpcc, is none). */
    clear(&r);
    add(&r, &JP2C);
    box(&r, "xml ", "\0\0\0\x08" "bpcc", 8);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("no jpch", error, at, NULL, 0);
    release(&b);

    /* A second ftyp later (T.800 I.5.2: one and only one). */
    clear(&r);
    add(&r, &JPCH);
    add(&r, &JP2C);
    x = 32 + RREQ.n + JPX_JP2H.n + r.n;
    box(&r, "ftyp", "jpx \0\0\0\0jpx ", 12);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("a second ftyp", error, at, "file.one-ftyp", x);
    release(&b);

    /* jp2h after a jpch, an ftbl, a jplh or an mdat: anywhere at the top
     * level (T.801 M.11.5), but before them in a baseline file (M.9.2.7),
     * with 'jpxb' in the list. */
    for (k = 0; k < 8; k++) {
        clear(&r);
        add(&r, &SIG);
        if (k < 4)
            add(&r, &FTYP_JPX);
        else
            box(&r, "ftyp", "jpx \0\0\0\x01jpx jpxb", 16);
        add(&r, &RREQ);
        if (k % 4 == 0)
            add(&r, &JPCH);
        else if (k % 4 == 1)
            ftbl(&r, 85, 88, 1);
        else if (k % 4 == 2)
            box(&r, "jplh", NULL, 0);
        else
            box(&r, "mdat", NULL, 0);
        add(&r, &JPX_JP2H);
        if (k % 4 != 0)
            add(&r, &JPCH);
        if (k % 4 != 1)
            add(&r, &JP2C);
        if (k % 4 == 1) {
            size_t d = begin(&r, "dtbl");
            u16(&r, 1);
            url(&r, "file:///f.jp2");
            end(&r, d);
        }
        error = jpx_headers(&r, &at);
        /* In a baseline file, the linked first codestream fails first
         * (M.9.2.5: its fragments in the file), at its flst. */
        expect(k % 4 == 0 ? "jp2h after jpch" : k % 4 == 1 ? "jp2h after ftbl"
               : k % 4 == 2 ? "jp2h after jplh" : "jp2h after mdat",
               error, at, k < 4 ? NULL : k == 5 ? "jpxb.fragments" : "jp2h.position",
               k < 4 ? 0 : k == 5 ? 12 + 24 + RREQ.n + 8 : r.n);
    }

    /* Codestream 0 in a j2cx, linked, in a baseline file: M.11.23 asks
     * for a top-level codestream, before any j2cx, so codestream 0 (M.11.6
     * counts those of a j2cx too) is never in one; the file fails that
     * rule, at its end, before M.9.2.5's on the fragments of codestream 0. */
    clear(&r);
    add(&r, &SIG);
    box(&r, "ftyp", "jpx \0\0\0\x01jpx jpxb", 16);
    add(&r, &RREQ);
    add(&r, &JPX_JP2H);
    add(&r, &JPCH);
    {
        size_t j = begin(&r, "j2cx"), d;
        box(&r, "j2ci", "\0\0\0\x01\0\0\0\0", 8);     /* Ncs 1, Ltbl 0 */
        ftbl(&r, 85, 88, 1);
        end(&r, j);
        d = begin(&r, "dtbl");
        u16(&r, 1);
        url(&r, "file:///f.jp2");
        end(&r, d);
    }
    error = jpx_headers(&r, &at);
    expect("jpxb, codestream 0 linked in a j2cx", error, at, "j2cx.top-codestreams", r.n);

    /* A dtbl of one byte, last. */
    clear(&r);
    add(&r, &JPCH);
    add(&r, &JP2C);
    x = 32 + RREQ.n + JPX_JP2H.n + r.n;
    box(&r, "dtbl", "\0", 1);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("a dtbl of one byte (header checks)", error, at, "dtbl.ndr-count", x);
    release(&b);

    /* A jpch child past the jpch. */
    clear(&r);
    add(&r, &JPCH);
    r.d[8 + 3] = 23;                            /* ihdr's LBox */
    add(&r, &JP2C);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("a jpch child past it", error, at, "box.framing", 32 + RREQ.n + JPX_JP2H.n + 8);
    release(&b);

    /* Two dtbl. */
    clear(&r);
    add(&r, &JPCH);
    add(&r, &JP2C);
    box(&r, "dtbl", "\0\0", 2);
    box(&r, "dtbl", "\0\0", 2);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("two dtbl", error, at, "jpx.one-dtbl", b.n);
    release(&b);

    /* The top level cut. */
    b = jpx_with(&r);
    b.n--;
    error = jpx_headers(&b, &at);
    expect("a box past the end", error, at, "box overruns its container", b.n + 1 - 10);
    release(&b);

    /* One jpch whose ihdr is not the codestream's: at the jpch. */
    clear(&r);
    x = 32 + RREQ.n + JPX_JP2H.n;
    add(&r, &JPCH);
    r.d[8 + 8 + 3] = 5;                         /* HEIGHT */
    add(&r, &JP2C);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("one jpch, its HEIGHT", error, at, "ihdr.height", x);
    release(&b);

    /* A jpch with a colr (T.801 M.11.7.2: in jp2h or a cgrp). */
    clear(&r);
    {
        size_t j = begin(&r, "jpch");
        add(&r, &IHDR);
        put(&r, "\0\0\0\x0B" "colr\x03\x00\x01", 11);
        end(&r, j);
    }
    add(&r, &JP2C);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("a jpch colr of METH 3", error, at, "colr.placement",
           32 + RREQ.n + JPX_JP2H.n + 8 + IHDR.n);
    release(&b);

    /* A linked codestream without an ihdr: jpch.ihdr at its jpch. */
    clear(&r);
    add(&r, &SIG);
    add(&r, &FTYP_JPX);
    add(&r, &RREQ_LINKED);
    box(&r, "jpch", NULL, 0);
    ftbl(&r, 85, 88, 1);
    {
        size_t d = begin(&r, "dtbl");
        u16(&r, 1);
        url(&r, "file:///f.jp2");
        end(&r, d);
    }
    /* Without jp2h, the colours in a jplh's cgrp (T.801 M.11.7.1). */
    box(&r, "jplh", "\0\0\0\x17" "cgrp" "\0\0\0\x0F" "colr\x01\x00\x01\x00\x00\x00\x11", 23);
    error = jpx_headers(&r, &at);
    expect("a linked codestream without ihdr", error, at, "jpch.ihdr",
           12 + 20 + RREQ_LINKED.n);

    /* creg in the second of two jplh: jpx.creg at the first. */
    clear(&r);
    add(&r, &JPCH);
    add(&r, &JP2C);
    x = 32 + RREQ.n + JPX_JP2H.n + r.n;
    box(&r, "jplh", NULL, 0);
    box(&r, "jplh", "\0\0\0\x0C" "creg\0\0\0\0", 12);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("creg in one jplh of two", error, at, "jpx.creg", x);
    release(&b);
    release(&r);
}

/* Reader Requirements boxes: ML 1, FUAM, DCM, NSF, then as given. */
static void check_rreqs(void) {
    static const struct {
        const char *what, *payload;
        size_t n;
        const char *want;
    } cases[] = {
        {"rreq of 4 bytes", "\x01\x80\x80\x00", 4,
         "rreq: contents are not ML, FUAM, DCM, NSF standard and NVF vendor features"},
        {"a standard feature cut", "\x01\x80\x80\x00\x01\x00\x05", 7,
         "rreq: contents are not ML, FUAM, DCM, NSF standard and NVF vendor features"},
        {"no NVF", "\x01\x80\x80\x00\x01\x00\x05\x80", 8,
         "rreq: contents are not ML, FUAM, DCM, NSF standard and NVF vendor features"},
        {"two vendor features", "\x01\x80\x80\x00\x01\x00\x05\x80\x00\x02"
         "0123456789abcdef\x80" "fedcba9876543210\x40", 44, NULL},
        {"a vendor feature cut", "\x01\x80\x80\x00\x01\x00\x05\x80\x00\x02"
         "0123456789abcdef\x80" "fedcba9876543210", 43,
         "rreq: contents are not ML, FUAM, DCM, NSF standard and NVF vendor features"},
        {"a byte after the features", "\x01\x80\x80\x00\x01\x00\x05\x80\x00\x00\x00", 11,
         "rreq.extent"},
    };
    size_t i, at;
    bytes b = {NULL, 0, 0};
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        const char *error;
        clear(&b);
        add(&b, &SIG);
        add(&b, &FTYP_JPX);
        box(&b, "rreq", cases[i].payload, cases[i].n);
        add(&b, &JPX_JP2H);
        add(&b, &JPCH);
        add(&b, &JP2C);
        error = jpx_headers(&b, &at);
        expect(cases[i].what, error, at, cases[i].want, cases[i].want ? 32 : 0);
    }
    release(&b);
}

/* ------------------------------------------------------------------------
 * Codestreams
 * ------------------------------------------------------------------------ */

static void seg(bytes *b, unsigned code, const void *body, size_t n) {
    u16(b, code);
    u16(b, (unsigned)n + 2);
    put(b, body, n);
}

/* SIZ: Xsiz, Ysiz, XOsiz, YOsiz, XTsiz, YTsiz, XTOsiz, YTOsiz; components
 * of 8 bits with the given sampling. */
static void siz(bytes *b, const uint32_t f[8], int ncomps, const uint8_t *xr, const uint8_t *yr) {
    int i;
    u16(b, HV_SIZ);
    u16(b, 38 + 3 * (unsigned)ncomps);
    u16(b, 0);
    for (i = 0; i < 8; i++)
        u32(b, f[i]);
    u16(b, (unsigned)ncomps);
    for (i = 0; i < ncomps; i++) {
        u8(b, 7);
        u8(b, xr ? xr[i] : 1);
        u8(b, yr ? yr[i] : 1);
    }
}

/* COD of one layer, no levels, code-blocks 64 x 64. */
static void cod(bytes *b) {
    seg(b, HV_COD, "\x00\x00\x00\x01\x00\x00\x04\x04\x00\x01", 10);
}
static void qcd(bytes *b) { seg(b, HV_QCD, "\x40\x40", 2); }

/* A PLT of values, 7 bits a byte. */
static void plt(bytes *b, unsigned zplt, const uint64_t *v, int n) {
    bytes body = {NULL, 0, 0};
    int i, k;
    u8(&body, zplt);
    for (i = 0; i < n; i++) {
        uint8_t g[10];
        uint64_t x = v[i];
        k = 0;
        do {
            g[k++] = x & 127;
            x >>= 7;
        } while (x);
        while (k-- > 0)
            u8(&body, g[k] | (k ? 0x80 : 0));
    }
    seg(b, HV_PLT, body.d, body.n);
    release(&body);
}

/* SOT, with Psot set by sot_end (0: left 0). */
static size_t sot(bytes *b, unsigned isot, unsigned tpsot, unsigned tnsot) {
    size_t at = b->n;
    u16(b, HV_SOT);
    u16(b, 10);
    u16(b, isot);
    u32(b, 0);
    u8(b, tpsot);
    u8(b, tnsot);
    return at;
}
static void sot_end(bytes *b, size_t at) { set32(b, at + 6, (uint32_t)(b->n - at)); }

/* A tile-part: SOT, PLT of one packet of `data` bytes, SOD, data. */
static void tile_part(bytes *b, unsigned isot, unsigned tpsot, unsigned tnsot, size_t data) {
    size_t at = sot(b, isot, tpsot, tnsot), i;
    uint64_t v = data;
    plt(b, 0, &v, 1);
    u16(b, HV_SOD);
    for (i = 0; i < data; i++)
        u8(b, 0x5A);
    sot_end(b, at);
}

/* SOC, SIZ of w x h (one tile, one component), COD, QCD. */
static void main_header(bytes *b, uint32_t w, uint32_t h) {
    uint32_t f[8] = {w, h, 0, 0, w, h, 0, 0};
    u16(b, HV_SOC);
    siz(b, f, 1, NULL, NULL);
    cod(b);
    qcd(b);
}

/* A codestream of 4 x 4 in one tile-part with a packet of 3 bytes. */
static bytes simple(void) {
    bytes b = {NULL, 0, 0};
    main_header(&b, 4, 4);
    tile_part(&b, 0, 0, 1, 3);
    u16(&b, HV_EOC);
    return b;
}

/* hv_codestream_check on exactly the bytes. */
static const char *cs_check(const bytes *b, unsigned flags, size_t *at) {
    uint8_t *p = exact(b);
    const char *error;
    *at = (size_t)-1;
    error = hv_codestream_check(p, 0, b->n, flags, at);
    free(p);
    return error;
}

/* The items of a codestream, walked here from its markers alone: SIZ,
 * the main-header segments, each tile-part (SOT to the end Psot gives, or
 * to EOC for Psot 0), its header segments, its data after SOD, and EOC.
 * Compared with hv_codestream_next: kind, code, start, end, the SOT and
 * the decoded segment each item carries, and the zero PLT entries of each
 * tile-part. */
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

static void compare_items(const char *what, const uint8_t *buf, size_t start, size_t end,
                          unsigned flags) {
    hv_codestream cs;
    hv_item item;
    size_t pos = start + 2, tp_end = 0;
    unsigned zeros = 0;
    int in_tile = 0, n = 0, ok = 1;
    char detail[128];

    if (hv_codestream_open(&cs, buf, start, end, flags) != 0) {
        check(0, what, cs.error);
        hv_codestream_close(&cs);
        return;
    }
    while (ok && hv_codestream_next(&cs, &item) == 1) {
        unsigned code = be16(buf + pos);
        size_t e;
        int kind;
        n++;
        if (!in_tile && code == HV_SOT) {
            uint32_t psot = get32(buf + pos + 6);
            tp_end = psot ? pos + psot : end - 2;
            kind = HV_TILE_PART;
            e = tp_end;
            ok = item.sot.isot == be16(buf + pos + 4) && item.sot.psot == psot &&
                 item.sot.tpsot == buf[pos + 10] && item.sot.tnsot == buf[pos + 11];
            in_tile = 1;
            zeros = 0;
        } else if (!in_tile && code == HV_EOC) {
            kind = HV_END;
            e = pos + 2;
        } else if (in_tile && code == HV_SOD) {
            kind = HV_TILE_DATA;
            ok = item.plt_padding == zeros && item.start == pos + 2 && item.code == 0;
            pos += 2;
            code = 0;
            e = tp_end;
            in_tile = 0;
        } else {
            kind = in_tile ? HV_TILE_SEGMENT : HV_SEGMENT;
            e = (code >= HV_NO_SEGMENT_FIRST && code <= HV_NO_SEGMENT_LAST)
                    ? pos + 2 : pos + 2 + be16(buf + pos + 2);
            ok = (item.siz != NULL) == (code == HV_SIZ) &&
                 (item.cod != NULL) == (code == HV_COD) &&
                 (item.qcd != NULL) == (code == HV_QCD) &&
                 (item.plt != NULL) == (code == HV_PLT) &&
                 (item.com != NULL) == (code == HV_COM);
            if (code == HV_PLT) {
                size_t p = item.plt->start;
                uint64_t v;
                ok &= item.plt->end == e && p == pos + 5;
                while (hv_plt_next(buf, &p, e, &v) == 1)
                    zeros += v == 0;
            }
            if (code == HV_COM)
                ok &= item.com->text == buf + pos + 6 && item.com->size == e - pos - 6;
            if (in_tile)
                ok &= memcmp(&item.sot, &cs.sot, sizeof cs.sot) == 0;
        }
        snprintf(detail, sizeof detail, "item %d: kind %d code %04X [%zu, %zu), expected %d %04X "
                 "[%zu, %zu)", n, (int)item.kind, item.code, item.start, item.end, kind, code,
                 pos, e);
        ok &= (int)item.kind == kind && item.code == code && item.end == e &&
              (kind == HV_TILE_DATA || item.start == pos);
        check(ok, what, detail);
        pos = kind == HV_TILE_PART ? pos + 12 : e;
    }
    check(cs.error == NULL && pos == end, what, cs.error ? cs.error : "not at the end");
    hv_codestream_close(&cs);
}

/* Every proper prefix of buf[start, end) is rejected, with every flag, and
 * none is read past (each in a buffer of exactly its size). */
static void check_prefixes(const char *what, const uint8_t *buf, size_t start, size_t end) {
    static const unsigned flags[] = {0, HV_READ_PADDED, HV_READ_PACKETS_JPIP, HV_READ_JPIP};
    size_t n, at;
    int f;
    for (n = start; n < end; n++) {
        uint8_t *p = malloc(n ? n : 1);
        memcpy(p, buf, n);
        for (f = 0; f < 4; f++)
            if (hv_codestream_check(p, start, n, flags[f], &at) == NULL) {
                check(0, what, "a prefix reads through");
                f = 4;
                n = end;
            }
        free(p);
    }
}

static void check_codestreams(void) {
    bytes b = {NULL, 0, 0};
    const char *error;
    size_t at, i;
    uint8_t *p;
    hv_codestream cs;

    b = simple();
    error = cs_check(&b, HV_READ_JPIP, &at);
    expect("a simple codestream", error, at, NULL, 0);
    p = exact(&b);
    compare_items("a simple codestream", p, 0, b.n, 0);
    check_prefixes("a simple codestream", p, 0, b.n);
    free(p);

    /* The open: start after end; the pair of flags; SOC; SIZ. */
    p = exact(&b);
    check(hv_codestream_open(&cs, p, 5, 4, HV_READ_VALIDATE) != 0 &&
          strcmp(cs.error, "codestream ends before it starts") == 0 && cs.error_at == 5,
          "start after end", cs.error);
    hv_codestream_close(&cs);
    check(hv_codestream_open(&cs, p, 0, b.n, (hv_read_mode)-1) != 0 &&
          strcmp(cs.error, "invalid codestream read mode") == 0,
          "invalid read mode", cs.error);
    hv_codestream_close(&cs);
    check(hv_codestream_open(&cs, p, 0, b.n, HV_READ_PADDED) == 0,
          "padded validation", cs.error);
    hv_codestream_close(&cs);
    fail_at = 0;                        /* the components */
    check(hv_codestream_open(&cs, p, 0, b.n, HV_READ_VALIDATE) != 0 && strcmp(cs.error, "out of memory") == 0 &&
          cs.error_at == 2,
          "open out of memory", cs.error);
    fail_at = -1;
    hv_codestream_close(&cs);
    check(cs.error != NULL && hv_codestream_siz(&cs) == NULL, "close keeps the error", NULL);
    fail_at = 1;                        /* cross-segment validation state */
    check(hv_codestream_open(&cs, p, 0, b.n, HV_READ_VALIDATE) != 0 &&
          hv_codestream_siz(&cs) == NULL, "failed open exposes no SIZ", cs.error);
    fail_at = -1;
    hv_codestream_close(&cs);
    free(p);

    /* The tile-part counts of 300 tiles, in two pages (256 and 44): tiles
     * 299, 0, 256 and 299 again; the second of 299 missing; Isot 300; a
     * TNsot given late, reached or not. Out of memory for the pages (at
     * the first tile-part, and the second's page) and none for a page
     * already there. */
    {
        static const struct {
            const char *what;
            int n;
            unsigned parts[4][3];       /* Isot, TPsot, TNsot */
            const char *error;
            int at;                     /* the tile-part it fails at, or -1: EOC */
        } cases[] = {
            {"tiles in two pages", 4, {{299, 0, 2}, {0, 0, 1}, {256, 0, 0}, {299, 1, 2}}, "codestream.tile-count", -1},
            {"a tile short of its TNsot", 3, {{299, 0, 2}, {0, 0, 1}, {256, 0, 0}},
             "sot.tnsot-count", -1},
            {"Isot past the tiles", 2, {{299, 0, 0}, {300, 0, 0}}, "sot.isot-range", 1},
            {"TNsot late, not reached", 2, {{299, 0, 0}, {299, 1, 3}}, "sot.tnsot-count", -1},
            {"TNsot late, reached", 3, {{299, 0, 0}, {299, 1, 3}, {299, 2, 0}}, "codestream.tile-count", -1},
        };
        uint32_t f[8] = {300, 1, 0, 0, 1, 1, 0, 0};
        size_t starts[4];
        int k, j;
        for (k = 0; k < (int)(sizeof cases / sizeof *cases); k++) {
            clear(&b);
            u16(&b, HV_SOC);
            siz(&b, f, 1, NULL, NULL);
            cod(&b);
            qcd(&b);
            for (j = 0; j < cases[k].n; j++) {
                starts[j] = b.n;
                tile_part(&b, cases[k].parts[j][0], cases[k].parts[j][1], cases[k].parts[j][2], 1);
            }
            u16(&b, HV_EOC);
            ncallocs = 0;
            nfrees = 0;
            error = cs_check(&b, 0, &at);
            check(nfrees == (k < 2 ? 5 : 4),
                  "close frees components, state, directory and each tile page", NULL);
            nfrees = -1;
            /* the component's state (hv_segments), the directory of 2
             * pages, 299's page of 44 tiles, 0's of 256 */
            if (k == 0)
                check(ncallocs == 4 && callocs[0] == 1 && callocs[1] == 2 && callocs[2] == 44 &&
                      callocs[3] == 256, "the tile pages' sizes", NULL);
            ncallocs = -1;
            if (cases[k].error == NULL)
                check(error == NULL, cases[k].what, error);
            else
                expect(cases[k].what, error, at, cases[k].error,
                       cases[k].at < 0 ? b.n - 2 : starts[cases[k].at]);
            /* Allocations: the components (0), their state (1, at SIZ),
             * the pages of the first tile-part (2, 3), tile 0's page (4),
             * none for tile 256. */
            if (k == 0) {
                p = exact(&b);
                for (j = 1; j <= 5; j++) {
                    fail_at = j;
                    nfrees = 0;
                    at = (size_t)-1;
                    error = hv_codestream_check(p, 0, b.n, HV_READ_VALIDATE, &at);
                    fail_at = -1;
                    check(nfrees == j, "allocation failure frees every earlier allocation", NULL);
                    nfrees = -1;
                    if (j == 5)
                        expect("no page to allocate, incomplete tile grid", error, at,
                               "codestream.tile-count", b.n - 2);
                    else
                        expect(j == 1 ? "the components' state out of memory"
                                      : "a tile page out of memory", error, at, "out of memory",
                               j == 1 ? 2 : starts[j < 4 ? 0 : 1]);
                }
                free(p);
            }
        }
        /* 256 tiles: one page, all of them. */
        f[0] = 256;
        clear(&b);
        u16(&b, HV_SOC);
        siz(&b, f, 1, NULL, NULL);
        cod(&b);
        qcd(&b);
        for (j = 0; j < 256; j++)
            tile_part(&b, (unsigned)j, 0, 1, 1);
        u16(&b, HV_EOC);
        ncallocs = 0;
        error = cs_check(&b, 0, &at);
        check(error == NULL && ncallocs == 3 && callocs[0] == 1 && callocs[1] == 1 &&
              callocs[2] == 256, "the tile page of 256 tiles", error);
        ncallocs = -1;
    }

    /* SIZ: Lsiz one past the end, exactly to it; components that are not
     * whole, and too many for Csiz. */
    b.d[5] = (uint8_t)(b.n - 4 + 1);
    b.d[4] = 0;
    {
        bytes t = {NULL, 0, 0};
        put(&t, b.d, 4 + 41);
        t.d[5] = 42;
        error = cs_check(&t, 0, &at);
        expect("Lsiz past the end", error, at, "truncated SIZ", 2);
        t.d[5] = 41;
        error = cs_check(&t, 0, &at);
        expect("Lsiz to the end", error, at, "expected a marker", 45);
        t.d[5] = 40;
        error = cs_check(&t, 0, &at);
        expect("a component of 2 bytes", error, at, "invalid SIZ", 2);
        release(&t);
    }
    release(&b);
    {
        uint32_t f[8] = {4, 4, 0, 0, 4, 4, 0, 0};
        uint8_t xr[3] = {1, 1, 0}, yr[3] = {1, 1, 1};
        u16(&b, HV_SOC);
        siz(&b, f, 3, xr, yr);
        error = cs_check(&b, 0, &at);
        expect("XRsiz 0", error, at, "invalid SIZ", 2);
        clear(&b);
        u16(&b, HV_SOC);
        siz(&b, f, 0, NULL, NULL);
        error = cs_check(&b, 0, &at);
        expect("no component", error, at, "invalid SIZ", 2);
    }

    /* The profile's SIZ: 2^31 wide; sub-sampled component 0 or 2. */
    {
        uint32_t f[8] = {2147483648u, 4, 0, 0, 2147483648u, 4, 0, 0};
        uint8_t xr[3] = {1, 1, 1}, yr[3] = {1, 1, 1};
        clear(&b);
        u16(&b, HV_SOC);
        siz(&b, f, 1, NULL, NULL);
        error = cs_check(&b, HV_READ_PACKETS_JPIP, &at);
        expect("2^31 wide", error, at, "SIZ: Xsiz or Ysiz above 2,147,483,647 (Siz-Profile)", 2);
        f[0] = f[4] = 4;
        for (i = 0; i < 3; i += 2) {
            xr[i] = 2;
            clear(&b);
            u16(&b, HV_SOC);
            siz(&b, f, 3, xr, yr);
            error = cs_check(&b, HV_READ_PACKETS_JPIP, &at);
            expect("XRsiz 2", error, at, "siz.component-sampling", 2);
            xr[i] = 1;
        }
        clear(&b);
        u16(&b, HV_SOC);
        u16(&b, HV_SIZ);
        error = cs_check(&b, 0, &at);
        expect("SIZ without Lsiz", error, at, "truncated SIZ", 2);
        clear(&b);
        u16(&b, HV_SOC);
        error = cs_check(&b, 0, &at);
        expect("SOC alone", error, at, "SOC is not followed by SIZ", 2);
        clear(&b);
        u8(&b, 0xFF);
        error = cs_check(&b, 0, &at);
        expect("half a SOC", error, at, "codestream does not start with SOC", 0);
    }

    /* Segments: one to the end of the codestream; one past it; the codes
     * without a segment at both ends of their range. */
    for (i = 0; i < 4; i++) {
        size_t m;
        clear(&b);
        main_header(&b, 4, 4);
        m = b.n;
        if (i < 2) {
            u16(&b, 0xFF70);
            u16(&b, 4);
            u16(&b, 0);
            if (i == 1)
                b.d[m + 3] = 5;
            error = cs_check(&b, 0, &at);
            expect(i ? "a segment past the end" : "a segment to the end", error, at,
                   i ? "marker segment overruns its header" : "expected a marker",
                   i ? m : b.n);
        } else {
            u16(&b, i == 2 ? 0xFF30 : 0xFF3F);
            tile_part(&b, 0, 0, 1, 3);
            u16(&b, HV_EOC);
            error = cs_check(&b, 0, &at);
            expect(i == 2 ? "0xFF30" : "0xFF3F", error, at, NULL, 0);
            p = exact(&b);
            compare_items("0xFF30 to 0xFF3F", p, 0, b.n, 0);
            free(p);
        }
    }

    /* SOT: cut; Psot 0 with and without EOC; Psot to EOC and past it. */
    {
        size_t t;
        clear(&b);
        main_header(&b, 4, 4);
        t = sot(&b, 0, 0, 1);
        b.n -= 1;
        error = cs_check(&b, 0, &at);
        expect("a SOT cut", error, at, "invalid SOT", t);
        clear(&b);
        main_header(&b, 4, 4);
        t = sot(&b, 0, 0, 1);
        u16(&b, HV_EOC);
        error = cs_check(&b, 0, &at);
        expect("Psot 0, EOC right after SOT", error, at,
               "Psot = 0 but the codestream does not end with EOC", t);
        b.n -= 2;
        u16(&b, HV_SOD);
        u16(&b, HV_EOC);
        error = cs_check(&b, 0, &at);
        expect("Psot 0, no data", error, at, NULL, 0);
        error = cs_check(&b, HV_READ_JPIP, &at);
        expect("Psot 0, no data, no PLT", error, at, "codestream.no-plt", t);
        clear(&b);
        main_header(&b, 4, 4);
        t = sot(&b, 0, 0, 1);
        {
            uint64_t v = 3;
            plt(&b, 0, &v, 1);
        }
        u16(&b, HV_SOD);
        put(&b, "\x5A\x5A\x5A", 3);
        u16(&b, HV_EOC);
        error = cs_check(&b, HV_READ_JPIP, &at);
        expect("Psot 0", error, at, NULL, 0);
        p = exact(&b);
        compare_items("Psot 0", p, 0, b.n, 0);
        check_prefixes("Psot 0", p, 0, b.n);
        free(p);
        b.d[b.n - 1] = 0xD8;
        error = cs_check(&b, 0, &at);
        expect("Psot 0 without EOC", error, at,
               "Psot = 0 but the codestream does not end with EOC", t);
        b.d[b.n - 1] = 0xD9;
        set32(&b, t + 6, (uint32_t)(b.n - 2 - t));
        error = cs_check(&b, 0, &at);
        expect("Psot to EOC", error, at, NULL, 0);
        set32(&b, t + 6, (uint32_t)(b.n - 1 - t));
        error = cs_check(&b, 0, &at);
        expect("Psot into EOC", error, at, "tile-part overruns the codestream", t);
    }

    /* COM: Rcom alone, and with text; in a tile-part header. */
    for (i = 0; i < 4; i++) {
        size_t m, t = 0;
        clear(&b);
        main_header(&b, 4, 4);
        m = b.n;
        if (i >= 2)
            t = sot(&b, 0, 0, 1);
        seg(&b, HV_COM, "\x00\x01xy", i % 2 ? 4 : 2);
        if (i >= 2) {
            uint64_t v = 3;
            plt(&b, 0, &v, 1);
            u16(&b, HV_SOD);
            put(&b, "\x5A\x5A\x5A", 3);
            sot_end(&b, t);
        } else {
            tile_part(&b, 0, 0, 1, 3);
        }
        u16(&b, HV_EOC);
        error = cs_check(&b, 0, &at);
        expect(i % 2 ? "a COM" : "a COM without text", error, at,
               i % 2 ? NULL : "invalid COM", i % 2 ? 0 : i >= 2 ? t + 12 : m);
        if (i % 2) {
            p = exact(&b);
            compare_items("a COM", p, 0, b.n, 0);
            free(p);
        }
    }

    /* Main and tile-part COD and QCD: bodies that do not decode, and a
     * second in a tile-part header. */
    for (i = 0; i < 6; i++) {
        size_t m, t;
        clear(&b);
        main_header(&b, 4, 4);
        m = b.n;
        if (i == 0)
            seg(&b, HV_COD, "\x00\x00\x00", 3);
        if (i == 1)
            seg(&b, HV_QCD, NULL, 0);
        t = sot(&b, 0, 0, 1);
        if (i == 2)
            seg(&b, HV_COD, "\x00\x00\x00", 3);
        if (i == 3)
            seg(&b, HV_QCD, NULL, 0);
        if (i == 4) {
            cod(&b);
            cod(&b);
        }
        if (i == 5) {
            qcd(&b);
            qcd(&b);
        }
        {
            uint64_t v = 3;
            plt(&b, 0, &v, 1);
        }
        u16(&b, HV_SOD);
        put(&b, "\x5A\x5A\x5A", 3);
        sot_end(&b, t);
        u16(&b, HV_EOC);
        error = cs_check(&b, 0, &at);
        {
            static const char *const want[] = {"codestream.one-cod-before-sot",
                                               "codestream.one-qcd-before-sot", "invalid COD",
                                               "invalid QCD", "tile.cod-once", "tile.qcd-once"};
            size_t where = i < 2 ? m : i < 4 ? t + 12 : t + 12 + (i == 4 ? 14 : 6);
            expect(want[i], error, at, want[i], where);
        }
    }

    /* PLT: Zplt alone; an entry cut; out of sequence. */
    for (i = 0; i < 3; i++) {
        size_t t;
        clear(&b);
        main_header(&b, 4, 4);
        t = sot(&b, 0, 0, 1);
        if (i == 0)
            seg(&b, HV_PLT, "\x00", 1);
        if (i == 1)
            seg(&b, HV_PLT, "\x00\x83", 2);
        if (i == 2) {
            uint64_t v = 3;
            plt(&b, 1, &v, 1);
        }
        u16(&b, HV_SOD);
        put(&b, "\x5A\x5A\x5A", 3);
        sot_end(&b, t);
        u16(&b, HV_EOC);
        error = cs_check(&b, 0, &at);
        /* A.7.3 concatenates increasing Zplt; a missing 0 is allowed. */
        expect(i == 2 ? "Zplt 1 first" : "a PLT without a whole entry", error, at,
               i == 2 ? NULL : "invalid PLT", t + 12);
    }

    /* The packet count, where the main COD lays the packets out and every
     * tile-part lists them: one entry more than the packets (1 per
     * component). Not with a COC or POC in the main header, a COD, COC or
     * POC in a tile-part header, an image origin (the tile origin is
     * within it), a tile smaller than the image, or a sub-sampled
     * component. The cases: 0 counted; 1, 2 main COC, POC; 3, 4, 5 tile
     * COD, COC, POC; 6, 7 XOsiz, YOsiz; 8, 9 XTsiz, YTsiz; 10 counted, 2
     * components; 11, 12, 13 XRsiz of the first, YRsiz of the second, both
     * of the second. */
    for (i = 0; i < 14; i++) {
        uint32_t f[8] = {4, 4, 0, 0, 4, 4, 0, 0};
        uint8_t xr[2] = {1, 1}, yr[2] = {1, 1};
        uint64_t v[3] = {1, 1, 1};
        int ncomps = i >= 10 ? 2 : 1, listed = i == 0 || i == 10;
        size_t t;
        clear(&b);
        if (i == 6 || i == 7) {
            f[i - 4] = 1;               /* XOsiz or YOsiz */
            f[0] = f[1] = f[4] = f[5] = 5;
        }
        if (i == 8 || i == 9)
            f[i - 4] = 3;               /* XTsiz or YTsiz: 2 x 1 or 1 x 2 tiles */
        if (i == 11)
            xr[0] = 2;
        if (i == 12 || i == 13)
            yr[1] = 2;
        if (i == 13)
            xr[1] = 2;
        u16(&b, HV_SOC);
        siz(&b, f, ncomps, xr, yr);
        cod(&b);
        qcd(&b);
        if (i == 1)
            seg(&b, HV_COC, "\x00\x00\x00\x04\x04\x00\x01", 7);
        if (i == 2)
            seg(&b, HV_POC, "\x00\x00\x00\x01\x01\x00\x01", 7);
        t = sot(&b, 0, 0, 1);
        if (i == 3)
            cod(&b);
        if (i == 4)
            seg(&b, HV_COC, "\x00\x00\x00\x04\x04\x00\x01", 7);
        if (i == 5)
            seg(&b, HV_POC, "\x00\x00\x00\x01\x01\x00\x01", 7);
        plt(&b, 0, v, ncomps + 1);
        u16(&b, HV_SOD);
        put(&b, "\x5A\x5A\x5A", (size_t)ncomps + 1);
        sot_end(&b, t);
        if (i == 8 || i == 9) {
            /* B.11 requires the second tile too; this case concerns the
             * deliberately unchecked packet count of a multi-tile image. */
            t = sot(&b, 1, 0, 1);
            plt(&b, 0, v, ncomps + 1);
            u16(&b, HV_SOD);
            put(&b, "\x5A\x5A\x5A", (size_t)ncomps + 1);
            sot_end(&b, t);
        }
        u16(&b, HV_EOC);
        error = cs_check(&b, 0, &at);
        if (listed)
            expect("one entry more than the packets", error, at, "plt.packet-count", b.n - 2);
        else
            check(error == NULL, "one entry more than the packets, not counted", error);
    }

    /* The first marker other than SOC; SOP and EPH in the main header; a
     * COM with 1 byte of Rcom; a PLT without Zplt; Zplt 1 with an entry
     * cut; a main COD and QCD that do not decode. */
    {
        static const struct {
            const char *what;
            int where;          /* 0 main header, 1 tile-part header */
            const char *seg;
            size_t n;
            const char *want;
        } bad[] = {
            {"SOP in the main header", 0, "\xFF\x91", 2, "main.marker-code"},
            {"EPH in the main header", 0, "\xFF\x92", 2, "main.marker-code"},
            {"a COM of Lcom 3", 0, "\xFF\x64\x00\x03\x00", 5, "invalid COM"},
            {"a PLT of Lplt 2", 1, "\xFF\x58\x00\x02", 4, "invalid PLT"},
            {"Zplt 1 and an entry cut", 1, "\xFF\x58\x00\x04\x01\x83", 6, "invalid PLT"},
            {"a tile COD of Lcod 5", 1, "\xFF\x52\x00\x05\x00\x00\x00", 7, "invalid COD"},
            {"a tile QCD of Lqcd 3", 1, "\xFF\x5C\x00\x03\x40", 5, "invalid QCD"},
        };
        for (i = 0; i < sizeof bad / sizeof *bad; i++) {
            size_t t = 0, m;
            clear(&b);
            main_header(&b, 4, 4);
            m = b.n;
            if (bad[i].where == 0)
                put(&b, bad[i].seg, bad[i].n);
            t = sot(&b, 0, 0, 1);
            if (bad[i].where == 1) {
                m = b.n;
                put(&b, bad[i].seg, bad[i].n);
            }
            {
                uint64_t v = 3;
                plt(&b, 0, &v, 1);
            }
            u16(&b, HV_SOD);
            put(&b, "\x5A\x5A\x5A", 3);
            sot_end(&b, t);
            u16(&b, HV_EOC);
            error = cs_check(&b, 0, &at);
            expect(bad[i].what, error, at, bad[i].want, m);
        }
        for (i = 0; i < 2; i++) {
            uint32_t f[8] = {4, 4, 0, 0, 4, 4, 0, 0};
            clear(&b);
            u16(&b, HV_SOC);
            siz(&b, f, 1, NULL, NULL);
            at = b.n;
            if (i == 0)
                seg(&b, HV_COD, "\x00\x00\x00", 3);
            else
                seg(&b, HV_QCD, "\x40", 1);
            u16(&b, HV_EOC);
            {
                size_t m = at;
                error = cs_check(&b, 0, &at);
                expect(i ? "a main QCD of Lqcd 3" : "a main COD of Lcod 5", error, at,
                       i ? "invalid QCD" : "invalid COD", m);
            }
        }
        clear(&b);
        u16(&b, HV_SIZ);
        error = cs_check(&b, 0, &at);
        expect("SIZ first", error, at, "codestream does not start with SOC", 0);
        clear(&b);
        main_header(&b, 4, 4);
        at = b.n;
        put(&b, "\xFF\x64\x00\x03\x00", 5);
        {
            size_t m = at;
            error = cs_check(&b, 0, &at);
            expect("a COM of Lcom 3, last", error, at, "invalid COM", m);
        }
    }

    /* SIZ: 28 bytes of SizFixed, last; a component and 2 bytes. */
    {
        uint32_t f[8] = {4, 4, 0, 0, 4, 4, 0, 0};
        clear(&b);
        u16(&b, HV_SOC);
        siz(&b, f, 1, NULL, NULL);
        b.d[5] = 30;
        b.n = 4 + 30;
        error = cs_check(&b, 0, &at);
        expect("a SIZ of 28 bytes, last", error, at, "invalid SIZ", 2);
        clear(&b);
        u16(&b, HV_SOC);
        siz(&b, f, 1, NULL, NULL);
        b.d[5] += 2;
        u16(&b, 0);
        cod(&b);
        qcd(&b);
        tile_part(&b, 0, 0, 1, 3);
        u16(&b, HV_EOC);
        error = cs_check(&b, 0, &at);
        expect("a SIZ of a component and 2 bytes", error, at, "invalid SIZ", 2);
    }

    /* hv_plt_next: an entry cut at the end it is given, and the bytes
     * after it. */
    {
        static const uint8_t e[] = {0x05, 0x83, 0x01};
        size_t pos = 1;
        uint64_t v;
        uint8_t *q = malloc(3);
        memcpy(q, e, 3);
        check(hv_plt_next(q, &pos, 2, &v) == -1 && pos == 1, "an entry cut", NULL);
        check(hv_plt_next(q, &pos, 3, &v) == 1 && pos == 3 && v == 385 &&
              hv_plt_next(q, &pos, 3, &v) == 0, "an entry of 2 bytes", NULL);
        free(q);
    }

    /* Zero PLT entries: the padding of each tile-part, with
     * HV_READ_PADDED. Two tile-parts of one tile, each a packet and
     * a zero entry; and the items of one with tile-part COD, COC, QCD and
     * COM. */
    {
        uint64_t v[2] = {3, 0};
        size_t t, first = 0;
        clear(&b);
        main_header(&b, 4, 4);
        for (i = 0; i < 2; i++) {
            t = sot(&b, 0, (unsigned)i, 2);
            if (i == 0)
                first = t;
            plt(&b, 0, v, 2);
            u16(&b, HV_SOD);
            put(&b, "\x5A\x5A\x5A", 3);
            sot_end(&b, t);
        }
        u16(&b, HV_EOC);
        error = cs_check(&b, HV_READ_PADDED, &at);
        expect("padding in each tile-part", error, at, NULL, 0);
        error = cs_check(&b, 0, &at);
        expect("padding without the flag", error, at, "plt.zero-length", first);
        p = exact(&b);
        compare_items("padding in each tile-part", p, 0, b.n, HV_READ_PADDED);
        free(p);
        clear(&b);
        main_header(&b, 4, 4);
        t = sot(&b, 0, 0, 1);
        cod(&b);
        seg(&b, HV_COC, "\x00\x00\x00\x04\x04\x00\x01", 7);
        qcd(&b);
        seg(&b, HV_COM, "\x00\x01xy", 4);
        plt(&b, 0, v, 1);
        u16(&b, HV_SOD);
        put(&b, "\x5A\x5A\x5A", 3);
        sot_end(&b, t);
        u16(&b, HV_EOC);
        p = exact(&b);
        compare_items("tile-part COD, COC, QCD and COM", p, 0, b.n, 0);
        check_prefixes("tile-part COD, COC, QCD and COM", p, 0, b.n);
        free(p);
    }

    /* Two tile-parts: SOT and EOC in a header; a segment after the data. */
    clear(&b);
    main_header(&b, 4, 4);
    {
        size_t t = sot(&b, 0, 0, 1);
        u16(&b, HV_EOC);
        sot_end(&b, t);
        u16(&b, HV_EOC);
        error = cs_check(&b, 0, &at);
        expect("EOC in a tile-part header", error, at, "tile-part header without SOD", t + 12);
    }
    clear(&b);
    main_header(&b, 4, 4);
    tile_part(&b, 0, 0, 1, 3);
    at = b.n;
    qcd(&b);
    u16(&b, HV_EOC);
    {
        size_t q = at;
        error = cs_check(&b, 0, &at);
        expect("QCD after the data", error, at, "codestream.segment-after-sot", q);
    }
    release(&b);

    /* The accessors after close, and after the end. */
    b = simple();
    p = exact(&b);
    {
        hv_item item;
        check(hv_codestream_open(&cs, p, 0, b.n, HV_READ_VALIDATE) == 0, "open", cs.error);
        while (hv_codestream_next(&cs, &item) == 1)
            ;
        check(hv_codestream_next(&cs, &item) == 0 && item.kind == 0 && item.end == 0,
              "after the end", NULL);
        check(hv_codestream_siz(&cs) != NULL && hv_codestream_cod(&cs) != NULL &&
              hv_codestream_qcd(&cs) != NULL, "the accessors", NULL);
        hv_codestream_close(&cs);
        check(hv_codestream_siz(&cs) == NULL && hv_codestream_cod(&cs) == NULL &&
              hv_codestream_qcd(&cs) == NULL && cs.error == NULL, "the accessors after close",
              NULL);
        check(hv_codestream_next(&cs, &item) == 0, "next after close", NULL);
        check(hv_codestream_open(&cs, p, 0, 3, HV_READ_VALIDATE) != 0, "open 3 bytes", NULL);
        hv_codestream_close(&cs);
        check(hv_codestream_next(&cs, &item) == -1 && cs.error_at == 2 &&
              strcmp(cs.error, "SOC is not followed by SIZ") == 0, "next after a failed open",
              cs.error);
        check(hv_codestream_open(&cs, p, 0, b.n, HV_READ_VALIDATE) == 0 && cs.error == NULL &&
              cs.error_at == 0 && hv_codestream_siz(&cs) != NULL &&
              hv_codestream_cod(&cs) == NULL && hv_codestream_qcd(&cs) == NULL,
              "open after closing a failed open", cs.error);
        hv_codestream_close(&cs);
    }
    free(p);
    release(&b);
}

/* The corpus: every codestream that reads through, its items against the
 * walk, and every prefix of the bases, as files and codestreams. */
static void check_corpus_items(void) {
    static const char *const names[] = {"jp2.jp2", "jp2-precincts.jp2", "jpx-embedded.jpx",
                                        "jpx-linked.jpx", "jpx-linked-frame1.jp2"};
    size_t i, n, at;
    for (i = 0; i < sizeof names / sizeof *names; i++) {
        bytes f = load(names[i]);
        hv_boxes it;
        hv_box x;
        const char *error;
        hv_boxes_file(&it, f.d, f.n);
        while (hv_boxes_next(&it, &x, &error, &at) == 1)
            if (x.type == HV_BOX_JP2C) {
                compare_items(names[i], f.d, x.payload, x.end, 0);
                check_prefixes(names[i], f.d, x.payload, x.end);
            }
        for (n = 0; n < f.n; n++) {
            bytes t = {NULL, 0, 0};
            put(&t, f.d, n);
            jp2(&t, &at);
            jpx(&t, &at);
            jp2h(&t, &at);
            jpx_headers(&t, &at);
            release(&t);
        }
        release(&f);
    }
}

/* Preserve diagnostic locations, not just the rule names. */
static void check_header_diagnostics(void) {
    static const struct {
        const char *file, *rule, *box;
    } cases[] = {
        {"jpx-embedded-header-file.ftyp-minor-73.jpx", "file.ftyp-minor", "ftyp"},
        {"jpx-embedded-header-jpx.ipr-93.jpx", "jpx.ipr", "jpch"},
        {"jpx-embedded-header-jpxb.header-override-162.jpx", "jpxb.header-override", NULL},
        {"jpx-embedded-header-jpxb.colour-163.jpx", "jpxb.colour", NULL},
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        bytes b = load(cases[i].file);
        size_t want = b.n, at;
        const char *error;
        if (cases[i].box != NULL) {
            for (want = 0; want + 8 <= b.n; want += get32(b.d + want))
                if (memcmp(b.d + want + 4, cases[i].box, 4) == 0) break;
            check(want < b.n, "diagnostic fixture has expected box", cases[i].file);
        }
        error = jpx_headers(&b, &at);
        expect(cases[i].file, error, at, cases[i].rule, want);
        release(&b);
    }
    {
        /* A reserved depth immediately outside BPCC must not be decoded.
         * This catches a one-byte overread even without ASan. */
        const uint8_t data[] = {7, 0x30};
        hv_box box = {0};
        hv_header h;
        size_t at = 0;
        const char *error;
        box.type = HV_BOX_BPCC; box.end = 1;
        hv_header_init(&h, HV_BOX_JPCH, 1);
        check(hv_rule_header_child(&h, HV_BOX_BPCC) == NULL, "BPCC child accepted", NULL);
        error = read_header_child(data, &box, &h, 0, &at);
        check(error == NULL && h.bpcc_count == 1, "BPCC stops at its box boundary", error);
    }
}

static void check_plt_cursor(void) {
    static const struct {
        const char *bytes;
        size_t size;
        unsigned z;
        const char *error;
    } cases[] = {
        {"\x03\x00", 2, 0, NULL},
        {"\x00\x03", 2, 0, "plt.padding-position"},
        {"\x00\x03\x80", 3, 0, "invalid PLT"},
        {"\x03", 1, 1, "plt.zplt-sequence"},
        {"\x00\x03", 2, 1, "plt.zplt-sequence"},
        {"\x00\x03\x80", 3, 1, "invalid PLT"},
        {"\x82\x80\x80\x80\x80\x80\x80\x80\x80\x00", 10, 0, "plt.value-overflow"},
        {"\x82\x80\x80\x80\x80\x80\x80\x80\x80\x00\x80", 11, 0, "invalid PLT"},
        {"\x82\x80\x80\x80\x80\x80\x80\x80\x80\x00", 10, 1, "plt.zplt-sequence"},
        {"\x82\x80\x80\x80\x80\x80\x80\x80\x80\x80", 10, 0, "invalid PLT"},
        {"\x82\x80\x80\x80\x80\x80\x80\x80\x80\x00\x01", 11, 0, "plt.value-overflow"},
    };
    hv_plt_reader a, b;
    uint64_t value;
    size_t i, at;
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        hv_plt range = {cases[i].z, 0, cases[i].size};
        uint8_t *input = malloc(cases[i].size);
        if (input == NULL) abort();
        memcpy(input, cases[i].bytes, cases[i].size);
        hv_plt_init(&a, HV_PLT_PROFILE);
        check(hv_plt_begin(&a, &range) == NULL,
              "cursor begins without reading entries", NULL);
        while (hv_plt_read(&a, input, &value) == 1) ;
        check((a.error == NULL && cases[i].error == NULL) ||
              (a.error != NULL && cases[i].error != NULL && strcmp(a.error, cases[i].error) == 0),
              "PLT diagnostic precedence", a.error);
        if (a.error != NULL)
            check(hv_plt_read(&a, input, &value) == -1,
                  "PLT error is terminal", NULL);
        free(input);
    }
    {
        const uint8_t entries[] = {3, 0};
        hv_plt range = {0, 0, sizeof entries};
        hv_plt_init(&a, HV_PLT_PROFILE);
        hv_plt_init(&b, HV_PLT_PROFILE);
        hv_plt_begin(&a, &range);
        hv_plt_begin(&b, &range);
        check(hv_plt_read(&a, entries, &value) == 1 && value == 3 && a.pos == 1 && b.pos == 0,
              "pause leaves the suffix unread and another cursor untouched", NULL);
        check(hv_plt_read(&b, entries, &value) == 1 && value == 3, "independent cursor", NULL);
        check(hv_plt_end_tile(&b, 3) != NULL, "cannot complete an unread suffix", NULL);
        check(hv_plt_read(&a, entries, &value) == 1 && value == 0 &&
              hv_plt_read(&a, entries, &value) == 0 &&
              hv_plt_end_tile(&a, 3) == NULL, "resume and complete tile", a.error);
        range.end = 1;
        hv_plt_begin(&a, &range);
        check(hv_plt_read(&a, entries, &value) == -1 && strcmp(a.error, "plt.padding-position") == 0,
              "padding state survives tile-part boundaries", a.error);
        hv_plt_init(&a, HV_PLT_PROFILE);
        hv_plt_begin(&a, &range);
        hv_plt_read(&a, entries, &value);
        check(hv_plt_end_tile(&a, 4) != NULL && strcmp(a.error, "plt.coverage") == 0,
              "deferred coverage", a.error);
    }
    {
        /* Resume mid-segment after the original buffer has been released.
         * Allocate its replacement first, guaranteeing a different address. */
        const uint8_t file[] = {0xaa, 0xbb, 3, 0x81, 1, 0};
        uint8_t *original = malloc(sizeof file), *replacement = malloc(sizeof file);
        hv_plt range = {0, 2, sizeof file};
        check(original != NULL && replacement != NULL, "remapped PLT buffers", NULL);
        if (original != NULL && replacement != NULL) {
            memcpy(original, file, sizeof file);
            memcpy(replacement, file, sizeof file);
            hv_plt_init(&a, HV_PLT_PROFILE);
            check(hv_plt_begin(&a, &range) == NULL &&
                  hv_plt_read(&a, original, &value) == 1 && value == 3 && a.pos == 3,
                  "read first packet before remapping", a.error);
            free(original);
            original = NULL;
            check(hv_plt_read(&a, replacement, &value) == 1 && value == 129 && a.pos == 5,
                  "resume on replacement buffer at saved offset", a.error);
            check(hv_plt_read(&a, replacement, &value) == 1 && value == 0 &&
                  hv_plt_read(&a, replacement, &value) == 0 &&
                  hv_plt_end_tile(&a, 132) == NULL,
                  "remapping preserves packet sum and padding validation", a.error);
        }
        free(original);
        free(replacement);
    }
    {
        bytes stream = simple();
        hv_codestream cs;
        hv_item item;
        hv_plt saved = {0, 0, 0};
        size_t data_size = 0;
        int status = hv_codestream_open(&cs, stream.d, 0, stream.n, HV_READ_JPIP_INDEX);
        while (status == 0 && (status = hv_codestream_next(&cs, &item)) == 1) {
            if (item.plt) saved = *item.plt;
            if (item.kind == HV_TILE_DATA) data_size = item.end - item.start;
            status = 0;
        }
        check(status == 0 && saved.end > saved.start && cs.plt_reader.count.packets == 0,
              "structural walk does not consume PLT", cs.error);
        hv_plt_init(&a, HV_PLT_PROFILE);
        hv_plt_begin(&a, &saved);
        while (hv_plt_read(&a, stream.d, &value) == 1) ;
        check(hv_plt_end_tile(&a, data_size) == NULL &&
              hv_plt_end(&a, hv_rule_packets(hv_codestream_siz(&cs),
                  &hv_codestream_cod(&cs)->sgcod, &hv_codestream_cod(&cs)->spcod)) == NULL,
              "consume saved ranges after structural completion", a.error);
        hv_plt_init(&a, HV_PLT_PROFILE);
        check(hv_plt_end(&a, hv_rule_packets(hv_codestream_siz(&cs),
                  &hv_codestream_cod(&cs)->sgcod, &hv_codestream_cod(&cs)->spcod)) != NULL,
              "packet count catches an unconsumed codestream", a.error);
        hv_codestream_close(&cs);
        stream.d[saved.end - 1] = 0x80;
        status = hv_codestream_open(&cs, stream.d, 0, stream.n, HV_READ_JPIP_INDEX);
        if (status == 0) while ((status = hv_codestream_next(&cs, &item)) == 1) ;
        check(status == 0, "malformed PLT is genuinely deferred", cs.error);
        hv_codestream_close(&cs);
        check(hv_codestream_check(stream.d, 0, stream.n, HV_READ_JPIP, &at) != NULL,
              "eager validation still rejects malformed PLT", NULL);
        check(hv_codestream_check(stream.d, 0, stream.n, HV_READ_JPIP_INDEX, &at) != NULL,
              "full validator refuses deferred flag", NULL);
        check(hv_codestream_open(&cs, stream.d, 0, stream.n, (hv_read_mode)-1) != 0,
              "deferred mode requires served profile", NULL);
        hv_codestream_close(&cs);
        release(&stream);
    }
}

/* ------------------------------------------------------------------------
 * Rules no vector reaches
 * ------------------------------------------------------------------------ */

/* A segment of a case: its code and body. */
typedef struct {
    unsigned code;
    const char *body;
    size_t n;
} gap_segment;

#define GAP_END {0, NULL, 0}
#define GAP(code, body) {code, body, sizeof body - 1}

/* A codestream of one case, and its expected error and offset. */
typedef struct {
    const char *what;
    unsigned rsiz;
    int ncomps;
    uint32_t w, h;
    uint8_t xrsiz;              /* every component's; 0 for 1 */
    int packets;                /* 0 for 1 */
    gap_segment main[2], tile[2];
    const char *error;
    size_t at;
} gap_case;

/* SOC, SIZ of w x h in one tile, with ncomps components and Rsiz rsiz,
 * COD, QCD, the main-header segments; one tile-part (SOT, its header
 * segments, a PLT of `packets` packets of one byte, SOD, the packets); EOC.
 * The main header of one component ends at 65, where the main-header
 * segments start. */
static bytes gap_stream(const gap_case *c) {
    bytes b = {NULL, 0, 0};
    uint32_t f[8];
    uint8_t xr[257];
    uint64_t v[257];
    int packets = c->packets ? c->packets : 1, i;
    const gap_segment *g;
    size_t at;
    f[0] = f[4] = c->w;
    f[1] = f[5] = c->h;
    f[2] = f[3] = f[6] = f[7] = 0;
    memset(xr, c->xrsiz ? c->xrsiz : 1, sizeof xr);
    u16(&b, HV_SOC);
    siz(&b, f, c->ncomps, xr, NULL);
    b.d[6] = (uint8_t)(c->rsiz >> 8);
    b.d[7] = (uint8_t)c->rsiz;
    cod(&b);
    qcd(&b);
    for (g = c->main; g->code != 0; g++)
        seg(&b, g->code, g->body, g->n);
    at = sot(&b, 0, 0, 1);
    for (g = c->tile; g->code != 0; g++)
        seg(&b, g->code, g->body, g->n);
    for (i = 0; i < packets; i++)
        v[i] = 1;
    plt(&b, 0, v, packets);
    u16(&b, HV_SOD);
    for (i = 0; i < packets; i++)
        u8(&b, 0x5A);
    sot_end(&b, at);
    u16(&b, HV_EOC);
    return b;
}

/* The rules of hv_segments, and of the main header's profiles, on
 * segments the corpus does not carry: component indices and progression
 * changes of two bytes (Csiz 257, whose main header ends at 833), tile-part
 * QCC and COC, SPrgn above 37, TLM of each Ttlm and Ptlm size, PLM entries
 * that do not end or overflow, a short PPM, and Profile 0 and 1 limits. */
static void check_segment_gaps(void) {
    static const gap_case cases[] = {
        {"a wide COC", 0, 257, 4, 4, 0, 0,
         {GAP(HV_COC, "\x01\x00\x00\x00\x04\x04\x00\x01"), GAP_END}, {GAP_END}, NULL, 0},
        {"a wide COC of one byte", 0, 257, 4, 4, 0, 0,
         {GAP(HV_COC, "\x01"), GAP_END}, {GAP_END}, "invalid COC", 833},
        {"a wide QCC", 0, 257, 4, 4, 0, 257,
         {GAP(HV_QCC, "\x01\x00\x40\x40"), GAP_END}, {GAP_END}, NULL, 0},
        {"a wide RGN", 0, 257, 4, 4, 0, 257,
         {GAP(HV_RGN, "\x01\x00\x00\x05"), GAP_END}, {GAP_END}, NULL, 0},
        {"a wide POC", 0, 257, 4, 4, 0, 0,
         {GAP(HV_POC, "\x00\x00\x00\x00\x01\x01\x01\x01\x00"), GAP_END}, {GAP_END}, NULL, 0},
        {"a wide POC short of Ppoc", 0, 257, 4, 4, 0, 0,
         {GAP(HV_POC, "\x00\x00\x00\x00\x01\x01\x01\x01"), GAP_END}, {GAP_END}, "invalid POC", 833},
        {"a tile-part QCC", 0, 1, 4, 4, 0, 0,
         {GAP_END}, {GAP(HV_QCC, "\x00\x40\x40"), GAP_END}, NULL, 0},
        {"a tile-part QCC in Profile 0", 1, 1, 4, 4, 0, 0,
         {GAP_END}, {GAP(HV_QCC, "\x00\x40\x40"), GAP_END}, "codestream.profile-0", 77},
        {"a tile-part COC of the 9-7 filter, no quantization", 0, 1, 4, 4, 0, 0,
         {GAP_END}, {GAP(HV_COC, "\x00\x00\x00\x04\x04\x00\x00"), GAP_END},
         "codestream.quantization-transform", 65},
        {"SPrgn 38", 0, 1, 4, 4, 0, 0,
         {GAP(HV_RGN, "\x00\x00\x26"), GAP_END}, {GAP_END}, NULL, 0},
        {"SPrgn 38 in Profile 0", 1, 1, 4, 4, 0, 0,
         {GAP(HV_RGN, "\x00\x00\x26"), GAP_END}, {GAP_END}, "codestream.profile-0", 65},
        {"SPrgn 38 in Profile 1", 2, 1, 4, 4, 0, 0,
         {GAP(HV_RGN, "\x00\x00\x26"), GAP_END}, {GAP_END}, "codestream.profile-1", 65},
        {"RSpoc 1 in Profile 0", 1, 1, 4, 4, 0, 0,
         {GAP(HV_POC, "\x01\x00\x00\x01\x02\x01\x00"), GAP_END}, {GAP_END},
         "codestream.profile-0", 65},
        {"TLM of 8-bit Ttlm, 16-bit Ptlm", 0, 1, 4, 4, 0, 0,
         {GAP(HV_TLM, "\x00\x10\x00\x00\x15"), GAP_END}, {GAP_END}, NULL, 0},
        {"TLM of 16-bit Ttlm and Ptlm", 0, 1, 4, 4, 0, 0,
         {GAP(HV_TLM, "\x00\x20\x00\x00\x00\x15"), GAP_END}, {GAP_END}, NULL, 0},
        {"TLM with Ptlm 13", 0, 1, 4, 4, 0, 0,
         {GAP(HV_TLM, "\x00\x10\x00\x00\x0d"), GAP_END}, {GAP_END}, "invalid TLM", 65},
        {"TLM with Ttlm 255", 0, 1, 4, 4, 0, 0,
         {GAP(HV_TLM, "\x00\x10\xff\x00\x15"), GAP_END}, {GAP_END}, "invalid TLM", 65},
        {"TLM of two tile-parts, one there", 0, 1, 4, 4, 0, 0,
         {GAP(HV_TLM, "\x00\x00\x00\x15\x00\x15"), GAP_END}, {GAP_END}, "tlm.tile-parts", 96},
        {"a PLM entry that does not end", 0, 1, 4, 4, 0, 0,
         {GAP(HV_PLM, "\x00\x02\x83\x83"), GAP_END}, {GAP_END}, "plm.length", 73},
        {"a PLM entry above 64 bits", 0, 1, 4, 4, 0, 0,
         {GAP(HV_PLM, "\x00\x0a\x82\xff\xff\xff\xff\xff\xff\xff\xff\x7f"), GAP_END}, {GAP_END},
         "plt.value-overflow", 81},
        {"PPM with three bytes after Zppm", 0, 1, 4, 4, 0, 0,
         {GAP(HV_PPM, "\x00\x01\x02\x03"), GAP_END}, {GAP_END}, "invalid PPM", 65},
        {"Profile 0 with XRsiz 3", 1, 1, 4, 4, 3, 0,
         {GAP_END}, {GAP_END}, "codestream.profile-0", 65},
        {"Profile 0, one tile, 129 wide at NL 0", 1, 1, 129, 4, 0, 0,
         {GAP_END}, {GAP_END}, "codestream.profile-0", 65},
        {"Profile 0 with a precinct of 1 x 1", 1, 1, 4, 4, 0, 0,
         {GAP(HV_COC, "\x00\x01\x00\x04\x04\x00\x01\x00"), GAP_END}, {GAP_END},
         "codestream.profile-0", 77},
        {"Profile 1 with Xsiz 2^31", 2, 1, 0x80000000u, 4, 0, 0,
         {GAP_END}, {GAP_END}, "codestream.profile-1", 65},
    };
    uint32_t f[8] = {256, 128, 0, 0, 128, 128, 0, 0};
    bytes b;
    size_t k, at;
    const char *error;
    for (k = 0; k < sizeof cases / sizeof *cases; k++) {
        b = gap_stream(&cases[k]);
        error = cs_check(&b, 0, &at);
        expect(cases[k].what, error, at, cases[k].error, cases[k].at);
        release(&b);
    }

    /* Profile 0: the first tile-parts in Isot order (Table A.45), tile 1's
     * first. */
    memset(&b, 0, sizeof b);
    u16(&b, HV_SOC);
    siz(&b, f, 1, NULL, NULL);
    b.d[7] = 1;
    cod(&b);
    qcd(&b);
    tile_part(&b, 1, 0, 1, 3);
    tile_part(&b, 0, 0, 1, 3);
    u16(&b, HV_EOC);
    error = cs_check(&b, 0, &at);
    expect("Profile 0, tile 1 first", error, at, "codestream.profile-0", 65);
    release(&b);
}

/* The rules of the box tree and the header boxes on JPX files the corpus
 * does not carry: labels of UTF-8 sequences, j2ci and jlxi out of place, a
 * j2cx in a j2cx, a cref to an IPR box, the boxes of a cgrp and of a res,
 * and running out of memory for the mdat boxes. Each box in `r` follows
 * the base's jp2h at 227. */
static void check_box_gaps(void) {
    static const struct {
        const char *what, *text;
        size_t n;
        int valid;
    } labels[] = {
        {"a label of two bytes", "\xc3\xa9", 2, 1},
        {"a label of three bytes", "\xe2\x82\xac", 3, 1},
        {"a label of four bytes", "\xf0\x9f\x98\x80", 4, 1},
        {"a label with a bad continuation", "\xc3\x41", 2, 0},
        {"an overlong label", "\xc0\x80", 2, 0},
        {"a label cut short", "\xe2\x82", 2, 0},
        {"a label of a surrogate", "\xed\xa0\x80", 3, 0},
        {"a label with 0xF8", "\xf8\x80\x80\x80\x80", 5, 0},
        {"a label above U+10FFFF", "\xf4\x90\x80\x80", 4, 0},
        {"a label of a C1 control", "\xc2\x85", 2, 0},
    };
    static const struct {
        const char *what, *jplh;
        size_t n;
        const char *error;
        size_t at;
    } cgrps[] = {
        {"a cgrp of a free box", "\0\0\0\x10" "cgrp" "\0\0\0\x08" "free", 16, "cgrp.empty", 235},
        {"a cgrp's colr short of its fields", "\0\0\0\x12" "cgrp" "\0\0\0\x0A" "colr\x01\x00", 18,
         "cgrp: colr shorter than its fields", 243},
        {"a cgrp's child past its end", "\0\0\0\x10" "cgrp" "\0\0\0\x40" "colr", 16, "box.framing",
         243},
    };
    bytes r = {NULL, 0, 0}, b;
    const char *error;
    size_t k, at, j, j2;

    for (k = 0; k < sizeof labels / sizeof *labels; k++) {
        clear(&r);
        add(&r, &JPCH);
        add(&r, &JP2C);
        box(&r, "lbl ", labels[k].text, labels[k].n);
        b = jpx_with(&r);
        error = jpx_headers(&b, &at);
        expect(labels[k].what, error, at, labels[k].valid ? NULL : "lbl.characters", 227);
        release(&b);
    }

    clear(&r);
    add(&r, &JPCH);
    add(&r, &JP2C);
    box(&r, "j2ci", "\0\0\0\x01\0\0\0\0", 8);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("a j2ci at the top level", error, at, "j2ci.placement", 227);
    release(&b);

    clear(&r);                          /* Ncs 2: its codestream and the inner j2cx's */
    add(&r, &JPCH);
    add(&r, &JP2C);
    j = begin(&r, "j2cx");
    box(&r, "j2ci", "\0\0\0\x02\0\0\0\0", 8);
    j2 = begin(&r, "j2cx");
    box(&r, "j2ci", "\0\0\0\x01\0\0\0\0", 8);
    add(&r, &JP2C);
    end(&r, j2);
    add(&r, &JP2C);
    end(&r, j);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("a j2cx in a j2cx", error, at, NULL, 0);
    release(&b);

    clear(&r);
    add(&r, &JPCH);
    add(&r, &JP2C);
    j = begin(&r, "jclx");
    box(&r, "free", NULL, 0);
    box(&r, "jlxi", "\0\0\0\x01\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20);
    end(&r, j);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("a jlxi second in its jclx", error, at, "jlxi.placement", 235);
    release(&b);

    clear(&r);                          /* the base's jpch, with a cref to an IPR box */
    j = begin(&r, "jpch");
    put(&r, JPCH.d + 8, JPCH.n - 8);
    box(&r, "cref", "jp2i" "\0\0\0\x18" "flst\0\x01\0\0\0\0\0\0\0\x0c\0\0\0\x01\0\0", 28);
    end(&r, j);
    add(&r, &JP2C);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("a cref to an IPR box", error, at, NULL, 0);
    release(&b);

    for (k = 0; k < sizeof cgrps / sizeof *cgrps; k++) {
        clear(&r);
        add(&r, &JPCH);
        add(&r, &JP2C);
        box(&r, "jplh", cgrps[k].jplh, cgrps[k].n);
        b = jpx_with(&r);
        error = jpx_headers(&b, &at);
        expect(cgrps[k].what, error, at, cgrps[k].error, cgrps[k].at);
        release(&b);
    }

    /* A res in jp2h (after ihdr and colr, at 77): its child past its end,
     * and a free box only. */
    b = jp2_plus("\0\0\0\x10" "res " "\0\0\0\x40" "resc", 16);
    error = jp2h(&b, &at);
    expect("a res's child past its end", error, at, "box.framing", 85);
    release(&b);
    b = jp2_plus("\0\0\0\x10" "res " "\0\0\0\x08" "free", 16);
    error = jp2h(&b, &at);
    expect("a res of a free box", error, at, "res.resc-resd", 77);
    release(&b);

    clear(&r);                          /* the first allocation: the mdat list */
    add(&r, &JPCH);
    add(&r, &JP2C);
    box(&r, "mdat", "abc", 3);
    b = jpx_with(&r);
    fail_at = 0;
    error = jpx_headers(&b, &at);
    fail_at = -1;
    check(error != NULL && strcmp(error, "out of memory") == 0, "an mdat box, out of memory",
          error);
    release(&b);
    release(&r);
}

/* The last supported superbox is fully checked, including its leaf
 * children and closing count rules. The next one reports an implementation
 * limit even when its contents would be valid. */
static void check_box_depth(void) {
    bytes r = {NULL, 0, 0}, b;
    size_t starts[HV_BOX_DEPTH_MAX + 1], at, offset;
    const char *error, *want;
    int kind, depth, fault, i;
    char what[96];

    for (kind = 0; kind < 2; kind++) {
        for (depth = HV_BOX_DEPTH_MAX - 1; depth <= HV_BOX_DEPTH_MAX + 1; depth++) {
            for (fault = 0; fault < 3; fault++) {
                clear(&r);
                add(&r, &JPCH);
                add(&r, &JP2C);
                for (i = 0; i < depth; i++) {
                    starts[i] = begin(&r, kind ? "j2cx" : "asoc");
                    if (kind)
                        box(&r, "j2ci", fault == 2 && i == depth - 1 ?
                            "\0\0\0\x02\0\0\0\0" : "\0\0\0\x01\0\0\0\0", 8);
                    else
                        box(&r, "lbl ", "a", 1);
                }
                offset = r.n;
                if (fault == 1) u8(&r, 0);   /* a truncated child header */
                else if (kind) add(&r, &JP2C);
                else if (fault == 0) box(&r, "free", NULL, 0);
                for (i = depth; i-- > 0;) end(&r, starts[i]);
                b = jpx_with(&r);
                want = NULL;
                if (depth > HV_BOX_DEPTH_MAX) {
                    want = "box.depth-limit";
                    offset = starts[HV_BOX_DEPTH_MAX];
                } else if (fault == 1) {
                    want = "box.framing";
                } else if (fault == 2) {
                    want = kind ? "j2ci.ncs" : "asoc.children";
                    offset = starts[depth - 1];
                }
                snprintf(what, sizeof what, "%d nested %s boxes, fault %d", depth,
                         kind ? "j2cx" : "asoc", fault);
                error = jpx_headers(&b, &at);
                expect(what, error, at, want, b.n - r.n + offset);
                release(&b);
            }
        }
    }

    /* An ftbl at the last supported level: its fragment must still be
     * checked after the tree succeeds. DR 0 with no mdat is invalid. */
    clear(&r);
    add(&r, &JPCH);
    add(&r, &JP2C);
    for (i = 0; i < HV_BOX_DEPTH_MAX - 1; i++) {
        starts[i] = begin(&r, "j2cx");
        box(&r, "j2ci", "\0\0\0\x01\0\0\0\0", 8);
    }
    starts[i] = begin(&r, "ftbl");
    offset = begin(&r, "flst");
    u16(&r, 1);
    u64(&r, 12);
    u32(&r, 1);
    u16(&r, 0);
    end(&r, offset);
    for (i = HV_BOX_DEPTH_MAX; i-- > 0;) end(&r, starts[i]);
    b = jpx_with(&r);
    error = jpx_headers(&b, &at);
    expect("fragment at the depth limit", error, at, "flst.mdat", b.n - r.n + offset);
    release(&b);
    release(&r);
}

/* M.11.3.1: complete NF tuples at both permitted ftbl locations, and
 * references to a dtbl later in the file. NDR must describe real URLs. */
static void check_fragment_lists(void) {
    static const struct {
        unsigned nf, tuples, trim, extra, dr, ndr;
        uint64_t off;
        const char *error;
    } cases[] = {
        {0, 0, 0, 0, 1, 1, 12, NULL},
        {1, 1, 0, 0, 1, 1, 12, NULL},
        {2, 2, 0, 0, 1, 1, 12, NULL},
        {0, 0, 1, 0, 1, 1, 12, "decode"},
        {0, 0, 2, 0, 1, 1, 12, "decode"},
        {1, 1, 0, 0, 1, 2, 12, "dtbl.ndr-count"},
        {1, 0, 0, 0, 1, 1, 12, "flst.nf-count"},
        {0, 1, 0, 0, 1, 1, 12, "flst.nf-count"},
        {2, 1, 0, 0, 1, 1, 12, "flst.nf-count"},
        {1, 1, 1, 0, 1, 1, 12, "flst.nf-count"},
        {1, 1, 0, 1, 1, 1, 12, "flst.nf-count"},
        {1, 1, 0, 0, 1, 1, 11, "decode"},
        {1, 1, 0, 0, 2, 1, 12, "flst.dr-range"},
        {1, 1, 0, 0, 1, 0, 12, "flst.dr-range"},
        {1, 1, 0, 0, 0, 1, 12, "flst.mdat"},
    };
    size_t k, at;
    int nested;
    for (nested = 0; nested < 2; nested++) for (k = 0; k < sizeof cases / sizeof *cases; k++) {
        bytes r = {NULL, 0, 0}, b;
        size_t j2cx = 0, ftbl, flst, dtbl = 0;
        unsigned i;
        const char *error;
        char what[80];
        add(&r, &JPCH); add(&r, &JP2C);
        if (nested) {
            j2cx = begin(&r, "j2cx");
            box(&r, "j2ci", "\0\0\0\x01\0\0\0\0", 8);
        } else add(&r, &JPCH); /* a header for the second top-level stream */
        ftbl = begin(&r, "ftbl"); flst = begin(&r, "flst");
        u16(&r, cases[k].nf);
        for (i = 0; i < cases[k].tuples; i++) {
            u64(&r, cases[k].off); u32(&r, 1); u16(&r, cases[k].dr);
        }
        r.n -= cases[k].trim;
        if (cases[k].extra) u8(&r, 0);
        end(&r, flst); end(&r, ftbl);
        if (nested) end(&r, j2cx);
        if (cases[k].ndr) {
            dtbl = begin(&r, "dtbl"); u16(&r, cases[k].ndr);
            box(&r, "url ", "\0\0\0\0file://a.jp2\0", 17);
            end(&r, dtbl);
        }
        b = jpx_with(&r);
        error = jpx_headers(&b, &at);
        snprintf(what, sizeof what, "%s fragment list case %zu", nested ? "nested" : "top-level", k);
        expect(what, error, at, cases[k].error,
               b.n - r.n + (cases[k].ndr == 2 ? dtbl : flst));
        release(&b); release(&r);
    }
}

/* The rules' functions on what the reader does not pass them: other
 * segment codes, a grid without tiles, box extents, jpxb colours from a
 * cgrp, and fragments in order. */
static void check_rule_functions(void) {
    SizFixed fixed;
    hv_siz siz;
    hv_segments s;
    hv_component component;
    hv_header jplh;
    uint64_t end = 0;

    memset(&fixed, 0, sizeof fixed);
    fixed.xsiz = fixed.ysiz = fixed.xtsiz = fixed.ytsiz = 4;
    fixed.csiz = 1;
    memset(&siz, 0, sizeof siz);
    siz.fixed = &fixed;
    siz.ncomponents = 1;
    hv_segments_init(&s, &siz, &component, 0);
    check(hv_segments_main(&s, HV_COM, NULL, NULL, (const uint8_t *)"", 0) == NULL,
          "hv_segments_main on COM", NULL);
    check(hv_segments_tile(&s, HV_COM, NULL, NULL, (const uint8_t *)"", 0) == NULL,
          "hv_segments_tile on COM", NULL);
    fixed.xtsiz = 0;
    check(hv_rule_tiles(&siz) == 0, "hv_rule_tiles, XTsiz 0", NULL);
    fixed.xtsiz = 4;
    fixed.xtosiz = 4;
    check(hv_rule_tiles(&siz) == 0, "hv_rule_tiles, XTOsiz = Xsiz", NULL);

    check(hv_rule_extent(HV_BOX_COLR, 1) != NULL &&
          strcmp(hv_rule_extent(HV_BOX_COLR, 1), "box.extent") == 0,
          "hv_rule_extent of another box", NULL);

    memset(&jplh, 0, sizeof jplh);
    jplh.cgrp = 1;
    jplh.cgrp_baseline = jplh.cgrp_approx = 1;
    check(hv_rule_jpxb_layer(NULL, NULL, &jplh) == NULL, "jpxb colours from a cgrp", NULL);
    jplh.cgrp_approx = 0;
    check(hv_rule_jpxb_layer(NULL, NULL, &jplh) != NULL &&
          strcmp(hv_rule_jpxb_layer(NULL, NULL, &jplh), "jpxb.colour") == 0,
          "jpxb colours from a cgrp without APPROX 1", NULL);

    check(hv_rule_jpxb_fragment(&end, 0, 10, 5) == NULL && end == 15 &&
          hv_rule_jpxb_fragment(&end, 0, 15, 1) == NULL && end == 16 &&
          strcmp(hv_rule_jpxb_fragment(&end, 0, 15, 1), "jpxb.fragments") == 0,
          "jpxb fragments in order, then one before the end", NULL);
}

static void check_plt_packets(void) {
    static const uint8_t bytes[] = {3,5,0,0};
    hv_plt segments[] = {{0,0,1},{1,1,2}};
    hv_plt_reader r;
    uint64_t offset, length;
    size_t at = 99;
    const char *error;
    check(hv_plt_check_segments(segments,2,&at)==NULL,"saved segment sequence",NULL);
    segments[1].zplt=2;
    error=hv_plt_check_segments(segments,2,&at);
    check(error && strcmp(error,"plt.zplt-sequence")==0 && at==1,"saved segment sequence error offset",error);
    segments[1].zplt=1;
    error=hv_plt_check_segments(NULL,0,&at);
    check(error && strcmp(error,"codestream.no-plt")==0,"missing saved segments",error);
    hv_plt_init(&r,HV_PLT_PROFILE);
    check(hv_plt_begin(&r,&segments[0])==NULL &&
          hv_plt_packet(&r,bytes,8,0,2,&offset,&length)==1 && offset==0 && length==3 &&
          hv_plt_packet(&r,bytes,8,0,2,&offset,&length)==0 &&
          hv_plt_begin(&r,&segments[1])==NULL &&
          hv_plt_packet(&r,bytes,8,1,2,&offset,&length)==1 && offset==3 && length==5 &&
          hv_plt_packet(&r,bytes,8,1,2,&offset,&length)==0 &&
          hv_plt_end_tile(&r,8)==NULL && hv_plt_end(&r,2)==NULL,
          "packet ranges across segments",r.error);
    hv_plt_init(&r,HV_PLT_PROFILE);
    hv_plt_begin(&r,&segments[0]);offset=UINT64_MAX;
    check(hv_plt_packet(&r,bytes,2,1,2,&offset,&length)==-1 &&
          strcmp(r.error,"plt.coverage")==0 && offset==UINT64_MAX,
          "oversized packet is not published",r.error);
    hv_plt_init(&r,HV_PLT_PROFILE);
    hv_plt_begin(&r,&segments[0]);offset=UINT64_MAX;
    check(hv_plt_packet(&r,bytes,8,1,2,&offset,&length)==-1 && offset==UINT64_MAX,
          "early PLT end is not published",r.error);
    segments[0].end=2;
    hv_plt_init(&r,HV_PLT_PROFILE);hv_plt_begin(&r,&segments[0]);offset=UINT64_MAX;
    check(hv_plt_packet(&r,bytes,3,1,2,&offset,&length)==-1 && offset==UINT64_MAX,
          "early data end is not published",r.error);
    segments[0].end=sizeof bytes;
    hv_plt_init(&r,HV_PLT_PROFILE);hv_plt_begin(&r,&segments[0]);
    check(hv_plt_packet(&r,bytes,8,1,2,&offset,&length)==1 &&
          hv_plt_packet(&r,bytes,8,1,2,&offset,&length)==1 &&
          hv_plt_packet(&r,bytes,8,1,2,&offset,&length)==0 &&
          hv_plt_end_tile(&r,8)==NULL && hv_plt_end(&r,2)==NULL,
          "packet indexing consumes final padding",r.error);
    hv_plt_init(&r,0);
    check(hv_plt_packet(&r,bytes,8,1,2,&offset,&length)==-1 &&
          strcmp(r.error,"PLT packet indexing requires HV_PLT_PROFILE")==0,
          "packet cursor requires the profile",r.error);
}

/* Repeated one-byte lengths may reuse a decoded value, but must still
 * update every entry's rules and stop at the current segment boundary. */
static void check_plt_repeated(void) {
    unsigned n;
    for (n = 0; n < 128; n++) {
        uint8_t first[] = {(uint8_t)n, (uint8_t)n};
        uint8_t remapped[] = {(uint8_t)n, (uint8_t)n};
        hv_plt range = {0, 0, sizeof first};
        hv_plt_reader r;
        uint64_t value;
        hv_plt_init(&r, HV_PLT_PROFILE);
        check(hv_plt_begin(&r, &range) == NULL &&
              hv_plt_read(&r, first, &value) == 1 && value == n && r.pos == 1 &&
              hv_plt_read(&r, remapped, &value) == 1 && value == n && r.pos == 2 &&
              hv_plt_read(&r, remapped, &value) == 0 && r.sum == 2 * n &&
              r.count.packets == (n != 0 ? 2u : 0u) && r.zeros == (n == 0 ? 2u : 0u),
              "repeated length preserves values, bounds, sum and counts", r.error);
        check(hv_plt_end_tile(&r, 2 * n) == NULL,
              "repeated length tile completion", r.error);
        if (n != 0) {
            hv_plt_reader fewer = r, more = r, invalid = r;
            check(hv_plt_end(&r, 2) == NULL &&
                  hv_plt_end(&fewer, 1) != NULL && strcmp(fewer.error, "plt.packet-count") == 0 &&
                  hv_plt_end(&more, 3) != NULL && strcmp(more.error, "plt.packet-count") == 0 &&
                  hv_plt_end(&invalid, 0) != NULL && strcmp(invalid.error, "codestream.packet-count") == 0,
                  "precomputed packet count exact boundaries", NULL);
        } else {
            check(hv_plt_end(&r, 1) != NULL && strcmp(r.error, "plt.zero-length") == 0,
                  "padding cannot replace an expected packet", r.error);
        }
    }
    {
        static const uint8_t entries[] = {3, 3, 0, 0, 3, 0, 0, 0x80};
        hv_plt range = {0, 0, sizeof entries - 1};
        hv_plt_reader r;
        uint64_t value;
        hv_plt_init(&r, HV_PLT_PROFILE);
        hv_plt_begin(&r, &range);
        while (hv_plt_read(&r, entries, &value) == 1) ;
        check(r.error != NULL && strcmp(r.error, "plt.padding-position") == 0,
              "repeated padding still forbids a later packet", r.error);
        range.end++;
        hv_plt_init(&r, HV_PLT_PROFILE);
        hv_plt_begin(&r, &range);
        while (hv_plt_read(&r, entries, &value) == 1) ;
        check(r.error != NULL && strcmp(r.error, "invalid PLT") == 0,
              "malformed suffix wins after repeated padding and a rule error", r.error);
    }
}

/* hv_plt_reader used out of order: a segment begun before the last is
 * read, an empty range, and the completions without HV_READ_JPIP or before
 * the tile-part is read. Each error is terminal. */
static void check_plt_misuse(void) {
    static const uint8_t entries[] = {3, 5};
    hv_plt range = {0, 0, 2};
    hv_plt_reader a;
    uint64_t value;
    /* Completion must reject the cursor state before checking this count. */
    const uint64_t unused_packets = 0;
    hv_plt_init(&a, HV_PLT_PROFILE);
    check(hv_plt_begin(&a, &range) == NULL && hv_plt_read(&a, entries, &value) == 1 &&
          hv_plt_begin(&a, &range) != NULL && strcmp(a.error, "PLT segment not consumed") == 0,
          "begun twice", a.error);
    check(hv_plt_read(&a, entries, &value) == -1, "begun twice, terminal", NULL);

    hv_plt_init(&a, HV_PLT_PROFILE);
    range.start = range.end = 1;
    check(hv_plt_begin(&a, &range) != NULL && strcmp(a.error, "invalid PLT") == 0,
          "an empty range", a.error);
    range.start = 0;
    range.end = 2;

    hv_plt_init(&a, HV_PLT_STANDARD);
    check(hv_plt_end_tile(&a, 8) != NULL &&
          strcmp(a.error, "PLT completion requires HV_PLT_PROFILE") == 0,
          "end_tile without HV_READ_JPIP", a.error);
    hv_plt_init(&a, HV_PLT_STANDARD);
    check(hv_plt_end(&a, unused_packets) != NULL &&
          strcmp(a.error, "PLT completion requires HV_PLT_PROFILE") == 0,
          "end without HV_READ_JPIP", a.error);

    hv_plt_init(&a, HV_PLT_PROFILE);
    check(hv_plt_begin(&a, &range) == NULL && hv_plt_read(&a, entries, &value) == 1 &&
          hv_plt_end(&a, unused_packets) != NULL &&
          strcmp(a.error, "PLT tile-part not completed") == 0,
          "end in the middle of a segment", a.error);
    hv_plt_init(&a, HV_PLT_PROFILE);
    check(hv_plt_begin(&a, &range) == NULL && hv_plt_read(&a, entries, &value) == 1 &&
          hv_plt_read(&a, entries, &value) == 1 && hv_plt_read(&a, entries, &value) == 0 &&
          hv_plt_end(&a, unused_packets) != NULL &&
          strcmp(a.error, "PLT tile-part not completed") == 0,
          "end before end_tile", a.error);
}

/* Opaque framing shares the validator's bounded field reads, without
 * interpreting bodies or treating SOT/SOP as marker-only codes. */
static void check_marker_framing(void) {
    static const uint16_t codes[] = {HV_QCD, HV_COM, HV_TLM, HV_PLM, HV_PLT,
                                     HV_SOT, HV_SOP, 0xFF70};
    static const unsigned lengths[] = {2, 3, 65535};
    static const uint16_t alone[] = {HV_SOC, HV_SOD, HV_EOC, HV_EPH, 0xFF30, 0xFF3F};
    size_t i, j, n;
    for (i = 0; i < sizeof codes / sizeof *codes; i++) {
        for (j = 0; j < sizeof lengths / sizeof *lengths; j++) {
            bytes b = {0};
            hv_marker m;
            const char *error;
            uint8_t *buf;
            u16(&b, codes[i]);
            u16(&b, lengths[j]);
            while (b.n < lengths[j] + 2u) u8(&b, 0xFF);
            buf = exact(&b);
            error = hv_marker_read(buf, 0, b.n, &m);
            check(error == NULL && m.code == codes[i] && m.start == 0 &&
                  m.payload == 4 && m.end == b.n, "opaque marker extent", error);
            for (n = 0; n < b.n; n++) {
                bytes prefix = {buf, n, n};
                uint8_t *short_buf = exact(&prefix);
                hv_marker before;
                memset(&m, 0x55, sizeof m);
                memcpy(&before, &m, sizeof before);
                error = hv_marker_read(short_buf, 0, n, &m);
                check(error != NULL && strcmp(error, n < 2 ? "expected a marker" :
                       "marker segment overruns its header") == 0,
                      "opaque marker prefix", error);
                check(memcmp(&m, &before, sizeof m) == 0,
                      "opaque error leaves result unchanged", NULL);
                free(short_buf);
            }
            free(buf);
            release(&b);
        }
    }
    for (i = 0; i < sizeof alone / sizeof *alone; i++) {
        uint8_t buf[] = {(uint8_t)(alone[i] >> 8), (uint8_t)alone[i], 0xFF, 0xFF};
        hv_marker m;
        const char *error = hv_marker_read(buf, 0, sizeof buf, &m);
        check(error == NULL && m.code == alone[i] && m.payload == 2 && m.end == 2,
              "marker-only extent", error);
    }
    {
        uint8_t buf[] = {0, 0xFF, 0x64, 0, 2};
        hv_marker m;
        const char *error = hv_marker_read(buf, 1, sizeof buf, &m);
        check(error == NULL && m.start == 1 && m.payload == 5 && m.end == 5,
              "marker at nonzero offset", error);
        check(hv_marker_read(buf, sizeof buf + 1, sizeof buf, &m) != NULL &&
              hv_marker_read(buf, SIZE_MAX, sizeof buf, &m) != NULL,
              "reversed marker range", NULL);
        buf[3] = 0; buf[4] = 1;
        check(hv_marker_read(buf, 1, sizeof buf, &m) != NULL,
              "marker length below two", NULL);
        buf[1] = 0; buf[2] = 0x64;
        check(hv_marker_read(buf, 1, sizeof buf, &m) != NULL,
              "invalid marker code", NULL);
        buf[1] = 0xFF; buf[2] = 0xFF;
        check(hv_marker_read(buf, 1, sizeof buf, &m) != NULL,
              "reserved FF marker code", NULL);
    }
}

static void check_selected_header(void) {
    hv_boxes boxes;
    hv_box box, header_box = {0}, stream = {0};
    hv_header header, saved;
    const char *error;
    size_t at = (size_t)-1;
    hv_boxes_file(&boxes, JP2.d, JP2.n);
    while (hv_boxes_next(&boxes, &box, &error, &at) == 1) {
        if (box.type == HV_BOX_JP2H) header_box = box;
        if (box.type == HV_BOX_JP2C) stream = box;
    }
    nfrees = 0;
    error = hv_read_jp2h_boxes(JP2.d, &header_box, &stream, &header, &at);
    check(error == NULL && at == (size_t)-1 && nfrees == 1,
          "selected header releases component view and preserves success offset", error);
    memcpy(&saved, &header, sizeof saved);
    nfrees = 0; fail_at = 0;
    error = hv_read_jp2h_boxes(JP2.d, &header_box, &stream, &header, &at);
    fail_at = -1;
    check(error != NULL && strcmp(error, "out of memory") == 0 && nfrees == 0 &&
          memcmp(&header, &saved, sizeof header) == 0,
          "selected header allocation failure preserves output", error);
    {
        hv_marker marker = {0};
        size_t pos = stream.payload;
        while (hv_marker_read(JP2.d, pos, stream.end, &marker) == NULL && marker.code != HV_COD)
            pos = marker.end;
        check(marker.code == HV_COD, "selected header MCT located", NULL);
        if (marker.code == HV_COD) {
            uint8_t mct = JP2.d[marker.payload + 4];
            JP2.d[marker.payload + 4] = 2;
            nfrees = 0;
            error = hv_read_jp2h_boxes(JP2.d, &header_box, &stream, &header, &at);
            check(error != NULL && strcmp(error, "invalid COD") == 0 && nfrees == 1 &&
                  memcmp(&header, &saved, sizeof header) == 0,
                  "selected header field failure frees view and preserves output", error);
            JP2.d[marker.payload + 4] = mct;
        }
    }
    nfrees = -1;
    at = (size_t)-1;
    error = hv_read_jp2h_boxes(JP2.d, &header_box, &stream, &header, &at);
    check(error == NULL && at == (size_t)-1,
          "selected header retry after allocation and field failure", error);
}

static void check_header_siz_fields(void) {
    static const struct { const char *file, *error; } opaque[] = {
        {"jp2-siz.rsiz-3.jp2", "siz.unsupported-profile"},
        {"jp2-siz.xtosiz-1.jp2", "siz.tile-origin"},
        {"jp2-siz.ytosiz-1.jp2", "siz.tile-origin"},
        {"jp2-rule-siz.tile-covers-origin-53.jp2", "siz.tile-covers-origin"}
    };
    for (size_t i = 0; i < sizeof opaque / sizeof *opaque; i++) {
        bytes b = load(opaque[i].file);
        hv_header header;
        hv_box box;
        size_t at = 0;
        const char *error = hv_read_jp2h(b.d, b.n, &header, &at);
        check(error == NULL, "header information ignores tile/capability admission", error);
        error = hv_check_jp2h(b.d, b.n, &at);
        check(error == NULL, "header consistency ignores tile/capability admission", error);
        error = hv_read_jp2(b.d, b.n, &box, &at);
        check(error == NULL, "SIZ fixture codestream extent", error);
        if (error == NULL) {
            error = hv_codestream_check(b.d, box.payload, box.end, HV_READ_VALIDATE, &at);
            check(error != NULL && strcmp(error, opaque[i].error) == 0,
                  "explicit SIZ validation retained", error);
        }
        release(&b);
    }
    {
        bytes b = load("jp2-siz.xsiz-2147483648.jp2");
        hv_boxes boxes, children;
        hv_box box, stream;
        hv_header header;
        size_t at = 0;
        const char *error = NULL;
        hv_boxes_file(&boxes, b.d, b.n);
        while (hv_boxes_next(&boxes, &box, &error, &at) == 1)
            if (box.type == HV_BOX_JP2H) {
                hv_boxes_children(&children, b.d, &box);
                while (hv_boxes_next(&children, &box, &error, &at) == 1)
                    if (box.type == HV_BOX_IHDR) {
                        b.d[box.payload + 4] = 0x80;
                        b.d[box.payload + 5] = b.d[box.payload + 6] = b.d[box.payload + 7] = 0;
                    }
            }
        error = hv_read_jp2h(b.d, b.n, &header, &at);
        check(error == NULL, "header consistency does not count tiles", error);
        error = hv_read_jp2(b.d, b.n, &stream, &at);
        check(error == NULL, "tile-count fixture extent", error);
        if (error == NULL) {
            error = hv_codestream_check(b.d, stream.payload, stream.end, HV_READ_VALIDATE, &at);
            check(error != NULL && strcmp(error, "siz.tile-count") == 0,
                  "explicit tile-count validation retained", error);
        }
        release(&b);
    }
}

static void check_header_coding_fields(void) {
    static const struct { const char *file, *error; } opaque[] = {
        {"jp2-cod.scod.reserved-1.jp2", "invalid COD"},
        {"jp2-cod.sgcod.progression-5.jp2", "invalid COD"},
        {"jp2-cod.sgcod.layers-0.jp2", "invalid COD"},
        {"jp2-cod.spcod.levels-33.jp2", "invalid COD"},
        {"jp2-cod.spcod.cbWidthExp-5.jp2", "cod.codeblock-area"},
        {"jp2-cod.spcod.cbStyle-64.jp2", "invalid COD"},
        {"jp2-cod.spcod.transform-2.jp2", "invalid COD"}
    };
    static const struct { const char *file, *error; } interpreted[] = {
        {"jp2-cod.sgcod.mct-2.jp2", "invalid COD"},
        {"jp2-cod.sgcod.mct-1.jp2", "siz.mct-components"}
    };
    size_t i;
    for (i = 0; i < sizeof opaque / sizeof *opaque; i++) {
        bytes b = load(opaque[i].file);
        hv_header header;
        hv_box box;
        hv_codestream cs;
        hv_item item;
        size_t at = 0;
        const char *error = hv_read_jp2h(b.d, b.n, &header, &at);
        check(error == NULL, "header information ignores unused COD fields", error);
        error = hv_check_jp2h(b.d, b.n, &at);
        check(error == NULL, "header consistency ignores unused COD fields", error);
        error = hv_read_jp2(b.d, b.n, &box, &at);
        check(error == NULL, "coding fixture codestream extent", error);
        if (error == NULL) {
            error = hv_codestream_check(b.d, box.payload, box.end, HV_READ_VALIDATE, &at);
            check(error != NULL && strcmp(error, opaque[i].error) == 0,
                  "explicit COD validation retained", error);
            int status = hv_codestream_open(&cs, b.d, box.payload, box.end, HV_READ_PACKETS);
            if (status == 0)
                while ((status = hv_codestream_next(&cs, &item)) == 1) {}
            check(status < 0 && strcmp(cs.error, opaque[i].error) == 0,
                  "packet-operation COD validation retained", cs.error);
            hv_codestream_close(&cs);
        }
        release(&b);
    }
    for (i = 0; i < sizeof interpreted / sizeof *interpreted; i++) {
        bytes b = load(interpreted[i].file);
        hv_header header;
        size_t at = 0;
        const char *error = hv_read_jp2h(b.d, b.n, &header, &at);
        check(error != NULL && strcmp(error, interpreted[i].error) == 0,
              "JP2 MCT channel checks retained", error);
        release(&b);
    }
    {
        bytes b = load("jp2.jp2");
        hv_box box;
        hv_marker m = {0};
        hv_header header;
        size_t at = 0, pos;
        const char *error = hv_read_jp2(b.d, b.n, &box, &at);
        check(error == NULL, "MCT bounds fixture", error);
        if (error == NULL) {
            pos = box.payload;
            while (hv_marker_read(b.d, pos, box.end, &m) == NULL && m.code != HV_COD)
                pos = m.end;
            check(m.code == HV_COD, "MCT bounds COD located", NULL);
            if (m.code == HV_COD) {
                b.d[m.start + 2] = 0;
                b.d[m.start + 3] = 6; /* SCod, progression, layers; no MCT. */
                error = hv_read_jp2h(b.d, b.n, &header, &at);
                check(error != NULL && strcmp(error, "invalid COD") == 0 && at == m.start,
                      "MCT byte must lie within COD segment", error);
            }
        }
        release(&b);
    }
    {
        bytes b = load("jpx-embedded.jpx");
        hv_served_sources jpx;
        hv_marker m = {0};
        size_t at = 0;
        const char *error = hv_served_jpx(b.d, b.n, &jpx, &at);
        check(error == NULL && jpx.count > 0, "JPX coding fixture", error);
        if (error == NULL) {
            if (jpx.count > 0) {
                hv_box *box = &jpx.jp2c[0];
                size_t pos = box->payload;
                while (hv_marker_read(b.d, pos, box->end, &m) == NULL && m.code != HV_COD)
                    pos = m.end;
                check(m.code == HV_COD && m.end - m.payload > 4, "JPX MCT located", NULL);
                if (m.code == HV_COD && m.end - m.payload > 4) {
                    b.d[m.payload + 4] = 2;
                    error = hv_check_jpx_headers(b.d, b.n, &at);
                    check(error == NULL, "JPX header consistency does not interpret MCT", error);
                    error = hv_codestream_check(b.d, box->payload, box->end, HV_READ_VALIDATE, &at);
                    check(error != NULL && strcmp(error, "invalid COD") == 0,
                          "JPX explicit COD validation retained", error);
                }
            }
            hv_served_sources_free(&jpx);
        }
        release(&b);
    }
}

static void check_packet_reader_modes(void) {
    static const hv_read_mode accepted[] = {HV_READ_PACKETS, HV_READ_PACKETS_JPIP};
    static const hv_read_mode rejected[] = {(hv_read_mode)-1, (hv_read_mode)99};
    bytes b = simple();
    size_t i;
    /* Operation checks are independent of declaration validation. */
    b.d[6] = b.d[7] = 0xff;
    for (i = 0; i < sizeof accepted / sizeof *accepted; i++) {
        hv_codestream cs;
        hv_item item;
        int status = hv_codestream_open(&cs, b.d, 0, b.n, accepted[i]);
        int saw_data = 0, saw_eoc = 0;
        if (status == 0) {
            while ((status = hv_codestream_next(&cs, &item)) == 1) {
                saw_data |= item.kind == HV_TILE_DATA;
                saw_eoc |= item.code == HV_EOC;
                check(item.qcd == NULL && item.com == NULL && item.plt == NULL,
                      "packet reader leaves bodies opaque", NULL);
            }
        }
        check(status == 0 && saw_data && saw_eoc,
              "packet reader flags retain whole-codestream traversal", cs.error);
        hv_codestream_close(&cs);
    }
    for (i = 0; i < sizeof rejected / sizeof *rejected; i++) {
        hv_codestream cs;
        int status = hv_codestream_open(&cs, b.d, 0, b.n, rejected[i]);
        check(status < 0 && strcmp(cs.error,
                                  "invalid codestream read mode") == 0,
              "reader rejects invalid modes", cs.error);
        hv_codestream_close(&cs);
    }
    {
        hv_codestream cs;
        int status;
        nfrees = 0;
        b.d[35] = 1;                         /* XTOsiz 1 beyond XOsiz 0. */
        status = hv_codestream_open(&cs, b.d, 0, b.n, HV_READ_PACKETS);
        check(status < 0 && strcmp(cs.error, "siz.tile-origin") == 0,
              "broader declaration retains tile-grid admission", cs.error);
        hv_codestream_close(&cs);
        check(nfrees == 1, "packet SIZ failure frees component view", NULL);
        b.d[35] = 0;
        nfrees = 0; fail_at = 0;
        status = hv_codestream_open(&cs, b.d, 0, b.n, HV_READ_PACKETS);
        fail_at = -1;
        check(status < 0 && strcmp(cs.error, "out of memory") == 0,
              "packet component allocation failure", cs.error);
        hv_codestream_close(&cs);
        check(nfrees == 0, "failed packet allocation leaves no component view", NULL);
        nfrees = -1;
        hv_marker marker = {0};
        size_t pos = 0;
        while (hv_marker_read(b.d, pos, b.n, &marker) == NULL && marker.code != HV_COD)
            pos = marker.end;
        check(marker.code == HV_COD, "packet MCT located", NULL);
        if (marker.code == HV_COD) {
            uint8_t mct = b.d[marker.payload + 4];
            hv_item item;
            b.d[marker.payload + 4] = 1;
            status = hv_codestream_open(&cs, b.d, 0, b.n, HV_READ_PACKETS);
            while (status == 0 && hv_codestream_cod(&cs) == NULL) {
                int next = hv_codestream_next(&cs, &item);
                if (next != 1) { status = -1; break; }
            }
            check(status < 0 && strcmp(cs.error, "siz.mct-components") == 0,
                  "broader declaration retains COD MCT geometry", cs.error);
            hv_codestream_close(&cs);
            b.d[marker.payload + 4] = mct;
        }
        status = hv_codestream_open(&cs, b.d, 0, b.n, HV_READ_PACKETS);
        check(status == 0, "packet reader retry after failures", cs.error);
        hv_codestream_close(&cs);
    }
    release(&b);
}

/* These bodies are copied or dropped by tools. Framing can reach the
 * first tile-part, while the explicit validator retains its diagnostic. */
static void check_opaque_bodies(void) {
    static const struct { const char *file, *error; uint16_t code; } cases[] = {
        {"jp2-rule-main.com-21.jp2", "invalid COM", HV_COM},
        {"jp2-rule-qcd.style-72.jp2", "qcd.style", HV_QCD},
        {"jp2-rule-qcc.component-69.jp2", "qcc.component", HV_QCC},
        {"jp2-rule-rgn.component-77.jp2", "rgn.component", HV_RGN},
        {"jp2-rule-crg.length-95.jp2", "invalid CRG", HV_CRG},
        {"jp2-rule-tlm.stlm-83.jp2", "invalid TLM", HV_TLM},
        {"jp2-rule-plm.length-87.jp2", "plm.length", HV_PLM},
        {"jp2-sot.isot-1.jp2", "sot.isot-range", HV_SOT},
        {"jp2-sot.tpsot-1.jp2", "sot.tpsot-sequence", HV_SOT}
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        bytes file = load(cases[i].file);
        hv_box box;
        hv_marker m;
        size_t at = 0, pos;
        int found = 0;
        const char *error = hv_served_jp2(file.d, file.n, &box, &at);
        check(error == NULL, cases[i].file, error);
        if (error == NULL) {
            pos = box.payload;
            while ((error = hv_marker_read(file.d, pos, box.end, &m)) == NULL) {
                found |= m.code == cases[i].code;
                if (m.code == HV_COM && cases[i].code == HV_COM) {
                    /* Reserved Rcom, without changing the segment extent. */
                    file.d[m.payload] = 0xFF;
                    file.d[m.payload + 1] = 0xFF;
                }
                if (m.code == HV_SOT) break;
                pos = m.end;
            }
            check(error == NULL && found && m.code == HV_SOT,
                  "opaque main-header traversal", error);
            error = hv_codestream_check(file.d, box.payload, box.end, HV_READ_VALIDATE, &at);
            check(error != NULL && strcmp(error, cases[i].error) == 0,
                  "explicit body validation retained", error);
            {
                hv_header header;
                error = hv_read_jp2h(file.d, file.n, &header, &at);
                check(error == NULL, "input header leaves other bodies opaque", error);
                error = hv_check_jp2h(file.d, file.n, &at);
                check(error == NULL, "header validation leaves other bodies opaque", error);
            }
            {
                hv_codestream cs;
                hv_item item;
                int status = hv_codestream_open(&cs, file.d, box.payload, box.end, HV_READ_PACKETS);
                if (status == 0) {
                    while ((status = hv_codestream_next(&cs, &item)) == 1) {
                        check(item.qcd == NULL && item.com == NULL && item.plt == NULL,
                              "packet-operation bodies opaque", NULL);
                    }
                }
                if (cases[i].code == HV_SOT)
                    check(status < 0 && strcmp(cs.error, cases[i].error) == 0,
                          "packet-operation tile framing retained", cs.error);
                else
                    check(status == 0, "packet-operation opaque traversal", cs.error);
                check(hv_codestream_qcd(&cs) == NULL, "opaque QCD accessor", NULL);
                hv_codestream_close(&cs);
            }
        }
        release(&file);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_reader <vector directory>\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    dir = argv[1];
    load_bases();
    check_boxes();
    check_file_starts();
    check_jp2_access();
    check_container_access();
    check_fragment_access();
    check_reference_access();
    check_large_jp2_access();
    check_jpx_files();
    check_link_paths();
    check_links();
    check_jp2_headers();
    check_jpx_header_boxes();
    check_rreqs();
    check_marker_framing();
    check_selected_header();
    check_header_siz_fields();
    check_header_coding_fields();
    check_packet_reader_modes();
    check_opaque_bodies();
    check_codestreams();
    check_plt_cursor();
    check_header_diagnostics();
    check_plt_misuse();
    check_plt_repeated();
    check_plt_packets();
    check_segment_gaps();
    check_box_gaps();
    check_box_depth();
    check_fragment_lists();
    check_rule_functions();
    check_corpus_items();
    {
        bytes *all[] = {&JP2, &JPX, &LINKED, &FRAME1, &SIG, &FTYP_JP2, &FTYP_JPX, &RREQ,
                        &RREQ_LINKED, &JP2H, &JPX_JP2H, &JPCH, &JP2C, &IHDR, &COLR};
        size_t i;
        for (i = 0; i < sizeof all / sizeof *all; i++)
            release(all[i]);
    }
    if (failures == 0)
        printf("all checks passed\n");
    return failures != 0;
}
