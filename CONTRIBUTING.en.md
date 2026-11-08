# Contributing

[简体中文](CONTRIBUTING.md)

Use the long-lived `gdi` (default) and `qml` branches. Keep shared logic / protocol fixes in separate commits and port them to the other branch; UI changes stay on their branch. Keep public documentation bilingual.

## Build

Windows x64, CMake, MSVC and Python 3.10+. QML additionally requires Qt 6.12.0 and the WinShun repository / commit pinned in `release.json`.

```powershell
cmake -S . -B build/release -A x64 -DBINARYLAB_OUTPUT_DIR="$PWD/build/bin"
cmake --build build/release --config Release
ctest --test-dir build/release -C Release --output-on-failure
python tools/package.py --build-dir build/bin --tag gdi-v0.3.2
```

For QML also pass `-DCMAKE_PREFIX_PATH=<Qt kit>` and `-DWINSHUN_DIR=<pinned checkout>`.
Isolated output avoids overwriting running EXE / DLL files. Do not stop user processes for a build.

## Release

1. Update `plugin.json.version` and `binarylab.rc`. `release.json` declares the edition, preview status and QML build dependencies. Keep numeric versions.
2. Update bilingual README, CHANGELOG, relevant screenshots and validation evidence. Check fresh installs, replacement in both directions, data preservation, queries and window icons. Document incomplete QML pages.
3. Build a clean commit, run relevant CTest and actual host checks. Push the branch, then dispatch `release.yml` with its `ref` and `publish=false`; wait for that verification to pass. Branch pushes and PRs do not trigger workflows.
4. Push `<variant>-vX.Y.Z` on that branch. CI validates the identity, builds, tests and publishes the package plus checksum. QML previews never become latest.
5. Verify GitHub assets and checksums serially. Never overwrite published assets; bump the patch for fixes.

Both IDs must stay `ra9fael.binarylab`. GDI packages no DLLs; QML packaging fails without `BinaryLabApp.dll`. Keep history JSONL compatible; back up before a breaking migration. Development directory links cannot be replaced by an archive; use isolated profiles for verification.

Use English Angular `type(scope): subject` commit subjects and bodies. Commit only task changes, never build / dist output, local paths or credentials. Update the host SDK `winshun.h` separately.
