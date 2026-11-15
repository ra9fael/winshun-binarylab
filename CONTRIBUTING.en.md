# Contributing

[简体中文](CONTRIBUTING.md)

Use the `qml` branch: it is the only maintained line. The native GDI edition ends at 0.3.3 and the `gdi` branch is archived; commit shared logic and protocol fixes directly to `qml`. Keep public documentation bilingual.

## Build

Windows x64, CMake, MSVC and Python 3.10+. QML additionally requires Qt 6.12.0 and the WinShun repository / commit pinned in `release.json`.

```powershell
cmake -S . -B build/release -A x64 -DBINARYLAB_OUTPUT_DIR="$PWD/build/bin"
cmake --build build/release --config Release
ctest --test-dir build/release -C Release --output-on-failure
python tools/package.py --build-dir build/bin --tag qml-v0.5.0
```

For QML also pass `-DCMAKE_PREFIX_PATH=<Qt kit>` and `-DWINSHUN_DIR=<pinned checkout>`.
Isolated output avoids overwriting running EXE / DLL files. Do not stop user processes for a build.

## Release

1. Update `plugin.json.version` and `binarylab.rc`. `release.json` pins the QML build dependencies. Keep numeric versions.
2. Update bilingual README, CHANGELOG, relevant screenshots and validation evidence. Check the GDI 0.3.3 replacement upgrade, fresh installs, data preservation, queries and window icons.
3. Build a clean commit, run relevant CTest and actual host checks. Push `qml`, then dispatch `release.yml` with its `ref` and `publish=false`; wait for that verification to pass. Branch pushes and PRs do not trigger workflows.
4. Push `qml-vX.Y.Z`. CI validates the identity, builds, tests and publishes the package plus checksum. QML publishes as latest.
5. Verify GitHub assets and checksums serially. Never overwrite published assets; bump the patch for fixes.

The plugin ID must stay `ra9fael.binarylab`; packages are `<id>-<version>.wsplugin`. QML packaging fails without `BinaryLabApp.dll`. Keep history JSONL compatible; back up before a breaking migration. Development directory links cannot be replaced by an archive; use isolated profiles for verification.

Use English Angular `type(scope): subject` commit subjects and bodies. Commit only task changes, never build / dist output, local paths or credentials. Update the host SDK `winshun.h` separately.
