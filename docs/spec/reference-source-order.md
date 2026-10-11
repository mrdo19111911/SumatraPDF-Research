# Reference collection

- Resolve citation metadata without downloading from journal sites. Keep a common title when matching records disagree on DOI; do not choose a DOI arbitrarily.
- Try arXiv ID, title, first author, then DOI; omit unavailable identifiers.
- For unresolved titles, try the longest comma-delimited segment, then the second longest; also try period-delimited segments for citations using that format. Ignore DOI fields and numeric segments. Matching metadata takes precedence over guesses.
- Require one matching arXiv result. Author search alone cannot select a paper.
- Skip arXiv searches for citations dated before 1991 unless an arXiv ID is given. Unknown dates keep the normal search order.
- Try Sci-Hub next, using the citation's DOI or a uniquely resolved DOI.
- Finally search Google with the complete original citation plus `pdf`, including authors, journal and date. Use the title only when no citation is available.
- Accept arXiv result pages as candidates. Verify their metadata against the citation before downloading and checking the PDF's first-page title.
- Report JavaScript-required Google responses as browser-required, not as empty search results. Browse Google opens the complete citation in a browser.
- Anna remains available for manual browsing; collection does not use it.
- Keep completed downloads when retrying. Never overwrite existing PDFs.
- Record every query, endpoint, result and error. An empty search result does not establish that a paper is unavailable.
- Generate an HTML report in the collection folder. List failures first, with original citations and attempts. Open the report when collection finishes.

Targeted checks: `tests/ad-hoc-arxiv-priority.ts`, `tests/ad-hoc-paper-library.ts`, `tests/ad-hoc-library-progress.ts`, `tests/ad-hoc-google-reference.ts`, `tests/ad-hoc-citation-routing.ts`.
