# Vendored OpenJPEG

The JPEG 2000 library of [OpenJPEG](https://www.openjpeg.org/) 2.5.4,
released in September 2025: the decoder the client uses. It is kept here so
that the client, and its WebAssembly build, do not depend on whichever
OpenJPEG version a system packages.

- Source: <https://github.com/uclouvain/openjpeg>, tag `v2.5.4`, commit
  `6c4a29b00211eb0430fa0e5e890f1ce5c80f409f`.
- The 22 `.c` and 29 `.h` files are those of `src/lib/openjp2/` that upstream
  builds into `libopenjp2` without its JPIP option. The local callback fix below
  changes `openjpeg.c`; decoder performance changes are listed below.
- `opj_config.h` and `opj_config_private.h` are not upstream files: upstream
  generates them with CMake. They are written by hand here.
  The private configuration selects the Windows CRT aligned allocator on
  Windows and `posix_memalign` elsewhere. `opj_malloc.c` includes `<malloc.h>`
  for the Windows allocator declarations.
- `LICENSE` is from the upstream root. OpenJPEG is distributed under the
  2-clause BSD license.

The `esajpip_client_wasm` target compiles it for WebAssembly or native decoding
tests. The core `esajpip_client` target does not depend on it.
`opj_clock.c` is not compiled: the library does not call it, and it does not
build for WebAssembly.

## Decoder optimizations

Local changes touch `dwt.c`, `t1.c`, `t2.c`, `tcd.c`, `tcd.h`, `tgt.c`, `thread.c`,
`mqc.c`, `mqc.h`, `mqc_inl.h` and `openjpeg.c` and add
`wasm-dwt-simd.h`, `neon-dwt-simd.h` and `decoder_header.h`.
`decoder_header.h` declares the scalar main-header coding accessor implemented
in `openjpeg.c`, avoiding a diagnostic codestream-info allocation.
`t1_cache.c` owns the bounded Tier-1 cache.
`decoder_workspace.c` owns its lifetime, the persistent thread pool, and up to
64 MiB of reusable aligned coefficient buffers. Its opaque local interface in
`decoder_workspace.h` keeps private codec and tile access inside OpenJPEG.
`tcd.c` splits the DC shift, rounding and clamping of decoded samples into row
bands on the decoder's thread pool and rounds irreversible samples with
WebAssembly SIMD, SSE2 or NEON, with the same results as `lrintf`.
For a supported whole-tile grayscale decode, the `gray8` and `gray8_compno`
fields in `tcd.h` select caller-owned byte output. The DC shift packs that
selected plane directly, bypassing its ordinary integer sample output.
The buffer must remain writable through decode completion; see
`opj_decoder_gray8_output` in `decoder_workspace.h` for the lifetime contract.
`neon-dwt-simd.h` maps the DWT's SSE kernels to AArch64 NEON, as
`wasm-dwt-simd.h` does for WebAssembly, and `dwt.c` transposes four rows at a
time with SSE or NEON as it does with WebAssembly SIMD.
The irreversible vertical pass loads and stores four 8-column strips row by
row, so that rows a power-of-two stride apart do not refetch each cache line.
For rows at least 16,384 samples wide, the horizontal 9/7 pass runs lifting
steps in chunks while loading the row buffer.
They preserve scalar tails and floating-point operation order;
compile without fast-math or fused multiply-add (`-ffp-contract=off`).
`tgt.c` allocates each tag tree and its initial nodes together. Larger replacement
node arrays use separate allocations, with ordinary resize and destruction.
The decoder creates absent trees only when a packet header is nonempty. Existing
trees are still reset and resized for each tile; the encoder creates them eagerly.
`mqc.c`, `mqc.h` and `mqc_inl.h` use packed decoder states and a 64-bit
reservoir; PTERM checks reconcile buffered input through `opj_mqc_dec_unread`.
`thread.c` lets the coordinator run queued jobs while waiting or under queue
backpressure. Tier-1 isolates compressed input whenever any worker exists,
because the coordinator and that worker can decode adjacent blocks together.
`t2.c` keeps its packet-header bit reader on the stack and defers tag trees
until a nonempty packet needs them.
The workspace binds its pool to a raw J2K decoder and drains and restores the
original pool before codec destruction. It stays active on the coordinator
through image and codec destruction so cached coefficient buffers are returned
to the workspace. Calls on one workspace must be serialized. Without an active
workspace, image allocation uses the ordinary aligned allocator.

## Updating

Keep local changes documented here. `openjpeg.c` uses exact-type callbacks
for the raw J2K decoder calls reached by `hvc_openjpeg_decode`: setup, header
read, codestream information, resolution/region selection, decode, end and destroy.
The J2K encoder setup, start, encode and end calls used by `client_image` have
the same adapters.
Each adapter converts the opaque handle through an ordinary C call instead
of calling a typed function through an incompatible function pointer. The
original dispatch triggers Clang function-type UBSan even on valid inputs.
JP2 and unused codec operations retain upstream dispatch and are not covered
by this fix. No sanitizer checks are disabled.

To change version, replace the files wholesale from a release tag, reapply or
retire the callback and decoder changes after testing, refresh `LICENSE`, and update the version in
the two configuration headers and the version, date and commit above. To check
a copy against an upstream checkout:

```sh
for f in client/vendor/openjpeg/*.[ch]; do
    case $f in */opj_config*.h|*/wasm-dwt-simd.h|*/neon-dwt-simd.h|*/decoder_header.h|*/decoder_workspace.[ch]|*/t1_cache.[ch]) continue;; esac
    cmp $f /path/to/openjpeg/src/lib/openjp2/$(basename $f)
done
```

`NOTICES` preserves the leading copyright and license comments from the vendored
C sources and headers, together with `LICENSE`. Update it when updating the vendor
sources. The WASM web output includes it as `OpenJPEG-NOTICES.txt`.
