# Notes for AI agents

Compositor is a macOS image editor for compositing and photo work, written in Swift (SwiftUI and AppKit, with some C for pixel work).

## Designing or editing a Compositor project

If you've been asked to make or change an image in a `.comp` project, you don't need the app's source code. Read [docs/writing-comp-files.md](docs/writing-comp-files.md): it covers the file format, the rules that make a project load, and how to write it safely while it's open, so the person can watch the canvas update as you work.

## Working on the app itself

- Build: open `Compositor.xcodeproj` and run the **Compositor** scheme, or `xcodebuild -project Compositor.xcodeproj -scheme Compositor -destination 'platform=macOS' build`.
- Tests: the `CompositorTests` target (`xcodebuild ... test -only-testing:CompositorTests`). CI runs these on every push.
- Match the surrounding code: its naming, its comment style and density.
- American spelling in code, comments and UI ("color", not "colour").
- The project file format is described in [docs/project-format.md](docs/project-format.md). A change to what's saved means a format version bump there and in `ProjectManifest.current`.

## Native Windows port

- The Windows implementation is C++20 / Qt 6 Widgets in `windows/`. Keep it native; do not introduce Electron or a browser runtime.
- Read `windows/README.md` for compatibility and build instructions. Use PowerShell 7 with `windows/scripts/build.ps1` to build and run Qt Test.
- Preserve the upstream `.comp` format and original PNG/metadata when pixels are untouched. Explicitly reject render features the Windows renderer cannot reproduce.
- Add or change UI strings in all `windows/translations/*.json` catalogs (currently eight languages).

- GitHub release titles and complete release notes use English first, with Simplified Chinese notes in a collapsible section below.
