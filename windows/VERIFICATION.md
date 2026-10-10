# Windows 0.3.0 verification

Built and verified locally on Windows x64 with MSVC 2022 and Qt 6.8.3.

- `windows/build-native/native-tests.xml`: 45 tests passed, 0 failed/skipped.
- `windows/test-results/package-native-0.3.0.xml`: 45 tests passed, 0 failed/skipped.
- Final ZIP SHA-256: `5a48d80ee0ab8e06311747c7de6043466efa58908910207ed4d5aeaad887e1c4`.
- Extracted ZIP started and rendered a project using only the Windows/System32 PATH.
- The complete 45-test suite also passed beside the extracted application's DLLs/plugins, with its bundled local model.
- English/Simplified/Traditional Chinese screenshots and 150%/200% DPI captures were inspected. All 11 Camera Raw sections have translated labels and accessible controls.

Tests exercise PSD/PSB raw/RLE/ZIP/prediction, grouped masks, curves, text/vector metadata, sensor-data DNG development, HEIC alpha, SVG, Camera Raw output/preset bounds and apply/cancel/undo, actual canvas liquify/polygon gestures, free distort, group transform/effects, feathered masks, inline text, shortcut persistence, local foreground inference, PSD/RAW UI import and clean/dirty external reload. Earlier project, mask, blend, brush, clipboard, layer, effect and localization regressions are retained.

PSD/PSB/DNG/SVG inputs are synthetic and reproducible. The optional HEIC test used libheif's `with-alpha-512x512.heic` fixture from the pinned source tree; set COMPOSITOR_HEIC_FIXTURE to enable it when rebuilding. This verification does not establish compatibility with every Photoshop document or camera model. macOS tests were not run.

Runtime artifacts and screenshots are in the ignored `windows/test-results/` folder; build log is `.tools/build-delivery.log`. Run `windows/scripts/build.ps1` with PowerShell 7 to repeat the native suite.
