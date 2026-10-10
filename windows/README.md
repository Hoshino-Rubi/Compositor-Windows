# Compositor for Windows

Native C++20 / Qt 6 Widgets, **0.3.0**. The original Swift / AppKit / Metal project is retained.
Detailed controls and compatibility notes are in [README.zh-CN.md](README.zh-CN.md).

## Run

Download the portable application from [GitHub Releases](https://github.com/Hoshino-Rubi/Compositor-Windows/releases/latest).

Extract `Compositor-0.3.0-win-x64.zip` and run `Compositor.exe`. Keep the DLLs, plugins, translations, models and licenses together.
Windows 10 / 11 x64. No Qt SDK, Python, Node.js or browser runtime is needed. English, Simplified Chinese and Traditional Chinese switch immediately.

## Editing and import

The native workspace follows the upstream tabs, tool options, 58px tool rail and layer panel. Icons are 24 logical pixels with 36px hit targets.
Independent tabs, undo, selection, layers/groups, 24 blend modes, brushes, retouching, shapes, rich text, masks, adjustments, effects, guides and full-resolution PNG/JPEG export are available.

Version 0.3 adds:

- PSD/PSB 8-bit RGB layers, raw/RLE/ZIP/prediction, groups, masks, clipping, adjustment layers, editable basic vectors and text style runs.
- LibRaw sensor-data decoding, 16-bit linear development and RAW preview controls; HEIC/HEIF and SVG import.
- Camera Raw Light, Color, Curve, Mixer, Point Color, Grading, Effects, Detail, Optics, Geometry and Calibration, using upstream pixel kernels. Async preview, histogram/vectorscope, clipping, sharpening-mask preview, guides, presets and undo.
- Liquify/smudge, polygonal lasso, feathered selections, ranged HSV, Motion Blur adjustment layers, group effects, group transforms and free distort.
- Local U2Netp subject/object selection and reversible background masks, inline rich text, configurable shortcuts and safe external-project reload.

Use Import or drag a PSD/RAW image onto the canvas. Open a `.comp` by selecting the project directory. R selects Blur; its mode selector offers Smudge and Liquify. Ctrl+T transforms selected layers/groups; the Layer menu offers Free Distort. Shift+F6 feathers a selection. Ctrl+Enter accepts inline text, Escape cancels.

## Compatibility

Reads `.comp` versions 1–11 and writes version 11. Untouched PNG bytes and non-rendering metadata survive round trips. Font/color runs remain editable; text reflows with Windows fonts. Pixel editing rasterizes text/shapes. Selection and history are session-only.

PSD import matches the upstream 8-bit RGB scope. CMYK and 16/32-bit PSD are rejected. Complex Photoshop objects, text warps and effects use raster fallbacks or conversion notices; PSD export is not provided.
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

Dependency revisions and download checksums are pinned in dependencies.lock.json and vcpkg.json. Setup needs network; application use is offline.
The build runs Qt Test, covering import/development, pixel output, Camera Raw, gestures/undo, selection, groups, model inference, document reload, translations and the original editing regressions.
The synthetic fixtures can be reproduced with `python windows/tests/generate_fixtures.py` from the repository root. Python is not an application dependency.

```powershell
./Compositor.exe --sample --language en --capture-size 1180x820 --capture ./sample.png
./Compositor.exe --render-project ./Example.comp --output ./flattened.png
```

Capture renders only the application's own widget. macOS/Xcode tests require macOS and were not run in this Windows build.
Compositor additions retain MIT licensing. Third-party dynamic libraries, model licenses and source locations are included in licenses/. Binaries are unsigned.
