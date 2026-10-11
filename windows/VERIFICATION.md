# Windows 0.4.1a3 pre-release verification

Verified on Windows x64 / MSVC 2022 / Qt 6.8.3, October 11, 2026 (Asia/Taipei). Third GPU-pipeline batch; **pre-release only**, stable Latest remains `windows-v0.4.0`. Upstream baseline remains `3081719d5178b466214860fe5fcf5691394dc558`.

- Native and extracted portable suites: **104 passed, 1 optional HEIC-alpha fixture skipped, 0 failed**; processes exited normally. `COMPOSITOR_REQUIRE_GPU=1` required actual hardware tests; `COMPOSITOR_GPU_BENCHMARK=1` enabled measurements. Reports: `windows/test-results/native-0.4.1a3.xml`, `windows/test-results/package-native-0.4.1a3.xml`; isolated new tests: `windows/test-results/gpu-blur-0.4.1a3.xml` (local generated artifacts).
- New coverage: 8/16-bit integer Gaussian approximation with exact basic-kernel CPU equality across transparency, fine samples, 1px/narrow/odd/cross-block images and radii 0.1–250. Includes linked/unlinked masks, clipping, ancestor opacity/masks, multiple/interleaved blur layers and first-layer transparent input. Complex masked stacks allow two 8-bit steps or four 16-bit steps versus CPU blending. High-zoom regions with accumulated halos and scaled previews match full reference output in fixtures.
- Cache/integration checks: unchanged request bypass; radius-only change without source preparation/upload/allocation; pixel edit partial source upload but full blurred-viewport readback; undo/redo; scratch memory released after removing blur; 5 MiB budget fallback; non-Normal and unsupported adjustment CPU fallback. Actual adjustment dialog creates a GPU preview and cancels without modifying document/history/dirty state. Existing 24-blend, cache, import, editing, export and language regressions passed.
- Portable benchmark, 1024×1024, one raster + one blur adjustment, six radius changes (5, 9, 13, 17, 21, 25), warm GPU shaders/source cache: 8-bit CPU **543.659 ms**, GPU **13.482 ms**; 16-bit CPU **777.579 ms**, GPU **68.938 ms**. All frames exactly match CPU. Each GPU run: 36 blur passes, zero source uploads/new texture allocations, six readbacks; 25,165,824 / 50,331,648 logical readback bytes. This is one local workload, not a universal guarantee or direct a2 comparison; initialization excluded and timings vary. Native full-suite figures were 543.759 / 12.164 ms (8-bit CPU/GPU) and 747.679 / 15.906 ms (16-bit CPU/GPU).
- ZIP `Compositor-0.4.1a3-win-x64.zip`: **26,268,034 bytes**, SHA-256 `1c562bd08e332e4416ec3860d4b47dae4ca1b31674fa42194bae1678a8770c3d`. Packaged executable SHA-256 `3d73d7c4a26991b643cdfa2f41977dc2246d4c78139a3cfab9afef2027db1c93` matches the tested Release executable. Test executable/Qt6Test/offscreen plugin exist only in the verification extraction, not the ZIP.
- Minimal-PATH portable launch with the bundled Windows Qt platform succeeded. Application-owned `windows/test-results/capture-0.4.1a3-windows.png` was visually checked for normal workspace rendering, readable menus/icons. Eight catalogs retain matching 607-key sets and version 0.4.1a3.
- Scope: GPU Gaussian Blur preview only, Normal blend. Other adjustment types/non-Normal blur/effects fall back to CPU. Visible blur causes full requested-viewport recomposition rather than dirty rectangles. CPU source/mask preparation, destructive filters, brushes, other kernels and export remain; no zero-copy display, new runtime dependency or project-format change. Existing format/HDR limits remain. No macOS/Xcode tests were run.

---

# Windows 0.4.1a2 pre-release verification

Verified on Windows x64 / MSVC 2022 / Qt 6.8.3, October 11, 2026 (Asia/Taipei). Second GPU-pipeline batch; **pre-release only**, stable Latest remains `windows-v0.4.0`. The upstream baseline remains `3081719d5178b466214860fe5fcf5691394dc558`.

