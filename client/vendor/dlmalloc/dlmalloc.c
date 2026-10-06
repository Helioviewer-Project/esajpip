// This file is a wrapper around malloc.c, which is the upstream source file.
// It sets configuration flags and controls which symbols are exported.

#include <stddef.h>
#include <malloc.h>
#include <stdint.h>
#include <unistd.h>

static void *wasm_morecore(intptr_t increment);
#define MORECORE wasm_morecore

// Define configuration macros for dlmalloc.

// WebAssembly doesn't have mmap-style memory allocation.
#define HAVE_MMAP 0

// WebAssembly doesn't support shrinking linear memory.
#define MORECORE_CANNOT_TRIM 1

// Disable sanity checks to reduce code size.
#define ABORT __builtin_unreachable()

// If threads are enabled, enable support for threads.
#ifdef _REENTRANT
#define USE_LOCKS 1
#endif

// Make malloc deterministic.
#define LACKS_TIME_H 1

// Disable malloc statistics generation to reduce code size.
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1

// Align malloc regions to 16, to avoid unaligned SIMD accesses.
#define MALLOC_ALIGNMENT 16

// Declare errno values used by dlmalloc. We define them like this to avoid
// putting specific errno values in the ABI.
extern const int __ENOMEM;
#define ENOMEM __ENOMEM
extern const int __EINVAL;
#define EINVAL __EINVAL

// Define USE_DL_PREFIX so that we leave dlmalloc's names prefixed with 'dl'.
// We define them as "static", and we wrap them with public names below. This
// serves two purposes:
//
// One is to make it easy to control which symbols are exported; dlmalloc
// defines several non-standard functions and we wish to explicitly control
// which functions are part of our public-facing interface.
//
// The other is to protect against compilers optimizing based on the assumption
// that they know what functions with names like "malloc" do. Code in the
// implementation will call functions like "dlmalloc" and assume it can use
// the resulting pointers to access the metadata outside of the nominally
// allocated objects. However, if the function were named "malloc", compilers
// might see code like that and assume it has undefined behavior and can be
// optimized away. By using "dlmalloc" in the implementation, we don't need
// -fno-builtin to avoid this problem.
#define USE_DL_PREFIX 1
#define DLMALLOC_EXPORT static inline

// This isn't declared with DLMALLOC_EXPORT so make it static explicitly.
static size_t dlmalloc_usable_size(void*);

// Include the upstream dlmalloc's malloc.c.
#include "malloc.c"

// Increase system-allocation granularity with the heap, keeping spare capacity
// below a quarter of its size. This bounds repeated memory.grow copies on
// engines that cannot reserve the full address range. Blocks keep exact sizes.
static void *wasm_morecore(intptr_t increment) {
    void *memory = sbrk(increment);
    if (increment > 0 && memory != (void *)-1) {
        size_t pages = __builtin_wasm_memory_size(0);
        size_t granularity_pages = 1;
        while (granularity_pages < pages / 8) granularity_pages *= 2;
        // Do not let spare-capacity growth prevent using the remaining wasm32
        // address space. At the limit, retain the one-page minimum.
        size_t remaining_pages = 65536 - pages;
        while (granularity_pages > remaining_pages && granularity_pages > 1)
            granularity_pages /= 2;
        mparams.granularity = granularity_pages * 65536;
    }
    return memory;
}

// Export the public names.

void *malloc(size_t size) {
    return dlmalloc(size);
}

void free(void *ptr) {
    dlfree(ptr);
}

void *calloc(size_t nmemb, size_t size) {
    return dlcalloc(nmemb, size);
}

void *realloc(void *ptr, size_t size) {
    return dlrealloc(ptr, size);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    return dlposix_memalign(memptr, alignment, size);
}

void* aligned_alloc(size_t alignment, size_t bytes) {
    return dlmemalign(alignment, bytes);
}

size_t malloc_usable_size(void *ptr) {
    return dlmalloc_usable_size(ptr);
}

// Define these to satisfy musl references.
void *__libc_malloc(size_t) __attribute__((alias("malloc")));
void __libc_free(void *) __attribute__((alias("free")));
void *__libc_calloc(size_t nmemb, size_t size) __attribute__((alias("calloc")));
