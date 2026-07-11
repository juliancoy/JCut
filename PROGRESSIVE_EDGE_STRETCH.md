# Final-Composite Progressive Edge Stretch

## Objective

Apply progressive edge stretch as an optional final Vulkan post-process. The
effect must use the completed composite—including media, masks, grading,
titles, captions, and overlays—not an individual clip texture.

For every output row and column, the effect finds the outermost visible
composite pixels in screen space and extends those pixels toward the
corresponding output edge. Preview zoom must not affect sampling.

## Required behavior

- Render all scene content before detecting stretch boundaries.
- Detect boundaries from final-composite alpha.
- Use output/canvas coordinates, never preview-widget coordinates.
- Preserve the original composite exactly wherever its alpha is nonzero.
- Begin the generated fill at the first visible edge pixel without a seam.
- Respect edge-pixel count, curve power, opacity, brightness, and saturation.
- Produce equivalent preview and export output.
- Avoid CPU readback and per-pixel CPU processing.
- Keep all work on the existing Vulkan device and frame command buffer.

## Architecture

Extend the existing master Vulkan render paths with an optional final stage:

```text
layers + masks + titles + overlays
              |
              v
transparent intermediate composite
              |
              v
row/column alpha-boundary reduction
              |
              v
fullscreen progressive-stretch pass
              |
              v
presentation / encoder conversion / readback
```

This is not a separate renderer, executable, queue, or Vulkan device. It uses
separate Vulkan pipeline objects owned by the existing renderer resources.

## Phase 1: Define the shared contract

Add a shared settings structure containing:

- enabled/effect mode
- output size
- alpha visibility threshold
- edge-pixel count
- progressive power
- fill opacity
- brightness and saturation

Define a shared recording contract used by direct preview and offscreen export.
The contract accepts the completed composite image/view, destination image or
framebuffer, frame slot, command buffer, and canvas rectangle.

Do not expose preview zoom through this contract.

## Phase 2: Intermediate composite target

### Offscreen/export

- Add an RGBA composite image per in-flight frame slot.
- Match export output dimensions and color format.
- Enable color-attachment, sampled, transfer, and storage usage as needed.
- Render every existing layer and every text/overlay pass into this image.
- Clear to transparent black (`0, 0, 0, 0`).
- Run the final pass into the existing export color image.
- Preserve existing NV12/YUV conversion and readback paths after the final pass.

### Direct preview

- Add a composite image per swapchain/frame slot rather than sharing an image
  still in use by another frame.
- Size it to the output canvas in device pixels, not the full preview window.
- Render all canvas content into it before presentation.
- Run the final pass while drawing the canvas to the swapchain.
- Keep preview chrome and areas outside the canvas out of boundary detection.

### Lifetime and synchronization

- Allocate size-dependent resources during renderer initialization/recreation.
- Destroy them only after the owning device/frame work is idle.
- Reuse resources only after the corresponding frame fence signals.
- Track every image layout explicitly.

## Phase 3: Boundary reduction

Create a compute shader that reads final-composite alpha and produces:

```cpp
struct RowEdges {
    uint left;
    uint right;
};

struct ColumnEdges {
    uint top;
    uint bottom;
};
```

Use sentinel values for rows or columns containing no visible pixels.

### Reduction strategy

- Treat alpha above the configured threshold as visible.
- Use workgroup-local min/max reduction.
- Process dimensions in tiles when they exceed the workgroup width.
- Merge tile results in a second small dispatch when required.
- Avoid a global atomic operation for every source pixel.
- Add deterministic tie behavior.

The boundary buffers are frame-slot resources. At 4K they remain small.

### Barriers

Record explicit dependencies for:

1. Composite color writes to compute shader reads.
2. Boundary initialization to reduction writes.
3. Tile reduction writes to merge reads, if a merge pass is used.
4. Final boundary writes to fullscreen shader reads.

Use sync2 APIs where already supported by the renderer; otherwise follow its
existing barrier conventions consistently.

## Phase 4: Final stretch shader

Add a fullscreen shader that reads:

- completed composite texture
- row-edge buffer
- column-edge buffer
- final-effect settings

For an opaque/visible composite pixel, output the original composite.

For a transparent pixel:

1. Load the row's left/right edges and column's top/bottom edges.
2. Determine which screen edge region contains the pixel.
3. Calculate normalized distance from the detected composite boundary to the
   corresponding canvas edge.
4. Apply `pow(distance, power)`.
5. Move inward from the exact boundary pixel by up to the configured edge-pixel
   count.
