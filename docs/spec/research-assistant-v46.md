# Research assistant v4.6

Extends the seven actions in `research-assistant-v45.md`.

- Reading/report colors follow the application immediately, without reloading text or resetting font size.
- A Dark/Light button in each report toggles the existing application reading theme.
- Math is tokenized before Markdown escapes are consumed and rendered after sanitization.
- Delimiters: `$...$`, `$$...$$`, `\(...\)`, `\[...\]`. Code spans and fences remain literal.
- KaTeX and WOFF2 fonts are served from embedded resources when a formula is encountered. No CDN.
- KaTeX trust is disabled; expansion and size are bounded. Failed rendering preserves source text.
- Closing a tab or replacing its file cancels its research worker. Modal file/CLI pickers revalidate the source path.
- Source IDs retain their bindings until the tab changes file; clearing one provider cannot invalidate another provider's citations.
- Research reports stay in their own windows. They are not inserted into a provider conversation that has never received them.
- Report titles show the action and PDF filename. Each report lists clickable document IDs and filenames.

## Validation

- `tests/ad-hoc-research-assistant.ts`: seven actions, indexed PDF sources, exact quote/region links, rejected citations, Reading font controls, four math delimiters, literal code, live theme changes and lazy original pages.
- Same test with `--lifetime`: a deliberately delayed mock provider must stop within two seconds of closing its document.
- `--sources` checks stable source IDs after Clear; `--history` checks that research does not fabricate provider history.
- Red evidence: `.work/toolchain/v46-red.log`, `v46-lifetime-red2.log` against v4.5.
- Context regressions: `v46-sources-red.log`, `v46-history-red.log` against v4.5.
- The provider is mocked. These checks establish integration behavior, not scientific correctness.

## Boundaries

Reading combines AI Markdown, native extracted text and original PDF images. It does not guarantee lossless reading-order reconstruction or correct formula transcription. Saving Markdown exports the AI report, not the native text/image fallback. Live source links require their in-app source registry.
