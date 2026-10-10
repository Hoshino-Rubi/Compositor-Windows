# Windows 0.3.1 verification

Built and verified on Windows x64 with MSVC 2022 and Qt 6.8.3, October 11, 2026 (Asia/Taipei).
Upstream: Compositor 1.4.9, b4bfdea87f9dc0cbc9eabfa63683eb1b9c2bca60 (76 merged commits).

- `windows/build-native/native-tests.xml`: 54 tests passed, 0 failed/skipped.
- `windows/test-results/native-windows-tests.xml`: 54 tests passed, 0 failed/skipped.
- `windows/test-results/package-native-0.3.1.xml`: 54 tests passed, 0 failed/skipped.
- Final ZIP SHA-256: `3ab7b4827dd784dfac70d085a5bae643bef306ad3f0c9a519c86980f3ab6231b`.
- The extracted ZIP started and rendered a project using only the Windows/System32 PATH.
- All 54 tests passed beside the extracted application's DLLs/plugins, with the bundled local model and embedded Camera Raw tables.
- English, Simplified Chinese and Traditional Chinese widget captures, including 150%/200% DPI, were inspected. New command search and navigator captures were inspected.

New regressions cover upstream Camera Raw reference values and tone ordering, deterministic RAW gradient quantization, exact quarter-turn pixel rendering with masks/selection/guides and undo, translucent inner effects and effect scaling, Scanlines alpha and undo, sized PNG/JPEG/PDF output, tiny lossless WebP alpha, translated command invocation and repeat-filter, physical canvas units and middle-drag zoom/navigation.

Existing PSD/PSB/DNG/HEIC/SVG import, Camera Raw controls, liquify, selections, model inference, rich text, transforms, layers/effects, project round trips, external reload and localization regressions remain covered. The optional HEIC test used the pinned libheif `with-alpha-512x512.heic` fixture; set COMPOSITOR_HEIC_FIXTURE to repeat it. Synthetic fixtures are reproducible. These checks do not establish support for every camera or Photoshop structure.

macOS tests are handled by the GitHub Verify workflow; they were not run locally on Windows. Runtime reports and widget captures are under ignored `windows/test-results/`. Rebuild with PowerShell 7 and `windows/scripts/build.ps1`.
