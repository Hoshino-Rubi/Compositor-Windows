# Windows 0.4.0 verification

Built on Windows x64 with MSVC 2022 and Qt 6.8.3 on October 11, 2026 (Asia/Taipei). The merged upstream `main` commit is `3081719d5178b466214860fe5fcf5691394dc558`.

- Native Qt Test suite: 88 functional tests passed, 1 optional HEIC-alpha fixture test skipped, 0 failed (`windows/build-native/native-tests.xml`). The skipped test needs the separate libheif alpha fixture, not an application dependency.
- Extracted portable ZIP: the same 88 tests passed, 1 optional skipped and 0 failed (`windows/test-results/package-native-0.4.0.xml`). For this test only, `compositor_tests.exe`, Qt6Test.dll and qoffscreen.dll were copied into the extraction; they are **not** included in the released ZIP. The packaged executable is byte-identical to the verified Release build.
- Portable application launched using the bundled Windows Qt platform plugin with a minimal PATH and captured its own English sample workspace. The native Windows capture has readable fonts and 24px tool icons (`windows/test-results/capture-0.4.0-gpu-windows.png`, generated locally and not bundled).
- ZIP `Compositor-0.4.0-win-x64.zip`: 26,252,662 bytes; SHA-256 `8cb705a35a790439db290c31afb9e2675d7081dc73b1ece8090b5a68bfe9c123`. The attached `.sha256` matches the archive.
- Eight JSON catalogs contain the same 607 keys, including the new PSD crop conversion report. The original `.comp` format remains v11 for 8-bit and v12 for 16-bit documents.
- Targeted coverage includes per-filter 8/16-bit undo, refined background masks, layer/group clipboard and cross-tab/Alt drag, canvas-only fullscreen, crop-from-selection, memory limits, PSD/PSB outside-canvas cropping for Raw/RLE/ZIP/predictor in 8/16-bit RGB, image recents, and existing editing/import regressions.
- Direct3D 11 hardware tests ran with `COMPOSITOR_REQUIRE_GPU=1`: all 24 blend modes at both 8/16-bit depth, mask/group opacity, and unsupported-document CPU fallback. Bounds are two 8-bit steps or four 16-bit steps versus CPU blending. Hardware is not required to run the application; unavailable devices fall back to CPU.
- New coverage also verifies original-resolution region output with effect/blur halos, coordinate-dependent noise fallback, visible-tile reuse/edit invalidation/cancellation, actual high-zoom Canvas pixels, precise eyedropper sampling, group/mask offsets, single-undo multi-layer moves, and maximized-state restoration. The test process exited normally after graphics-resource cleanup.
- Limitations remain: CPU brushes/filters/transforms/export and live adjustment/effect compositing; full-size temporary composites for noise/grain/motion-blur adjustments; CMYK and 32-bit PSD and floating-point HDR. The overview is capped at 2560px; high-zoom detail uses original-resolution tiles. macOS/Xcode tests were not run in this Windows environment.

---

# Windows 0.3.4 verification

Built and verified on Windows x64 with MSVC 2022 and Qt 6.8.3, October 11, 2026 (Asia/Taipei). The upstream baseline remains Compositor 1.4.9 (`b4bfdea87f9dc0cbc9eabfa63683eb1b9c2bca60`).

- `windows/build-native/native-tests.xml`: 72 functional tests passed, 0 failed/skipped.
- `windows/test-results/package-native-0.3.4-dialog.xml`: 72 functional tests passed with the packaged runtime, 0 failed/skipped.
- ZIP: 26177497 bytes; extracted application: 57832630 bytes. Increase over v0.3.3: 299525 bytes compressed.
- ZIP SHA-256: `17df375da133ca3acfa9ef73712bfb989dad4c49440e55fb65547ccd8a516e03`.
- The extracted executable matches the tested Release build and reports version 0.3.4.
- The portable application opened a native 16-bit PNG in all eight languages with only Windows system directories in PATH. Qt translations for German, Spanish, French, Japanese, Korean and both Chinese variants are present in the package. Standard dialog buttons are tested in the five new languages.
- All 553 catalog entries have identical key sets and preserve placeholders and import extensions. Locale normalization covers regional aliases, traditional/simplified Chinese, system selection and unsupported-language fallback.
- Preference tests preserve document identity, rendered pixels, dirty state and undo while changing language and settings; verify settings across new windows; apply default depth/resolution only to new canvases; and check per-section reset behavior.
- Shortcut tests reject duplicate/reserved bindings, keep unapplied edits while translating, apply and persist valid changes across a language change, and restore defaults. Saved bindings use Qt portable syntax independently of display language.
- Preferences is an owned, window-modal QDialog. Lifecycle tests cover singleton reopening, Close/Escape/window-close behavior, discarding unapplied shortcuts, preserving applied shortcuts, and unchanged workspace geometry and document revision.
- Preferences captures and layout assertions cover all eight languages, all three sections, 820×640 and 680×480 logical window sizes, and additional 1.5x and 2x scale-factor runs. Tests verify window bounds, visible close controls and translated input hints. Workspace and Camera Raw captures cover all eight languages.

Existing regressions cover native 16-bit precision, PSD/PSB/RAW/HEIC import, Camera Raw, rendering, filters, retouching, brush gestures, project round trips, image/project pickers and model inference. The pinned libheif alpha fixture and optional psd-tools 16-bit reference fixtures were enabled, so no fixture tests were skipped.

The application remains native C++20 / Qt 6, with no new runtime dependency. Language and preferences do not change the project format: 8-bit documents write v11, and 16-bit documents write v12. Older Windows/macOS builds reject v12. CMYK and floating-point/32-bit editing remain unsupported. macOS/Xcode tests were not run locally.
