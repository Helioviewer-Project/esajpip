# JPEG 2000 file model and test-vector corpus

## Start here

esajpip reads JPEG 2000 files (`.jp2`, `.jpx`) and serves pieces of them to
JHelioviewer over JPIP. `jpip/index/image_index.cc` uses the shared reader
in `jpeg2000/hv_reader.c`; `server/storage/file_manager.cc` opens and maps the sources.
The reader has to reject malformed input without
crashing, and it has to accept exactly the files the project promises to
support ([`JPIP_PROFILE.md`](../JPIP_PROFILE.md), "Supported JPEG 2000
sources").

This directory is a **formal description of what a valid file looks like**,
written in a machine-readable notation (ASN.1 with ACN encoding rules), plus
a small program that turns that description into a **corpus of test files**:
hundreds of tiny `.jp2`/`.jpx` files, each labeled "the server must
accept this" or "the server must reject this". `tests/server/file_manager_test.cc` then
opens every one and checks that the server agrees, and
`tests/jpeg2000/test_profile.c` does the same for the reader,
which shares most of the cross-field rules with the harness
(`../jpeg2000/hv_rules.c`).

The point of doing it this way rather than writing test files by hand:

- The labels are evaluated mechanically from the model and its cross-field
  rules rather than assigned to individual vectors by hand. The model and
  rules are still human-authored and must be checked against the cited
  standards. Every field of every marker segment and box gets a test at each
  edge of its allowed range, systematically, which nobody does by hand.
- The description exists in two layers — *what the standard allows* and
  *what esajpip supports* — so a test failure tells you which of the two
  the parser disagrees with, and cites the clause of the standard.
- The description is the specification. [`JPIP_PROFILE.md`](../JPIP_PROFILE.md)
  says in prose what the server accepts; `spec/*.asn1` says it in a form a
  test can prove.

The ASN.1 compiler runs offline, on a developer machine, only when the
description changes; it is never part of the build. The server tests consume
only the committed corpus through plain CMake/CTest. The server now links the
reader/writer library and its committed generated header decoders through
`jpip`. `jpeg2000/generate.sh` generates the header types of `jpeg2000-io.asn1`,
the profile types the reader checks against, and the box types and list
elements it decodes one at a time (the script's `pdus` list) into
`jpeg2000/generated/`, which is committed and built by the normal CMake build.

**Status.** With the upstream compiler revision pinned here, the complete
model generates C, that C builds as strict C11, and the sanitized harness
writes the corpus with unique names and no label mismatches. `spec/VERSION`
pins unmodified upstream ASN1SCC 4.9.7.0. The eleven local fixes for
issue #417 are archived under `spec/asn1scc-patches/reference/161cc246/`; no
patches are applied. The corpus is committed under `tests/vectors/j2k/`, and `file_manager_test.cc` checks every
manifest row against the server parser and lazy packet indexer.

If you are here to **run the current server tests**, use `./tests/run.sh` and
the [test guide](../tests/README.md). You need nothing from this directory.
If you are here to **change what the server accepts**, read
"Background", then "Quick start", then edit the model and regenerate. If a
corpus test **just failed**, go to "When a vector fails".

## Scope and development order

This suite is intended to establish that esajpip accepts the JPEG 2000 served
profile it documents and rejects malformed or unsupported structures for the
right reason. It can provide strong evidence for the parts of T.800 and T.801
that the server parses: boxes, marker framing, coding parameters, PLT packet
lengths, tile-part structure, fragment tables, data references, and the
cross-field relationships between them. The separate standard and profile
layers also make an intentional reduction or leniency visible instead of
silently treating current parser behavior as the standard.

It is not a complete JPEG 2000 conformance suite. In particular, it does not
validate entropy-coded packet contents, inverse transforms, color processing,
decoded samples, rendering, or every legal JPX organization. esajpip does not
perform those operations. A passing corpus means that the server agrees
with the modeled structural rules and its declared served profile; it does
not mean that an arbitrary JPEG 2000 decoder is conformant.

The corpus and the server tests cover progression order independently,
PLT and tile-part boundaries, linked-file reference graphs and fragment
extents, opaque metadata, and alternate box encodings; the alternate
encodings use explicit server fixtures, as the ACN model cannot express
their wire form. Further extensions should follow this order when a
concrete need justifies them:

1. Keep the deferred-ACN compiler regressions passing and the compiler
   revision pinned in `spec/VERSION`. Build the generated code as strict
   C11 and run the harness in Linux Docker with AddressSanitizer and
   UndefinedBehaviorSanitizer whenever the model changes. Do not hand-edit
   generated output or corpus labels.
2. Expand the highest-value production paths first: packet indexing across
   progression orders, layers, precincts, PLT segments and tile-parts, followed
   by complete linked-JPX graphs and their companion JP2 files.
3. Then model additional valid-but-unsupported forms such as `Psot = 0`,
   `LBox = 0`, XLBox, deeper association trees, Multiple Codestream boxes, and
   more general JPX layouts. The result may be an explicit profile rejection;
   modeling a form does not oblige the server to support it.
4. Keep extensions to T.808 requests, JPIP channel state, JPP-stream framing,
   hvJP2K output, and decoder interoperability as separate test layers. Reuse
   this corpus where useful, but do not make the source-file model responsible
   for HTTP, session, or image-decoding behavior.

Every new rule needs at least one accepted vector at or near its boundary and
one rejected vector that violates only that rule. Each must cite the applicable
standard clause, state whether it belongs to the standard or profile
layer, and exercise `GetPacket` when rejection can occur during lazy indexing.
Independent tools such as jpylyzer, OpenJPEG, Kakadu, or Grok are useful for
comparison, but none replaces the cited standard plus the model as the expected
result.

## Background you need

### A JPEG 2000 file, in one picture

A `.jp2` or `.jpx` file is a sequence of **boxes**. A box is a 4-byte length
`LBox`, a 4-byte type (four ASCII characters), and a payload. Some boxes
(superboxes) contain more boxes. One box type, `jp2c`, contains a
**codestream**, which is the actual compressed image.

```
.jp2 file
├── jP      signature box (must be first; contents 0D 0A 87 0A)
├── ftyp    file type box (must be second)
├── jp2h    superbox: ihdr (image size), colr (colour space), ...
└── jp2c    the codestream
            ├── SOC             marker: start of codestream
            ├── SIZ  segment    image and tile size, components
            ├── COD  segment    coding style: decomposition levels, precincts, ...
            ├── QCD  segment    quantization (esajpip skips its contents)
            ├── SOT  segment    start of a tile-part; Psot = its total length
            │   ├── PLT segment packet lengths (esajpip needs at least one)
            │   ├── SOD         marker: start of data
            │   └── ...         packet data, up to the Psot boundary
            └── EOC             marker: end of codestream
```

A codestream is a sequence of **marker segments**: a 2-byte marker code
(`FF51` = SIZ, `FF52` = COD, …), a 2-byte length `Lxxx` that counts itself,
and a body. The main header runs from SOC to the first SOT; then one or
more tile-parts; then EOC.

A `.jpx` movie for JHelioviewer usually has **no codestreams in it at all**:
it has one `jpch` box per frame, one `ftbl`/`flst` box per frame pointing
into a separate `.jp2` file (an `flst` "fragment" = file offset + length +
which file), and one `dtbl` box holding the `url` boxes that name those
files. That is the "linked JPX" form and the production workload.

The formal references are ITU-T T.800 (JPEG 2000 Part 1: Annex A is the
codestream, Annex I is the JP2 file format) and T.801 (Part 2: Annex M has
the JPX boxes). The model files cite the relevant table or clause for each
field.

The standard layer requires the Reader Requirements box that T.801 M.11.1
places immediately after `ftyp`, and the `MinV` a writer must set (0 for
JP2 and 1 for JPX; `file.ftyp-minor`). The profile layer accepts a missing
`rreq` and any `MinV`, because the server ignores both and T.800 I.5.2
requires readers to parse a file whatever its `MinV`. Generated base vectors
carry the conforming `MinV`, and the JPX bases a minimal `rreq` box, so they
are valid at both layers.

### ASN.1 and ACN, in ten lines

**ASN.1** is a notation for describing structured data: records
(`SEQUENCE`), one-of-several (`CHOICE`), lists (`SEQUENCE OF`), byte strings
(`OCTET STRING`), integers with allowed ranges (`INTEGER (1..16384)`). It
describes *what the values are*, not how they are laid out in bytes.

**ACN** is a companion notation, from ESA, that says *how the bytes are laid
out*: this integer is 16 bits big-endian; this list has as many elements as
that earlier field says; this field is present only when that flag is set;
this list fills whatever is left of the enclosing region (`size deduced`).
With ACN you can describe an existing binary format exactly, byte for byte.

**asn1scc** is ESA's compiler for both. From a `.asn1` file and its `.acn`
file it generates C: a struct per type, an encoder (struct → bytes), a
decoder (bytes → struct, refusing anything that does not match the layout),
and a constraint checker (does every integer sit in its allowed range?).
That generated decoder is what labels the corpus.

The two notations look like this (from `j2k-headers.asn1` / `.acn`):

```asn1
Sgcod ::= SEQUENCE {                    -- ASN.1: the fields and their ranges
    progression     INTEGER (0..4),     -- Table A.16: LRCP RLCP RPCL PCRL CPRL
    layers          INTEGER (1..65535),
    mct             INTEGER (0..1)
}
```

```
Sgcod [] {                              -- ACN: the byte layout of the same fields
    progression     [encoding pos-int, size 8],
    layers          [encoding pos-int, size 16, endianness big],
    mct             [encoding pos-int, size 8]
}
```

### Vocabulary

| Term | Meaning here |
| --- | --- |
| **model** | The four `spec/*.asn1` files and their `*.acn` companions, together. |
| **layer 1 / standard** | Types with the ranges and rules of T.800/T.801. The model's `Iplt` is ten bytes at most, a corpus bound: the rules read a longer entry, for which T.800 sets no bound, and the profile takes ten. |
| **layer 2 / profile** | `*-Profile` types and rules: normally layer 1 narrowed to what esajpip serves, with explicit documented leniencies matching deployed server behavior. |
| **vector** | One generated `.jp2`/`.jpx` file in the corpus. |
| **label** | The vector's verdict at each layer: `valid` or `invalid`. |
| **base** | One of four canonical, valid files everything else is derived from. |
| **mutant** | A base with one deliberate change (a field out of range, a rule broken, a length off by one). |
| **cross-field rule** | A validity rule ASN.1 cannot state (e.g. "csiz equals the number of components"); listed at the end of each `.asn1`. Most are in `../jpeg2000/hv_rules.c`, which the harness (`harness/crossfield*.{h,c}`) and the reader share; the structural ones (segment placement and count, the `Zplt` sequence, PLT sums, the file's first boxes, some box counts) are implemented in both, under the same names. |
| **determinant** | An ACN field whose value controls the size or presence of another (e.g. `Lxxx` sizes the segment body). |
| **harness** | `spec/harness/`: the offline C program that builds and labels the corpus. |
| **manifest** | `tests/vectors/j2k/manifest.tsv`: one row per vector with its labels. |

The shared box-tree validator descends into at most `HV_BOX_DEPTH_MAX`
(32) nested superboxes, counting a top-level superbox as level 1. It checks
all children of the last supported level and returns `box.depth-limit`
for a deeper superbox. This means validation is incomplete, not that the
file violates T.800/T.801. The boolean harness label is non-valid with that
reason; it must not be interpreted as a standards defect. The same applies
to `siz.unsupported-profile` and `siz.unsupported-capabilities`: the
validator has not established conformance for that declaration. The recursion
limit is separate from the ASN.1 whole-file model's corpus bounds on list
and opaque-payload sizes. Depth-boundary regressions are constructed in
`tests/jpeg2000/test_reader.c`, outside the conformance-vector manifest.

## Files

| File | Role |
| --- | --- |
| `j2k-headers.asn1` / `.acn` | Marker segment bodies: SIZ, COD, QCD, PLT (with its packet-length entries, `Iplt`), COM, and the elements of the bodies the framing keeps opaque (COC, QCC, RGN, POC, TLM, PLM, PPM, PPT, CRG), which `../jpeg2000/hv_rules.c` reads one at a time. |
| `j2k-codestream.asn1` / `.acn` | Codestream framing: SOC, main header, tile-parts (SOT, tile headers, SOD, data), EOC. Imports the bodies. |
| `jp2-boxes.asn1` / `.acn` | JP2/JPX box tree; `jp2c` carries a full codestream; `jpch`/`ftbl`/`flst`/`dtbl`/`url`/`asoc` and the header boxes (`jp2h`/`jplh` with `ihdr`, `bpcc`, `colr`, `pclr`, `cmap`, `cdef`, `res`, `cgrp`), `uinf`, `ftyp` and `rreq` in full, other boxes opaque. Imports the codestream. |
| `jpeg2000-io.asn1` / `.acn` | The types only the reader/writer in `../jpeg2000/` needs: the box header (LBox, TBox, XLBox), a marker code, Lxxx, SOT, the Reader Requirements box in parts (`RreqHeader`, `RreqStandardFeature`, `RreqVendorFeature`, `FeatureCount`), the palette's NE and NPC (`PclrCounts`), and a Number List entry (`NlstEntry`, which the whole-file model keeps opaque). Decoded one at a time; lengths are ASN.1 fields, so `LBox = 0`, `LBox = 1` with XLBox, and `Psot = 0` are all expressible. Not used by the corpus harness. Everything else the reader and writer decode or encode is the whole-file model's own type (see "The reader/writer handles one element at a time"). `../jpeg2000/generate.sh` generates only the types in its `pdus` list and what they depend on: these, `CodSegment` and `QcdSegment` of `j2k-codestream.asn1`, the elements of SIZ, PLT and COM in `j2k-headers.asn1` (`SizFixed`, `Component`, `Zplt`, `Iplt`, `Rcom`) and the body elements `../jpeg2000/hv_rules.c` reads one at a time (`Qcd`, `ComponentIndex`, `ComponentIndex-Wide`, `CocStyle`, `RgnStyle`, `PocChange`, `PocChange-Wide`, `TlmHeader`, `Ttlm`, `Ttlm-Wide`, `Ptlm`, `Ptlm-Wide`, `Zplm`, `Zppm`, `Zppt`, `CrgEntry`), the profile types `SizFixed-Profile`, `MainMarkerCode-Profile` and `TileMarkerCode-Profile`, and `FtypHeader`, `Brand`, `DataReferenceCount`, `UrlHeader`, `FragmentCount`, `Fragment`, the box tree rules' `UuidCount`, `UuidId` and `CrefType`, and the header box types (`Ihdr`, `BitDepth`, `ColrHeader`, `CmapEntry`, `CdefCount`, `CdefEntry`, `Resolution`) of `jp2-boxes.asn1`. A type the reader or writer needs must be added to that list. |
| `modules` | The four modules, in import order: the one list that `../jpeg2000/generate.sh`, `check-model.sh` and `../jpeg2000/CMakeLists.txt` read. |
| `VERSION` | The exact upstream asn1scc revision used to generate the corpus and `../jpeg2000/generated/`. |
| `asn1scc-patches/` | Retired patch series grouped by original upstream base, with their original application order (not applied). |
| `build-asn1scc.sh` | Exports `VERSION` from a local compiler repository into a temporary clean tree, builds unmodified upstream in Docker, and runs CONTAINING, deduced-size, ACN v2, wire and `-icdPdus` regressions. |
| `check-model.sh` | The static checks (alone with `--static`, which needs only sh, sed and awk): each box-type mapping function against the CHOICEs it serves and the `'abcd'` sentinel of `other`; `../jpeg2000/hv_codes.h` against the values the model states, every constant written `HV_X = 0x...`; each marker-code value set against the fields of its segment `SEQUENCE`; the ACN encodings the model restates and the ASN.1 types it copies, the parts of `Rreq` in `jpeg2000-io.asn1` among them; the tile-part and dimension limits restated in C. Then, with the Docker image: `../jpeg2000/generated/` against the model, the complete model generated and built as strict C11 with ASan/UBSan, the corpus harness run, duplicate vector names rejected, and `harness/writers.c` run on the corpus; last, OpenJPEG's `opj_decompress` on every standard-valid vector (see "Quick start"). |
| `COVERAGE.md` | Maps modeled T.800/T.801 rules to corpus evidence, server enforcement, deliberate profile decisions, and remaining boundaries. |
| `harness/vectors.c` | The generator: builds bases, derives mutants, labels, writes files and manifest. |
| `harness/label.{h,c}` | The labels of a file at both layers: the generated decoders of each layer, `box_bounds_ok`, the cross-field rules, and the companion oracle of linked JPX files. |
| `harness/writers.c` | The writer of `../jpeg2000` judged by the model: labels the files `hv_rewrite` (every vector the reader reads through), `hv_transcode` and `hv_merge` (every `.jp2` they take) write from the corpus, and checks that each is in the writer's form (`hv_rewrite` gives it back byte for byte). With a seed, it also mutates each vector at random and checks the mutants the same way: a differential fuzz of the writer. |
| `harness/crossfield*.{h,c}` | The cross-field rules, written once and instantiated for both layers' struct types. They call the rules of `../jpeg2000/hv_rules.c`, shared with the reader, for SIZ and COD, tile-parts, PLT entries, the packet count, the JPX boxes and the header boxes; the structural rules (segment placement and count, the `Zplt` sequence, PLT sums, the file's first boxes, and `jp2.one-codestream`, `ftbl.one-flst` and `dtbl.ndr-count`) are implemented here and in `../jpeg2000/hv_reader.c` under the same names, and `flst.nf-count` is shared with the standard reader through `hv_rule_fragments`. |
| `harness/mapping.{h,c}` | The ACN mapping functions: three length mappings, because `Lxxx`, `Psot` and `LBox` count more than the payload (+2, +12, +8; `lxxx` is `../jpeg2000/hv_mapping.c`'s, which the harness links), and the decode-only box-type mappings `boxtype`, `resboxtype`, `cgrpboxtype`, `jp2boxtype`, `jp2stdboxtype` and `jp2hboxtype`. |
| `../tests/vectors/j2k/` | The committed corpus: the vectors plus `manifest.tsv`. |

## How the pieces fit

```
   spec/*.asn1 + *.acn  ──asn1scc (offline)──▶  generated C: structs, encoder,
                                                 decoder, constraint checker
                                                          │
   harness/vectors.c  ─── builds 4 base files as structs ─┤
                      ─── mutates them ───────────────────┤
                      ─── encodes ─▶ bytes ─▶ decodes ────┘──▶ label per layer
                      ─── + harness/crossfield.c rules ──────▶ (valid/invalid)
                          (shared rules: jpeg2000/hv_rules.c)  │
                                        tests/vectors/j2k/*.jp2, *.jpx, manifest.tsv
                                                          │
   tests/server/file_manager_test.cc ── OpenImage + GetPacket on each ▶ must match the label
   tests/jpeg2000/test_profile.c ─ hv_served_jp2/jpx + HV_READ_JPIP ▶ must match the profile label
   tests/jpeg2000/test_rewrite.c ─ hv_rewrite of each ────────────▶ must give the vector back
   harness/writers.c ─ hv_rewrite, hv_transcode, hv_merge ──▶ label.c: expected labels
```

Two facts make the generated code a good judge of validity. First, the
model puts every length (`Lxxx`, `Psot`, `LBox`) under ACN's control:
the encoder computes them, the decoder enforces them, and a body that does
not exactly fill its segment is a decode error (`size deduced`). No harness
code ever computes a length. Second, most placement rules are types: a PLT
in the main header or an `flst` at the top level cannot even be expressed,
so the decoder rejects it. The type of a superbox's children admits some
boxes the standard places elsewhere, and C rules reject those: an `flst`
in a `jpch` decodes and fails `box.flst-placement`, a `jpch` in a `jpch`
`box.nested-top-level`. Below the levels the types reach, and inside the
boxes they keep opaque, the box tree rules (`hv_rule_box_tree` in
`jpeg2000/hv_rules.c`) read the file's bytes: every box of every superbox of
the file's kind, to `HV_BOX_DEPTH_MAX` levels, with the placement and count
rules of T.800 Annex I and T.801 Annex M. At layer 1 a file is a JPX file
when its `ftyp` brand is `jpx `, a JP2 file otherwise (`Jp2File`, which
keeps every box T.801 adds opaque, as a JP2 reader skips it); a JPX file
that lists `jp2 ` is checked as a JP2 file too. Layer 2 goes by the
extension, as the server does.

## Quick start (regenerating the corpus)

Run this workflow when a model or harness changes, or when updating the
compiler pin. Everything runs offline. Run these commands from the repository
root.

1. Get an asn1scc repository containing the commit pinned in `spec/VERSION`.
   Its checked-out branch and working tree do not matter: the build script
   exports that upstream commit to a clean tree, builds the unmodified
   compiler in Linux Docker, and runs the relevant upstream regressions:

   ```sh
   spec/build-asn1scc.sh ~/git/asn1scc esajpip-asn1scc
   ```

   Advance the pin only deliberately, then regenerate and review the corpus
   in the same change.
2. Regenerate the reader's code with the same compiler:

   ```sh
   ASN1SCC_IMAGE=esajpip-asn1scc jpeg2000/generate.sh
   ```

   `jpeg2000/generate.sh` runs the compiler in that Docker image unless
   `ASN1SCC` names a local copy of the same unmodified upstream build, using
   a compatible .NET 10 runtime, for example
   `ASN1SCC="dotnet $HOME/jhv/asn1scc-bin/asn1scc/asn1scc.dll"`.
   `check-model.sh` ignores `ASN1SCC`: it runs `jpeg2000/generate.sh --check`
   with the image it builds the corpus with, so that both come from one
   compiler.
3. Run the complete generation and harness gate:

   ```sh
   ASN1SCC_IMAGE=esajpip-asn1scc spec/check-model.sh
   ```

   This checks that `jpeg2000/generated/` matches the model, so regenerate it
   before running the gate, and then has OpenJPEG's `opj_decompress` (on
   the host; `OPJ_DECOMPRESS` names another) decode every standard-valid
   vector: each it rejects must be on a limit of OpenJPEG's the script
   lists, with the error OpenJPEG gives, and each listed limit must have a
   vector. `spec/check-model.sh --static` runs the checks that need no
   compiler; it takes a second.

   To retain the generated corpus, pass an empty output directory. The
   script refuses a directory that is not empty, so it cannot regenerate
   `tests/vectors/j2k/` in place. To replace the committed corpus, generate
   into a new empty directory, diff its `manifest.tsv` against the committed
   one (see "Regeneration policy"), then replace the contents of
   `tests/vectors/j2k/` with those of the new directory, removing the old
   files first so that vectors no longer generated do not remain.

   It reports how many vectors were written and how many are valid at both
   layers. It exits non-zero if any vector did not get the label and the
   reason its mutant expects (see "What the harness generates"); each such
   line names the vector, the expected label and reason, and the actual
   labels and the reasons at both layers. On the first run after an
   intentional model change, investigate any mismatch before trusting the
   corpus.

   The generic compiler `-atc` generator is intentionally not used for this
   model: its large bounded, inline container types make generic exhaustive
   values impractical, and it cannot replace the JPEG 2000 cross-field and
   mutation checks. Small compiler regressions cover the backend mechanisms;
   `check-model.sh` covers their composition in the real model.
4. Run the server tests, and the reader and transcoder tests, which check
   the corpus labels too; read "When a vector fails" for anything red:

   ```sh
   ./tests/run.sh
   tests/run.sh
   ```

   Both runners read `ESAJPIP_TEST_BUILD_DIR` for their build directory.
   Their defaults differ; if you set it, give each runner its own.
5. Commit the changed model or harness, `spec/VERSION` and
   `spec/asn1scc-patches/` if they changed, `jpeg2000/generated/`, and
   `tests/vectors/j2k/` together. Never edit vector files or manifest labels
   by hand.

## Reading a model file

Each `.asn1` has the same shape: a header comment stating scope and design,
**Layer 1** types with the standard's ranges and a table/clause citation on
every field, **Layer 2** `*-Profile` types, and at the end the cross-field
rules for each layer. The `.acn` mirrors the type names and gives each
field its width and encoding, plus the determinants.

Conventions worth knowing before you edit:

- **Corpus bounds.** Some `SIZE` upper bounds are far smaller than the
  standard's and are marked "corpus bound" (e.g. 32 top-level boxes, 128 PLT
  entries). asn1scc allocates every list at its maximum inside the struct,
  so standard-sized bounds would make a `Jp2Family` far larger than its
  877 MB with the corpus bounds. A file that exceeds a corpus bound is
  not standard-invalid; the harness never generates one. Of the marker
  segment bodies only `Plt` and `Com` have a
  corpus bound; the reader/writer reads those segments one element at a
  time (below), so no type needs their standard bound. `Cod` and `Qcd`
  are at the standard's bounds (Lcod 12 to 45, Lqcd 4 to 197).
- **Layer 2 normally narrows layer 1.** A `*-Profile` type is either a `WITH
  COMPONENTS` subtype (ranges narrowed) or, where framing has to be repeated
  with profile bodies inside `CONTAINING`, a copy with profile types
  substituted. It has fewer alternatives or a smaller marker-code value set
  where the server rejects a whole class (`TileSegment-Profile` is `plt`
  only; `MainMarkerCode-Profile` excludes COC, POC, PPM, the codes T.800
  places elsewhere and 0xFF30 to 0xFF3F; `Jp2Payload-Profile` keeps every
  box of a .jp2 but `jP`, `ftyp` and `jp2c` opaque). A box payload is a
  `CHOICE` selected by the box type. A marker segment (`MainSegment`,
  `TileSegment`) is a `SEQUENCE` of `OPTIONAL` fields, each present when the
  sibling `code` has its value (`present-when`); every code of the
  `MainMarkerCode` or `TileMarkerCode` value set selects exactly one field
  (`check-model.sh` checks this), and the constraint check on `code`
  rejects any other code, so a segment always has its field. The deliberate
  type-level exception is the `other` field, for a marker code T.800 does
  not define, which is skipped by its length: in `MainSegment` and
  `TileSegment` at layer 1 and in `MainSegment-Profile` at layer 2
  (`*-main-unknown` is valid at both layers). The profile also keeps
  main-header QCD and COM bodies opaque, as well as what the server does
  not interpret (`jp2h`, `jplh`, `rreq` and
  `asoc` in a .jpx, and the contents of the children of `jpch`), so the
  header-box rules, the placement of boxes in those superboxes and the
  `rreq` contents are checked at layer 1 only, and it omits the standard
  cross-field rules the server does not enforce, among them a missing JPX
  `rreq`, trailing zero-valued PLT entries, the Rsiz value and the rules
  of the segments the server skips. Every `standard=invalid,
  profile=valid` vector in the corpus comes from one of these;
  `COVERAGE.md` lists them by kind. Unknown box types are valid at both
  layers, as required by T.800 I.8 and T.801 M.12.
- **The reader/writer handles one element at a time.** No type of
  `jpeg2000-io.asn1` holds a list: asn1scc would allocate it at the
  standard's bound (16,384 components, 65,532 packet lengths, 65,535
  features), up to megabytes per struct. The reader decodes the fixed part
  of a segment or box, then its elements one at a time up to the end the
  segment or box length gives: SIZ as `SizFixed` and `Csiz` `Component`s,
  PLT as `Zplt` and `Iplt`s, COM as `Rcom` and text left in place, a
  Fragment List box as `FragmentCount` and a `Fragment`, and a Reader
  Requirements box as `RreqHeader`, the features and `FeatureCount`. The
  writer encodes the same parts one at a time and measures the length it
  wrote; no box or segment length comes from a type's size. The elements
  are the whole-file model's own types, so the two share one encoding.
  Where an
  element takes an ACN parameter from its list (a Reader Requirements mask
  is ML bytes), `jpeg2000-io.asn1` has a copy whose mask is `size deduced`
  instead (`RreqStandardFeature`, `RreqVendorFeature`), and `RreqHeader`
  restates Rreq's head; `check-model.sh` checks both against `Rreq`, and
  `PclrCounts` against `PclrHeader`. Only COD and QCD, at most 45 and 197
  bytes, are decoded whole, as `CodSegment` and `QcdSegment`.
