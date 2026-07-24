#!/usr/bin/env python3
"""Generate deterministic file checksums and a file-level SPDX 2.3 SBOM."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app-dir", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--git-revision", required=True)
    parser.add_argument("--qt-version", required=True)
    args = parser.parse_args()

    app_dir = args.app_dir.resolve()
    metadata_dir = app_dir / "usr/share/doc/jcut"
    metadata_dir.mkdir(parents=True, exist_ok=True)
    generated_names = {"SHA256SUMS", "sbom.spdx.json"}
    files = sorted(
        path
        for path in app_dir.rglob("*")
        if path.is_file() and path.name not in generated_names
    )

    checksums = []
    spdx_files = []
    relationships = []
    for index, path in enumerate(files, start=1):
        relative = path.relative_to(app_dir).as_posix()
        checksum = sha256(path)
        checksums.append(f"{checksum}  {relative}")
        file_id = f"SPDXRef-File-{index}"
        spdx_files.append(
            {
                "SPDXID": file_id,
                "fileName": f"./{relative}",
                "checksums": [{"algorithm": "SHA256", "checksumValue": checksum}],
                "licenseConcluded": "NOASSERTION",
                "copyrightText": "NOASSERTION",
            }
        )
        relationships.append(
            {
                "spdxElementId": "SPDXRef-Package-JCut",
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": file_id,
            }
        )

    (metadata_dir / "SHA256SUMS").write_text(
        "\n".join(checksums) + "\n", encoding="utf-8"
    )
    namespace_seed = hashlib.sha256(
        f"{args.version}:{args.git_revision}:{args.qt_version}".encode()
    ).hexdigest()
    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"JCut-{args.version}",
        "documentNamespace": f"https://jcut.invalid/spdx/{namespace_seed}",
        "creationInfo": {
            "created": "1970-01-01T00:00:00Z",
            "creators": ["Tool: JCut-release-metadata"],
        },
        "packages": [
            {
                "SPDXID": "SPDXRef-Package-JCut",
                "name": "JCut",
                "versionInfo": args.version,
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": True,
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "NOASSERTION",
                "copyrightText": "NOASSERTION",
                "externalRefs": [
                    {
                        "referenceCategory": "OTHER",
                        "referenceType": "git-revision",
                        "referenceLocator": args.git_revision,
                    },
                    {
                        "referenceCategory": "OTHER",
                        "referenceType": "qt-version",
                        "referenceLocator": args.qt_version,
                    },
                ],
            }
        ],
        "files": spdx_files,
        "relationships": relationships,
    }
    (metadata_dir / "sbom.spdx.json").write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
