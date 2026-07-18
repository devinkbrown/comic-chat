# Portable asset provenance audit

## Status

The 27 files in `src/assets/testdata/` were added in local commit
`eea74ff4a63bd1637b5a4ba92a02cc45cce5ab95`, whose subject describes them as
local Microsoft assets and "not for push". The commit itself does not identify
a source archive or conversion procedure.

A byte- and record-level audit on 2026-07-16 established a reproducible origin
for all 27 current files. Twenty-six can be generated exactly from same-named
assets in the MIT-licensed Microsoft checkout at revision
`c7df00f60bc8e9fdef413f139e61f7c37e024684`; current `xeno.avb` is a verbatim
copy from that revision, imported through `tools/import_pinned_xeno.sh`.

The previous 27-file set was byte-identical to a local extraction of the
Microsoft Chat 2.5 final release identified by `CChat25.INF` version
`4.71.2302.0`. That extraction has no retained installer URL/checksum and its
historical EULA prohibits third-party distribution. The one final-release-only
artwork variant, Xeno, was therefore removed rather than attributed to the MIT
source tree.

This finding does not apply to `legacy/`: its imported files have a separate
complete manifest and byte/Git-blob verification in `legacy/PROVENANCE.md` and
`legacy/UPSTREAM-SHA256SUMS.txt`.

## Portable font atlases

Microsoft's `fonts.cpp:72-92` constructs a second, italic Western face for
whispers, and `balloon.cpp:1813-1817` selects it only in
`CBWoodringWhisper`. The portable substitutes are Comic Neue Bold and Comic
Neue Bold Italic from Comic Neue commit
`ef5be72411141d01f0b865df8edb47e552c11c3c`, licensed under the SIL Open Font
License retained verbatim at `src/render/COMIC_NEUE_LICENSE.txt`.

| Pinned source | SHA-256 | Generated data | SHA-256 |
| --- | --- | --- | --- |
| `Fonts/TTF/ComicNeue/ComicNeue-Bold.ttf` | `3e7e5fccfd7e0788f317b43312151c1bd5cf058c9697a8d83eac3939050bd61e` | `src/render/fontdata.bin` | `437a505ab8d21363773f5c25c880562b3e2675780f1e5eafc8bd1204086d7507` |
| `Fonts/TTF/ComicNeue/ComicNeue-BoldItalic.ttf` | `5c312c2a2fa64eee82f3b87fcfab8f3b12a5e59b043124401d322eb323cfbf16` | `src/render/fontdata_italic.bin` | `c32558edf1c6ceb2f65231ca57b729fc276843473e2f4b7d4ae514ff9b5bc174` |

`tools/generate_font.py` verifies both source hashes and the complete generated
Zig/data hashes before writing any output. `tools/font-requirements.txt` pins
Pillow 12.2.0 (FreeType 2.14.3 in its supported wheel), preventing a different
rasterizer from silently changing checked-in glyph coverage or metrics. The
normal face remains the default for UI, titles, speech, thought, and action
text; only pure `BM_WHISPER` Woodring text uses the italic atlas.

## Verified byte-exact derivations

The following transformations were applied to the pinned paths shown. The
resulting complete-file SHA-256 values are the current values in the table
below, not merely equivalent decoded images.

| Current files | Pinned upstream files | Exact transformation |
| --- | --- | --- |
| `anna.avb`, `armando.avb`, `dan.avb`, `hugh.avb`, `jordan.avb`, `lance.avb`, `margaret.avb`, `mike.avb`, `susan.avb`, `tiki.avb`, `tongtyed.avb` | `v2.5-beta-1-modern/comicart/<same name>` | In `AK_COPYRIGHT`, replace the single Windows-1252 copyright byte `a9` with ASCII `(c)`; increase that record's 16-bit size and `AK_OFFSET_ADJUSTMENT` by two. Every other byte is unchanged. |
| `bolo.avb`, `cro.avb`, `denise.avb`, `kevin.avb`, `kwensa.avb`, `lynnea.avb`, `maynard.avb`, `rebecca.avb`, `sage.avb`, `scotty.avb` | `v2.5-beta-1-modern/artpack1/<same name>` | After the six-byte header, add `AK_OFFSET_ADJUSTMENT` with size 4 and value 79. Before the first new-format record, add a 67-byte `AK_COPYRIGHT` payload containing `Copyright (c) 1996, 1997, 1998 Microsoft Corporation\\nJim Woodring` plus NUL. Every original byte retains its order. |
| `den.bgb`, `volcano.bgb` | `v2.5-beta-1-modern/artpack1/<same name>` | Add `AK_OFFSET_ADJUSTMENT` value 65 and a 53-byte NUL-terminated `AK_COPYRIGHT` payload containing `Copyright (c) 1996, 1997, 1998 Microsoft Corporation`; reverse the first and third byte of every three-byte `AK_COLORPALETTE` entry. All compressed image bytes are unchanged. |
| `field.bgb`, `pastoral.bgb`, `room.bgb` | `v2.5-beta-1-modern/comicart/<same name>` | Apply the two-byte copyright edit above. Decompress the 50,400-byte 315x315 4-bpp image, change each scanline's two padding bytes (offsets 158 and 159 of its 160-byte stride) from `cd cd` to `00 00`, recompress at level 9 with zlib 1.0.8, and update the compressed-size field. |
| `xeno.avb` | `v2.5-beta-1-modern/comicart/xeno.avb` | Verbatim copy guarded by the pinned revision and source SHA-256 in `tools/import_pinned_xeno.sh`. |