- **Shared, not copied.** A fixed part the reader/writer decodes on its
  own is a type of the whole-file model that the whole-file type uses as
  a field (`SizFixed` in `Siz`, `FtypHeader` in `Ftyp`, `UrlHeader` in
  `DataEntryUrl`, `FragmentCount` in `FragmentList`, `DataReferenceCount`
  in `DataReferences`), so there is one definition. `jpeg2000-io.asn1`
  defines only what the whole-file model cannot lend: `BoxHeader` and
  `SotSegment`, whose lengths are fields there (`check-model.sh` compares
  `SotSegment` with `TilePart` but for Psot), a marker code and Lxxx on
  their own, the copies above, and a Number List entry (`NlstEntry`),
  which the whole-file model keeps opaque.
- **ACN properties are explicit on profile structures.** asn1scc does not
  inherit field encodings through `WITH COMPONENTS` constraints. Profile
  structures that must be encoded independently therefore repeat the base
  structure and have their own ACN entry. This is deliberate duplication at
  the wire-description boundary, not a second interpretation of the format.

## The test contract

For every vector, `file_manager_test` reads the committed file and its companions
from the corpus directory under the names in its manifest row, calls
`FileManager::OpenImage`, and — if that succeeds — indexes every declared
packet of every codestream with `GetPacket`.
"Accept" means both succeed; "reject" means either fails. The second step
matters because the server parses PLT entries lazily: a malformed or
over-long packet length is only detected when a packet is indexed.

