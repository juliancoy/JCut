# Include Audit

Generated from `build/compile_commands.json` plus project-local `#include "..."` parsing.

## Start Here

Open these first:

```bash
less artifacts/include_audit/reports/fan_in_top.md
less artifacts/include_audit/reports/watched_header_reach.md
xdg-open artifacts/include_audit/graphs/top_fan_in_graph.svg
```

## Layout

| Folder | Purpose |
|---|---|
| `reports/` | Human-readable Markdown summaries |
| `graphs/` | Graphviz DOT files and rendered SVG graphs |
| `data/` | Spreadsheet/script friendly raw tables |
| `compiler/` | Compiler-derived `-H` include trees for selected heavy translation units |

## Key Files

- `reports/fan_in_top.md`: ranked headers by how many compile-database translation units can reach them.
- `reports/watched_header_reach.md`: reach report for headers like `background_fill_effect.h`, `render_vulkan_shared.h`, `editor_shared.h`.
- `graphs/top_fan_in_graph.svg`: smaller browsable graph of the top 30 fan-in headers, highlighted in yellow.
- `graphs/include_graph.svg`: full project-local direct include graph. Dense, but complete.
- `data/fan_in_top.tsv`: ranking table for sorting/filtering in a spreadsheet.
- `compiler/*.includes.txt`: compiler `-H` include trees, including Qt/system/generated headers.

## Reading The Audit

Use `reports/fan_in_top.md` to identify architectural pressure points. Headers with high translation-unit fan-in cause broad rebuilds and deserve review for splitting, forward declarations, or moving narrow constants/types into smaller headers.

Use `graphs/top_fan_in_graph.svg` to see which direct includers pull those high-fan-in headers into the project.

Use `compiler/*.includes.txt` when you need the exact compiler include expansion for a specific translation unit.

Regenerate compiler include trees with:

```bash
./scripts/generate_include_trees.py render_vulkan_shared.cpp direct_vulkan_preview_window.cpp
```

Use Clang explicitly if desired:

```bash
./scripts/generate_include_trees.py --compiler clang++ render_vulkan_shared.cpp
```

## Scope

The project-local graph excludes vendored/generated-heavy areas: `build`, `external`, `ffmpeg`, `sam3`, `.git`, and similar directories.
