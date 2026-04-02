#!/usr/bin/env python3
import argparse
import copy
import json
from datetime import datetime
from pathlib import Path
from typing import Any


TIME_EPSILON = 0.05


def infer_paths_from_media(media_path: Path) -> tuple[Path, Path]:
    base = media_path.with_suffix("")
    return base.with_suffix(".json"), base.parent / f"{base.name}_editable.json"


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"{path} is not a JSON object")
    if not isinstance(data.get("segments"), list):
        raise ValueError(f"{path} does not contain a top-level 'segments' array")
    return data


def normalize_text(value: Any) -> str:
    return " ".join(str(value or "").strip().casefold().split())


def approx_equal(a: Any, b: Any) -> bool:
    try:
        return abs(float(a) - float(b)) <= TIME_EPSILON
    except (TypeError, ValueError):
        return False


def words_match(original_word: dict[str, Any], edited_word: dict[str, Any]) -> bool:
    original_text = normalize_text(original_word.get("word"))
    edited_text = normalize_text(edited_word.get("word"))
    if not original_text or not edited_text:
        return False

    if original_text != edited_text:
        return False

    original_start = original_word.get("start")
    original_end = original_word.get("end")
    edited_start = edited_word.get("start")
    edited_end = edited_word.get("end")

    if approx_equal(original_start, edited_start) and approx_equal(original_end, edited_end):
        return True
    if approx_equal(original_start, edited_start):
        return True
    if approx_equal(original_end, edited_end):
        return True
    return True


def merge_segment_words(
    original_words: list[dict[str, Any]],
    edited_words: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], int]:
    merged: list[dict[str, Any]] = []
    restored_count = 0
    original_index = 0
    edited_index = 0

    while original_index < len(original_words) and edited_index < len(edited_words):
        original_word = original_words[original_index]
        edited_word = edited_words[edited_index]

        if words_match(original_word, edited_word):
            merged.append(copy.deepcopy(edited_word))
            original_index += 1
            edited_index += 1
            continue

        lookahead_match = False
        for future_original_index in range(original_index + 1, len(original_words)):
            if words_match(original_words[future_original_index], edited_word):
                lookahead_match = True
                break
            if future_original_index - original_index >= 8:
                break

        if lookahead_match:
            restored_word = copy.deepcopy(original_word)
            restored_word["skipped"] = True
            merged.append(restored_word)
            restored_count += 1
            original_index += 1
            continue

        merged.append(copy.deepcopy(edited_word))
        edited_index += 1

    while original_index < len(original_words):
        restored_word = copy.deepcopy(original_words[original_index])
        restored_word["skipped"] = True
        merged.append(restored_word)
        restored_count += 1
        original_index += 1

    while edited_index < len(edited_words):
        merged.append(copy.deepcopy(edited_words[edited_index]))
        edited_index += 1

    return merged, restored_count


def reconstitute_editable(original_doc: dict[str, Any], edited_doc: dict[str, Any]) -> tuple[dict[str, Any], int]:
    rebuilt_doc = copy.deepcopy(edited_doc)
    original_segments = original_doc.get("segments", [])
    rebuilt_segments = rebuilt_doc.get("segments", [])

    if len(rebuilt_segments) < len(original_segments):
        rebuilt_segments.extend(
            copy.deepcopy(original_segments[len(rebuilt_segments):])
        )

    restored_count = 0
    for segment_index, original_segment in enumerate(original_segments):
        if not isinstance(original_segment, dict):
            continue

        while segment_index >= len(rebuilt_segments):
            rebuilt_segments.append({})

        rebuilt_segment = rebuilt_segments[segment_index]
        if not isinstance(rebuilt_segment, dict):
            rebuilt_segment = {}
            rebuilt_segments[segment_index] = rebuilt_segment

        original_words = original_segment.get("words", [])
        edited_words = rebuilt_segment.get("words", [])
        if not isinstance(original_words, list):
            original_words = []
        if not isinstance(edited_words, list):
            edited_words = []

        merged_words, segment_restored = merge_segment_words(original_words, edited_words)
        rebuilt_segment["words"] = merged_words

        for key, value in original_segment.items():
            if key == "words":
                continue
            rebuilt_segment.setdefault(key, copy.deepcopy(value))

        restored_count += segment_restored

    rebuilt_doc["segments"] = rebuilt_segments
    return rebuilt_doc, restored_count


def write_backup(path: Path) -> Path:
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_path = path.with_name(f"{path.name}.{timestamp}.bak")
    backup_path.write_text(path.read_text(encoding="utf-8"), encoding="utf-8")
    return backup_path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Restore words deleted from an editable transcript by copying them from the original transcript and marking them skipped."
    )
    parser.add_argument("--media", type=Path, help="Media path used to infer <base>.json and <base>_editable.json")
    parser.add_argument("--original", type=Path, help="Original transcript JSON")
    parser.add_argument("--editable", type=Path, help="Editable transcript JSON to rewrite")
    parser.add_argument("--dry-run", action="store_true", help="Report what would change without writing")
    args = parser.parse_args()

    if args.media:
        if args.original or args.editable:
            parser.error("--media cannot be combined with --original/--editable")
        original_path, editable_path = infer_paths_from_media(args.media)
    else:
        if not args.original or not args.editable:
            parser.error("provide either --media or both --original and --editable")
        original_path, editable_path = args.original, args.editable

    original_path = original_path.resolve()
    editable_path = editable_path.resolve()

    if not original_path.exists():
        raise FileNotFoundError(f"Original transcript not found: {original_path}")
    if not editable_path.exists():
        raise FileNotFoundError(f"Editable transcript not found: {editable_path}")

    original_doc = load_json(original_path)
    editable_doc = load_json(editable_path)
    rebuilt_doc, restored_count = reconstitute_editable(original_doc, editable_doc)

    print(f"original: {original_path}")
    print(f"editable: {editable_path}")
    print(f"restored_missing_words: {restored_count}")

    if args.dry_run:
        return 0

    backup_path = write_backup(editable_path)
    with editable_path.open("w", encoding="utf-8") as handle:
        json.dump(rebuilt_doc, handle, indent=4, ensure_ascii=False)
        handle.write("\n")

    print(f"backup: {backup_path}")
    print("status: updated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