| Standard | Profile | The server must |
| --- | --- | --- |
| valid | valid | accept |
| valid | invalid | reject, for the profile reason |
| invalid | (invalid) | reject |
| invalid | valid | accept only for an explicit documented profile leniency; otherwise investigate the model |

A vector is *valid at a layer* when both hold: the generated ACN decoder
accepts it (this is what catches region overruns, leftover bytes, box
types with no alternative at their level, and marker codes the layer does
not admit in their position; the decoder ends with the layer's generated
constraint check, `<Type>_IsConstraintValid`, so a value out of range is a
decode failure too), and the cross-field rules for that layer hold
(`crossfield.c`, with the shared rules of `../jpeg2000/hv_rules.c`). The
compiler performs the evaluation, but the ASN.1/ACN model and the
imperative cross-field rules are human-maintained sources that cite the
corresponding standard clauses.

The corpus tests *acceptance* (open plus complete packet indexing), not
serving. Packet data in generated files is arbitrary bytes; nothing here
claims a file decodes to an image.

Manifest columns: `file kind standard profile reason field note companions
profile_reason`. `reason` is the first check that failed at the stricter
failing layer — `decode` (which includes the constraints and the harness's
physical LBox boundary check, `box_bounds_ok`) or a cross-field rule name
such as `plt.coverage` — or `-` for a valid vector; `field` names the
mutated field or rule (or `-` for a base); `note` is the mutant's intent in
words; `companions` lists files that must sit next to the vector (the `.jp2`
frames a linked JPX points at, for every vector derived from a linked JPX);
`profile_reason` is the first check that failed at the profile layer, or `-`
for a profile-valid vector (it differs from `reason` when layer 1 fails
first). The harness checks in the order the reader reads: the file's first
boxes, then the boxes in file order, then each codestream's segments where
they occur, so both name the same rule for a file that breaks several.

