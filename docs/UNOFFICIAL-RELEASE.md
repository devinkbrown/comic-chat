# Creating an unofficial modern build release

The repository's modernized client is a historical worked example, not an official
product release. GitHub Actions can produce an unsigned, portable ZIP file for
archivists, educators, and people experimenting with forks.

> **Note:** The earlier `v1.0-pre-modern` Windows lane (and its
> `ComicChat-1.0-pre` package) has been **archived** to the
> `version/v1.0-pre-modern` branch and is no longer built from `main`.

## Build the packages

1. Open the repository's **Actions** tab.
2. Select **Build unofficial modern clients**.
3. Choose **Run workflow** on the commit to package.
4. Download the `comic-chat-unofficial-modern-builds-*` artifact after the job
   finishes.

The artifact contains:

- `ComicChat-2.5-beta-1-unofficial-modern.zip`
- `SHA256SUMS.txt`

The ZIP contains a statically linked x86 executable, its bundled `ComicArt`
directory, the original help file, documentation, the repository license, and a
provenance notice. The workflow extracts the ZIP into a random path containing
spaces, launches it with an unrelated working directory, and verifies that it
remains running.

## Publish the manual GitHub release

1. Draft a new release with a date-based tag such as
   `unofficial-modern-builds-YYYY-MM`.
2. Use a title such as **Unofficial modern builds - Month YYYY**.
3. Mark the release as a **pre-release**.
4. Attach the ZIP file and `SHA256SUMS.txt`.
5. Identify the source commit and state that the executables are unsigned,
   unsupported archival builds rather than an official Microsoft product
   release.

No installer or code-signing material is required. Users should extract an
entire ZIP before running the executable so `ComicArt` remains beside it.
