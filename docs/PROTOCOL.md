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
