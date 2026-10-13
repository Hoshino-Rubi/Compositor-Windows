# Compositor for Windows

Native C++20 / Qt 6 Widgets, **0.4.1a6 (pre-release)**. The original Swift / AppKit / Metal project is retained.
Detailed controls and compatibility notes are in [README.zh-CN.md](README.zh-CN.md).

## Run

Download the alpha from [GitHub Releases](https://github.com/Hoshino-Rubi/Compositor-Windows/releases/tag/windows-v0.4.1a6). The stable [latest release](https://github.com/Hoshino-Rubi/Compositor-Windows/releases/latest) remains 0.4.0.

Extract `Compositor-0.4.1a6-win-x64.zip` and run `Compositor.exe`. Keep the DLLs, plugins, translations, models and licenses together.
Windows 10 / 11 x64. No Qt SDK, Python, Node.js or browser runtime is needed. Eight interface languages switch immediately in Preferences: English, Simplified Chinese, Traditional Chinese, Japanese, Korean, German, French and Spanish.

## Version 0.4.1a6 — GPU liquify/smudge and transfer reuse (pre-release)

- Adds Direct3D 11 pixel processing for larger Liquify/Smudge brush segments in 8/16-bit documents. Dabs within one pointer-move segment remain ordered; only a bounded local region with sampling margins is uploaded, followed by one readback.
- Preserves path selections, feathered selection masks, layer/mask targets and undo/redo. Small operations, unavailable hardware or insufficient resources use the reference CPU implementation. Sequential luminance denoising/error diffusion retain their a5p1 CPU routing.
- Reuses input, packed-output and staging textures across same-size/depth filter, effects and warp requests. New pixels are uploaded and processed each time; stale results are not reused. Transfer resources count toward the cache budget and are released by cache clearing.

**Scope:** this ports the existing Windows Liquify/Smudge pixel algorithm, not macOS's whole-stroke resident textures/accumulated displacement field. Qt display still requires synchronous readback; paint/eraser/clone/healing, perspective transforms, some mask preparation and export remain CPU-backed. Other a5/a5p1 limitations remain; project format, runtime dependencies and the eight interface languages are unchanged. **Pre-release only; stable Latest stays 0.4.0.** See [release notes](releases/0.4.1a6.md) for verification and measurements.

## Version 0.4.1a5p1 — Sequential filter performance fix (pre-release)

- Atkinson and Floyd–Steinberg dithering use the reference CPU implementation instead of serial GPU dispatches. The other eight Dither styles retain their GPU paths.
- Camera Raw requests with active luminance denoising run entirely on the CPU, avoiding serial GPU work and intermediate transfers while preserving reference pixels. Camera Raw without active luminance denoising, disabled Detail, temporary clipping previews and sharpening-mask previews retain the GPU path.
- Routing happens before GPU initialization, upload or cache eviction. Filter/Camera Raw previews and Apply share the policy; 8/16-bit editing, selections and undo/redo are preserved. No new setting, dependency or project-format change.

Other a5 GPU features and alpha limitations remain. **Pre-release only; stable Latest stays 0.4.0.** See [patch release notes](releases/0.4.1a5p1.md) for validation and timings. The sections below describe previous releases.

## Version 0.4.1a5 — GPU filters, effects and Camera Raw (pre-release)

- GPU preview processing for Hue/Saturation (including ranges/colorize), Black & White, Color Balance, Gradient Map, Add Noise, Grain and Motion Blur. All twelve adjustment types can use all 24 blend modes; the previous five are no longer Normal-only.
- GPU layer effects: outside/inside Stroke, Drop/Inner Shadow, Outer/Inner Glow and Color Overlay, with masks, clipping, folder effects and opacity.
- Camera Raw pixel stages run on Direct3D 11: light/color/calibration, curves, mixer/point color, grading, texture/clarity/dehaze/glow/grain/vignette, optics, geometry, denoise, sharpening and clipping/mask overlays. Small lookup-table construction, font atlases and aggregation of GPU statistics remain on CPU.
- GPU Vignette, Bloom / Glow, all ten Dither styles, Tonal Contrast, Lens Correction and Scanlines. Filter-dialog previews, destructive filter application and Camera Raw preview/apply now use the GPU path when resources permit; selection handling and undo remain native.

**Alpha limitations:** this is coverage of the listed processing stages, not pixel-identical output or a guarantee of speed. Qt/GPU resampling and antialiasing differ; isolated inner-stroke corner samples differ by up to about 5% in fixtures. Small arithmetic differences can flip Hard Mix or error-diffusion thresholds. Serial luminance denoising/error diffusion can be slower on GPU. Source/mask preparation, brushes/retouch, UI analysis and export remain CPU-backed. GPU processing uses synchronous readback, a bounded scratch pool and the existing memory budget; requests above 8MP or available resources fall back to CPU. Hard Mix on adjustment layers requires hardware FP64. `COMPOSITOR_DISABLE_GPU=1` selects the reference CPU path. Live-effect export still uses that reference; destructive GPU edits save their actual resulting pixels. Project format and runtime dependencies are unchanged. **Pre-release only**, stable Latest remains **0.4.0**. See [release notes](releases/0.4.1a5.md) for validation and timings.

## Version 0.4.1a4 — GPU tone adjustments (pre-release)

- Accelerate **Normal-blend Invert, Exposure, Levels and Curves adjustment layers** in 8/16-bit previews, including adjustment-dialog previews and stacks with Gaussian Blur, raster layers, masks, clipping and folder opacity.
- Generate a complete per-channel lookup table with the reference CPU adjustment, then apply it to image pixels on Direct3D 11. The table has **256 entries for 8-bit and 65,536 for 16-bit**, not an 8-bit approximation of a high-depth curve. Cached alpha factors preserve the native preview's unpremultiplication behavior.
- Reuse tables when editing source pixels, masks or opacity. Changing adjustment parameters updates the table without reuploading the source image or allocating new textures. Unchanged previews skip compute and readback.
- Tone-only stacks retain dirty-rectangle compositing and readback. Adjustment-setting/order changes recompose the requested viewport; visible Gaussian Blur still requires full requested-region computation with halos. Table allocations count toward the cache budget and unsupported documents/devices use the CPU renderer.

**Scope:** lookup-table generation and source/mask preparation remain on CPU. This accelerates preview image processing, not destructive filter application, brushes, Camera Raw, other adjustments/effects or export. Only Normal-blend adjustments listed above (plus Gaussian Blur from a3) are GPU-backed. GPU readback is synchronous. Table generation has a cold/parameter-change cost, especially for 16-bit curves; no universal speedup is promised. Project format, saved/exported pixels, eight interface languages and runtime dependencies are unchanged. **Pre-release only**; stable Latest remains 0.4.0.

## Version 0.4.1a3 — GPU Gaussian Blur adjustments (pre-release)

- Run **Normal-blend Gaussian Blur adjustment layers** on Direct3D 11 compute in 8/16-bit previews, including multiple blur layers interleaved with raster layers. The six horizontal/vertical integer box passes preserve the CPU approximation's edge clamping and per-pass truncation; a shared-memory prefix scan avoids per-pixel work proportional to blur radius.
- Composite the adjusted result with the layer mask, linked/unlinked mask placement, clipping source, ancestor masks and combined layer/folder opacity. Mask coverage remains CPU-prepared and cached. Non-Normal blur adjustments, other adjustment types, or live layer effects conservatively use the CPU renderer.
- Reuse source/coverage textures while changing blur radius; unchanged frames skip both blur and readback. Two reusable blur scratch textures count toward the existing cache budget. Insufficient hardware/resources or budget fall back to CPU.
- Adjustment-dialog previews now use the accelerated preview renderer, with a stable temporary layer identity across control changes. Cancel leaves document pixels, history and dirty state unchanged. High-zoom region previews retain blur halos to prevent tile seams.

**Scope:** this batch migrates Gaussian Blur **preview computation**, not destructive filter application or export. A changed preview containing a visible blur recomputes the entire requested viewport/region because blur has spatial dependencies; raster-only previews retain a2's dirty rectangles. CPU mask/surface preparation, brush/retouch kernels, other filters/Camera Raw, effects and export remain. Readback is synchronous, not zero-copy. The `.comp` format, eight interface languages and runtime dependencies are unchanged. Published **only as a pre-release**; stable Latest remains 0.4.0.

## Version 0.4.1a2 — Dirty-region compositing and packed readback (pre-release)

- Recompose only the bounding rectangle of changed prepared layer pixels. All visible layers are blended within that rectangle, starting with transparent pixels rather than an old composite. Unchanged areas keep the previous result. Changes to visible layer order, visibility or blend modes conservatively trigger a full viewport recomposite.
- Pack the GPU result directly into native 8-bit BGRA or 16-bit RGBA integer samples before readback. Only the changed rectangle is copied to staging and then into the QImage; the full floating-point CPU conversion loop is removed. Hardware support is checked, with CPU fallback if the required resources cannot be created.
- Mask/clipping edits, linked/unlinked masks, transforms, folder opacity and undo/redo use the actual prepared-surface difference, covering both the old and new position. Metadata or source-image changes with identical prepared pixels avoid dispatch and readback. Previously returned images remain immutable.
- `COMPOSITOR_GPU_FULL_FRAME=1` is a diagnostic control that forces full viewport recomposition/readback while retaining the source cache and packed output. `COMPOSITOR_DISABLE_GPU=1` still selects the reference CPU renderer; the existing cache budget and eviction controls remain.

**Scope:** this accelerates preview compositing and transfer, not brush/filter editing kernels. Changed surfaces and their differences are still prepared on the CPU. Changed previews still synchronize a GPU readback and may copy the full CPU image when an older frame remains in use; distant edits share one bounding rectangle, not a sparse tile list. Live adjustments/effects and export remain CPU-backed. No zero-copy display, project-format change, new runtime dependency or universal speedup claim. This and subsequent batches remain **pre-releases only**; stable Latest stays 0.4.0.

## Version 0.4.1a1 — GPU preview cache (pre-release)

This is the first incremental GPU-pipeline alpha, not a complete GPU editor. Subsequent batches use `0.4.1a2`, `0.4.1a3`, and so on, and are published **only as pre-releases** until explicitly approved for a stable release.

- Reuses Direct3D preview layer-surface textures, ping-pong compositing targets and staging textures across frames instead of allocating them on every render. Unchanged layer surfaces skip CPU preparation and upload; identical composite requests reuse the final image without compute dispatch or GPU readback.
- Pixel edits compare the new CPU-prepared surface against its cached mirror and upload only the changed bounding rectangle with `UpdateSubresource`, including native 16-bit samples. Blend-mode-only changes reuse existing source textures.
- Cache identities include document, viewport origin, canvas/target dimensions and depth. Pixel/mask edits, linked/unlinked masks, clipping sources, folder opacity, visibility, transforms, order, undo/redo and depth changes invalidate the affected entries.
- Least-recently-used eviction limits estimated retained GPU and CPU cache resources to at most 512 MiB, further reduced by the Direct3D adapter's reported memory budget/availability. Requests that do not fit use the reference CPU renderer. Set `COMPOSITOR_GPU_CACHE_MB` (1–512) to lower this cap; `COMPOSITOR_DISABLE_GPU=1` releases the preview cache and uses CPU previews.

**Still CPU-backed:** brush/eraser/clone/healing kernels, transforms, masks, filters/Camera Raw, effects and export. Changed surfaces are still prepared and compared on the CPU; changed composites still require a complete viewport compute pass and readback for Qt Widgets. This alpha does not introduce zero-copy display or promise faster results on every GPU. Project format and export precision are unchanged. Keep backups when testing pre-releases.

## Version 0.4.0

Merged the upstream `main` branch through `3081719d5178b466214860fe5fcf5691394dc558` (October 11, 2026). The Qt port now adds:

- Separate Vignette, Bloom / Glow, Dither (including halftone and text styles), Tonal Contrast and Lens Correction filters, with previews, editable controls and undo. Both 8- and 16-bit documents are supported; dithering intentionally reduces RGB color levels.
- Basic and Advanced Remove Background preview. Advanced mode offers edge refinement, matte contrast and edge shift; both modes commit a reversible layer mask rather than deleting image pixels.
- Copy/cut/paste entire selected layers and folders when no pixel selection is active, including masks, live metadata, group hierarchy and 16-bit precision. Alt-drag duplicates a layer; drag to another document tab or layer panel copies it. A pixel selection retains pixel clipboard behavior.
- Snap To Guides, Grid, Layers and Canvas Bounds controls for moving and cropping; crop can start from a selection, snap during drag, or expand symmetrically with Alt. Movable/lockable guides, pixel grid at high zoom and togglable transform controls. Line shape tool, aligned clone/sample-all-layers controls, three healing modes and separate Blur radius.
- Canvas-only full screen (`F`, `Escape` to leave) and Open Recent for projects and images.
- Memory-scaled document limits: up to 30,000px per side, 200MP per canvas, 400MP of image pixels and 400MP of masks, and 10,000 layers on sufficiently equipped machines. Lower-memory systems retain conservative floors (64MP canvas, 100MP each for image and mask); CPU editing of very large canvases can be slow or run out of memory.
- Oversized layered PSD/PSB imports crop pixels outside the canvas when that lets the document fit the memory budget; a conversion report identifies affected layers. Raw, RLE, ZIP and ZIP-prediction 8/16-bit layers are covered.
- Original-resolution 512px preview tiles at 100% physical-pixel zoom and above, with bounded visible-tile caching, pan/zoom updates, edit invalidation and exact-pixel eyedropper sampling. The overview/navigator remains at most 2560px per side.
- Direct3D 11 hardware compute preview compositing for 8/16-bit raster layers and all 24 blend modes, including masks and folder opacity. Unsupported documents/devices automatically use the reference CPU renderer; document pixels and exports are unchanged.
- Group duplication offsets descendants and independent masks together; multi-layer reordering is one undo step. Leaving canvas-only fullscreen restores the previous maximized state.
- All new UI labels are available in the eight bundled languages.

**Known limitations:** GPU acceleration covers preview blend compositing, not brushes, filters, transforms, export or the full Metal pipeline. Documents with live adjustments/effects use CPU preview compositing. Noise, grain and motion-blur adjustment layers require a full-size temporary composite for accurate tile coordinates; other region renders use effect/blur halos. Those cases can remain slow or memory-intensive. CMYK/32-bit PSD and floating-point HDR remain unsupported. This release does not claim full performance or pixel-for-pixel feature parity with macOS.

## Version 0.3.4

- A **Preferences** window, available from Edit → Preferences (`Ctrl+,`), with General, Canvas and Keyboard shortcuts settings.
- **General:** interface language or Follow system, remembered window size/position, and automatic reload of externally changed projects. Only clean documents reload; unsaved edits remain protected.
- **Canvas:** default 8/16-bit depth and 72/300 DPI for new canvases, rulers, grid, guides, snapping and grid spacing. Display settings apply immediately and persist across launches; document defaults do not convert existing images.
- **Keyboard shortcuts:** edit and Apply in Preferences, with duplicate/reserved-key checks and per-section defaults. Escape, Delete and Ctrl+Y remain fixed shortcuts. Choose Apply to save shortcut changes; closing Preferences discards unapplied shortcuts.
- Added complete Japanese, Korean, German, French and Spanish catalogs, bringing the total to eight languages. All 553 strings, placeholders and import extensions are checked. Regional locales resolve to the corresponding language; unsupported system languages fall back to English. Qt dialog translations are bundled too.
- Updated stale import, RAW and project-format messages to match the existing 16-bit support. The `.comp` format and editing precision are unchanged.

## Version 0.3.3

- Native **16-bit/channel RGB editing**, alongside the existing 8-bit mode. Choose the depth when creating a canvas or use Image → 8/16 Bits/Channel. Importing a 16-bit image promotes the document; reducing to 8-bit requires confirmation and can be undone.
- 16-bit PNG/TIFF and layered RGB PSD/PSB import, including raw, PackBits RLE, ZIP and ZIP prediction, `Lr16` records and 16-bit masks. Embedded RGB PSD profiles are converted to the sRGB working space without reducing depth. RAW development now retains 16-bit pixels.
- High-depth compositing and blend modes, adjustments, Camera Raw, layer effects, brushes/eraser/clone/healing, gradients, text/shapes, liquify, transforms, selections/masks and undo. Tests exercise values between the 256 steps of an 8-bit image.
- 16-bit PNG export and project save/reopen. JPEG and single-page raster PDF remain 8-bit outputs, stated in the export dialog.
- **Project compatibility:** 8-bit documents still save as `.comp` v11. 16-bit documents use v12 with `bitDepth: 16`; older Windows builds and the original macOS application reject v12. Convert a copy to 8-bit for compatibility.
- English, Simplified Chinese and Traditional Chinese depth controls and conversion messages.

## Version 0.3.2

- Open Image (`Ctrl+O`) now selects supported image files, including PNG, JPEG, WebP, TIFF, PSD/PSB and RAW, and opens them in a new document tab.
- Open Project (`Ctrl+Shift+O`) still selects a `.comp` directory. Both actions are available on the welcome page.
- Import Images (`Ctrl+I`) and canvas drops keep adding image layers to the current document. Opening preserves unsaved edits in other tabs; canceling the picker/RAW development or failing to decode leaves the current document intact.
- Updated English, Simplified Chinese and Traditional Chinese labels and documentation.

## Version 0.3.1

Merged all 76 upstream commits through Compositor 1.4.9 (`b4bfdea87f9dc0cbc9eabfa63683eb1b9c2bca60`, October 10, 2026).
The native Windows implementation now includes:

- Updated Camera Raw measured tables for Light/Color, Mixer, Calibration, parametric curves and Color Grading; upstream noise reduction, optics and detail fixes. Pixel work uses Windows parallel scheduling.
- RAW 16-to-8-bit dithered quantization and corrected inner effects on translucent pixels.
- Search Commands (`Ctrl+F`), including tools and modes; Last Filter (`Ctrl+Alt+F`); fullscreen (`F`, Escape to leave).
- Scanlines with spacing, thickness, dots, wobble, displacement, smoothing, threshold, color split, density, contrast, black level, colors and glow.
- Lossless 90° canvas placement rotation, including masks, guides and selection; image-size changes scale layer effects.
- Export As with size, aspect ratio and transparency preview; PNG/JPEG and one-page raster PDF at document DPI.
- New Canvas in pixels/inches/cm/mm at 72 or 300 DPI, transparent/white/black backgrounds; navigator at 300% zoom and above; Ctrl+middle-drag zoom; reveal selected layers.
- Tiny lossless WebP import compatibility with Qt 6.8.

Controls introduced in 0.3.1 were translated in English, Simplified Chinese and Traditional Chinese. The upstream Swift source is merged intact; Windows-specific behavior still follows the compatibility limits below. macOS update UI and platform-specific rendering are not Windows implementations.

## Editing and import

The native workspace follows the upstream tabs, tool options, 58px tool rail and layer panel. Icons are 24 logical pixels with 36px hit targets.
Independent tabs, undo, selection, layers/groups, 24 blend modes, brushes, retouching, shapes, rich text, masks, adjustments, effects, guides and sized PNG/JPEG/PDF export are available.

Version 0.3 adds:

- PSD/PSB 8-bit RGB layers, raw/RLE/ZIP/prediction, groups, masks, clipping, adjustment layers, editable basic vectors and text style runs.
- LibRaw sensor-data decoding, 16-bit linear development and RAW preview controls; HEIC/HEIF and SVG import.
- Camera Raw Light, Color, Curve, Mixer, Point Color, Grading, Effects, Detail, Optics, Geometry and Calibration, using upstream pixel kernels. Async preview, histogram/vectorscope, clipping, sharpening-mask preview, guides, presets and undo.
- Liquify/smudge, polygonal lasso, feathered selections, ranged HSV, Motion Blur adjustment layers, group effects, group transforms and free distort.
- Local U2Netp subject/object selection and reversible background masks, inline rich text, configurable shortcuts and safe external-project reload.

Use Open Image (`Ctrl+O`) for a new image document, Open Project (`Ctrl+Shift+O`) for a `.comp` directory, or Import (`Ctrl+I`)/canvas drops to add image layers to the current document. PSD/PSB always open as layered documents. R selects Blur; its mode selector offers Smudge and Liquify. Ctrl+T transforms selected layers/groups; the Layer menu offers Free Distort. Shift+F6 feathers a selection. Ctrl+Enter accepts inline text, Escape cancels.

## Compatibility

Reads `.comp` versions 1–12. Writes version 11 for 8-bit documents and version 12 for 16-bit documents. Untouched PNG bytes and non-rendering metadata survive round trips. Font/color runs remain editable; text reflows with Windows fonts. Pixel editing rasterizes text/shapes. Selection and history are session-only.

PSD/PSB import supports 8-bit and 16-bit RGB, extending the upstream 8-bit scope. CMYK and 32-bit PSD remain unsupported. Editing uses integer sRGB, not a floating-point/HDR pipeline. Complex Photoshop objects, text warps and effects use raster fallbacks or conversion notices; PSD export is not provided.
Camera Raw matches this application's controls, not Adobe's proprietary plugin or lens database. Windows object selection uses local U2Netp foreground inference and connected components, which can differ from macOS Vision.
CPU/Direct3D preview rendering, fonts, geometry interpolation and effect edges can differ from Metal; pixel-identical output is not claimed. GPU previews can differ from integer CPU output by small rounding amounts; saved/exported pixels keep the reference CPU path. High-zoom tiles display source-resolution pixels; the overview is limited to 2560px per side. PSD layers cropped during import lose their off-canvas pixels. Set `COMPOSITOR_DISABLE_GPU=1` before launch to force CPU previews.

Canvas: up to 30,000px per side, 200MP total, 400MP each for source images and masks, and 10,000 layers; actual pixel budgets scale down with physical RAM. Large operations take time and memory.
External changes reload clean tabs; dirty tabs retain their edits. Staged saves keep backups and check disk digests; directory replacement is not power-loss atomic.

## Build

Visual Studio 2022 C++ / Windows SDK, CMake 3.24+, PowerShell 7 and Qt 6.8.3 MSVC 2022 x64 (Image Formats and SVG).

```powershell
pwsh -File ./windows/scripts/prepare-dependencies.ps1
pwsh -File ./windows/scripts/build.ps1 -QtDir C:/Qt/6.8.3/msvc2022_64
pwsh -File ./windows/scripts/package.ps1 -QtDir C:/Qt/6.8.3/msvc2022_64 -SkipBuild
```

The upstream pixel sources are translated at CMake configure time from Apple blocks to synchronous MSVC parallel loops without editing their math. If upstream changes `CameraRawTables.bin`, run `python windows/scripts/repack-camera-raw.py`; CMake rejects a stale packed resource. The generated Qt resource preserves the exact upstream table bytes.

The 16-bit implementations share the original algorithms through checked-in C++ specializations. Normal builds do not require Python. After changing the corresponding source algorithms, regenerate them with `python windows/scripts/generate-wide-pixels.py` and `python windows/scripts/generate-wide-processing.py`; CMake checks their source hashes and rejects stale output. These scripts are development tools and are not part of the portable application.

Dependency revisions and download checksums are pinned in dependencies.lock.json and vcpkg.json. Setup needs network; application use is offline.
The build runs Qt Test, covering native 16-bit precision, import/development, pixel output, Camera Raw, gestures/undo, selection, groups, model inference, document reload, translations and the original editing regressions.
The synthetic fixtures can be reproduced with `python windows/tests/generate_fixtures.py` from the repository root. Python is not an application dependency.

```powershell
./Compositor.exe --sample --language en --capture-size 1180x820 --capture ./sample.png
./Compositor.exe --render-project ./Example.comp --output ./flattened.png
```

Capture renders only the application's own widget. macOS/Xcode tests require macOS and were not run in this Windows build.
Compositor additions retain MIT licensing. Third-party dynamic libraries, model licenses and source locations are included in licenses/. Binaries are unsigned.
