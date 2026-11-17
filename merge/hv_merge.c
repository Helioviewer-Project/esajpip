/* hv_merge: merges JP2 files into a JPX file for JHelioviewer and the
 * JPIP server (see merge.h).
 *
 *   hv_merge -i jp2file[,jp2file...] [jp2file ...] -o jpxfile [-links]
 *            [--validate | --transcode [-p W,H]] [-s argfile]
 *
 * Options use exact names and separate values. Repeated -i names accumulate;
 * -s inserts the words of a file at that position ("-" for standard
 * input), split as split_words describes; a -s in the file is a usage error.
 * -h or --help anywhere on the command line prints help before any -s file
 * is read.
 *
 * The output replaces the file named (hv_file): an existing output keeps
 * its permissions, and one that is a symbolic link is written where the
 * link points. Exit status: 0 on success (and for -h), 1 on error, 2 on
 * usage errors. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tools/hv_cli.h"
#include "tools/hv_file.h"
#include "merge.h"

typedef struct {
    char **v;
    size_t n, cap;
} list;

static int push(list *l, char *s) {
    if (l->n == l->cap) {
        size_t cap = l->cap ? 2 * l->cap : 64;
        char **v = realloc(l->v, cap * sizeof *v);
        if (v == NULL)
            return -1;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = s;
    return 0;
}

static void usage(FILE *f) {
    fprintf(f, "usage: hv_merge -i jp2file[,jp2file...] [jp2file ...] -o jpxfile "
               "[-links] [--validate | --transcode [-p W,H]] [-s argfile]\n");
}

static void help(void) {
    usage(stdout);
    printf("\nMerge JP2 files into JPX files.\n\n"
           "options:\n"
           "  -h, --help   show this help message and exit\n"
           "  -i FILE...   input JP2 filenames, comma or space separated; may repeat\n"
           "  -o FILE      output JPX filename; required exactly once\n"
           "  -links       record links rather than actual codestream data\n"
           "  --validate   validate the complete codestream, including PLT entries\n"
           "  --transcode  transcode every input in memory to RPCL with PLT, then embed it\n"
           "  -p W,H       precinct powers of 2 from 2 to 32,768 (default 128,128)\n"
           "  -s FILE      insert the words of FILE (- for stdin), with quotes and\n"
           "               backslash escapes (see merge/README.md); no -s inside\n"
           "\nUse exact option names and separate values; -p requires --transcode.\n"
           "--transcode cannot be combined with -links or --validate.\n");
}

static int blank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* The words of `text`, separated by whitespace: backslash escapes any
 * character outside quotes, single quotes keep everything, double quotes
 * let backslash escape only a double quote or a backslash. Words point into
 * `text`, which is rewritten. -1 on an unterminated quote or a final
 * backslash, -2 out of memory. */
static int split_words(char *text, list *words) {
    char *r = text, *w = text;
    while (*r) {
        char *word;
        while (blank(*r))
            r++;
        if (!*r)
            break;
        word = w;
        while (*r && !blank(*r)) {
            if (*r == '\\') {
                if (!r[1])
                    return -1;
                *w++ = r[1];
                r += 2;
            } else if (*r == '\'') {
                for (r++; *r && *r != '\''; )
                    *w++ = *r++;
                if (!*r++)
                    return -1;
            } else if (*r == '"') {
                for (r++; *r && *r != '"'; ) {
                    if (*r == '\\' && (r[1] == '"' || r[1] == '\\'))
                        r++;
                    *w++ = *r++;
                }
                if (!*r++)
                    return -1;
            } else {
                *w++ = *r++;
            }
        }
        if (*r)
            r++;                                 /* the separator */
        *w++ = 0;
        if (push(words, word) != 0)
            return -2;
    }
    return 0;
}

/* The whole of a file ("-": standard input) as a string, or NULL, with
 * *no_memory set when an allocation failed. A read error or a NUL byte,
 * which would end the text early, also gives NULL: either would leave a
 * prefix of the arguments and merge fewer frames than asked for. */
