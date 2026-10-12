# Research workspace

Status: native floating tools implemented; unified panel tabs remain a proposal.

## Layout

- Document tabs above the PDF; tools float over the reading area.
- Group labels inline: File, Read, AI, Sources, Share.
- PDF owns the central reading area.
- One right panel with Chat, KG, Connected Papers and References tabs.
- Panel state belongs to the active document.
- AUTO uses the PDF area after subtracting visible panels.

## Panel behavior

Close, collapse, resize and detach work consistently in every panel.
Collapse retains the session and exposes a restore button. Closing never
deletes the conversation. A separate Clear chat action does that.

A floating dock has a drag handle. Clicking opens its tools; hovering only
shows a tooltip. Dragging near a window edge offers docking. Keyboard users
can select Dock, Float and Restore without dragging.

## Icons

Use one stroke style and size. Chat uses a speech bubble; KG a dependency
graph; Connected Papers a network; references a library with download;
Google Drive a labeled drive action; Zalo the letter Z. Keep labels for
unfamiliar actions and tooltip text for every icon.

## AI states

Show disconnected, preparing, working, cancelled and failed states. If the
CLI is missing, offer discovery, Browse and login instructions. Never leave
an empty panel without an explanation. Authentication must be distinct from
having the Claude desktop application installed.

## Acceptance

- Panels never cover the PDF while docked.
- Tooltip placement accounts for DPI and screen edges.
- Tabs retain independent paper context.
- KG hover previews sources; click navigates and highlights the PDF.
- Closing a panel restores AUTO layout.
- Every long task has progress, cancellation and a useful error.
- Local tests do not send documents or change the user's account.

## Verified changes, 2026-10-12

The native browser now has a window class that supports caption controls,
resize borders and minimize. It can dock on the right, float, collapse and
restore via its toolbar command. Group labels are inline on horizontal
toolbars. Tooltips use DPI-scaled clearance and avoid the cursor at screen
edges.

Tests cover native browser controls and cookie persistence, toolbar layout,
chat context, AUTO with chat, KG navigation with a mock provider and installer
extraction. Real provider login, uploads and messages were not exercised.

Research 4.8.1 adds the shared Chat/KG/Related pane tab bar.

## Research 4.7.6

Tools start collapsed in a draggable overlay. The grip moves the launcher;
Tools expands or collapses it. AI opens icons with full action names. No
toolbar space is reserved. Position is clamped to the PDF area and saved in
DIPs. `ResearchFloatingTools = false` restores the legacy toolbar.

Chat opens through `CmdPaperChat`. Its provider selector offers Auto, Claude
Code, OpenAI Codex, Grok and Antigravity. Auto detects installed CLIs and retries
the current question with another installed provider after a quota or login
error. Manual selection disables automatic switching. Conversations remain
separate per paper and provider. Setup opens provider configuration or guidance.

The installer retains the SumatraPDFResearch identity. Windows metadata and
Help/About identify Research 4.7.6; packaging rejects mismatched versions. The
older installation named SumatraPDF is a separate app and is not removed.

Hidden tests cover dragging, expansion, page-box visibility, the AI menu,
backend fallback with local mocks, About metadata and installer extraction.

## Research 4.7.7

The Tools launcher is a blue 88-DIP circle with a highlight and shadow.
Floating groups remain visible while the toolbox is collapsed. Action cards
show icons and labels, using 14-point type, 28-DIP icons and 56-DIP hit areas.
The toolbox and menus scroll within a bounded viewport. Group menus open
beside the launcher when space permits. Page navigation uses Go to page.

Hidden tests cover every group's menu, circle size, labels, command execution,
dragging, collapse and mouse-wheel scrolling in a short window.

## Research 4.7.8

Collapsed tools show one circle. The circle and empty toolbox space can be
dragged. A short click opens or collapses it; a drag never activates the click.
Lost capture cancels dragging. Category tabs show one group at a time.

The full Fluent shell remains an interactive design; see
[Research Fluent redesign](RESEARCH-FLUENT-REDESIGN.md).

## Research 4.8.1

Fluent-style light/dark surfaces cover document tabs, floating tools, chat,
KG and themed dialog buttons. Tools uses a category rail beside a labeled
action grid. Chat has an explicit Send button. Research pane tabs switch
between Chat, KG and Related without leaving a hidden pane open underneath.

AUTO fits retained PDF content to the actual canvas after panel and window
size changes. Interface typography changes do not change PDF text size.
See the redesign document for remaining differences from the prototype.
