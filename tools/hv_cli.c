#include <errno.h>
#include <stdlib.h>

#include "hv_cli.h"

static int exponent(const char *s, char **end) {
    long v;
    int e = 0;
    if (*s < '0' || *s > '9')
        return -1;
    errno = 0;
    v = strtol(s, end, 10);
    if (errno != 0 || *end == s || v < 2 || v > 32768 || (v & (v - 1)) != 0)
        return -1;
    while ((1L << e) < v)
        e++;
    return e;
}

int hv_cli_precincts(const char *value, int *ppx, int *ppy) {
    char *end;
    int x = exponent(value, &end), y;
    if (x < 0 || *end != ',' || (y = exponent(end + 1, &end)) < 0 || *end)
        return -1;
    *ppx = x;
    *ppy = y;
    return 0;
}