The rule lists at the end of each `.asn1` state each rule with its
citation and name some of them. The names themselves are defined where the
rules are implemented: the shared rules in `../jpeg2000/hv_rules.c`; the
structural rules (segment placement and count, the `Zplt` sequence, PLT
sums, the file's first boxes, some box counts) in
`harness/crossfield_impl.h` and `harness/crossfield.c` and, under the same
names, in `../jpeg2000/hv_reader.c`. `flst.nf-count` also runs in the standard
reader through `hv_rule_fragments`, including inside `j2cx`; the served
profile instead requires `flst.one-fragment`. In the manifest, the two cross-file names,
`flst.source-extent` and `url.missing-companion`, come from the companion
oracle in `harness/label.c`. Some rules of the reader never appear as a
manifest reason because a type rejects the vector first, with reason
`decode`: `tile.marker-code`, and those the rule lists name
(`main.marker-code`, `codestream.tile-part-limit`, `siz.zero-origin`,
`siz.component-sampling`, `cod.sop-markers`, `url.length`,
`flst.one-fragment`). Only `file.size-limit` is in no
vector: the corpus holds no file over `INT_MAX` bytes.

## When a vector fails

For the server, `file_manager_test` prints `Generated vector <name> was
accepted, expected rejection` or `Generated vector <name> was rejected
during <stage>, expected acceptance`, each followed by the manifest's
`(<reason>: <note>)`. For the reader, `test_profile` (run by
`tests/run.sh`) prints
`FAIL profile label: <name>: reader <result>, manifest <label> (<profile_reason>)`
when its profile checks disagree with the `profile` column, where
`<result>` is `valid` or the reader's error and offset (and the linked
file it was reading); when its header-box checks break their contract with
the `standard` and `reason` columns, it prints `FAIL (a)` to `FAIL (d)`
lines, and `FAIL (e)` or `FAIL (f)` when the reader names another rule than
`profile_reason`, or than `reason` for a codestream rule at the standard
layer; which `../tests/jpeg2000/test_profile.c` explains at its top, with the
same details. Either way, look the name up in `manifest.tsv`; the cases
below name the server, and apply to the reader in the same way:

