# Review v4.6

Scope: changes to the original Windows researcher toolbar, paper assistant and Reading report. Earlier native reader, sharing and voice features are not a complete audit of this release.

## Correctness

| Finding                                                         | Change                                                                        | Regression                                |
| --------------------------------------------------------------- | ----------------------------------------------------------------------------- | ----------------------------------------- |
| Closing/replacing a paper left its research provider running    | Cancel through the tab lifecycle; reject a changed source after modal pickers | `--lifetime`                              |
| Clear invalidated the source map used by other providers        | Keep immutable source bindings until the tab changes file                     | `--sources`                               |
| Visible chat included a report absent from the provider session | Keep reports in their own windows                                             | `--history`                               |
| Reading did not follow live theme changes                       | Update native controls and CSS variables without rebuilding content           | Main toolbar and report Dark/Light checks |
| Reading displayed raw formula markup                            | Tokenize math before Markdown, sanitize, then render with local KaTeX         | Four delimiters; code remains literal     |

Red tests were run against v4.5. Green tests must use the final executable, including its portable package.

## Maintenance

- Reuse the existing theme, source navigation, embedded assets, cancellation and Win32 abstractions.
- No changes to vendored libraries; no new library download or CDN at runtime.
- Keep tab teardown gated out of ng. This review targets the Windows reader.
- Maintain targeted ad-hoc tests rather than adding the slow provider fixture to the normal suite.

## Research workflow

- Each research action has a toolbar icon and tooltip.
- Selected-text explanation requires a selection; figure/table explanation receives an actual rendered image.
- Version comparison identifies current PDF as old and the selected PDF as new.
- Report titles identify the PDF and action; clickable document IDs identify comparison sources.
- Reading font size changes independently of PDF zoom. Native text and original pages remain accessible.

## Limits

AI integration tests use a mock provider. They do not verify mathematical proofs, symbol completeness or correct transcription of every equation. Reading is not a lossless PDF reconstruction. Rendering an expanded original page and verifying a citation still perform synchronous engine work; responsiveness on very complex PDFs needs further testing. No general claim of zero bugs follows from these checks.
