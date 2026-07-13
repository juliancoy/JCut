from pathlib import Path

import cv2
import numpy as np
from PIL import Image


def save_grayscale_png(path: Path, image: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(image, mode="L").save(path)


def save_binary_mask_outputs(
    path: Path,
    mask: np.ndarray,
    union_mask_dir: Path | None = None,
    combined_mask_dir: Path | None = None,
) -> None:
    """Save a prompt mask and optionally a nearest-neighbor OR composite."""
    save_grayscale_png(path, mask)
    if union_mask_dir is None or combined_mask_dir is None:
        return

    existing_path = union_mask_dir / path.name
    combined = mask
    if existing_path.exists():
        with Image.open(existing_path) as existing_image:
            existing = np.asarray(existing_image.convert("L"))
        if existing.shape != mask.shape:
            existing = cv2.resize(
                existing,
                (mask.shape[1], mask.shape[0]),
                interpolation=cv2.INTER_NEAREST,
            )
        combined = np.maximum(mask, existing)
    save_grayscale_png(combined_mask_dir / path.name, combined)
