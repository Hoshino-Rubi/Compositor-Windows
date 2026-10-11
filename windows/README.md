# Compositor for Windows

Native C++20 / Qt 6 Widgets, **0.3.4**. The original Swift / AppKit / Metal project is retained.
Detailed controls and compatibility notes are in [README.zh-CN.md](README.zh-CN.md).

## Run

Download the portable application from [GitHub Releases](https://github.com/Hoshino-Rubi/Compositor-Windows/releases/latest).

Extract `Compositor-0.3.4-win-x64.zip` and run `Compositor.exe`. Keep the DLLs, plugins, translations, models and licenses together.
Windows 10 / 11 x64. No Qt SDK, Python, Node.js or browser runtime is needed. Eight interface languages switch immediately in Preferences: English, Simplified Chinese, Traditional Chinese, Japanese, Korean, German, French and Spanish.

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
CPU rendering, fonts, geometry interpolation and effect edges can differ from Metal; pixel-identical output is not claimed.

Canvas: 16,384px per side, 64MP total, 100MP of source images and masks each, 1,000 layers. Large operations take time and memory.
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
