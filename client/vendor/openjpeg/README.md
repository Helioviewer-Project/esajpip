# Vendored OpenJPEG

The JPEG 2000 library of [OpenJPEG](https://www.openjpeg.org/) 2.5.4,
released in September 2025: the decoder the client uses. It is kept here so
that the client, and its WebAssembly build, do not depend on whichever
OpenJPEG version a system packages.

- Source: <https://github.com/uclouvain/openjpeg>, tag `v2.5.4`, commit
  `6c4a29b00211eb0430fa0e5e890f1ce5c80f409f`.
- The 22 `.c` and 29 `.h` files are those of `src/lib/openjp2/` that upstream
  builds into `libopenjp2` without its JPIP option. The local callback fix below
  changes `openjpeg.c`; the other upstream files are unmodified.
- `opj_config.h` and `opj_config_private.h` are not upstream files: upstream
  generates them with CMake. They are written by hand here.
- `LICENSE` is from the upstream root. OpenJPEG is distributed under the
  2-clause BSD license.

The `esajpip_client_wasm` target compiles it for WebAssembly or native decoding
tests. The core `esajpip_client` target does not depend on it.
`opj_clock.c` is not compiled: the library does not call it, and it does not
build for WebAssembly.

## Updating

Keep local changes documented here. `openjpeg.c` uses exact-type callbacks
for the raw J2K decoder calls reached by `hvc_openjpeg_decode`: setup, header
read, codestream information, resolution selection, decode, end and destroy.
Each adapter converts the opaque handle through an ordinary C call instead
of calling a typed function through an incompatible function pointer. The
original dispatch triggers Clang function-type UBSan even on valid inputs.
JP2 and unused codec operations retain upstream dispatch and are not covered
by this fix. No sanitizer checks are disabled.

To change version, replace the files wholesale from a release tag, reapply or
retire that fix after testing upstream dispatch, refresh `LICENSE`, and update the version in
the two configuration headers and the version, date and commit above. To check
a copy against an upstream checkout:

```sh
for f in client/vendor/openjpeg/*.[ch]; do
    case $f in */opj_config*.h) continue;; esac
    cmp $f /path/to/openjpeg/src/lib/openjp2/$(basename $f)
done
```
