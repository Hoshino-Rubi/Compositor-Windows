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
