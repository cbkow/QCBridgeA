# Releasing QCBridgeA

The same shape as QCView's, QCBridgeAE's and QCBridgeN's flows: everything
is a script in `packaging/`, nothing secret is in the repository, the
credentials are in the login keychain.

## Version

`CMakeLists.txt`: `project(QCBridgeA VERSION x.y.z)`. The macOS Info.plist,
the Windows version resource and `--help` take it from there. The Inno
script carries its own copy: `packaging/windows/qcbridgea.iss`
`MyAppVersion`.

## macOS

| Thing | Check it | If it's missing |
|---|---|---|
| Developer ID Application + Installer certs | `security find-identity -v` shows both | Apple Developer account → Certificates |
| notarytool profile `QCView` | `xcrun notarytool history --keychain-profile QCView` | `xcrun notarytool store-credentials "QCView" --apple-id <id> --team-id 5Z4S9VHV56` |
| codesign unprompted | signing runs without dialogs | "Allow all applications" on the private key in Keychain Access |

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"
# the check that matters, on a Mac with the AJA software and a device:
# QCView with this build registered, Start on the AJA row, the card describes
# itself on the row and the monitor shows the viewport
packaging/macos/build-pkg.sh --notarize
# → dist/QCBridgeA-<version>-arm64.pkg, and its SHA-256 on the last line
```

The app is one static binary at the macOS 13.0 floor; `build-pkg.sh` refuses
a binary that links anything outside /usr/lib and /System, smoke-tests the
staged bundle (`--seconds 1`: it reaches the bus and quits), signs it with
the hardened runtime, notarizes and staples the app, then the pkg.
`packaging/macos/notarize.sh <file>` does one file on its own.

## Windows

On the Windows box, from a Release build (`cmake -S . -B build-release -G
Ninja -DCMAKE_BUILD_TYPE=Release` under vcvars64, VS 2022 or later):

    packaging\windows\build-setup.cmd
    → dist\QCBridgeA-<version>-Setup-x64.exe

It stages the exe and the licence, refuses an exe that needs the Visual C++
runtime DLLs (the build links the static runtime, as the AJA SDK's static
library does), and runs Inno Setup 6 on `qcbridgea.iss`. The installer
writes the registration manifest to `%ProgramData%\QCView\Outputs\aja.json`
and removes it on uninstall. The Setup is not code-signed; signing is a
certificate away (`signtool` on the exe and the Setup).

## Then

```bash
git tag -a v<version> -m "QCBridgeA <version>" && git push origin main v<version>
```

GitHub release `v<version>` with the pkg and the Setup attached; the README
links the latest release.

## Checking a build after the fact

```bash
codesign --verify --deep --strict --verbose=2 /Applications/QCBridgeA.app
xcrun stapler validate /Applications/QCBridgeA.app
xcrun stapler validate dist/QCBridgeA-<version>-arm64.pkg
spctl --assess --type install -vv dist/QCBridgeA-<version>-arm64.pkg
cat "/Library/Application Support/QCView/Outputs/aja.json"
```