- **`standard=valid profile=valid`, server rejected.** Either the parser is
  too strict, or the model is wrong about the standard. Read the `note`,
  find the field in the `.asn1`, read the cited table. If the standard
  agrees with the model, fix the parser; if not, fix the model and
  regenerate.
- **`standard=valid profile=invalid`, server accepted.** The parser is more
  lenient than `JPIP_PROFILE.md` promises. Either tighten the parser or
  change the profile (both the doc and the `*-Profile` type).
- **`standard=invalid`, server accepted.** First decide whether layer 1 is
  incomplete or the standard truly forbids the file. If the model is
  incomplete, correct it with a citation and regenerate. If the standard
  forbids the file, either tighten the parser or document a deliberate server
  leniency in `JPIP_PROFILE.md`; do not make the standard layer call it valid.
- **`standard=invalid profile=valid`.** This is a deliberate server leniency,
  such as a JPX file without `rreq` or a malformed header box the server
  keeps opaque; `COVERAGE.md` lists every kind in the corpus. Confirm that
  the standard citation and profile rationale are both explicit; otherwise
  investigate the model. Unknown box types are valid at both layers because
  T.800 I.8 and T.801 M.12 require readers to skip them.

The `field` column also tells you which of the harness's mutation classes
produced the vector (see "What the harness generates"), which narrows down
the parser code involved: a length mutant points at `hv_boxes_next` or
`hv_codestream_next` framing checks, a field mutant at the corresponding
generated decoder, and a rule mutant at the checks in `hv_rules.c`.

Two things that look unusual are intentional. An `Iplt` written in more
7-bit groups than its value needs is valid at both layers: the server bounds
packet lengths by value, not by byte count (`plt.iplt-five-bytes`,
`plt.iplt-six-bytes`). A zero-valued `Iplt` entry after the last packet is
standard-invalid but profile-valid because the server accepts it in deployed
files (`plt.trailing-zero`). The model counts nonzero entries against the
COD-derived packet set and rejects a zero entry within that set or a nonzero
entry after it, even when the PLT lengths still cover the tile-part data.

## Compiler checks