static char *read_text(const char *path, int *no_memory) {
    int in = strcmp(path, "-") == 0;
    FILE *f = in ? stdin : fopen(path, "rb");
    char *text = NULL;
    size_t n = 0, cap = 0, got;
    *no_memory = 0;
    if (f == NULL)
        return NULL;
    do {
        if (n + 4096 + 1 > cap) {
            char *t = realloc(text, cap = 2 * cap + 4096 + 1);
            if (t == NULL) {
                free(text);
                if (!in)
                    fclose(f);
                *no_memory = 1;
                return NULL;
            }
            text = t;
        }
        n += got = fread(text + n, 1, 4096, f);
    } while (got > 0);
    if (ferror(f) || memchr(text, 0, n) != NULL) {
        free(text);
        if (!in)
            fclose(f);
        return NULL;
    }
    if (!in)
        fclose(f);
    text[n] = 0;
    return text;
}

typedef struct {
    list inputs;
    const char *output;
    int links, validate, transcode, precincts, ppx, ppy;
} arguments;

/* 0 on success, 1 for help, -1 for usage, -2 for allocation failure. */
static int parse(char **argv, size_t argc, arguments *a) {
    size_t i;
    for (i = 0; i < argc; i++) {
        const char *option = argv[i];
        if (strcmp(option, "-h") == 0 || strcmp(option, "--help") == 0)
            return 1;
        if (strcmp(option, "-links") == 0)
            a->links = 1;
        else if (strcmp(option, "--validate") == 0)
            a->validate = 1;
        else if (strcmp(option, "--transcode") == 0)
            a->transcode = 1;
        else if (strcmp(option, "-i") == 0) {
            size_t first = a->inputs.n;
            while (i + 1 < argc && argv[i + 1][0] != '-')
                if (push(&a->inputs, argv[++i]) != 0)
                    return -2;
            if (a->inputs.n == first)
                return -1;
        } else if (strcmp(option, "-o") == 0 || strcmp(option, "-p") == 0) {
            if (i + 1 == argc || argv[i + 1][0] == '-')
                return -1;
            if (strcmp(option, "-o") == 0) {
                if (a->output != NULL)
                    return -1;
                a->output = argv[++i];
            } else {
                a->precincts = 1;
                if (hv_cli_precincts(argv[++i], &a->ppx, &a->ppy) != 0)
                    return -1;
            }
        } else
            return -1;
    }
    return 0;
}

typedef struct {
    char **names;
    const struct stat *output;  /* existing output, otherwise NULL */
} input_files;

/* An input mapped while hv_merge_files has it open: the file is opened
 * (without blocking, so that a FIFO fails at once), mapped and closed
 * again, and the mapping dropped on close. Mapped, not read: the checks
 * and a linked merge touch the headers only, and an embedded merge reads
 * the codestream once. An input that another process truncates meanwhile
 * raises SIGBUS when the lost bytes are read, which ends hv_merge; hv_file
 * removes the temporary output first, so the output is left as it was. */
