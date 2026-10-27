/* opj_config_private.h: what OpenJPEG's CMake generates from
 * opj_config_private.h.cmake.in, written by hand for the vendored copy (see
 * README.md): Windows uses the CRT aligned allocator; macOS, Linux and
 * WebAssembly with a WASI libc have posix_memalign. Files are not read
 * through OpenJPEG. */
#define OPJ_PACKAGE_VERSION "2.5.4"

#if defined(_WIN32)
#define OPJ_HAVE__ALIGNED_MALLOC
#else
#define OPJ_HAVE_POSIX_MEMALIGN

#if !defined(_POSIX_C_SOURCE)
/* Get the declaration of posix_memalign. */
#define _POSIX_C_SOURCE 200112L
#endif
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define OPJ_BIG_ENDIAN
#endif
