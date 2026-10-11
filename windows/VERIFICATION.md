# Windows 0.3.2 verification

Built and verified on Windows x64 with MSVC 2022 and Qt 6.8.3, October 11, 2026 (Asia/Taipei).
The upstream baseline remains Compositor 1.4.9, `b4bfdea87f9dc0cbc9eabfa63683eb1b9c2bca60`.

- `windows/build-native/native-tests.xml`: 57 tests passed, 0 failed/skipped (offscreen).
- `windows/test-results/package-native-0.3.2.xml`: 57 tests passed, 0 failed/skipped (Windows platform, extracted portable package).
- Final ZIP SHA-256: `208178c087bf54b8036fbb89789205b9c040a80cef492132072fbb8fbe7f6287`.
- ZIP size: 25777900 bytes. The packaged executable matches the tested Release build and reports version 0.3.2.
- The extracted application started, opened a PNG via its command-line entry and ran its tests with only the Windows/System32 PATH, using packaged DLLs/plugins/models.
- English, Simplified Chinese and Traditional Chinese welcome-page captures and the opened PNG were inspected.

New regressions exercise Open Image and Import through file pickers with PNG/JPEG files and Unicode/spaced paths, preservation of existing dirty tabs/undo, cancellation and decode failure, Open Project through a directory picker, and RAW opening/cancellation. Qt Test uses widget file dialogs for automation; the shipped application retains the default native Windows file dialogs. PSD/RAW canvas import into an existing document is also covered.

The existing project round-trip, PSD/PSB/DNG/HEIC/SVG/WebP, Camera Raw, liquify, selection, rendering, export and localization tests remain in the suite. The optional HEIC case used the pinned libheif `with-alpha-512x512.heic` fixture. macOS tests were not run locally.

Reproduce with PowerShell 7 and `windows/scripts/build.ps1`. Runtime reports and captures are under ignored `windows/test-results/`.
