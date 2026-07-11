#!/usr/bin/env python3
"""Live regression probe: click a contiguous-section row with xdotool."""

import json
import shutil
import subprocess
import sys
import time
import urllib.request
import urllib.error

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:40130"


def get(path):
    try:
        with urllib.request.urlopen(BASE + path, timeout=5) as response:
            return json.load(response)
    except urllib.error.URLError as error:
        raise SystemExit(f"editor control server is unavailable at {BASE}: {error.reason}") from error


def post(path, payload):
    request = urllib.request.Request(
        BASE + path,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.load(response)


def walk(node):
    yield node
    for child in node.get("children", []):
        yield from walk(child)


if not shutil.which("xdotool"):
    raise SystemExit("xdotool is required for this live UI regression probe")
health = get("/health")
window_ids = subprocess.run(
    ["xdotool", "search", "--pid", str(health["pid"]), "--name", "JCut"],
    check=True, capture_output=True, text=True).stdout.split()
if not window_ids:
    raise SystemExit("could not locate the visible JCut window")
window_id = window_ids[0]
subprocess.run(["xdotool", "windowactivate", "--sync", window_id], check=True)
subprocess.run(["xdotool", "windowmove", window_id, "0", "0"], check=True)
time.sleep(0.2)

ui = get("/ui?refresh=1")["window"]


def select_tab(widget, label):
    result = post("/ui", {
        "op": "tab_select", "path": widget["path"],
        "tabLabel": label, "timeoutMs": 10000,
    })
    if not result.get("ok"):
        raise SystemExit(f"failed to select {label} tab: {result}")


outer_tabs = next((n for n in walk(ui)
                   if n.get("role") == "tab_widget" and
                   any(tab.get("label") == "Speakers" for tab in n.get("tabs", []))), None)
if outer_tabs:
    select_tab(outer_tabs, "Speakers")
ui = get("/ui?refresh=1")["window"]
work_tabs = next((n for n in walk(ui) if n.get("id") == "speakers.work_tabs"), None)
if not work_tabs:
    raise SystemExit("speakers.work_tabs is unavailable")
select_tab(work_tabs, "Sections")
ui = get("/ui?refresh=1")["window"]
table = next((n for n in walk(ui) if n.get("id") == "speakers.sections_table"), None)
if not table or not table.get("visible") or int(table.get("rows", 0)) < 1:
    raise SystemExit("speakers.sections_table must be visible and contain a row")

rect = table.get("geometry", {}).get("global")
if not rect:
    raise SystemExit("UI inspection did not return table geometry")
x = int(rect["x"]) + min(160, int(rect["width"]) // 2)
window_rect = ui.get("geometry", {}).get("global", {})
visible_top = max(int(rect["y"]), int(window_rect.get("y", rect["y"])))
visible_bottom = min(int(rect["y"]) + int(rect["height"]),
                     int(window_rect.get("y", 0)) + int(window_rect.get("height", rect["height"])))
if visible_bottom - visible_top < 40:
    raise SystemExit("contiguous-section table has no clickable visible area")
y = min(visible_top + 60, visible_bottom - 20)
baseline = int(get("/health")["current_frame"])
subprocess.run(["xdotool", "mousemove", str(x), str(y), "click", "1"], check=True)

deadline = time.time() + 5
target = None
selected_row = -1
while time.time() < deadline:
    payload = get("/ui?refresh=1")["window"]
    selected = next((n for n in walk(payload) if n.get("id") == "speakers.sections_table"), {})
    selected_row = int(selected.get("currentRow", -1))
    if selected_row >= 0:
        target = int(get("/health")["current_frame"])
        break
    time.sleep(0.1)
if target is None:
    raise SystemExit("xdotool did not select a visible contiguous-section row")
print(json.dumps({"ok": True, "before": baseline, "after": target,
                  "selectedRow": selected_row}))
