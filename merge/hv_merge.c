/* hv_merge: merges JP2 files into a JPX file for JHelioviewer and the
 * JPIP server, as hvJP2K's hv_jpx_merge does (see merge.h), with the same
 * command line, parsed as hvJP2K's argparse parser does:
 *
 *   hv_merge -i jp2file1,jp2file2,... [jp2file ...] -o jpxfile [-links]
 *                [-s argfile]
 *
 *   -i      input JP2 files, comma or space separated, in the JPX order
 *   -o      output JPX file
 *   -links  link the codestreams instead of copying them
 *   -s      read more arguments from a file, or standard input for "-"
 *           (split as Python's shlex.split does, quotes and backslashes
 *           included), then parse the command line and them again
 *   -h      print the usage
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
    fprintf(f, "usage: hv_merge -i jp2file1,jp2file2,... [jp2file ...] -o jpxfile "
               "[-links] [--validate] [-s argfile]\n");
}

static void help(void) {
    usage(stdout);
    printf("\nMerge JP2 files into JPX files.\n\n"
           "options:\n"
           "  -h, --help  show this help message and exit\n"
           "  -i          comma separated input JP2 filenames\n"
           "  -o          output JPX filename\n"
           "  -links      record links rather than actual codestream data\n"
           "  --validate  validate the complete codestream, including PLT entries\n"
           "  -s          read arguments from file\n");
}

/* Whitespace as shlex has it: space, tab, line feed and carriage return. */
static int blank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* The words of `text`, split as Python's shlex.split does: whitespace
 * separates them, backslash escapes any character outside quotes, single
 * quotes keep everything, double quotes let backslash escape only a double
 * quote or a backslash. Words point into `text`, which is rewritten. -1 on
 * an unterminated quote or a final backslash. */
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
            return -1;
    }
    return 0;
}

/* The whole of a file ("-": standard input, as hvJP2K's fileinput) as a
 * string, or NULL: also on a read error and on a NUL byte, which would end
 * the text early. Either would leave a prefix of the arguments, and merge
 * fewer frames than asked for. */