6. Sample the composite at that screen-space coordinate.
7. Apply fill brightness, saturation, and opacity.
8. Composite the untouched original over the generated fill using consistent
   premultiplied-alpha rules.

At corners where horizontal and vertical fills overlap, compare normalized
boundary distances and select deterministically. If a hard directional switch
is visible, blend across a small, resolution-independent transition band.

Rows or columns without visible pixels must fall back to the valid direction.
If neither direction is valid, output the configured canvas background.

## Phase 5: Master-pipeline integration

Remove creation of edge-stretch background layers from individual clip loops.
The clip render order and grading behavior otherwise remain unchanged.

Add a final-pass decision after all ordinary graphics and text passes:

```cpp
if (finalCompositeProgressiveStretchEnabled(settings)) {
    recordBoundaryReduction(...);
    recordProgressiveStretchFinalPass(...);
} else {
    recordOrdinaryCompositeCopyOrPresentation(...);
}
```

Ensure non-progressive background modes retain their documented behavior unless
they are deliberately migrated in a separate change.

Use identical settings construction for preview and export. Differences should
be limited to the final destination and canvas-to-surface transform.

## Phase 6: Resource and pipeline setup

Extend the existing Vulkan resource owners with:

- composite images, views, and memory per frame slot
- row/column edge buffers per frame slot
- optional tile-result buffers
- descriptor-set layouts and descriptor sets
- boundary reduction shader module and compute pipeline
- optional merge shader module and compute pipeline
- fullscreen vertex/fragment modules and graphics pipeline, or a final compute
  pipeline if destination storage-image support is preferable
- pipeline layouts and push-constant definitions

Keep push constants within device limits. Put larger or less frequently changed
settings in the existing frame-uniform mechanism or a dedicated small uniform
buffer.

Use debug names when Vulkan debug utilities are available.

## Phase 7: Tests

### Pure/contract tests

- Progressive stretch is scheduled only after all media and overlay passes.
- Individual clip loops no longer insert progressive background layers.
- Preview zoom is absent from final-effect sampling inputs.
- Push-constant and buffer layouts match GLSL alignment.
- Empty edge sentinels and corner-direction selection are deterministic.

### GPU/render tests

Render controlled alpha composites and compare output pixels for:

- centered portrait media in landscape output
- translated clip
- rotated clip
- scaled clip
- multiple separated clips
- irregular alpha mask
- soft/feathered alpha edge
- title and caption extending beyond media
- overlay-only frame
- empty composite
- negative/reflected transform
- odd output dimensions
- 1x and 2x preview zoom producing identical canvas pixels
- preview/export parity

Verify the first generated pixel adjacent to the composite samples the actual
outermost visible pixel and that the configured progressive curve reaches the
expected inward sample at the screen edge.

### Vulkan validation

Run relevant tests with validation enabled and require no:

- image-layout errors
- missing memory dependencies
- descriptor lifetime errors
- simultaneous sampled/attachment use of one image
- frame-slot reuse hazards

## Phase 8: Performance validation

Measure GPU time separately for:

- composite pass
- boundary reduction
- optional merge
- final stretch

Test at 1080p and 4K with representative multi-layer projects. Confirm that
resources are not reallocated per frame and that the final shader performs a
bounded number of buffer reads and texture samples per pixel.

If reduction becomes material at 4K, optimize workgroup/tile sizes based on
timestamp results rather than introducing unmeasured complexity.

## Rollout order

1. Add shared contracts and tests.
2. Add shaders and resource creation behind a disabled internal flag.
3. Integrate offscreen rendering and validate deterministic image output.
4. Integrate direct preview using the same recording logic.
5. Remove the old per-clip progressive path.
6. Enable the final pass and run parity, validation, and performance tests.
7. Remove the temporary internal flag after both renderers are verified.

## Acceptance criteria

- Progressive stretch samples only the completed final composite.
- Every fill begins at the final composite's outermost alpha-visible pixel for
  its row or column.
- Arbitrarily transformed and masked content transitions without a seam.
- Titles, captions, and overlays participate in boundary detection.
- Preview zoom does not change output-canvas sampling.
- Preview and export results match within established color tolerances.
- No CPU readback is used by the effect.
- No Vulkan validation errors are introduced.
- `./build.sh` completes successfully.
- All targeted and existing Vulkan parity tests pass.

## Non-goals

- Filling internal transparent holes.
- Computing an arbitrary Euclidean nearest-edge distance field.
- Running the effect on preview UI outside the output canvas.
- Moving the effect to a separate Vulkan device, queue, process, or renderer.
- Changing unrelated background-fill modes as part of this implementation.
