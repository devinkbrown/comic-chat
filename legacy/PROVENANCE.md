# Legacy port provenance

This directory contains a faithful, modern-Windows port of Microsoft Comic
Chat 2.5 beta 1 alongside the new portable ComicChat implementation.

## Upstream source

- Repository: <https://github.com/microsoft/comic-chat>
- Revision: `c7df00f60bc8e9fdef413f139e61f7c37e024684`
- Upstream source directory: `v2.5-beta-1-modern/`
- Revision date: 2026-07-15
- Imported on: 2026-07-16

The following paths were copied without source edits:

| Local path | Upstream path | Purpose |
| --- | --- | --- |
| `source/` | `v2.5-beta-1-modern/` | Microsoft Chat 2.5 beta 1 and its modern NMAKE build |
| `artifacts/inc/` | `artifacts/inc/` | Shared historical build headers and resources |
| `artifacts/core/` | `artifacts/core/` | Shared historical implementation units included by the client |
| `artifacts/lib/i386/` | `artifacts/lib/i386/` | Historical x86 `zlib.lib` build dependency |
| `artifacts-modern/core/` | `artifacts-modern/core/` | Modern compiler fixes for shared implementation units |
| `LICENSE` | `LICENSE` | Upstream MIT license |

`UPSTREAM-SHA256SUMS.txt` records every imported file. Run
`./scripts/verify-import.sh` on Linux, Git Bash, or WSL to verify that
the imported snapshot is intact and that no undeclared file has appeared in
the imported paths. When an upstream checkout is available, pass its path to
also compare every local file with the pinned Git blob:

```sh
./scripts/verify-import.sh /path/to/microsoft-comic-chat
```

Files outside those imported paths are ComicChat project build, packaging,
verification, and documentation additions. They do not claim Microsoft
authorship or sponsorship.

## Licensing and marks

The imported upstream repository is published under the MIT License; its
license is retained as `LICENSE`. The historical `source/license.txt` is also
retained verbatim because it is part of the source snapshot.

Microsoft names, logos, and product artwork may be trademarks. Builds made
from this directory must be described as unofficial and unsupported and must
not imply Microsoft sponsorship. `NOTICE.txt` is included in every package for
that reason.
