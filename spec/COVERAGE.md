# JPEG 2000 served-profile coverage

This map connects the rules in the ASN.1/ACN model to generated vectors and
the esajpip source that enforces the served profile. It answers a narrower
question than a JPEG 2000 decoder conformance suite: does esajpip accept the
file structures it documents and reject malformed or unsupported structures?

The authoritative list of individual vectors and labels is
`tests/vectors/j2k/manifest.tsv`. A label is written as `standard/profile`.
For example, `valid/invalid` usually means that the structure is permitted by
the cited JPEG 2000 standard but excluded from the esajpip served profile.
For linked-source extent and availability tests, the standard label covers
the JPX structure only; the profile oracle additionally checks the companion
file. A structurally valid fragment list need not reference a valid codestream.
Vector names below are representative. The manifest-driven test runs all of them.
The server reaches these checks through `jpeg2000::ImageIndex` in `jpip`;
its FileManager only opens and maps sources.

## Coverage map

| Structure and reference | Model and rule | Accepted evidence | Rejected evidence | Server enforcement and profile decision |
| --- | --- | --- | --- | --- |
| Box lengths and containment, T.800 Annex I and T.801 Annex M | `TopBox`, `InnerBox`, ACN `lbox`, and `size deduced` regions in `jp2-boxes.*` | The four base files and all unchanged structural mutants | `*-len-*` vectors for top-level boxes and interpreted superboxes | `hv_boxes_next` bounds each box against its enclosing container. Metadata boxes that esajpip skips as a whole, including `jp2h`, `jplh`, and `uinf`, are opaque in the profile model, and so are the contents of the children of `jpch`. The inner-length mutants exercise `jp2h`, `jpch`, and in the linked base `ftbl` and `dtbl`. Those inside `jp2h` are standard-invalid but profile-accepted; those inside `jpch`, `ftbl` and `dtbl` are invalid at both layers, because the server walks their children. |
| JP2 signature, T.800 I.4 and I.5.1 | `file.signature` in `crossfield_impl.h` | `jp2.jp2` | `jp2-sig-bad.jp2`, `jp2-rule-file.signature-37.jp2` | `ImageIndex::Open` selects `hv_served_jp2` or `hv_served_jpx` from the extension supplied by FileManager; both check the complete 12-byte signature. |
| File Type box, T.800 I.5.2 and T.801 M.8 | `Ftyp`, `hv_rule_ftyp` (`file.ftyp-brand`, `file.ftyp-compatibility`), `file.ftyp-minor`, and the file's kind at layer 1 by the brand (`hv_ftyp_is_jpx`) | `jp2-rule-file.ftyp-compatibility-36.jp2`; `jp2-rule-file.ftyp-brand-34.jp2` (brand `abcd`, `jp2 ` listed: a JP2 file, `valid/invalid`) and `jp2-header-file.ftyp-brand-126.jp2` (a JPX file named `.jp2`, `valid/invalid`) | `jpx-embedded-rule-file.ftyp-brand-104.jpx` (a JP2 file without `jp2 `: `file.ftyp-compatibility`), `jp2-rule-file.ftyp-compatibility-35.jp2`; `jp2-header-file.ftyp-minor-99.jp2` and `jpx-embedded-header-file.ftyp-minor-73.jpx` (standard only); `jp2-rule-file.ftyp-second-58.jp2` (a box before ftyp) and `jp2-rule-file.two-boxes-59.jp2` (the signature alone) | `hv_rule_ftyp` requires the extension-specific brand and a matching compatibility entry. The standard layer tells a JP2 file from a JPX file by the brand (T.801 M.11.7.2) and requires `jp2 ` in a JP2 file's list, whatever its brand (T.800 I.5.2). `MinV` is intentionally ignored by both profile and server. |
| Unknown box handling, T.800 I.8 and T.801 M.12 | `boxtype` decode mapping and `other` alternatives in `TopPayload` and `InnerPayload` | `jp2-unknown-top-wxyz.jp2`, `jp2-unknown-top-binary.jp2`, `jpx-unknown-top-wxyz.jpx`, `jpx-unknown-inner-wxyz.jpx` | Not applicable | `hv_served_jp2` and `hv_served_jpx` skip unrecognized boxes within the enclosing box boundary. |
| SIZ image, tile, and component fields, T.800 Tables A.9 and A.11 | `Siz` (`SizFixed` and `Component`), `Siz-Profile` (`SizFixed-Profile` and `Component-Profile`), and the `siz.*` cross-field rules | `jp2.jp2`, `jp2-siz.rsiz-2.jp2` | `jp2-siz.xsiz-0.jp2`, `jp2-siz.csiz-2.jp2`, `jp2-siz.xtosiz-1.jp2`, `jp2-rule-siz.xsiz-52.jp2`; `jp2-rule-siz.tile-covers-origin-53.jp2` (the first tile ends at the image origin) | `hv_codestream_next` checks field ranges and relationships. The profile additionally requires one tile, zero origins, unit component sampling, signed-32-bit dimensions, and a packet count that fits signed JPIP state. The `siz.xsiz-52` vector keeps one tile and matching PLT entries for its 65,536 packets (eight 128-entry PLT segments in each of 64 tile-parts), so its only profile failure is the dimension limit. `Rsiz` is preserved rather than used as a blanket extension rejection. |
| COD coding style, T.800 Tables A.12 through A.21 | `Cod`, `Scod`, `Sgcod`, `Spcod`, `PrecinctSize`, `Cod-Profile` (no SOP), and `cod.*` rules | `jp2-cod.sgcod.progression-3.jp2`, `jp2-cod.spcod.cbStyle-63.jp2`, `jp2-precincts-cod.spcod.precincts.lowest.ppx-0.jp2` | `jp2-cod.sgcod.progression-5.jp2`, `jp2-cod.spcod.cbStyle-64.jp2`, `jp2-precincts-cod.spcod.precincts.higher[0].ppx-0.jp2` | `hv_codestream_next` accepts the five Part 1 progression orders, validates code-block and precinct fields, rejects reserved code-block style bits and SOP, and retains EPH support. The server derives all packet geometry from this COD. |
| MCT prerequisites, T.800 A.6.1 and G.2 | `siz.mct-components` | A normal no-MCT base and the hand-built valid-MCT fixture | `jp2-cod.sgcod.mct-1.jp2`, `jp2-rule-siz.mct-components-50.jp2` (MCT in a tile-part COD) and hand-built depth/sampling mismatches; `jp2-rule-siz.mct-geometry-54.jp2` (three components, one sampled 2:1) | `hv_rule_siz` checks MCT prerequisites against the decoded SIZ components and COD: at least three components, with matching depth and sampling for the first three. |
| Main-header marker placement, T.800 A.1, A.3 and Table A.1 | `Codestream`, `MainSegment` (with `other` and `noSegment`), `MainMarkerCode-Profile`, `codestream.one-*`, and `codestream.segment-after-sot` | `jp2-rule-main.com-21.jp2`, `jp2-main-unknown.jp2` (an `FF70` segment, skipped by its length: A.1); `jp2-main-ff30.jp2`, `jp2-tile-unknown.jp2` and `jp2-tile-ff30.jp2` are standard-valid only | `jp2-rule-codestream.one-cod-before-sot-0.jp2`, `jp2-rule-codestream.one-qcd-before-sot-1.jp2`, `jp2-rule-codestream.segment-after-sot-3.jp2`, `jp2-main-ppt.jp2`, `jp2-main-sop.jp2`, `jp2-main-ff00.jp2`, `jp2-main-ff30-length.jp2`, `jp2-tile-tlm.jp2` | `hv_codestream_next` requires one main COD and QCD before the first SOT. It rejects COC and POC, because they would alter packet layout, and PPM (`jp2-rule-main.packet-headers-moved-48.jp2`), because it moves the packet headers out of the tile-part data. It rejects PPT, SOP and EPH, which T.800 places elsewhere, 0xFF00, which is not a marker, and 0xFF30 to 0xFF3F, which have no segment to skip (T.800 A.1.3). It checks QCD and COM segment boundaries but leaves their bodies opaque in the served profile. The standard layer checks their length ranges (T.800 Tables A.27 and A.43) and Rcom (Table A.44). It skips other length-delimited main-header markers, and permits only SOT or EOC after tile-part data. |
| Codestream extent, T.800 A.4.4 and I.5.4 | `Codestream.extra`, `Codestream-Profile.extra`, `codestream.extent` | Existing JP2 and embedded JPX bases; a following box remains outside the codestream region | `jp2-after-eoc-*`, `jp2-precincts-after-eoc-*`, `jpx-embedded-after-eoc-*`: 1, 2, 64 and 65 trailing bytes inside `jp2c` | Both model layers reject trailing bytes: `codestream.extent` through the 64-byte corpus bound, `decode` above it. The reader independently checks EOC against its codestream boundary (`codestream.extent`). |
| SOT and tile-part sequence, T.800 A.4.2 | `TilePart` and the `sot.*` rules, per tile | `jp2-sot.tnsot-0.jp2`, `jp2-rule-sot.two-tile-parts-7.jp2`, `jp2-rule-sot.tnsot-late-8.jp2`, `jp2-rule-codestream.tile-part-limit-9.jp2`; `jp2-rule-sot.two-tiles-49.jp2` is standard-valid only | `jp2-sot.isot-1.jp2` (`sot.isot-range`), `jp2-sot.tpsot-1.jp2`, `jp2-rule-sot.tnsot-inconsistent-19.jp2`, `jp2-rule-codestream.tile-part-limit-20.jp2`; `jp2-rule-sot.tpsot-below-tnsot-55.jp2` (two tile-parts, both TNsot 1) | `hv_codestream_next` enforces one tile, sequential `TPsot`, consistent nonzero `TNsot`, and at most 64 tile-parts. `Psot=0` is supported only for the final tile-part, whose data runs up to the codestream's EOC without being scanned for markers; hand-built tests cover it because that encoding is outside the ACN model. |
| Tile-header restrictions, T.800 A.6.1 and A.6.4 | `TileSegment` for the standard layer and PLT-only `TileSegment-Profile` | PLT-only bases | `jp2-rule-tile.header-marker-11.jp2`, `jp2-rule-tile.header-coding-default-25.jp2`, `jp2-rule-tile.cod-once-27.jp2`; `jp2-rule-tile.qcd-once-56.jp2` and `-57.jp2` (two QCD in a tile-part header; QCD in the second tile-part); `jp2-tile-tlm.jp2` (a TLM, which T.800 places in the main header: `tile.marker-code` in the reader) | The served profile rejects COD, QCD, COM, and other tile-header markers. The client receives an empty tile-header data-bin, so accepting layout overrides there would make server and client packet geometry disagree. |
| PLT syntax and sequence, T.800 A.7.3 | `Plt`, `Iplt`, `hv_rule_zplt` (`plt.zplt-index` at the standard layer: each Zplt once, in any order; `plt.zplt-sequence` in the profile) | `jp2-precincts-rule-plt.two-markers-10.jp2`, `jp2-rule-plt.iplt-five-bytes-22.jp2`, `jp2-rule-plt.iplt-six-bytes-30.jp2`; `jp2-precincts-rule-plt.zplt-index-103.jp2` (Zplt 1, then 0: standard-valid only) | `jp2-plt.zplt-1.jp2` and unterminated or length-mutated PLT vectors | `ImageIndex::ReadCodestream` records the PLT descriptors from `hv_codestream_next` and checks sequential `Zplt` with `hv_rule_zplt`. `hv_plt_read` validates packet-length encodings lazily. Non-minimal encodings of up to ten bytes remain accepted because T.800 defines the represented value, not a shortest form; longer ones are rejected, as by the profile's `Iplt`. The standard layer reads any number of leading zero groups (Table A.36 sets no limit; `tests/jpeg2000/test_profile.c`). |
| PLT coverage and packet count, T.800 A.7.3, B.6, and B.9 | `plt.coverage`, `plt.zero-length`, `plt.packet-count`, `plt.padding-position`, and `codestream.packet-count` | All bases and complete multi-tile-part vectors; trailing zero entries after the packet set | Short and long PLT coverage, merged packet lengths, zero entries within the packet set, extra nonzero entries, and packet-count overflow | `ImageIndex::NextPacket` uses `hv_plt_read` for lazy lengths and `hv_plt_end_tile` / `hv_plt_end` for coverage and packet-count completion checks. The final packet is published only after those checks pass. The model counts packet entries where COD determines the layout. `CodingParameters::FillPrecinctCounts` rejects packet totals outside signed JPIP indexing. Trailing zero-valued PLT entries remain a documented compatibility exception. |
| Segments across headers, T.800 A.6 to A.9 and Table A.45 | `hv_segments` (`jpeg2000/hv_rules.c`), standard layer: the bodies of COC, QCC, RGN, POC, TLM, PLM, PPM, PPT and CRG read one element at a time, QCD's style and length, QCD/QCC entry coverage of the effective COD/COC decomposition (surplus permitted), the transform against the quantization style and the multiple component transform, Profile 0 and 1, TLM, PLM, PPM and PPT against the tile-parts, SOP and EPH in the data | The rule mutants' valid shapes (`jp2-rule-qcc-68.jp2`, `-rgn-76.jp2`, `-tlm-80.jp2`, `-plm-84.jp2`, `-crg-93.jp2`, ...); COC and POC in `jp2-rule-main.packet-layout-override-32.jp2` and `-33.jp2` (standard-valid only) | One rule mutant per rule, `coc.*`, `qcc.*`, `qcd.*`, `rgn.*`, `poc.*`, `tlm.*`, `plm.*`, `ppm.*`, `ppt.index`, `crg.*`, `sop.length`, `codestream.*` | The server rejects COC, POC, PPM and PPT, and skips QCC, RGN, TLM, PLM and CRG without reading them, so the profile keeps those opaque. |
| JP2 codestream cardinality, T.800 Annex I JP2 organization | `Jp2File-Profile` and `jp2.one-codestream` | `jp2.jp2`, `jp2-precincts.jp2` | `jp2-rule-jp2.one-codestream-38.jp2` (two), `jp2-header-jp2.one-codestream-10.jp2` (none) | `hv_served_jp2` requires exactly one top-level `jp2c`. It reads no other box, so the profile layer keeps every other box of a .jp2 opaque (`Jp2Payload-Profile`; `jp2-rule-ftbl.one-flst-51.jp2`). Other JP2 metadata is preserved but is not a complete JP2 semantic validation. |
| JPX Reader Requirements, T.801 M.11.1 | `jpx.reader-requirements`, `Rreq` (ML, NSF and NVF as ACN determinants), `rreq.extent` | JPX bases with `rreq`, `jpx-embedded-header-rreq.mask-length-61.jpx` | `jpx-embedded-rule-jpx.reader-requirements-13.jpx`, `jpx-embedded-header-rreq.ml-62.jpx`, `-rreq.nsf.jpx`, `-rreq.nvf.jpx`, `-rreq.extent.jpx` are `invalid/valid` | The standard layer requires one `rreq` immediately after `ftyp`, with well-formed contents. esajpip ignores it and accepts its absence as a deliberate profile reduction. `hv_merge` writes it with the generated encoder (`hv_write_rreq`), and its tests check it with `hv_check_jpx_headers`. |
| JP2 header boxes, T.800 I.5.3 (JP2) and T.801 M.11.5 to M.11.7 (JPX) | `Ihdr`, `Bpcc`, `Colr`, `Pclr`, `Cmap`, `Cdef`, `Res`, `Cgrp` in `jp2-boxes.*`; the header rules at the end of `jp2-boxes.asn1` (`hv_rules.c`, standard layer only) | `jp2-header-jp2h.palette-0.jp2`, `jp2-header-cdef.opacity-49.jp2`, `jp2-header-jp2h.resolution-1.jp2`, `jp2-header-jp2.ipr-76.jp2`, `jpx-embedded-header-jpx.header-defaults-50.jpx`, `jpx-embedded-header-jpx.no-jp2h-52.jpx`, `jpx-embedded-header-colr.cielab-parameters-68.jpx` and `-colr.ciejab-parameters-69.jpx` (EP after EnumCS, T.801 M.11.7.4), `jpx-embedded-header-jpch.ipr-box-78.jpx`, `jpx-embedded-header-jpx.ipr-top-94.jpx` (IPR 1 in a `jpch`, the IPR box at the top level), `jpx-embedded-header-jplh.creg-80.jpx`, `jpx-embedded-header-jplh.cgrp-84.jpx` (a `cgrp` with an unknown box), `jp2-header-jp2h.cgrp-90.jp2` (a `cgrp` in a JP2 file, where T.800 does not define it) | The other `jp2-header-*` and `jpx-embedded-header-*` vectors: one per rule, and field values outside the types (`ihdr.c`, `cmap.mtyp`, `res.vd`, ...); `jpx-embedded-header-jpx.one-jp2h-81.jpx` (two jp2h: `invalid/valid`); in a `cgrp`, `jpx-embedded-header-colr.approx-85.jpx`, `-colr.one-method-86.jpx`, `-cgrp.empty-87.jpx`, and `-cgrp.placement-88.jpx` (in a `jpch`) and `-89.jpx` (at the top level) | The server reads none of these boxes and passes them to the client as metadata, so the profile keeps them opaque and every such vector is `invalid/valid`, except `jp2-header-jp2.one-codestream-10.jp2` (no `jp2c`), which is invalid at both layers. The tools that write files check them: `hv_transcode` and `hv_merge` reject an input that fails `hv_check_jp2h`, and `hv_merge`'s output passes `hv_check_jpx_headers`. |
| JPX codestream headers and numbering, T.801 M.11.6 | `jpx.codestream-count`, `jpx.no-jpch`, and source-box-order rules | `jpx-embedded.jpx`, `jpx-embedded-rule-jpx.box-order-12.jpx` | `jpx-embedded-rule-jpx.codestream-count-15.jpx`, `jpx-embedded-rule-jpx.no-jpch-17.jpx` | `hv_served_jpx` numbers top-level `jp2c` and `ftbl` boxes in physical order and requires one `jpch` per served codestream. The profile does not implement the standard fallback to `jp2h` when `jpch` is absent. |
| Data Reference and URL boxes, T.801 M.11.2 and Data Entry URL box, T.800 I.7.3.2 | `DataReferences`, `UrlHeader` (VERS and FLAG 0), `DataEntryUrl-Profile`, `dtbl.ndr-count`, and `url.*` | `jpx-linked.jpx` | `jpx-linked-rule-dtbl.ndr-count-5.jpx`, `jpx-linked-rule-url.version-flags-7.jpx` (VERS 1: `decode` at both layers, `url.version-flags` in the reader), `jpx-linked-rule-url.file-scheme-6.jpx`, `jpx-linked-rule-url.terminator-19.jpx` (a byte after LOC's NUL); `jpx-linked-rule-dtbl.non-url-25.jpx` (an xml box in the dtbl), `jpx-linked-rule-url.percent-encoding-26.jpx` (the escape `%zz`: `valid/invalid`) | `hv_served_jpx` accepts one top-level `dtbl` containing version-zero, flag-zero local `file://` references, each ending at LOC's NUL; T.800 I.7.3.2 requires VERS and FLAG 0 in every Data Entry URL box. Remote URLs are outside the served profile. The model's 1,024-character LOC maximum limits corpus allocation; the server, reader, and merger impose no matching cap. |
| Fragment tables, T.801 Annex M Fragment Table box | `FragmentList`, `FragmentList-Profile`, `Fragment`, `ftbl.one-flst`, `flst.nf-count`, and `flst.dr-range` | `jpx-linked.jpx` | `jpx-linked-rule-ftbl.one-flst-4.jpx`, `jpx-linked-rule-flst.dr-range-3.jpx`, `jpx-linked-rule-flst.dr-external-2.jpx`, `jpx-linked-rule-flst.nf-count-20.jpx`, `jpx-linked-rule-flst.one-fragment-21.jpx` (a byte after the only fragment: `decode` at both layers, `flst.one-fragment` in the reader); at the edges of T.801 Table M.17, `jpx-linked-rule-flst.off-22.jpx` (OFF 11: invalid at both layers), `jpx-linked-rule-flst.source-extent-23.jpx` (LEN 0) and `jpx-linked-rule-flst.one-fragment-24.jpx` (NF 0, no fragment), both `valid/invalid` | `hv_served_jpx` requires one fragment per `flst` and one `flst` per `ftbl`. The profile requires an external reference and later verifies the fragment against the complete codestream extent in the linked JP2. |
| Fragments in the file, T.801 M.4 and M.11.3.1 | `hv_rule_fragments`: complete NF tuples, decoded fields and DR within NDR, also inside `j2cx`; a fragment with DR 0 within one `mdat` box's payload (`flst.mdat`), the first at SOC (`flst.codestream-start`); standard layer | `jpx-linked-rule-flst.dr-external-2.jpx`, `jpx-linked-rule-jpx.linked-shape-10.jpx` (codestreams in `mdat` boxes) | `jpx-linked-rule-flst.mdat-28.jpx` (a fragment at `ftyp`), `jpx-linked-rule-flst.codestream-start-29.jpx` (after SOC) | The server links to other files only (`flst.dr-external`). |
| JP2 compatibility and baseline JPX, T.800 I.5.2, T.801 M.3.6 and M.9.2 | A JPX file that lists `jp2 ` is checked as a JP2 file too (`jp2.*`, `jp2h.position`, `colr.method`, `colr.enumcs` on jp2h's first `colr`); one that lists `jpxb` by the baseline rules (`jp2h.position`, `jpxb.colour`, `jpxb.header-override`, `jpxb.fragments`); standard layer | `jpx-embedded-header-jpx.jp2-compatible-158.jpx`, `jpx-embedded-rule-file.ftyp-compatibility-36.jpx` (a baseline file) | `jpx-embedded-header-jp2.one-jp2h-159.jpx`, `-colr.enumcs-160.jpx`, `-jp2h.position-161.jpx`, `-jpxb.header-override-162.jpx`, `-jpxb.colour-163.jpx`, `jpx-linked-rule-jpxb.fragments-27.jpx` | The server applies neither set of rules. |
| Box placement, T.800 I.2.2 and T.801 M.11 | In the children of `jpch`, `ftbl` and `dtbl` at both layers `hv_rule_child` (`box.*`, `dtbl.non-url`, `jpch.nested-jp2c`), the model's opaque `jpch`, `ftbl`, `dtbl` and `asoc` alternatives of `InnerPayload`, and at the top level no alternative for `flst` and `url`; elsewhere, at the standard layer, the box tree rules (`hv_rule_box_tree`, on the file's bytes, at every level of every superbox of the file's kind) | `jpx-linked.jpx`, `jpx-asoc-nested.jpx`, `jp2-header-ihdr.top-level-110.jp2`, `jpx-embedded-header-jpx.lbl-164.jpx`, `jp2-top-flst.jp2` (a box T.800 does not define), `jpx-embedded-header-jplh.opct-130.jpx`, `-jpch.cref-137.jpx`, `-jpx.comp-139.jpx`, `-jpx.drep-143.jpx`, `-jpx.j2cx-151.jpx` (standard-valid only), `-jpx.jclx-167.jpx`, `-jpx.j2cx-ltbl-174.jpx` (standard-valid only), `jp2-header-jp2.uinf-100.jp2`, `-jp2.uuid-108.jp2` | `jpx-embedded-rule-jpch.nested-jp2c-18.jpx`, `jpx-nested-jpch.jpx`, `jpx-nested-ftbl.jpx`, `jpx-nested-dtbl.jpx`, `jpx-linked-rule-box.flst-placement-17.jpx`, `jpx-linked-rule-box.url-placement-18.jpx`; `jpx-top-flst.jpx` and `jpx-top-url.jpx` (`decode` at both layers); `jpx-embedded-header-j2cx.order-166.jpx` (a `j2cx` before a `jp2c`), `-j2ci.placement-170.jpx`, `-j2ci.ncs-171.jpx`, `-j2cx.contents-172.jpx`, `-j2ci.ltbl-173.jpx` and `-j2cx.top-codestreams-175.jpx` (invalid at both layers, as the profile has no `j2cx`); standard only: `jp2-header-box.nested-jp2c-66.jp2`, `jpx-embedded-header-box.flst-placement-67.jpx`, `jp2-header-box.top-level-91.jp2`, `jp2-header-box.containment-165.jp2` (a `colr` in `res`), `jpx-embedded-header-jclx.headers-168.jpx`, `-jclx.order-169.jpx`, `-jlxi.placement-176.jpx`, `-jlxi.counts-177.jpx`, `-jlxi.repetition-178.jpx`, `-jclx.contents-179.jpx`, `-jlxi.frames-180.jpx`, `-grp.placement-181.jpx`, `jp2-top-url.jp2`, the `file.one-*`, `box.nested-top-level-*`, `colr.placement-*`, `opct.*`, `creg.*`, `pxfm.once`, `cref.placement`, `comp.*`, `copt.placement`, `inst.placement`, `drep.once`, `gtso.*`, `asoc.children`, `lbl.characters`, `box.framing`, `jpx.colr`, `uinf.contents`, `ulst.nu-count`, `uuid.id` and uinf `url.*` header vectors | `hv_served_jpx` descends into top-level `jpch`, `ftbl` and `dtbl` only, and rejects `jp2c`, `jpch`, `ftbl` and `dtbl` below the top level, `flst` outside an `ftbl` and `url` outside a `dtbl`, the top level included; it skips a header box, `nlst` or `lbl` at the top level. The standard layer reads the rest of the tree, which the server skips; in a JP2 file only T.800's superboxes (`jp2h`, `res`, `uinf`), as a JP2 reader skips the boxes T.801 adds. |
| Linked-JPX source shape | Profile rules `jpx.mixed-sources`, `jpx.linked-shape`, `url.jp2-target` | `jpx-linked.jpx` and its two companion JP2 files | `jpx-linked-rule-jpx.mixed-sources-9.jpx`, `jpx-linked-rule-jpx.linked-shape-10.jpx` (no top-level `dtbl`, the codestreams in this file in `mdat` boxes, DR 0: standard-valid; the DR rules come after the count rules, as in the reader), `jpx-linked-rule-url.jp2-target-8.jpx` | This is a Helioviewer served-profile decision, not a general T.801 restriction. `hv_served_jpx` supports either embedded codestreams or one complete linked JP2 per codestream, never a mixture or JPX-to-JPX recursion. |

## Intentional standard/profile differences

The manifest holds 222 `invalid/valid` vectors, of nine kinds:

- Inner box lengths inside `jp2h` are checked by the standard model but not by
  esajpip, which preserves that metadata without interpreting it. These are the
  `jp2[-precincts]-len-28-*`, `jp2[-precincts]-len-3e-*`,
  `jpx-embedded-len-40-*`, `jpx-embedded-len-56-*`, `jpx-linked-len-43-*` and
  `jpx-linked-len-59-*` vectors.
- The contents of the header boxes (`jp2h`, `jpch`, `jplh`) and the rest of the
  box tree: the `jp2-header-*` and `jpx-embedded-header-*` vectors that break a
  header rule or field range, a placement or cardinality rule of the box tree
  (`box.*`, `file.one-*`, `colr.placement`, `cgrp.placement`, `opct.*`,
  `creg.*`, `pxfm.once`, `cref.placement`, `comp.*`, `copt.placement`, `inst.placement`,
  `drep.once`, `gtso.*`, `asoc.children`, `uinf.contents`, `ulst.nu-count`,
  `url.*`, `uuid.id`, `lbl.characters`, `jpx.colr`, `jclx.*`, `jlxi.*`,
  `grp.placement`), a JP2 compatibility rule (`jp2.mct-colourspace`) or a
  baseline JPX rule (`jpxb.colour`, `jpxb.header-override`), other than the
  `rreq` ones below. The server does not read them; the tools reject such
  inputs.
- The contents of the JPX Reader Requirements box, which the profile keeps
  opaque: `jpx-embedded-header-rreq.ml-62.jpx`,
  `jpx-embedded-header-rreq.nsf.jpx`, `jpx-embedded-header-rreq.nvf.jpx`,
  `jpx-embedded-header-rreq.extent.jpx` and
  `jpx-embedded-header-rreq.deprecated-feature-152.jpx`. A missing JPX Reader
  Requirements box is rejected by the standard layer but accepted by the
  server: `jpx-embedded-rule-jpx.reader-requirements-13.jpx` and
  `jpx-linked-rule-jpx.reader-requirements-0.jpx`.
- Segments the server skips or does not relate: the rule mutants of the
  Segments across headers row whose rule is `qcc.*`, `qcd.style`,
  `qcd.reserved-bits`, `qcd.length`, `rgn.*`, `tlm.*`, `plm.*`, `crg.*`,
  `codestream.profile-0`, `codestream.profile-1`,
  `codestream.quantization-transform`, `codestream.quantization-coverage`, `codestream.sop-unsignalled` or
  `codestream.eph-unsignalled`.
- Zero-valued PLT entries after the logical packet set are accepted for
  deployed files (every one of 4,014 EUI files checked carries them; see
  `jpeg2000/README.md`). The vectors are
  `jp2-rule-plt.trailing-zero-16.jp2` and
  `jpx-embedded-rule-plt.trailing-zero-16.jpx`.
- Rsiz, which the server keeps as it is: value 3 declares the 2k cinema
  profile in T.800:2015 and T.800:2019 Table A.10; it is not reserved.
  The standard validator does not implement that profile and returns
  `siz.unsupported-profile`. The fixtures `jp2-siz.rsiz-3.jp2` and
  `jpx-embedded-siz.rsiz-3.jpx` also violate Table A.46 independently:
  they have one component instead of three. Their bytes and non-valid
  standard labels are retained. The T.801 extensions value in a JP2 file
  remains rejected (`jp2-siz.rsiz-32768.jp2`, `jp2.rsiz`).
- Association contents are opaque to the server. `jpx-asoc-child-length.jpx`
  has a malformed inner box length and `jpx-asoc-one-child.jpx` violates the
  two-child minimum in T.801 M.11.11. The standard model rejects both; the
  profile accepts them. The server test verifies that each complete payload
  remains one metadata bin. `jpx-asoc.jpx` is valid at both layers, while
  `jpx-asoc-outer-length.jpx` exceeds the file boundary and is rejected by both.
- Boxes of a .jp2 other than the signature, file type and codestream boxes
  are opaque to the server, which reads none of them: `jp2-top-url.jp2` puts a
  `url` at the top level, where T.800 I.2.2 does not allow it.
  `jp2-top-flst.jp2` and `jp2-rule-ftbl.one-flst-51.jp2` hold boxes T.800 does
  not define, which a JP2 reader skips (T.800 I.8): valid at both layers.
- A baseline JPX file (`jpxb` in the compatibility list) whose first
  codestream is in another file: `jpx-linked-rule-jpxb.fragments-27.jpx`
  (T.801 M.9.2.5). The server serves linked codestreams whatever the brand.

A main-header marker segment whose code T.800 does not define is skipped by
its length at both layers (T.800 A.1): `jp2-main-unknown.jp2`,
`jp2-precincts-main-unknown.jp2` and `jpx-embedded-main-unknown.jpx` are
valid.

`valid/invalid` vectors cover standard-permitted structures outside the served
profile and linked sources that fail cross-file checks. They cover nonzero origins, multiple tiles, component
subsampling, SOP, main or tile-header packet-layout overrides, missing PLT,
more than 64 tile-parts, dimensions beyond signed 32 bits, oversized
packet state, two codestreams in a .jp2
(`jp2-rule-jp2.one-codestream-38.jp2`), remote or non-JP2 links, an empty
LOC or one with an invalid escape (`jpx-linked-rule-url.loc-30.jpx`,
`jpx-linked-rule-url.percent-encoding-26.jpx`), COM and undefined marker
codes in a tile-part header and 0xFF30 to 0xFF3F markers
(T.800 A.1.3), PLT segments out of Zplt order
(`jp2-precincts-rule-plt.zplt-index-103.jp2`), a file whose brand is not the
one its extension names (`jp2-rule-file.ftyp-brand-34.jp2`,
`jp2-header-file.ftyp-brand-126.jp2`), a third codestream in a `j2cx`
(`jpx-embedded-header-jpx.j2cx-151.jpx`), and general JPX organizations that
esajpip does not serve.

## Coverage boundaries

The companion oracle measures codestream extents when writing each generated
JP2, then compares them with decoded `flst` claims. The four vectors
`jpx-linked-rule-flst.source-extent-12.jpx` to `-15.jpx` shift the start or
end by one byte, and `jpx-linked-rule-url.missing-companion-*` names an
unavailable file.
All five pass the structural standard model but fail the profile oracle and
server. The matching linked base is the accepted boundary case.

`CheckSourceForms` adds 28 explicit server cases for alternate framing and
container forms. It covers normal, zero, and extended box lengths, exact and
overrunning enclosing boundaries, truncated XLBox headers, `Psot=0` endings
and PLT coverage, recursive associations, and excluded `j2cx` storage. Every
accepted file is packet-indexed. Nested associations are also checked for
preservation as one complete metadata bin. These cases do not receive ASN.1
model labels.

The generated `jpx-graph-*` fixtures exercise sequential, reversed, and repeated
data references with distinct companion JP2 files (T.801 M.11.2 and M.11.3.1).
One companion has one packet, the other four packets across two tile-parts.
The server test checks the resolved filenames, packet counts, and exact packet
extents while interleaving forward and backward lookups across codestreams.
The repeated-reference case also leaves one data-reference entry unused.
The model checks the JPX structure; these server checks verify its resolution
against the companion files.

The generated `plt.boundaries` fixtures put packet lengths 1, 127, 128, and
129 in separate PLT markers across two tile-parts (T.800 A.7.3 and Table A.36).
The server test checks exact lengths and offsets, including backward lookups
after indexing the second tile-part. The `plt.second-part-short` and
`plt.second-part-long` variants change only the final length by one byte and
must fail during indexing. These fixtures cover both JP2 and embedded JPX.

The hand-built progression fixtures independently check the packet order from
T.800 B.12.1.1–B.12.1.5 for all five progressions, using two layers, two
resolutions, two components, and two precincts per resolution. Expected source
offsets come from explicit packet sequences rather than the server's index
formulas. They also check lazy indexing, retrieval of earlier packets, and
identical JPP responses across source progressions. This exercises packet
placement, not entropy decoding.

The generated corpus does not express these paths:

- `Psot=0`, `LBox=0`, and XLBox. The `CheckSourceForms` cases cover the
  supported open-ended forms and malformed container boundaries.
- The semantics of JPX composition. The box tree rules check where the
  composition, desired reproductions and multiple codestream (`j2cx`) boxes
  are and how often (`comp.*`, `copt.placement`, `inst.placement`, `drep.once`, `gtso.*`,
  `jpx-embedded-header-jpx.j2cx-151.jpx`), and the server test checks the
  `j2cx` storage exclusion; what the instructions render remains outside this
  suite.
- Whether an XML or IPR box holds well-formed XML (T.800 I.7.1, T.801
  N.5.4). Both are opaque at both layers; the check needs an XML parser.
- Entropy-coded packet correctness, transforms, color interpretation, sample
  decoding, and rendering. esajpip does not perform those operations.
- JPIP request semantics and JPP-stream response framing. Those belong to the
  protocol and live-server tests, not this source-file model.

Corpus bounds such as 32 top-level boxes, 128 entries per PLT segment, and
4 KiB opaque payloads limit what the generator can allocate. They are not
JPEG 2000 limits and do not become server limits.

## Complementary JPP response coverage

`tests/server/jpp_validation.h`, run by the live-server test, independently reads
T.808 A.2 message headers and D.3 EOR messages. Sixteen runs combine embedded or
linked JPX, plain or gzip responses, absent or partial cache-model claims, and
`stream` or `context` selection. Each codestream has 130 precincts, but their
widths, precinct sizes, and layer counts differ. Large final-precinct packets
exercise multi-byte offsets and lengths as well as Bin-ID continuation bytes.
Small response budgets and the server's 128-byte test chunks split packets
across chunks and responses.

The test compares every payload with its source bytes, reconstructs main-header,
tile-header, precinct, and metadata bins, and checks offsets, completion flags,
EOR reasons, and the absence of retransmission after the window is complete.
Expected metadata includes independently built codestream and association
placeholders, with a nested association preserved in a separate metadata bin.
Partial `Hm` and `P` models seed known prefixes before transfer.

Two additional stateful runs, one per source form, cover:

- `len=0`, `1`, and `2`, absent `len`, and metadata-only requests;
- partial `M0`/`M1` models, including a prefix ending inside a placeholder;
- single-pixel precinct boundaries, an empty intersection, backward scrubbing,
  overlapping selections, and default window offset and size;
- cache continuity after TCP reconnection and pipelined cached requests;
- a new channel on an existing socket, complete-bin claims, independent
  channel caches, and continued use after closing the other channel;
- discarding cache claims under a mismatched target ID.

Each window permits only its independently selected bins. Revisiting completed
precincts must not resend bytes, including after cache-prefix compaction.
Reader self-tests cover header inheritance, defaults resetting between
responses, and rejection of truncated, overflowing, reserved, misplaced, and
incorrect-payload messages.

This is a served-profile response oracle, not a general JPIP decoder. It does
not validate entropy-coded samples or every legal extended message class.
The separate real-data check on September 19, 2026 used 100 timestamp-ordered
EUI FSI frames in a linked JPX. Headless JHV loaded the JPIP URL and recorded
through SAMP, reporting completion. `ffprobe` confirmed 100 frames at
4096 × 4096. Neither server nor JHV logs contained a serving or decoding error.
It does not replace the release checks in `DEVELOPMENT.md` (the 4,014-frame
fixture, the Debian build).

## Keeping this map accurate

When a model or parser rule changes:

1. Regenerate the corpus and review every manifest label change.
2. Run `file_manager_test`, which checks opening and every declared packet for all
   profile-valid rows and confirms rejection of every profile-invalid row.
3. Update this map when a rule class, served-profile decision, or coverage
   boundary changes. Individual edge vectors remain discoverable in the
   manifest and do not need separate rows here.

### Effective coding and JPX regression cases

The appended rule vectors cover insufficient QCD and QCC entries, permitted
surplus entries, and a Profile 1 tile COD that repairs the main default.
`tests/jpeg2000/test_rules.c` also checks component overrides, derived quantization,
all components overridden, and isolation between successive tiles.

The appended header vectors cover complete, truncated, and overlong nested
`j2cx/ftbl/flst` lists, invalid offsets and data references, and JPX channel
associations with and without JP2 compatibility. The reader tests also cover
these fragment checks at file level and validate the actual DTBL URL count.

OpenJPEG rejects `jpx-embedded-header-jpx.cdef-colour-197.jpx` before reading
CDEF: its JP2 reader requires a PCLR box before CMAP. The vector uses JPX's
direct component mapping without a palette (T.801 M.11.6) and multiple
channels associated with one color (M.11.7.5). This specific decoder limit
is recorded in `check-model.sh`; the fixture and its standard-valid label
are retained.