static int map_input(void *context, size_t i, hv_merge_input *in, char *error,
                     size_t error_size) {
    const input_files *files = context;
    char **names = files->names;
    struct stat st;
    int fd;
    in->path = names[i];
    in->buf = NULL;
    in->size = 0;
    if ((fd = open(names[i], O_RDONLY | O_NONBLOCK)) < 0 || fstat(fd, &st) != 0) {
        snprintf(error, error_size, "cannot open %s: %s", names[i], strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(error, error_size, "cannot open %s: not a regular file", names[i]);
        close(fd);
        return -1;
    }
    /* Reuse the open file's identity rather than stat every input separately. */
    if (files->output != NULL && st.st_dev == files->output->st_dev &&
        st.st_ino == files->output->st_ino) {
        snprintf(error, error_size,
                 "%s is also the output: hv_merge cannot replace an input file",
                 names[i]);
        close(fd);
        return -1;
    }
    in->size = (size_t)st.st_size;
    if (in->size == 0) {
        in->buf = (const uint8_t *)"";
    } else {
        void *p = mmap(NULL, in->size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            snprintf(error, error_size, "cannot map %s: %s", names[i], strerror(errno));
            close(fd);
            return -1;
        }
        in->buf = p;
    }
    close(fd);
    return 0;
}

static void unmap_input(void *context, size_t i, hv_merge_input *in) {
    (void)context;
    (void)i;
    if (in->size != 0)
        munmap((void *)in->buf, in->size);
}

/* The JPX file, written in place of `output` (hv_file): an existing
 * output keeps its permissions, and a symbolic link its target, as when
 * hvJP2K opens the output for writing. No merge may replace an input,
 * through any name or link. */
static int merge(char **names, size_t n, const char *output, int links, int validate, int ppx, int ppy, char *error,
                 size_t error_size) {
    struct stat out;
    input_files files = {names, NULL};
    hv_merge_inputs inputs = {map_input, unmap_input, &files};
    hv_file f;

    if (stat(output, &out) == 0)
        files.output = &out;
    if (hv_file_create(&f, output, HV_FILE_KEEP_MODE, error, error_size) != 0)
        return -1;
    if (hv_merge_files(&inputs, n, links, validate ? HV_OUTPUT_JPIP : HV_OUTPUT_JPEG2000, ppx, ppy, f.file, error, error_size) != 0) {
        hv_file_abort(&f);
        return -1;
    }
    return hv_file_commit(&f, error, error_size);
}

int main(int argc, char **argv) {
    arguments a;
    list names = {NULL, 0, 0}, texts = {NULL, 0, 0}, all = {NULL, 0, 0};
    char error[4096];
    size_t i;
    int status;

    memset(&a, 0, sizeof a);
    a.ppx = a.ppy = 7;
    for (i = 1; i < (size_t)argc; i++)
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            help();
            return 0;
        }
    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            char *text;
            size_t first = all.n, j;
            int no_memory;
            if (i + 1 == (size_t)argc ||
                (argv[i + 1][0] == '-' && strcmp(argv[i + 1], "-") != 0))
                goto usage;
            if ((text = read_text(argv[++i], &no_memory)) == NULL) {
                if (no_memory)
                    goto oom;
                fprintf(stderr, "hv_merge: cannot read the arguments in %s\n", argv[i]);
                status = 1;
                goto done;
            }
            if (push(&texts, text) != 0) {
                free(text);
                goto oom;
            }
            if ((status = split_words(text, &all)) != 0) {
                if (status == -2)
                    goto oom;
                fprintf(stderr, "hv_merge: cannot read the arguments in %s\n", argv[i]);
                status = 1;
                goto done;
            }
            for (j = first; j < all.n; j++)
                if (strcmp(all.v[j], "-s") == 0)
                    goto usage;
        } else if (push(&all, argv[i]) != 0)
            goto oom;
    }
    if ((status = parse(all.v, all.n, &a)) != 0)
        goto parsed;
    if ((a.transcode && (a.links || a.validate)) || (a.precincts && !a.transcode))
        goto usage;
    if (a.inputs.n == 0 || a.output == NULL)
        goto usage;
    for (i = 0; i < a.inputs.n; i++) {
        char *p = a.inputs.v[i], *comma;
        for (;;) {
            if ((comma = strchr(p, ',')) != NULL)
                *comma = 0;
            if (*p && push(&names, p) != 0)
                goto oom;
            if (comma == NULL)
                break;
            p = comma + 1;
        }
    }
    status = merge(names.v, names.n, a.output, a.links, a.validate,
                   a.transcode ? a.ppx : 0, a.transcode ? a.ppy : 0, error, sizeof error) != 0;
    if (status != 0)
        fprintf(stderr, "hv_merge: %s\n", error);
    goto done;

parsed:
    if (status == 1) {
        help();
        status = 0;
        goto done;
    }
    if (status == -1)
        goto usage;
oom:
    fprintf(stderr, "hv_merge: out of memory\n");
    status = 1;
    goto done;
usage:
    usage(stderr);
    status = 2;
done:
    free(names.v);
    for (i = 0; i < texts.n; i++)
        free(texts.v[i]);
    free(texts.v);
    free(all.v);
    free(a.inputs.v);
    return status;
}
