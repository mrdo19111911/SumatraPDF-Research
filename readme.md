# SumatraPDF Research

A Windows research reader based on [SumatraPDF](https://github.com/sumatrapdfreader/sumatrapdf).
This is an independent fork; custom release numbers are not upstream SumatraPDF versions.

[Download Windows x64](https://github.com/mrdo19111911/SumatraPDF-Research/releases/latest)

Choose `-install.exe` for the Windows installer or `.zip` for portable use. Close the running reader before upgrading. The installer retains upstream SumatraPDF's installation identity; choose a separate directory if you also use the original reader. The installer is unsigned.

## Read and explore

- AUTO reading adjusts single/facing layout and content zoom to the available PDF pane.
- Paper KG appears beside one PDF page. Hover shows original context; click navigates and highlights a verified source passage. Closing KG restores AUTO.
- Chat state and KG are separate for each document tab. AI actions share a toolbar group.
- Research tools explain selections, build symbol indexes, compare papers or versions, and discuss figures with page references.
- Dark theme, adaptive top/right toolbar, an embedded research browser and expanded Markdown rendering.
- Reference collection searches arXiv and other configured sources and produces an HTML report of attempts and failures.
- Vietnamese narration can use locally downloaded speech tools. Drive and Zalo integrations need the user's own setup.

## Limits

KG generation requires a configured Claude Code CLI. Its relationships are AI proposals, not verified mathematical proofs. Matching a quote verifies its location only. Scanned PDFs need OCR.

KG interaction, source checks and AUTO fallback were tested with a mock AI provider. Reasoning quality on real papers has not been audited. Network sources may require login, have quotas, or fail. Google Drive needs the user's Desktop OAuth configuration. Zalo prepares sharing; the user sends the file.

Release archives contain application files and clean defaults. They exclude local document corpora, chat history, tokens and saved reading sessions.

## Build and test

Windows, Visual Studio C++ tools and Bun are required:

```sh
bun cmd/build.ts -rel
```

Output: `out/rel64/SumatraPDF.exe`. See the existing build scripts and [feature specifications](docs/spec) for details.

For targeted KG tests, build debug first, then run:

```sh
bun cmd/build.ts -dbg
bun tests/run-hidden.ts tests/ad-hoc-paper-map-model.ts --no-build
bun tests/run-hidden.ts tests/ad-hoc-paper-map.ts --no-build
```

The hidden test runner keeps native test windows off the visible desktop. Some ad-hoc benchmarks need external document fixtures; those corpora are not distributed here.

## Upstream and licenses

SumatraPDF is a multi-format (PDF, EPUB, MOBI, CBZ, CBR, FB2, CHM, XPS, DjVu) reader
for Windows under (A)GPLv3 license, with some code under BSD license (see
AUTHORS).

Upstream information:

- [Website](https://www.sumatrapdfreader.org/free-pdf-reader)
- [Manual](https://www.sumatrapdfreader.org/manual)
- [Developer Information](https://www.sumatrapdfreader.org/docs/Contribute-to-SumatraPDF)
