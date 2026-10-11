# Research assistant v4.5

- RA01: Seven toolbar actions: grounded question, selection explanation, symbol index,
  paper comparison, version comparison, figure/table explanation, reading view.
- RA02: Extract every page with document/page identifiers. Treat source as data.
  State missing text, uncertainty and unverified claims.
- RA03: References use `sumatra-source:D/P#quote`. Only registered PDFs and valid
  physical page numbers may open. Highlight an exact quote only on that page.
  Selected figures can use `#rect=x,y,width,height`; reject rectangles outside the PDF page.
- RA04: Comparison accepts additional local PDFs; version comparison accepts one.
  Model output must cite both sources and distinguish absence from contradiction.
- RA05: Figure explanation supplies an actual rendered page or selection image.
- RA06: Reading view offers text-size controls and lazy original page images.
  AI text/layout is a reading aid; original pages remain the authority for mathematics.
  Every page also retains its complete native extracted text, even if AI output stops early.
- RA07: Work runs off the UI thread. Cancel, Clear and closed/replaced tabs invalidate
  delivery. Tests use a local provider fixture; live scientific correctness needs review.

New commands: `CmdResearchAsk`, `CmdResearchSelection`, `CmdResearchSymbols`,
`CmdResearchCompare`, `CmdResearchVersions`, `CmdResearchFigure`, `CmdResearchReading`.

The website documentation checkout is absent; command documentation stays here.

The comparison file picker adds the current PDF first. For versions it is the old
version; the selected PDF is the new version. Figure analysis renders the first
selected page/rectangle, or the current page when nothing is selected. Reports are
saved under the profile's `ResearchReports`; Save report exports Markdown. Source
links are active in the report window/chat, not portable links for other Markdown apps.

Tests: `tests/ad-hoc-research-assistant.ts` uses an isolated provider fixture and real
PDF extraction, PNG rendering and WebView2. It checks indexed pages, two-document
comparison input, clicked citations, font sizing, lazy originals and Clear cancellation.
Live model reasoning and faithful layout recognition are not established by these tests.
