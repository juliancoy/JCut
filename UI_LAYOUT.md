# UI Layout (KISS)

This file defines non-negotiable layout rules to keep the editor usable and predictable.

## Principles

1. No hidden columns by resize:
   - Users must not be able to drag-resize the center pane until side panes are effectively covered.
2. Stable 3-column structure:
   - Explorer (left), Editor (center), Inspector (right) are always visible.
3. Bounded local resizing:
   - Sub-layout controls (like the Inspector tab rail) may resize only within safe limits.
4. Prefer hard constraints over reactive fixes:
   - Use minimum widths and non-collapsible splitters instead of trying to recover after bad states.

## Implemented Constraints

### Main horizontal splitter (`editor_setup.cpp`)

- `setChildrenCollapsible(false)`
- `setCollapsible(0, false)`
- `setCollapsible(1, false)`
- `setCollapsible(2, false)`
- Minimum widths:
  - Explorer: `220`
  - Editor center pane: `520`
  - Inspector: `240`

These prevent dragging the center pane to overlap/hide side panels.

### Inspector tab rail (`inspector_pane.cpp`)

- Tab rail width is clamped to `120..260` in:
  - `tabSizeHint()`
  - drag-resize path (`mouseMoveEvent`)

This prevents the tab rail from growing so wide it crowds the inspector content.

## Verification Checklist

1. Drag left and right splitter handles aggressively:
   - Side panes remain visible.
   - Center pane never consumes entire window.
2. Drag Inspector tab rail resize edge:
   - Width cannot exceed `260`.
   - Inspector content remains visible.
3. Restart editor:
   - Layout behavior remains consistent.

## Scope

- These constraints are intentionally simple and global.
- If future UX requires more flexibility, update this file and keep limits explicit.