The compiler is built from the exact upstream revision in `spec/VERSION`
without local patches. Issue [#417](https://github.com/esa/asn1scc/issues/417)
integrated all eleven local fixes in upstream 4.9.7.0, with additional
corrections. The original series remains in
`spec/asn1scc-patches/reference/161cc246/`. The resolved issue reports and
local reproducers were removed after retirement. The compiler build runs
upstream regression cases, wire tests and `-icdPdus` checks.
Issue [#415](https://github.com/esa/asn1scc/issues/415) added cases under
asn1scc's `v4Tests/test-cases/acn/25-ACNV2-BOUNDARIES/` for the backend
mechanisms exposed by this model:

1. a one-bit Boolean determinant produced inside a referenced structure and
   consumed by an optional sibling;
2. constrained `CONTAINING` specializations that would otherwise collide in
   their generated C names;
3. a length determinant crossing nested `SEQUENCE` and `CHOICE` boundaries;
4. a mapped length determinant crossing a parameterized `CHOICE` boundary.

The compiler converts those cross-scope determinant relationships into
explicit ACN parameters, reserves and patches producer fields at the scope
that owns the value, and preserves mapping functions when patching measured
lengths. `build-asn1scc.sh` runs the upstream C cases and byte-exact wire
checks. `check-model.sh` then generates and runs the complete JP2/JPX model
under strict C11, ASan and UBSan.

Patch `0006` makes `CONTAINING` wrappers validate constrained referenced
subtypes. In this model it changes only the validator calls for
`Siz-Profile`, `FragmentList-Profile`, and `DataEntryUrl-Profile`. With it,
the profile decoder reports `decode` for invalid SIZ origins, sampling, or
dimensions and for a nonzero URL version before the shared C rules run. The
reader keeps those C rules for named diagnostics; the corpus harness checks
representative overlapping cases against the subtype validators. The
1,024-character LOC maximum in `DataEntryUrl` is a corpus allocation bound,
not a served-profile limit: the server, reader, and merger impose no
1,024-character cap.

These checks deliberately separate compiler responsibility from model
responsibility. ASN.1 constraints and ACN regions validate local shape and
lengths. `crossfield.c` handles relationships such as marker ordering, packet
coverage, JPX link rules, and distinctions between the standard and the served
profile. A future compiler feature such as a post-decoding validator could
invoke some of those rules automatically, but it would not make the rules
disappear or make generic `-atc` values representative of JPEG 2000 files.

## What the harness generates

`harness/` is self-contained: it does not consume asn1scc's own test values
(their identifiers vary between releases). It builds four canonical,
profile-valid files with the generated structs, derives every other vector
from them, and labels everything by decoding.

Bases: `jp2` (one codestream, default precincts), `jp2-precincts` (one
decomposition level with explicit precinct sizes, so the precinct rules have
something to mutate), `jpx-embedded` (`rreq`, `jp2h`, two `jpch` with an
`ihdr` each, two `jp2c`), and `jpx-linked` (`rreq`, `jp2h`, two `jpch`, two
`ftbl`/`flst`, one `dtbl` with two `url` boxes). For the linked base the
harness first writes the two referenced frames (`jpx-linked-frame1.jp2`,
`-frame2.jp2`), reads their codestream offsets back, and puts them into the
`flst` fragments, so the vector really resolves; the manifest's `companions`
column lists them.

The profile oracle also compares decoded fragment claims with the extents
measured from the generated companions. Four vectors shift the fragment start
or end by one byte, and one names a missing companion. Their `standard=valid`
label means that the JPX structure passes the standard model; it does not
certify the referenced byte range as a valid codestream. The cross-file
failure appears in the profile result as `flst.source-extent` or
`url.missing-companion`. The oracle resolves only the named corpus companions,
not arbitrary filesystem paths or network resources.

From each base:

1. **The base itself.**
2. **Field mutants** (`field_mutants[]` in `vectors.c`): one scalar field
   set to one interesting value — `min − 1`, `max + 1` at each layer's
   bound, and semantically loaded values (`xtsiz = 3` below `xsiz`,
   `progression = 3`). Names look like `jp2-siz.xosiz-1`.
3. **Rule mutants** (`rule_mutants[]`, `linked_rule_mutants[]`): one
   structural change per cross-field rule — a second COD or QCD, no QCD, a
   QCD after the tile-part, no PLT, contradictory TNsot, 65 tile-parts, a
   packet length beyond or short of the data, COD twice in a tile header or
   in a second tile-part, COD, QCD or COM in a tile header, COC, POC or PPM
   in the main header, 2:1 component sampling, a packet count above 2^31, no
   `jP` box, a wrong or unlisted `ftyp` brand, two `jp2c`, a `jp2c` inside a
   `jpch`, fewer `jp2c` than `jpch`, no `jpch`, `jp2c` and `ftbl` in one
   JPX, two `dtbl`, no `dtbl`, `DR = 0`, `NDR` mismatch, two `flst`, `NF` 2
   with one fragment, an `http` URL, a link to a `.jpx`, a byte after a
   URL's NUL — plus the valid shapes the rules must *not* reject: two
   tile-parts, TNsot given only in the second, 64 tile-parts, packet lengths
   split over two PLT segments, a COM in the main header, and `jp2c` boxes
   ahead of the `jpch` boxes. The deployed trailing-zero PLT form is generated
   as a deliberate `standard=invalid, profile=valid` case. Names look like
   `jp2-rule-codestream.no-plt-4` (the number is the mutant's index in its
   table). Append new rule mutants to preserve all existing fixture names;
   inserting, removing, or reordering entries renumbers later fixtures.
   Changes to the model or compiler can still change existing fixture bytes
   or labels and require separate review.
4. **Header mutants** (`header_mutants[]`, for `jp2` and `jpx-embedded`):
   the JP2 header boxes, the JPX codestream headers and the box tree.
   Valid shapes (a palette, a resolution box, `jp2h` defaults with empty
   `jpch` boxes, a restricted ICC profile, a `uinf`, a `uuid`, an `opct`,
   `comp`, `drep` or `cref` where T.801 puts them, a `j2cx`, a JPX file
   that lists `jp2 `, one named `.jp2`, ...) and one violation per header
   rule, box tree rule or field range, plus the `rreq` mask length and a
   `jp2c` or `flst` inside `jp2h`. The boxes the model keeps opaque are
   written as `other` boxes and given their type on the wire
   (`retype_patch`). Names look like `jp2-header-jp2h.position-9`,
   numbered like the rule mutants.
5. **Length mutants**: every `Lxxx`, `Psot` and `LBox` the standard layer
   reads patched to `n − 1`, `n + 1`, and below its minimum. Invalid at
   both layers, except the children of a box the profile keeps opaque
   (`jp2h` in a .jpx, every superbox in a .jp2), which are
   `standard=invalid, profile=valid`. Names: `jp2-len-<offset>-<value>`.
6. **Code mutants**: each marker segment's code is patched to `FF70`
   (undefined), `FF51` (SIZ out of place) and `FF90` (SOT where a segment
   was). Each replaces a segment the codestream needs or puts SIZ or SOT
   out of place, so all are invalid at both layers, `FF70` included,
   although both layers skip an unknown main-header code: `FF70` over the
   main COD or QCD fails `codestream.one-cod-before-sot` or
   `codestream.one-qcd-before-sot`. The exception is `FF70` over a PLT,
   `standard=valid, profile=invalid`: layer 1 skips it and needs no PLT.
   Structured
   rule mutants cover box types without accidentally removing a different
   mandatory box: one appends an unknown `'abcd'` box, and another places a
   `jp2c` inside `jpch`. Names: `jp2-code-<offset>-<value>`.
7. **Signature mutant**: the `jP` box contents zeroed. Name: `jp2-sig-bad`.
8. **Insertion mutants** (bases with a codestream): bytes the structured
   mutants cannot produce, inserted with every enclosing length grown to
   match. PPT, SOP, `FF30` read with a length, and `FF00` before the first
   SOT, invalid at both layers; `FF70`, a code T.800 does not define, before
   the first SOT, valid at both layers (A.1: skipped by its length); `FF30`,
   a marker without a segment, there, and `FF70` and `FF30` in the
   tile-part header, `standard=valid, profile=invalid` (A.1.3: skipped);
   TLM in the tile-part header, invalid at both. Names: `jp2-main-ppt`,
   `-main-sop`, `-main-ff30-length`, `-main-ff00`, `-main-unknown`,
   `-main-ff30`, `-tile-tlm`, `-tile-unknown`, `-tile-ff30`.
9. **Reader Requirements mutants** (`jpx-embedded`): NSF and NVF one too
   high, and a byte after the vendor features, which the structured
   mutants cannot produce because ML, NSF and NVF are ACN determinants.
   Standard-invalid, profile-valid. Names: `jpx-embedded-header-rreq.nsf`,
   `-rreq.nvf`, `-rreq.extent`.

Beyond the bases, the harness generates:

- **Unknown-box vectors**: an unknown top-level box whose encoded type is
  patched to `wxyz` and to a non-ASCII value in a JP2
  (`jp2-unknown-top-wxyz`, `jp2-unknown-top-binary`), and to `wxyz` at the
  top level and inside a `jpch` in an embedded JPX
  (`jpx-unknown-top-wxyz`, `jpx-unknown-inner-wxyz`), all valid at both
  layers.
- **Nested superboxes**: an empty `jpch`, an `ftbl` holding one `flst`,
  and a `dtbl` with NDR 0, each inside the first `jpch` of an embedded JPX
  (`jpx-nested-jpch`, `jpx-nested-ftbl`, `jpx-nested-dtbl`): each is a
  valid box in the wrong place, `box.nested-top-level` at both layers.
- **Association vectors**: an embedded JPX with an `asoc` holding a label
  and an XML box (`jpx-asoc`, valid at both layers); with an inner LBox
  below 8 (`jpx-asoc-child-length`) or only one child
  (`jpx-asoc-one-child`), standard-invalid and profile-valid; with an
  outer LBox past the end of the file (`jpx-asoc-outer-length`), invalid
  at both; and with a label and a nested `asoc` (`jpx-asoc-nested`), valid
  at both, as T.801 M.11.11 lets associations nest.
- **JPX graph vectors**: a second companion, `jpx-graph-frame2.jp2`, with
  the `plt.boundaries` codestream, and three linked JPX files whose two
  fragments reference `jpx-linked-frame1.jp2` and `jpx-graph-frame2.jp2`
  in order, in reverse order, or the second twice
  (`jpx-graph-sequential`, `jpx-graph-reversed`, `jpx-graph-repeated`),
  valid at both layers.

Labels are never written by hand: `label()` decodes each vector with the
layer-1 decoder and the layer-2 decoder for its kind (constraints
included), runs `crossfield.c`, and records the first failing reason in the
manifest's `reason` column. Before each decoder, `box_bounds_ok` checks the
physical LBox boundaries (see "Mapping functions"); a failure there is
also reported as `decode`.

Every vector the harness emits carries an *expectation*: its label
(`X_VALID`, `X_STD` for invalid at both layers, `X_LENIENT` for
standard-invalid but profile-valid, a documented leniency, `X_PROF` for
standard-valid but profile-invalid) and its reason, what the manifest's
`reason` column must say: a rule name, `decode` where a type rejects the
vector before any rule runs, or nothing for a valid vector. Where an entry
is named after a layer-2 rule that a layer-1 rule preempts in the manifest
(`plt.padding-position`, whose vector is `plt.zero-length` at layer 1, and
`url.terminator`, `box.extent` at layer 1), the entry also states the
layer-2 reason, which `emit()` checks too. So an entry's `field` names the
rule it breaks, and its reason says whether that rule or a type rejects
it. This is not a second source of truth for the corpus — the manifest
always holds what the decoders said — it is a self-check that the mutant
did what its note claims. A setter that writes the wrong field, a
structural mutant that leaves the file valid, or a model rule that quietly
stopped firing, or that another check now preempts, shows up as a mismatch
at generation time rather than as an inexplicable server-test result
later. Bases expect valid at both layers; length mutants expect `decode`,
and so do code mutants but an `FF70` over the main COD or QCD
(`codestream.one-cod-before-sot`, `codestream.one-qcd-before-sot`).

### Generated-API adaptation

`vectors.c` names the generated encoders through one macro at the top
(`ENC` → `<Type>_ACN_Encode`) and the encoded size through another
(`FILE_BUFFER_BYTES`, `Jp2Family_REQUIRED_BYTES_FOR_ACN_ENCODING`); it and
`label.c` call `<Type>_Initialize`, `<Type>_ACN_Decode` and
`<Type>_IsConstraintValid` by name; `mapping.h` names the
mapping-function prototypes through `MAPPING_ENCODE_NAME`/`_DECODE_NAME`;
`crossfield.h` includes the three generated headers it needs by module name.
Field access assumes asn1scc's C conventions: `nCount`/`arr` for
`SEQUENCE OF` and `OCTET STRING`, `kind`/`u.<alt>` and `<Type>_<alt>_PRESENT` for
`CHOICE`, `exist.<field>` for `OPTIONAL`, a contained type appearing
directly for `OCTET STRING (CONTAINING X)`, and a NUL-terminated `char`
array for `IA5String`. If your release differs on any of these, the compiler
will tell you at exactly the access sites; nothing is hidden behind
reflection.

Struct sizes: the generated `Jp2Family` is 876,572,424 bytes, and
`Jp2File-Profile` and `JpxFile-Profile` 334,405,384 each (asn1scc allocates
every corpus bound inline); the harness allocates its few instances on the
heap. If your compiler reports larger sizes, lower the corpus bounds in the
`.asn1` files (they are marked) rather than the harness.

The reader/writer decodes one header or one list element at a time (see
"Reading a model file"), so its types are small. Their sizes with asn1scc
4.9.3.0 on a 64-bit target:

| Type | Bytes |
| --- | ---: |
| `CodSegment` | 616 |
| `QcdSegment` (194 bytes) | 208 |
| `SizFixed` | 80 |
| `Component` | 32 |
| `Zplt`, `Rcom` | 8 |
| `Iplt` | 88 |
| `RreqHeader` | 32 |
| `RreqStandardFeature` | 24 |
| `RreqVendorFeature` | 28 |
| `Fragment` | 24 |
| `SotSegment` | 40 |
| `BoxHeader` | 32 |
| `Ihdr` | 128 |
| `Resolution` | 120 |
| `PclrCounts` | 16 |

The largest type in `../jpeg2000/generate.sh`'s `pdus` list is `CodSegment`.
None is sized by a list's bound: the palette's column depths, like SIZ's
components, are decoded one at a time.

The harness, the library in `../jpeg2000/` (`hv_reader.c`, `hv_writer.c`,
`hv_rewrite.c`, `hv_rules.c`, `hv_geometry.c`, `hv_walk.c`, and
`hv_mapping.c`, which
provides the `lxxx` mapping for `jpeg2000/generated/` and the harness), and the
tools in `../transcode/` and `../merge/`, which use the generated struct
types, depend on the generated API, so they are what to touch when
regenerating with a newer asn1scc. The server and `file_manager_test` never see
generated code.

### Mapping functions

Three length mappings work in both directions: `psot` and `lbox` in
`harness/mapping.c`, and `lxxx` in `../jpeg2000/hv_mapping.c`, which the reader,
the writer and the harness share. ACN's length determinants count payload
bytes, while the wire length fields also count themselves and their
neighbors:

| Name | Where | Encode (model → wire) | Decode (wire → model) |
| --- | --- | --- | --- |
| `lxxx` | every marker segment | `n + 2` | `n − 2` for `n >= 2`; otherwise an invalid size |
| `psot` | SOT | `n + 12` | `n − 12` |
| `lbox` | every box | `n + 8` | `n − 8` |

For `Lxxx < 2`, `lxxx_decode` returns the largest `asn1SccUint`. The
generated `CONTAINING` decoder rejects that size at its bounds check,
before reading the body. This is the failure convention for the existing
mapping API, which returns only a value and cannot report an error.

The box-type mappings are decode-only, and declared, as all mapping
functions, with `asn1SccUint`, as the generated code calls them. `boxtype`
preserves every box type listed in `TopPayload`, `InnerPayload` and their
`-Profile` versions and maps every other 32-bit TBox value to `'abcd'`
(`MAPPING_OTHER_BOX`), which selects the opaque `other` alternative.
`resboxtype`, inside a Resolution box, preserves only `resc` and `resd`;
`cgrpboxtype`, inside a Colour Group box, only `colr`. `jp2stdboxtype`,
for a JP2 file at layer 1, preserves only `jP`, `ftyp`, `jp2c` and
`jp2h`, and `jp2hboxtype`, inside its `jp2h`, only `ihdr`, `bpcc`,
`colr`, `pclr`, `cmap`, `cdef` and `res`. `jp2boxtype`, for a .jp2 at the
profile layer, preserves only `jP`, `ftyp` and `jp2c`. The model uses the
normalized type for validity checks; the
server retains the original bytes. The model encoder writes `'abcd'` for
`other`, so the corpus generator patches encoded TBox values to exercise
other unknown types. `check-model.sh` verifies each mapping's list against
the CHOICEs its determinants select, and that every `other` alternative is
`'abcd'`; and that each marker code and box type named in
`../jpeg2000/hv_codes.h` is the value the model states for the field of that
name (a `present-when` value, a fixed INTEGER field or a termination
pattern); the script lists the few it names that the model does not state
(SOP, EPH, the FF30 to FF3F range, twelve box types and the brands).

Before decoding, the corpus harness checks physical LBox boundaries. The
generated `CONTAINING` decoder uses LBox as a temporary stream size, which
can otherwise read beyond the buffer for malformed lengths. The check
follows the boxes each layer reads as boxes (`children_offset` in `label.c`,
the harness's one list of superboxes: at layer 1 the children of the
top-level superboxes and of `res` and `cgrp` inside them, at layer 2 those
of the top-level `jpch`, `ftbl` and `dtbl` of a .jpx). A file that fails it
is labeled `decode` at that layer. The reader's `hv_is_superbox` lists more
(`res`, `cgrp`, `comp`, `drep`, `jclx`, `j2cx` and `grp`, at any depth)
and leaves `dtbl` to its callers, because `hv_walk` and `hv_transcode`
check the framing of every box that holds boxes.

## The test loop in `file_manager_test.cc`

`CheckGeneratedCorpus()` reads every row of
`tests/vectors/j2k/manifest.tsv`. Corpus files remain in their committed
directory, so linked-JPX companions resolve under the same names used when
the harness generated them.

For each row, it compares the `profile` label with
`IndexGeneratedVector()`, which opens the file and attempts to index every
packet in every codestream. The loop collects profile mismatches, recording
whether each vector was unexpectedly accepted or where it was rejected, along
with the manifest's `reason` and `note` fields. After the loop, it prints
the collected mismatches and fails if any occurred. The `field` column
selects extra PLT-boundary and association-metadata checks for the applicable
rows.

The lazy packet lookup matters: some malformed packet-length tables are
detectable only when the packet index is built, not while the file structure is
opened.

`file_manager_test.cc` also has hand-built fixtures. Those that cover a
gap in the model ("Gaps") are the only test of that behavior; the rest
overlap the corpus but run without it.

## Gaps and limits of the model

The corpus never generates `Psot = 0`, `LBox = 0` or `LBox = 1` with
`XLBox`: the model is narrower than the parser there, and a hand-written
test covers each (`CheckSourceForms`, below). The other items state how far
the model goes and where its layers differ:

- `Psot = 0` (tile-part to EOC) and `LBox = 0` (box to end of file): a
  region cannot be both determinant-sized and deduced.
- The contents of the XML and IPR boxes are opaque at both layers. T.800
  I.7.1 requires an XML box to hold a well-formed XML document, and T.801
  N.5.4 an IPR box to hold well-formed XML 1.0; checking that needs an XML
  parser, which neither the model nor `jpeg2000/` has.
- `LBox = 1` with `XLBox`: two possible determinants for one payload.
- Marker segments T.800 does not define are skipped by their length at
  both layers (A.1: "the decoder shall use the length parameter to discard
  the marker segment"; `*-main-unknown`, valid), and markers 0xFF30 to
  0xFF3F without a segment at layer 1 (A.1.3); the profile rejects those,
  and any segment but PLT in a tile-part header, as the server does.
  Unknown box types are valid at both layers and are decoded as opaque
  boxes.
- Required EPH markers after every packet header (T.800 A.8.2) are not
  checked: packet headers are opaque to the framing model and reader.
  `transcode/tier2.c` checks them when transcoding.
- The Profile-1 multi-tile rule (`Rsiz = 2`) follows T.800 (08/2002,
  11/2015 and 06/2019), Table A.45: `XTsiz / min(XRsiz_i, YRsiz_i) >= 1024`.
  Equality passes, and the bound does not apply to a single tile. The
  official T.803 (02/2024) streams `p1_04`, `p1_05` and `p1_06` conflict
  with this bound and fail `codestream.profile-1`. This discrepancy remains
  unresolved; the comparison and original conformance files are preserved.
  The current T.800 (2024) wording has not been verified. This has no
  effect on supported server sources, which must use a single tile.
- The standard layer validates `Rsiz` 0, 1 and 2 and the existing Part 2
  declarations in JPX. T.800:2019 Table A.10 also defines cinema, broadcast,
  IMF and PRF-based profile declarations; their restrictions are not
  implemented here (`siz.unsupported-profile`). CAP-signalled capabilities
  outside the accepted Part 2 declarations are unvalidated
  (`siz.unsupported-capabilities`). Neither diagnostic means that the
  declaration itself is reserved. The serving profile preserves any Rsiz
  and validates packet-layout features separately.
- CAP (0xFF50, T.800:2019 A.5.2) and PRF (0xFF56, A.5.3) are length-framed
  opaque main-header segments. Their bodies, ordering, duplicate counts
  and placement are not semantically validated at the standard layer;
  the serving profile rejects every non-PLT tile-header segment. Their
  preservation does not establish support for the capabilities they signal.
  The Scod and code-block-style constraints enforce the Part 1 encoder
  zero-bit requirements (Tables A.13 and A.19), not the conditional decoder
  handling of extension bits.
- The standard model decodes one level of `asoc` children and requires at
  least two; the box tree rules read a nested `asoc` (`jpx-asoc-nested`). The profile
  treats `asoc` contents as opaque metadata, like the server. `flst` and
  `url` at the top level fail to decode at both layers of a JPX file, as
  the server rejects them there; the other boxes that belong inside another
  (a header box, `nlst`, `lbl`) are opaque at the top level, which the
  server skips, and the box tree rules place them at the standard layer
  (`box.top-level`, `colr.placement`).
- The Multiple Codestream (`j2cx`) and Compositing Layer Extensions
  (`jclx`) boxes are checked as boxes (their placement and order, what
  they hold, the counts of their `j2ci` and `jlxi`: M.11.21 to M.11.24),
  and the fragment tables of a `j2cx` too, but their codestreams are not
  read, and what they describe is left out: the codestream headers a
  `jclx` replicates (M times C) against the codestreams, the Codestream
  Registration boxes they must all hold if one does, the codestream
  indices a `creg` or `nlst` there may hold, and which codestreams a
  top-level `creg` refers to. Deeper association trees and `j2cx` storage
  are exercised by explicit server fixtures.

`CheckSourceForms` covers 28 cases outside or alongside the ACN model:
normal, zero, and extended box lengths in JP2 and JPX; short, oversized, and
truncated XLBox headers; nested box limits; final zero-length parent and
child boxes; `Psot=0` with complete, missing, or misplaced EOC, incorrect
PLT coverage, and bytes FF D9 inside packet data; nested opaque
associations; and excluded `j2cx` codestream storage. Accepted files are
indexed through every declared packet. These tests do not claim that the
generated decoder supports those alternate encodings.

## Regeneration policy

Regenerate when a model or harness changes, or when moving to a newer asn1scc:

1. update `spec/VERSION` when the upstream compiler revision changes and
   run its regressions. Any new local fixes must record their upstream base
   and numbered application order. Retire superseded series under
   `asn1scc-patches/reference/<upstream-base>/` and remove their resolved
   issue reports and local reproducers;
2. regenerate `jpeg2000/generated/` (Quick start step 2), then run
   `check-model.sh` (Quick start step 3), which checks the generated reader
   code against the model;
3. diff `manifest.tsv` against the previous one — new or removed rows must be
   explainable by the model, harness, or compiler change; label flips are
   findings;
4. rerun Quick start step 4 (the server and tool test runners);
5. commit the changed inputs, `VERSION`, `jpeg2000/generated/`, and corpus together.

Never edit vector files or manifest labels by hand.

## Scope and limits

Covered: every marker the server reads, the full codestream framing, and the
JP2/JPX box structure including the linked-JPX form that is the production
workload. Not covered, because ACN cannot describe them: the JPIP request
syntax (text), and the JPP response stream (its message headers are
7-bit-group chains like `Iplt`, but the payload is sized by the *value*
assembled from the chain, which no determinant can reference).

The complementary live-server suite in `tests/server/server_test.cc` includes an
independent JPP reader in `tests/server/jpp_validation.h`. It reconstructs data-bins
across HTTP chunks and successive responses, then compares them byte-for-byte
with independently assembled source payloads and metadata placeholders.
It covers embedded and linked JPX with unequal codestream geometry, partial
and complete cache models, association metadata, cropped and changing windows,
`stream` and `context` selections, small `len` budgets, and gzip. Stateful runs
check reconnection, pipelining, browser-style socket reuse, channel-cache
isolation, and target-ID mismatches. The reader also has inheritance and
malformed-message self-tests. See `COVERAGE.md` for the complete matrix.
Run it with:

```sh
cmake --build build
ctest --test-dir build --output-on-failure -R '^server$'
```

These synthetic packet payloads test transport, not image decoding. A real
JHV movie remains the decoding and rendering check. `ESAJPIP_SANITIZE` can
also be enabled for the live-server suite.

## Frequently asked

**Why not just write the test files by hand?** `file_manager_test.cc` already
contains a focused collection of hand-built cases. The corpus adds hundreds of
systematic cases, one per field, bound, and layer, with expected
outcomes derived rather than guessed. When the standard and the profile are
both written down formally, "what should the server do with this?" stops being
a judgment call.

**Why two layers?** So a failure says *which* rule the parser disagrees
with. "Rejects a valid standard file" and "accepts something the profile
excludes" are different bugs with different fixes, and layer 1 rejections
cite the standard's table so the disagreement can be settled by reading it.

**Why is asn1scc not in the build?** It only needs to run when the model
changes. For the corpus, the generated code is a judge, used once per model
change to label files; the tests read the labeled files. For the reader in
`../jpeg2000/`, its output is committed (`jpeg2000/generated/`) and compiled like any
other source. Keeping the compiler offline keeps asn1scc and .NET out of the
build.

**Why does the model have "corpus bounds"?** asn1scc's C structs embed
every list at its maximum size. Even with the corpus bounds a `Jp2Family`
is 877 MB; bounds like "65,535 boxes" would make it far larger. The bounds
only limit what the harness generates; they are not claims about the
standard.

**Can the generated decoder replace `file_manager.cc`?** Not the whole-file
decoder: the server indexes multi-megabyte files by offset without copying,
and ACN models fully decoded, bounded records. The shared reader in `../jpeg2000/`
now supplies that parsing: it steps from header to header, decodes one header
or one list element at a time with the generated code, and applies the shared
rules to JP2 and JPX files alike. `jpip` adds deferred packet indexing and
response generation. FileManager retains source opening and mapping. The
whole-file model stays the specification and the corpus oracle.
