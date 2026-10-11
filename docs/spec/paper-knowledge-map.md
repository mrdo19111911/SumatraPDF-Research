# Paper Knowledge Map

`CmdPaperMap` opens the current paper's KG beside its PDF. Hover shows surrounding source text. Click navigates and highlights the exact passage. Closing KG restores the PDF area and AUTO content layout.

## Interaction

- One PDF page and KG occupy separate panes with a draggable splitter.
- AUTO measures the PDF pane after layout, including when KG closes.
- Each paper keeps its own KG while its document tab remains open.
- The toolbar KG button toggles the pane; its close button hides it.
- Hover never navigates. Unverified anchors never navigate.
- Layers filter ideas, proof structure or references. Search filters node labels.
- Arrow direction is prerequisite to dependent result. AI relationships remain unverified.

## Sources

Claude receives the indexed text of all physical pages through the existing research assistant. It returns bounded JSON with typed nodes, typed edges and verbatim page anchors. The renderer validates the schema and inserts labels as text.

The source service checks the page, copying permissions, extracted quote and text-search location before returning context or navigating. A matched quote verifies its location, not the mathematical claim or relationship. Coverage is reported by the provider, not certified by the reader.

## Validation

- `tests/ad-hoc-paper-map-model.ts`: malformed schema, missing anchors, invalid relations and duplicate IDs.
- `tests/ad-hoc-paper-map.ts`: mocked provider, source hover, highlighted navigation, invalid source rejection, per-paper state, close/reopen, AUTO and dark mode.
- Existing toolbar and AUTO/chat tests cover the shared frame layout.

## Boundaries

- Requires the configured Claude Code provider for generation.
- Scanned pages need OCR. This version does not certify proof correctness.
- JSON reports are saved under the settings directory's `ResearchReports` folder. Pane state is retained for the current document session.
- Website command documentation cannot be updated here: the adjacent sumatra-website checkout is absent. Add `CmdPaperMap : "Paper Knowledge Map"` to Commands.md and the next version's New commands list when that checkout is available.