The background compression was reproduced with upstream zlib tag `v1.0.8`
(commit `6759211ad8a5006689216a86c3267bb503bfccc1`). The historical
`legacy/artifacts/lib/i386/zlib.lib` independently identifies itself as zlib
1.0.8. Its level-9 output matches all three current compressed streams byte for
byte; a newer zlib stream is semantically equivalent but is not always byte
identical.

### Resolved Xeno difference

The former Xeno file had SHA-256
`d2f290af1d47ed79a81eb5fb2b1824dfb85359b4aa8f7ecc792d8a9fce03a303`.
After the common copyright metadata shift, one of its 21 image streams still
contained 60 changed decoded pixel bytes that no pinned source or converter
accounted for. It was replaced with the pinned upstream blob rather than
publishing an unverified final-release variant. Reimport and verify it with:

```sh
tools/import_pinned_xeno.sh /path/to/microsoft-comic-chat
```

## Current portable checksums

| File | SHA-256 |
| --- | --- |
| `anna.avb` | `6c599c83117a446f3ac60a48a5c5c96deea682787dbfccebd33a08a268b1ff54` |
| `armando.avb` | `bd1b792c36ce1f82c33ba7e835c9580809595da40e11b0748c0798d411c4fa19` |
| `bolo.avb` | `796d3c96d5ba61488d6fbe8f37162412e575a765a57157432b036eb1d709e8dd` |
| `cro.avb` | `0d47191a5783ba2fbbfb704876fbc5c42a39249a7f07f154ede30bb377bb3e49` |
| `dan.avb` | `708084c0add2300378bb7d639d9adb5068b8821d721bc67e36a0ddff715651e9` |
| `den.bgb` | `b86947de0dc1e30664fa961171c9b934688c5721ac05dd68354f8d7b371c6ae6` |
| `denise.avb` | `d15ba4a8880603ee0927532940a2154604bccb030be0a5bfc3df6359f533ef86` |
| `field.bgb` | `ffe4f83f578a7afcde2b22e5ed27b64d7efc312505153a59308b112d6029fc92` |
| `hugh.avb` | `a2687b1c7a07464525f6856c9e6578a6a2d1d3c6626f52187ec254fb085ebdff` |
| `jordan.avb` | `805273b2669f5a45f9b819aba80647791cd467c50ecba94f213273300fb6a322` |
| `kevin.avb` | `f4b6f77e2f37aaa54dc6e7e20d7fbde6ee490a714dd77a6dc4586d267848e305` |
| `kwensa.avb` | `a779c061ee6afa091b890be008be67986d291b88464e933da30721413017824b` |
| `lance.avb` | `afb69477e032157d570372a6ddf8c37521a2597a869a3d801993b58d356a2798` |
| `lynnea.avb` | `437df4958fd1a7fb10e699d4baa4c6aa54618960b5fa6a4db8f3c55ffb8e784f` |
| `margaret.avb` | `56e54f01ae98a48c39199a14ee288ab086c54043a10d66ddbeb515764469ffef` |
| `maynard.avb` | `64c389b920cf2eed124ef247eba3c2127cd2e1bb515fdcb13aa0821ac9e36aa0` |
| `mike.avb` | `a4126d344d0df6435bb6f701292c877f6fff21e3f9ec745ffda7b5623cf7d96e` |
| `pastoral.bgb` | `33a84300f5171f613e600e75abb3e6276c374d9d861e415eb183c4092ecacc8d` |
| `rebecca.avb` | `6b695cccbde201fdeb6ed6a70db887837af54b3c3ad3e2e54ae718ec9b325e6d` |
| `room.bgb` | `d606ba42ce780a347941f2a45fa52c50ec7a309f4a9de565f4bb9c610f9ced3d` |
| `sage.avb` | `6dee206cc6b8c5402df9fda4fc292c525989a315c912a81c076994ca59944e47` |
| `scotty.avb` | `a6226e8f744476a9d9ddde570df6bd5ffb70e84200683d6743782ca4705b75e9` |
| `susan.avb` | `6fd8f3c21c77d0f32eb95341c1f29bec7f8d43ab733d7932849b3c5f56f6a225` |
| `tiki.avb` | `34ed44eff924213ad03445109d70739249ea876f35870e2646a9bc197fd5a0b5` |
| `tongtyed.avb` | `eb45e8e121fb5e486683556643f3fb95d6ac0241f83c3ebf5b8a3caf7227b129` |
| `volcano.bgb` | `182a8ed446800bc1ed209a3f550f60293f2d23ed6cd9546bbd41bbec96c2e0cc` |
| `xeno.avb` | `67f176d23c1701918498fffe148dfb67f2fc836850ccf29f26647ed547fe292d` |

Reproduce the local checksum list with:

```sh
sha256sum src/assets/testdata/*
```

## AVB/BGB release condition

The portable asset provenance condition is satisfied: every current AVB/BGB
is either a byte-exact derivation of, or a verbatim checksum-guarded import
from, the pinned MIT snapshot. Future asset updates must preserve an immutable
source path, source checksum, license, and reproducible import/transformation.
