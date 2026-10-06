# WASI dlmalloc

`malloc.c` is unchanged. `dlmalloc.c` is the wrapper from
[WebAssembly/wasi-libc](https://github.com/WebAssembly/wasi-libc/tree/165235bc467d5fa52d424f5d82587dfb76ed9d54/dlmalloc/src),
commit `165235bc467d5fa52d424f5d82587dfb76ed9d54`.

Only the WASM target links this allocator. The wrapper configures 16-byte
alignment, disables mmap and memory trimming, and uses WASI libc's `sbrk`
for linear-memory growth. The local `MORECORE` hook updates system-allocation
granularity after each growth: a power of two between one eighth and one quarter
of current linear memory, with a 64 KiB minimum and capped by remaining wasm32
address space. Individual block sizes retain
upstream dlmalloc behavior. Native targets retain their platform allocator.

A local JavaScriptCore shell allocation test with nine live instances and 64 MiB
of 32 KiB blocks per instance reduced growth events from 9,225 to 243. Linear
memory per instance rose from 65.4 MiB to 80.5 MiB. This measures growth behavior;
application decode time also includes OpenJPEG and reconstruction.

Doug Lea's source carries its CC0 notice. WASI libc's additions are used
under the accompanying MIT license.
