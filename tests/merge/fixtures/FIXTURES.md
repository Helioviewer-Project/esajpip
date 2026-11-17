# Merge fixtures

`input/` holds two JP2 files from hvJP2K's JPX tests
(`hvJP2K/jpx/test/swap-ref/000.jp2` and `001.jp2`, renamed `swap_000.jp2`
and `swap_001.jp2`): 1,024×1,024, one 8-bit component with a 256-entry
palette (`pclr`, `cmap`), FITS headers as XML, and trailing zero PLT
entries.

`expected/merged.jpx` is the output below with a zero-filled `free` box
inserted after `rreq`, making their combined length 44 bytes. All other
bytes are what hvJP2K's `hv_jpx_merge` (commit 473de1a)
made of these two files followed by the six Kakadu references of the
transcoder's tests that are within the served profile
(`../../transcode/fixtures/kakadu/`, in name order, without the
origin-129 file):

    hv_jpx_merge -i fixtures/input/swap_000.jp2 fixtures/input/swap_001.jp2 \
        ../../transcode/fixtures/kakadu/2015_12_21__00_10_34_34__SDO_AIA_AIA_171.jp2 \
        ../../transcode/fixtures/kakadu/solo_fsi174_127x129_RLCP_PLT.jp2 \
        ../../transcode/fixtures/kakadu/solo_fsi174_509x513_PCRL.jp2 \
        ../../transcode/fixtures/kakadu/solo_fsi174_510x514_LRCP.jp2 \
        ../../transcode/fixtures/kakadu/solo_fsi174_511x513_LRCP_PLT.jp2 \
        ../../transcode/fixtures/kakadu/synthetic_rgb_129x129_CPRL_SOP_EPH.jp2 \
        -o fixtures/expected/merged.jpx

The set covers the JPX boxes the merge writes: the second file has the
first file's JP2 Header box (empty `jpch` and `jplh`); the others differ
in size and components, have no palette where the first has one (a
generated `cmap`), and a different color specification (`cgrp`); every
file carries XML (`asoc`, `nlst`). The first file's `colr` has APPROX 0,
which the JPX file writes as 1.

A linked merge names the inputs by absolute path, so it is not a fixture:
the tests compare it with this one box by box.
