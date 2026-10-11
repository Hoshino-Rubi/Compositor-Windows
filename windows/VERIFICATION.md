# Windows 0.3.4 verification

Built and verified on Windows x64 with MSVC 2022 and Qt 6.8.3, October 11, 2026 (Asia/Taipei). The upstream baseline remains Compositor 1.4.9 (`b4bfdea87f9dc0cbc9eabfa63683eb1b9c2bca60`).

- `windows/build-native/native-tests.xml`: 71 functional tests passed, 0 failed/skipped.
- `windows/test-results/package-native-0.3.4.xml`: 71 functional tests passed with the packaged runtime, 0 failed/skipped.
- ZIP: 26177615 bytes; extracted application: 57832270 bytes. Increase over v0.3.3: 299643 bytes compressed.
- ZIP SHA-256: `19eeaf488222f88e170891e32b8fbf1d32e5a230d1856f67d71368bc1322ad5d`.
- The extracted executable matches the tested Release build and reports version 0.3.4.
- The portable application opened a native 16-bit PNG in all eight languages with only Windows system directories in PATH. Qt translations for German, Spanish, French, Japanese, Korean and both Chinese variants are present in the package. Standard dialog buttons are tested in the five new languages.
- All 555 catalog entries have identical key sets and preserve placeholders and import extensions. Locale normalization covers regional aliases, traditional/simplified Chinese, system selection and unsupported-language fallback.
- Preference tests preserve document identity, rendered pixels, dirty state and undo while changing language and settings; verify settings across new windows; apply default depth/resolution only to new canvases; and check per-section reset behavior.
- Shortcut tests reject duplicate/reserved bindings, keep unapplied edits while translating, apply and persist valid changes across a language change, and restore defaults. Saved bindings use Qt portable syntax independently of display language.
- Preferences captures cover all eight languages and all three sections, with additional 1.5x and 2x scale-factor runs. Workspace and Camera Raw captures cover all eight languages. Representative native captures were inspected for readable labels, layout and translated input hints.

Existing regressions cover native 16-bit precision, PSD/PSB/RAW/HEIC import, Camera Raw, rendering, filters, retouching, brush gestures, project round trips, image/project pickers and model inference. The pinned libheif alpha fixture and optional psd-tools 16-bit reference fixtures were enabled, so no fixture tests were skipped.

The application remains native C++20 / Qt 6, with no new runtime dependency. Language and preferences do not change the project format: 8-bit documents write v11, and 16-bit documents write v12. Older Windows/macOS builds reject v12. CMYK and floating-point/32-bit editing remain unsupported. macOS/Xcode tests were not run locally.
