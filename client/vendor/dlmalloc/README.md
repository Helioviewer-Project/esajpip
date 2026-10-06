# WASI dlmalloc

`dlmalloc.c` and `malloc.c` are unchanged copies from
[WebAssembly/wasi-libc](https://github.com/WebAssembly/wasi-libc/tree/165235bc467d5fa52d424f5d82587dfb76ed9d54/dlmalloc/src),
commit `165235bc467d5fa52d424f5d82587dfb76ed9d54`.

Only the WASM target links this allocator. The wrapper configures 16-byte
alignment, disables mmap and memory trimming, and uses WASI libc's `sbrk`
for linear-memory growth. Native targets retain their platform allocator.

Doug Lea's source carries its CC0 notice. WASI libc's additions are used
under the accompanying MIT license.
