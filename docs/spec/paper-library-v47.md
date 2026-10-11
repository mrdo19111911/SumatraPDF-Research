# Paper library v4.7.1

## User flow

- Toolbar: arXiv search and Collect all referenced papers.
- arXiv search shows title, authors, abstract and identifier; Download saves a PDF and Open PDF uses Sumatra's reader.
- Collect pins the source PDF when opened. Extraction runs off the UI thread.
- Review/edit citations, choose a destination, then Collect. Process references sequentially: arXiv API, Sci-Hub, Anna's Archive.
- Stop keeps completed PDFs. Retry in the same window skips completed files.
- Each row reports its source/status. Unresolved references have embedded-browser shortcuts.
- A loading bar covers extraction/search. Collection shows processed/total references and a separate downloaded PDF count; retry resets processed progress and Stop keeps partial progress.
- Default destination is `<source PDF path>-references`, beside the source PDF. Browse folder overrides it.
- HTTPS mirror addresses are editable and persisted. Network errors and CAPTCHA never count as downloads.

## Matching and limits

- Extract numbered/bracketed references after References/Bibliography; deduplicate identifiers/citations.
- Without a heading, inspect the final pages for labels 1, 2, 3 in order with dates or DOI/arXiv identifiers in at least two citations. This is a heuristic; review the list before collecting.
- Wrapped unnumbered, multiple-column or scanned bibliographies require review, editing or OCR. This is not an exhaustive bibliography recognizer.
- arXiv: explicit version must match; otherwise accept the current version. Match DOI or a normalized full title occurring in the citation; reject multiple matches.
- Anna: support CrazyZard-compatible `/search` JSON and `/download?path=` metadata, or the upstream website HTML selectors without Python. Require one unique PDF record with a complete matching title; validate title and an explicit DOI again on its detail page. Multiple editions/results require Browse. Try the published links sequentially, without bypassing verification or queues.
- `ResearchAnnaUrl` is the website base. `ResearchAnnaApiUrl` is optional; users run their own compatible server. Remote endpoints require HTTPS; HTTP is limited to localhost/127.0.0.1. The upstream repo hard-codes the old `.org` website and pins 2023 dependencies including uvloop; it is not a ready Windows installer.
- Crossref resolves missing DOI identifiers only when a unique complete result title occurs in the citation.
- Validate downloaded PDF with the document engine. Create new files; never overwrite existing PDFs.
- No promise that every reference exists in these services. Books, unpublished work, absent identifiers and unavailable mirrors can remain unresolved.
- arXiv calls are serialized with a cancellable interval; cancellation during an active HTTP operation is bounded by network timeouts.
- The native website workflow does not require an AI provider or Python. Running the optional third-party API server requires its own runtime/dependencies.
- Limits: 2,000 distinct references, 8 MB bibliography text, 20 arXiv results per search, 1 MB HTML/metadata and 64 MB per PDF. Larger or unsupported documents need a separate workflow.

## Verification

- [x] Release build: `library-build18.log`, zero errors/warnings.
- [x] Hidden local HTTP fixtures: extraction, duplicate citations, arXiv metadata, source order, wrong result rejection, valid PDFs, missing rows, retries.
- [x] Existing fetch/browser regression; direct toolbar icons, adaptive toolbar and AUTO/chat layout.
- [x] Real arXiv metadata and PDF smoke check: `library-arxiv-final-live.log`. Real PDFs: 40 and 51 extracted references, `library-final-counts.log`.
- [x] Portable package and SHA256 verification: `library-portable-final.log`; ZIP payload hashes verified.
- [ ] Actual Anna book download: blocked by website/network verification; not verified.
- [x] Sci-Hub direct PDF smoke check; DOI lookup currently presents CAPTCHA.

API reference: [arXiv manual](https://info.arxiv.org/help/api/user-manual.html).

TDD evidence in `.work/toolchain/`: `library-red.log`, `library-heading-red.log`, `library-mirror-red.log`, `library-ui-red.log`, `library-headless-red.log`. These cover the missing command, repeated heading, LibGen record chain, dashboard resource paths and heading-free bibliography.

## Online evidence (2026-10-10)

Atiyah–Macdonald, _Introduction to Commutative Algebra_: direct `.gl`, `.pk`, `.gd` probes failed TLS from this machine. The embedded `.gl` browser displayed **Checking your browser…** (screenshot `.work/toolchain/anna-browser-error.png`), rather than book results. No Anna book PDF was obtained. The CrazyZard `/download` route only supplies metadata and links; it cannot make a protected link automatically downloadable.

Anna native adapter: MIT attribution is in `research/THIRD-PARTY.txt` and `research/Anna-API-MIT-LICENSE.txt`. API reference: [CrazyZard repository](https://github.com/CrazyZard/annas-archive-api). The configurable website domains and browser-verification limitation are described in [Anna FAQ](https://annas-archive.gl/faq).

Anna TDD evidence: `library-anna-red.log` (old provider fails new API fixture), `library-anna-icon-red.log` (record-fetch lifetime failure), `library-anna-match-red.log`, `library-anna-html-red.log`, then `library-anna-green.log` (green API/HTML, alternate link, wrong DOI and ambiguous editions).

Final hidden regression logs: `library-final-ad-hoc-research-tools.log`, `library-final-ad-hoc-research-toolbar.log`, `library-final-ad-hoc-adaptive-toolbar.log`, `library-final-ad-hoc-auto-chat-layout.log`.

Final book check (2026-10-11): native WinHTTP failed with code 12175, no PDF saved (`library-anna-live-screen.log`). Screenshots show the collector error and browser verification; included in the package under `docs/evidence/`.

## Progress and complete collection check (2026-10-11)

- Progress: indeterminate extraction/search, processed/total references during collection, separate downloaded PDF count, retry reset and partial Stop state. UI test fails before the progress element is added and passes afterwards. Native fixture verifies 5/5 processed and 4 downloaded in the rendered UI.
- Release: `library-progress-build.log`, zero errors/warnings. Targeted hidden native test: `library-progress-native-green.log`; UI state test: `library-progress-ui-green.log`.
- Two complete real v4.7 collections, without citation edits: 1912.02928v4 downloaded 23/40 (57.5%); paper2 downloaded 13/51 (25.5%). Total 36/91 (39.6%): arXiv 24, Sci-Hub 12, Anna 0. All 36 files have different hashes. Details: `research/benchmark-2026-10-11/RESULTS.md`.
- These ratios include all extracted references, including books; they do not measure every obtainable PDF on the web. The final Anna request failed TLS for all unresolved rows. Intermediate provider errors are not retained. Title guessing can also omit valid arXiv matches.
- v4.7.1 changes progress reporting only; the provider search/download algorithm used for these ratios is unchanged.
