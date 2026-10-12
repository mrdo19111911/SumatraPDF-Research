# Research workspace

Status: interactive proposal; the installed UI has not been replaced.

## Layout

- Document tabs above a compact toolbar.
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

The proposed panel tab bar and draggable floating tool dock remain design
work; they are not included in this bug-fix build.
