/* hv_error.h: formatting an error message into a caller's buffer, for the
 * functions that report errors as text (hv_geometry, and the tools'
 * transcode, tier2 and merge code), and the numbers in it. */
#ifndef HV_ERROR_H
#define HV_ERROR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes the message, as vsnprintf formats it, to error (size bytes, NUL
 * terminated; nothing when size is 0), and returns -1, so that a caller
 * can write `return hv_fail(error, size, ...);`. */
int hv_fail(char *error, size_t size, const char *format, ...)
#if defined(__MINGW32__)
    __attribute__((format(gnu_printf, 3, 4)))
#elif defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* n with thousands separators, as messages state limits ("65,536"), in
 * out; returns out. */
const char *hv_grouped(uint64_t n, char out[32]);

#ifdef __cplusplus
}
#endif

#endif
