# Comic Chat protocol & file format (reverse-engineered)

Source of truth: Ghidra decompilation of the original `cchat.exe` (Microsoft
Comic Chat 2.5, build 1998-06-26). Symbols were stripped but **RTTI survived**,
recovering class names like `CIrcProto`, `CIrcSocket`, `CComicCharacterEntry`.

## Transport: comic-over-IRC

Comic Chat is a normal IRC client. It sends RFC1459 commands:

```
NICK <nick>
USER <user> <host> . :<real>
JOIN <#channel> [key]
MODE <target> <modes...>
PRIVMSG <target> :<text>
NOTICE  <target> :<text>
MODE ISIRCX            <- negotiates Microsoft's IRCX extensions
```

Comic state (avatar, pose, emotion, balloon kind) is **embedded inside the
message text** as tagged records, so a plain IRC client just sees readable text
while a comic client decodes the panel. The same record grammar is used for
saved `.ccc` transcripts.

## Record grammar

- Lines are `CRLF`-terminated.
- Fields within a line are `TAB`-delimited.
- The first field is a **keyword** selecting the record type.
- The original reader is a keyword-dispatch loop over `CArchive::ReadString`:
  read line → match first token → `new <RecordType>` → populate.

### Document headers

| Keyword             | Meaning                          |
|---------------------|----------------------------------|
| `#CHATCONVERSATION` | start of a comic transcript      |
| `#CHATLOCATOR`      | locator/bookmark document        |

### Records

| Keyword          | Type           | Notes                              |
|------------------|----------------|------------------------------------|
| `comicchar`      | comicchar      | `comicchar \t <name> \t <data>`    |
| `changeavatar`   | changeavatar   | switch speaker avatar              |
| `backdrop`/`Backdrop` | backdrop  | set panel background               |
| `Character`      | character      | character definition               |
| `Text`           | text           | spoken line (normal balloon)       |
| `WHISPER`        | whisper        | whispered line                     |
| `ACTION`         | action         | action / thought line              |
| `URL`            | url            | shared hyperlink                   |
| `SOUND`          | sound          | sound cue                          |
| `starthistory`   | starthistory   | begin history replay               |
| `getinfo`        | getinfo        | request peer info                  |
| `nick`/`NICK`    | nick           | nickname record                    |

### Session tags (keyword keeps the trailing colon)

| Keyword        | Type         | Notes                       |
|----------------|--------------|-----------------------------|
| `IRCSERVER:`   | irc_server   | `IRCSERVER:\t<host>`        |
| `IRCCHANNEL:`  | irc_channel  | `IRCCHANNEL:\t<#channel>`   |
| `COMICSDATA:`  | comics_data  | comic payload blob          |

Implemented in [`src/proto/record.zig`](../src/proto/record.zig).

## Asset formats: `.avb` (avatar) / `.bgb` (background)

Observed binary layout (decoder lives in `src/assets/`, WIP):

```
offset 0 : magic 0x8181 (bytes 81 81)
         : version word  (02 00 for .avb, 03 00 for .bgb)
         : header fields
         : length-prefixed name      (e.g. "Anna")
         : length-prefixed copyright  ("Copyright (c) 1996,1997,1998
                                        Microsoft Corporation\n<artist>")
         : embedded indexed palette   (RGB triples, visible as a ramp)
         : encoded bitmap data        (codec TBD from loader RE)
```

Avatars encode the full set of emotion/gesture poses (the "emotion wheel").

### Emotion-wheel pose table

After the copyright string each avatar carries a table of per-pose records,
one per drawable pose, terminated by a sentinel. Each record is laid out as:

```
i16 mouth.x, mouth.y     ; word-balloon / mouth anchor (0,0 on body poses)
i16 mid.x,   mid.y       ; small per-pose delta (±20)
i16 neck.x,  neck.y      ; neck-join anchor
06  01 01 01 04 03 03    ; record marker
u32 pointer              ; monotonic; several wheel cells share one value
... padding ...
i16 code                 ; emotion-wheel code (marker+18)
```

The **emotion code** is the key field (decoder: `bgb.poseTable`). Verified
across anna/cro/bolo/hugh/tiki:

- **Head poses** (mouth ≠ 0,0) use codes **1..8** — the eight spokes of the
  emotion wheel — plus **9 = neutral/centre**, which is the most common head
  code in every avatar.
- **Body poses** (mouth = 0,0) use a wider gesture vocabulary, codes **1..12**.
- The head table ends with a code-0 sentinel; the body table ends with a
  junk-code record. Both are filtered out.

The wheel's named spokes, from the original help index, are: happy, laughing,
sad, angry, shouting, afraid/scared, coy, shy/bored (center = neutral/deadpan).

**Still open:** the exact code→spoke naming and the code→bitmap linkage. The
`u32` pointer is monotonic but shared by multiple records, and the count of
distinct pointers does not match the count of decoded head bitmaps, so a simple
rank correlation is unreliable. Resolving it needs RE of the loader's pose-table
reader (`CComicCharacterEntry`). Until then `comic/emotion.zig` keeps a
best-effort enum→index mapping (correct for the leading neutral/happy/talking/
surprised faces, which the authored bitmap order happens to front-load) rather
than driving selection off the unverified linkage.

## Auto-layout

The famous automatic comic-strip composition (panel breaks, character
placement, balloon layout, tail routing) is documented in:

> D. Kurlander, T. Skelly, D. Salesin. *"Comic Chat."* SIGGRAPH '96.

We reimplement from the paper rather than from decompiled GDI calls.

## Decompilation workspace

`/home/kain/comicchat_extract/analysis/decompiled/_ALL.c` — full Ghidra dump
(8058 functions). Re-dump with the Java post-script
`analysis/DecompileAll.java` (modern Ghidra dropped Jython; `.py` needs
PyGhidra).