- Native and extracted portable suites: **98 passed, 1 optional HEIC-alpha fixture skipped, 0 failed**. Hardware required with `COMPOSITOR_REQUIRE_GPU=1`; both benchmark cases enabled with `COMPOSITOR_GPU_BENCHMARK=1`. Processes exited normally. Reports: `windows/test-results/native-0.4.1a2.xml` and `windows/test-results/package-native-0.4.1a2.xml` (local generated artifacts).
- New coverage: packed native BGRA8/RGBA16 samples including transparency and sub-8-bit values, odd row pitches, edge rectangles; partial/full recomposite equality for all 24 blends at both depths; zero new allocations on warm edits; immutable old frames; changed visible order/blend/visibility, empty stacks, identical prepared samples and DPI-only updates; masks/clipping, fractional movement/rotation, independent masks, folder opacity, undo/redo. Existing CPU output, cache budget, document/viewport separation, GPU-disable fallback and application regressions also passed.
- Portable benchmark: 1024×1024, 8-bit, 6 layers, 12 changes within 32×32 pixels. Both runs use warm caches and packed output. Full (`COMPOSITOR_GPU_FULL_FRAME=1`): 54.323 ms, 50,331,648 readback bytes, 75,497,472 layer-pixels. Partial: 37.430 ms, 49,152 readback bytes, 73,728 layer-pixels. Both: 49,152 upload bytes, zero allocations, 12 prepared surfaces and 12 readbacks. Every frame compared equal. These counters exclude driver pitch/transfer overhead and dispatch padding; no universal or direct old-version speedup claim.
- ZIP `Compositor-0.4.1a2-win-x64.zip`: **26,263,883 bytes**, SHA-256 `eb8b3df69e53d05cc3611176878e62015b68a77d01f79163355ba8f3368cea48`. Packaged executable SHA-256 `6cc3b0a02b5a819c20e8d69115a84a5638b69cf46ec39737bb109d1468b77b93` matches the tested Release build. Test executable, Qt6Test and offscreen plugin were added only to the verification extraction, not the ZIP.
- Minimal-PATH portable launch with bundled Windows platform plugin succeeded. Application-owned `windows/test-results/capture-0.4.1a2-windows.png` was visually checked: normal canvas colors/alpha, readable menus and tool icons. Eight catalogs retain matching 607-key sets and version 0.4.1a2.
- Scope limits: CPU surface preparation/diffing and editing/filter/export kernels remain. Changed previews still synchronize readback; shared old QImages can require a full CPU image copy. Distant changes use one dirty bounding rectangle. Live adjustments/effects use CPU previews. No zero-copy display or format change; existing CMYK/32-bit/HDR limits remain. Cache budget estimates retained resources rather than peak working-set or exact driver allocation. macOS/Xcode tests were not run.

---

# Windows 0.4.1a1 pre-release verification

Verified on Windows x64 / MSVC 2022 / Qt 6.8.3, October 11, 2026 (Asia/Taipei). First GPU-pipeline batch; **pre-release only**, with stable Latest remaining `windows-v0.4.0`. Upstream remains `3081719d5178b466214860fe5fcf5691394dc558`.

- Native and extracted portable suites: **93 passed, 1 optional HEIC-alpha fixture skipped, 0 failed**. GPU tests required hardware via `COMPOSITOR_REQUIRE_GPU=1`; the benchmark also ran with `COMPOSITOR_GPU_BENCHMARK=1`. Both processes exited normally.
- Reports: `windows/test-results/native-0.4.1a1.xml`, `windows/test-results/package-native-0.4.1a1.xml`, and the isolated `windows/test-results/gpu-cache-0.4.1a1.xml` (local generated artifacts).
- New coverage: unchanged-frame reuse without GPU dispatch/readback; 8/16-bit partial uploads; zero new texture allocations on warm edits; masks, clipping, hierarchy, placement, blend mode, visibility, ordering, undo/redo and depth invalidation; document/viewport separation; LRU eviction and CPU fallback under a 2 MiB test cap; disabling acceleration releases cached resources.
- Portable-package benchmark: 1024×1024, 8-bit, 6 layers, 12 changes in a 32×32 region. Cold cache: 308.505 ms, 301,989,888 uploaded bytes, 108 allocations, 72 prepared surfaces, 12 readbacks. Reused cache: 124.306 ms, 49,152 uploaded bytes, 0 allocations, 12 prepared surfaces, 12 readbacks. This measures this implementation with/without retained cache in one workload, not a universal guarantee or a complete old-release benchmark.
- ZIP `Compositor-0.4.1a1-win-x64.zip`: **26,262,098 bytes**, SHA-256 `43e13273a00221b968908f32c04d0a73b6f778595dfef81ade5906968a70af97`. Packaged executable matches the tested Release build byte for byte; test executable/Qt6Test/offscreen plugin were added to the verification extraction only, not the ZIP.
- Minimal-PATH launch using bundled Windows Qt plugin and application-owned capture succeeded: `windows/test-results/capture-0.4.1a1-windows.png`. Eight catalogs retain the same 607 keys and report version 0.4.1a1.
- Scope limits: CPU surface preparation/diffing, full changed-viewport GPU recomposition/readback, CPU editing/filter/export kernels, and existing format limits remain. Cache budget describes estimated retained CPU/GPU resources, not peak working-set or exact driver allocation. No macOS/Xcode tests were run.

---

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