static char *read_text(const char *path) {
    int in = strcmp(path, "-") == 0;
    FILE *f = in ? stdin : fopen(path, "rb");
    char *text = NULL;
    size_t n = 0, cap = 0, got;
    if (f == NULL)
        return NULL;
    do {
        if (n + 4096 + 1 > cap) {
            char *t = realloc(text, cap = 2 * cap + 4096 + 1);
            if (t == NULL) {
                free(text);
                if (!in)
                    fclose(f);
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
    list inputs;            /* the -i words */
    const char *output, *argfile;
    int links, validate;
} arguments;

enum { OPT_NONE, OPT_HELP, OPT_I, OPT_O, OPT_S, OPT_LINKS, OPT_VALIDATE, OPT_UNKNOWN };

/* The option strings, as hvJP2K's argparse parser has them. */
static const struct {
    const char *name;
    int option;
} options[] = {
    {"-h", OPT_HELP}, {"--help", OPT_HELP}, {"-i", OPT_I}, {"-o", OPT_O},
    {"-links", OPT_LINKS}, {"-s", OPT_S}, {"--validate", OPT_VALIDATE},
};
#define NOPTIONS (sizeof options / sizeof *options)

static int starts_with(const char *s, const char *prefix, size_t n) {
    return strncmp(s, prefix, n) == 0;
}

/* A number as argparse's negative-number pattern has it: -N, -N.N or -.N. */
static int negative_number(const char *word) {
    const char *p = word + 1;
    int digits = 0;
    while (*p >= '0' && *p <= '9')
        p++, digits = 1;
    if (*p == 0)
        return digits;
    if (*p++ != '.' || *p == 0)
        return 0;
    while (*p >= '0' && *p <= '9')
        p++;
    return *p == 0;
}

/* What argparse makes of a word (_parse_optional): OPT_NONE for a value
 * (not starting with "-", "-" alone, a negative number, or a word with a
 * space that names no option), OPT_UNKNOWN for any other word starting
 * with "-", otherwise the option it names: in full, as a prefix of it
 * ("-lin", "--he"), with a value after "=" ("-o=file.jpx", *equals then
 * set), or as a one-letter option with a value attached ("-ofile.jpx").
 * *value is the value, or NULL. No word is a prefix of two options. */
static int classify(char *word, char **value, int *equals) {
    char *eq = strchr(word, '=');
    size_t i, n = strlen(word);
    *value = NULL;
    *equals = 0;
    if (word[0] != '-')
        return OPT_NONE;
    for (i = 0; i < NOPTIONS; i++)
        if (strcmp(word, options[i].name) == 0)
            return options[i].option;
    if (n == 1)
        return OPT_NONE;
    if (eq != NULL)
        for (i = 0; i < NOPTIONS; i++)
            if (strlen(options[i].name) == (size_t)(eq - word) &&
                starts_with(word, options[i].name, (size_t)(eq - word))) {
                *value = eq + 1;
                *equals = 1;
                return options[i].option;
            }
    for (i = 0; i < NOPTIONS; i++) {
        const char *name = options[i].name;
        if (word[1] != '-') {
            if (strlen(name) == 2 && starts_with(word, name, 2)) {
                *value = word + 2;
                return options[i].option;
            }
            if (starts_with(name, word, n))
                return options[i].option;
        } else {
            size_t prefix = eq != NULL ? (size_t)(eq - word) : n;
            if (starts_with(name, word, prefix)) {
                *value = eq != NULL ? eq + 1 : NULL;
                *equals = eq != NULL;
                return options[i].option;
            }
        }
    }
    if (negative_number(word) || strchr(word, ' ') != NULL)
        return OPT_NONE;
    return OPT_UNKNOWN;
}

/* Nonzero if argv[i] is a value, not an option. */
static int is_value(char **argv, size_t i) {
    char *value;
    int equals;
    return classify(argv[i], &value, &equals) == OPT_NONE;
}

/* Option -i, -o or -s with its value, attached or in the next words, as
 * argv[*i] ends: -i takes the words up to the next one that is not a
 * value (one, if attached), -o and -s one word. 0, -1 on a usage error, -2
 * out of memory. */
static int take(int option, char *value, char **argv, size_t argc, size_t *i, arguments *a) {
    if (option == OPT_I) {
        a->inputs.n = 0;
        if (value != NULL)
            return push(&a->inputs, value) != 0 ? -2 : 0;
        while (*i + 1 < argc && is_value(argv, *i + 1))
            if (push(&a->inputs, argv[++*i]) != 0)
                return -2;
        return a->inputs.n == 0 ? -1 : 0;
    }
    if (value == NULL) {
        if (*i + 1 == argc || !is_value(argv, *i + 1))
            return -1;
        value = argv[++*i];
    }
    if (option == OPT_O)
        a->output = value;
    else
        a->argfile = value;
    return 0;
}

/* Parses argv as argparse does for hvJP2K's options: an option's value is
 * attached to it or in the next words, which must not look like options;
 * a later occurrence of an option replaces an earlier one; a word that is
 * no option's value, or an unknown option, is an error once every word is
 * read, so that -h after it still asks for help. A value attached to -h
 * without "=" is read as more one-letter options ("-hi a.jp2" is -h -i
 * a.jp2); -h is then taken once they are. 0; 1 for -h; -1 on a usage
 * error; -2 out of memory. */
static int parse(char **argv, size_t argc, arguments *a) {
    char *value;
    size_t i;
    int unrecognized = 0, option, equals, status;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            return -1;      /* argparse: the rest are values, which no option takes */
        option = classify(argv[i], &value, &equals);
        switch (option) {
        case OPT_HELP:
            if (value == NULL)
                return 1;
            if (equals || value[0] == '-')  /* argparse: ignored explicit argument */
                return -1;
            /* -hX...: -X with the rest attached, if X names an option. */
            option = value[0] == 'i' ? OPT_I : value[0] == 'o' ? OPT_O
                   : value[0] == 's' ? OPT_S : OPT_HELP;
            if (option == OPT_HELP)
                return 1;
            value = value[1] == '=' ? value + 2 : value[1] != 0 ? value + 1 : NULL;
            status = take(option, value, argv, argc, &i, a);
            return status != 0 ? status : 1;
        case OPT_VALIDATE:
            if (value != NULL)
                return -1;
            a->validate = 1;
            break;
        case OPT_LINKS:
            if (value != NULL)          /* only as "-links=": ignored explicit argument */
                return -1;
            a->links = 1;
            break;
        case OPT_I:
        case OPT_O:
        case OPT_S:
            if ((status = take(option, value, argv, argc, &i, a)) != 0)
                return status;
            break;
        default:
            unrecognized = 1;
            break;
        }
    }
    return unrecognized ? -1 : 0;
}

typedef struct {
    char **names;
    const struct stat *output;  /* existing linked output, otherwise NULL */
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
                 "%s is also the output: a linked JPX file cannot replace a file it links to",
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
 * hvJP2K opens the output for writing. A linked JPX file cannot replace
 * one of its inputs (the same file, through any name or link), which it
 * would link to; an embedded one can. */
static int merge(char **names, size_t n, const char *output, int links, int validate, char *error,
                 size_t error_size) {
    struct stat out;
    input_files files = {names, NULL};
    hv_merge_inputs inputs = {map_input, unmap_input, &files};
    hv_file f;

    if (links && stat(output, &out) == 0)
        files.output = &out;
    if (hv_file_create(&f, output, HV_FILE_KEEP_MODE, error, error_size) != 0)
        return -1;
    if (hv_merge_files(&inputs, n, links, validate ? HV_OUTPUT_JPIP : HV_OUTPUT_JPEG2000, f.file, error, error_size) != 0) {
        hv_file_abort(&f);
        return -1;
    }
    return hv_file_commit(&f, error, error_size);
}

int main(int argc, char **argv) {
    arguments a;
    list names = {NULL, 0, 0}, extra = {NULL, 0, 0}, all = {NULL, 0, 0};
    char *text = NULL, error[4096];
    size_t i;
    int status;

    memset(&a, 0, sizeof a);
    if ((status = parse(argv + 1, (size_t)argc - 1, &a)) != 0)
        goto parsed;
    /* hvJP2K reads the file only when -s names one (an empty name does
     * not), and only once: a -s among its words is parsed and ignored. */
    if (a.argfile != NULL && a.argfile[0] != 0) {
        /* argparse's reparse: the command line, then the file's words. */
        if ((text = read_text(a.argfile)) == NULL || split_words(text, &extra) != 0) {
            fprintf(stderr, "hv_merge: cannot read the arguments in %s\n", a.argfile);
            status = 1;
            goto done;
        }
        for (i = 1; i < (size_t)argc; i++)
            if (push(&all, argv[i]) != 0)
                goto oom;
        for (i = 0; i < extra.n; i++)
            if (push(&all, extra.v[i]) != 0)
                goto oom;
        free(a.inputs.v);
        memset(&a, 0, sizeof a);
        if ((status = parse(all.v, all.n, &a)) != 0)
            goto parsed;
    }
    if (a.inputs.n == 0 || a.output == NULL)
        goto usage;
    /* Names are split at commas, empty ones dropped (hvJP2K is lenient
     * with stray commas); none left fails as hvJP2K does, with "no JP2
     * input files". */
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
    status = merge(names.v, names.n, a.output, a.links, a.validate, error, sizeof error) != 0;
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
    free(extra.v);
    free(all.v);
    free(a.inputs.v);
    free(text);
    return status;
}
