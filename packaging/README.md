# Desktop integration

Comic Chat accepts a `.ccc` conversation or `.ccr` locator as its only command-line
argument. The application opens the document and keeps the normal secure connection
workflow active.

On Windows, run `packaging\install-windows-associations.ps1` from PowerShell inside
the extracted binary package. It registers both formats for the current user and does
not require administrator access.

On Linux or BSD desktops, install `comicchat.desktop` under
`~/.local/share/applications/` and `comicchat-mime.xml` with the desktop's
`xdg-mime` or shared-mime-info tooling. The executable must be on `PATH`.
