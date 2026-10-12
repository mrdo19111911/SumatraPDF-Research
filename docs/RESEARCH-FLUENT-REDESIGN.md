# Research Fluent redesign

Status: native Fluent-style surfaces implemented for Research 4.8.1.

## Native implementation

- Light/dark colors, readable UI type and rounded dialog buttons share the
  existing theme and virtual-control layers.
- Document tabs use rounded surfaces and an accent underline.
- Tools opens a vertical category rail beside a bounded action grid. It keeps
  its circular launcher, drag behavior and zero reserved toolbar space.
- Chat, KG and Related share a right-pane tab bar. Only one pane is active;
  switching preserves conversation data, and closing restores PDF space.
- Chat has a Send button, spaced controls and readable Markdown. KG uses the
  same typography, colors and rounded controls.
- AUTO still uses the final PDF canvas dimensions. Tests check containment of
  every retained content rectangle after panel and window size changes.

The prototype's separate Sources tab and navigation footer are not implemented.
Menus, file pickers and combo boxes retain Windows controls. This is custom
styling on the existing C++ UI, not a new WinUI runtime.

## Scope

Redraw document tabs, reading chrome, floating tools, Chat/KG panels,
references, search, provider setup and dialogs with one Fluent 2 style.
Retain the PDF engine and existing research actions.

## Interaction

- Collapsed tools show one circle. Click opens; hold and drag moves it.
- A drag crossing the Windows drag threshold must not activate a click.
- Blank toolbox space also drags. Capture loss cancels the gesture.
- Expanded tools show one selected category, with icons and readable labels.
- Chat, KG and sources share the right pane; closing restores AUTO layout.
- Selecting a KG source navigates the PDF and highlights its context.
- Switching paper changes the conversation context.
- Errors, progress, cancellation and setup remain visible inside their feature.

## Visual system

- Segoe UI; regular body text, medium headings, no blanket bold type.
- Neutral surfaces, blue active states, consistent light/dark tokens.
- Soft shadows for floating surfaces; simple buttons without offset gray slabs.
- Consistent icon stroke, spacing, corner radii and focus states.
- Interface text size is independent of PDF page zoom.
- Default launcher position stays outside Chat input and navigation controls.

## Architecture

Keep the C++ engine. Apply shared styling through the existing virtual control
and drawing layers before considering another UI runtime. WebView2 remains
available for chat, Markdown and embedded research pages. Replacing the visual
style does not require rewriting the document engines.

Route interactions through UI intents, document/research services and existing
controllers. UI components must not launch CLI processes or perform network
requests directly. Theme, active document, viewport, panel state and launcher
position need one owner each.

## Delivery checks

- Source mapping and command behavior remain unchanged.
- Every visible action works by keyboard and mouse.
- No clipped labels or unreachable controls at supported DPI/window sizes.
- Drag, click tolerance and cancellation have targeted native tests.
- Real inference, login and sharing require separate integration validation.
- New dialog and panel appearances must be verified in the running app.

## Verified, 2026-10-13

Release build: zero warnings/errors. Hidden native tests cover the category
rail, action cards, drag/click/cancellation, scrolling, light/dark rendering,
Chat/KG tab transitions, per-paper conversations and source navigation.

AUTO content containment passes 25 states across top, adaptive and floating
toolbars, including portrait resize, restoration and panel close. Browser
tests cover docking, close, resize and persistent cookies/localStorage.
Send/fallback uses local fake CLIs; no real inference is exercised. Version
metadata, Help/About and installer payload extraction pass. No installation
was performed on the user's machine.

References: [Fluent color](https://fluent2.microsoft.design/color),
[typography](https://fluent2.microsoft.design/typography),
[elevation](https://fluent2.microsoft.design/elevation/).
