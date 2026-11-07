# Contributing to ra9fael.binarylab

Thanks for helping. Bug reports, ideas and pull requests are all welcome.

## Reporting a bug

Open an issue with:

- the plugin version (Win顺 → 设置 → 插件 → 已安装) and your Win顺 version,
- Windows version and display setup (number of monitors, scaling),
- what you typed or did, what you expected, and what happened,
- the plugin's messages from Win顺's message log (消息记录), if any.

Remove anything private (paths with your user name, host names, IPs,
environment values) before posting.

## Development setup

- Windows 10/11 x64.
- CMake 3.21+ and a C++17 compiler: Visual Studio 2022 or later (MSVC, what
  CI and releases use) or MinGW-w64 GCC 13+.
- Python 3.10+ for `tools/package.py` and the integration tests (standard
  library only).
- Win顺 itself, to try the plugin.

```powershell
cmake -S . -B build -A x64                 # MSVC; for MinGW use -G "MinGW Makefiles"
cmake --build build --config Release       # writes binarylab.exe next to plugin.json
ctest --test-dir build -C Release --output-on-failure
```

To run your build inside Win顺, link the repository into Win顺's plugin folder
instead of installing a copy (adding a folder through the settings page copies
it, so later builds would not reach Win顺):

```powershell
New-Item -ItemType Junction -Path "$env:LOCALAPPDATA\WinShun\plugins\ra9fael.binarylab" -Target (Get-Location)
```

Then enable the plugin in **设置 → 插件 → 已安装**. Win顺 refuses to replace a
junction when you install a package of the same id. **Do not uninstall a linked
plugin from the settings page**: uninstall removes the folder recursively and
has no junction check, so it can delete the files in your clone. Remove the link
itself with `cmd /c rmdir "$env:LOCALAPPDATA\WinShun\plugins\ra9fael.binarylab"`
(this deletes only the junction). A running `binarylab.exe` is locked; rename it
(for example to `binarylab.exe.old`) before rebuilding, and Win顺 restarts the
plugin with the new exe. Remove an installed non-linked copy of the same id
first.

`winshun.h` is a byte-for-byte copy of the Win顺 SDK header
(`WinShun/src/plugins-sdk/winshun.h`). Do not edit it here; update it from
upstream as a separate commit.

## Tests

- `ctest` runs `binarylab_tests`: the bits / bytes / struct engines, the
  expression evaluator, CRC and parity, and a `#pragma pack` layout oracle
  that compares the engine with the compiler's own `sizeof` / `alignof` /
  `offsetof`. It has no OS dependency.
- There is no scripted integration test yet. [TESTPLAN.md](TESTPLAN.md)
  lists the manual and `wsbench` checks and their last results; update it
  when you verify something new.

## Pull requests

1. Fork, create a branch from `main`.
2. Keep a pull request to one topic. Follow the existing style: C++17, no
   third-party libraries, no runtime DLLs, all code and comments in English.
   Documentation is bilingual: `README.md` is the Chinese default and
   `README.en.md` its English counterpart (see "Documentation languages").
   User-facing strings in plugin.json stay bilingual zh/en.
3. Add or update unit tests for logic changes. CI must pass.
4. Before opening the PR, run `bits 0x5A`, `bytes ...` and `struct ...` in Win顺 and open the editor once.
5. If behaviour, settings or permissions change, update **both**
   `README.md` and `README.en.md`, and add a line under `## [Unreleased]` in
   `CHANGELOG.md`.
6. Commit messages use the Angular convention
   ([Conventional Commits](https://www.conventionalcommits.org/)):
   `type(scope): imperative summary`, a blank line, then the body. Allowed
   types: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `build`,
   `ci`, `chore`, `revert`. Lowercase summary, no trailing period.

Do not commit `binarylab.exe`, `build/` or `dist/`; `.gitignore` already excludes them.

### Permissions

Adding or widening a permission in `plugin.json` needs a reason in the PR and
in the README's permissions section. Win顺 asks users to confirm permissions
again when they change, so keep them minimal.

## Roadmap

Planned phases and the decisions behind them are in [ROADMAP.md](ROADMAP.md).
Please open an issue before starting work on a phase, so the design can be
agreed first.

## Documentation languages

Win顺 is Chinese-first but also ships an English UI (`winshun_en.ts`), so the
user-facing documentation follows the same split:

| File | Language | Role |
|---|---|---|
| `README.md` | 简体中文 | Default. GitHub renders it on the repository home page. |
| `README.en.md` | English | Full counterpart for English-UI users. |

Both files start with a language switch and end with a link back, so readers can
jump in either direction. **Keep both in sync**: any change to structure,
settings, permissions or install steps must be applied to both in the same PR.
Code identifiers, settings keys, command syntax, JSON and paths stay in English
in both files. The English README uses Win顺's real English UI labels
(`Settings → Plugins → Installed → Add plugin → Archive (.zip / .wsplugin)`),
while the Chinese one uses the Chinese labels.

`CONTRIBUTING.md`, `CHANGELOG.md`, `TESTPLAN.md` and `ROADMAP.md` stay in
English.

## Releasing (maintainers)

The version in `plugin.json` is the single source of truth. Versions follow
[Semantic Versioning](https://semver.org/).

1. Bump `version` in `plugin.json` and anything that echoes it in the code.
2. In `CHANGELOG.md`, rename `## [Unreleased]` to `## [x.y.z] - YYYY-MM-DD`,
   add a fresh empty `## [Unreleased]` above it, and update the links at the end.
3. Check locally:
   ```powershell
   cmake --build build --config Release
   ctest --test-dir build -C Release --output-on-failure
   python tools/package.py --tag vx.y.z
   ```
   then install `dist/ra9fael.binarylab-x.y.z.wsplugin` into Win顺 from the archive
   and try it.
4. Commit (`chore(release): vx.y.z`), tag and push:
   ```powershell
   git tag -a vx.y.z -m "vx.y.z"
   git push origin main vx.y.z
   ```
5. The `release` workflow builds with MSVC, runs the unit tests, checks that the
   tag matches `plugin.json` and that the CHANGELOG has the section, and
   publishes a GitHub Release with the `.wsplugin` and its `.sha256`.

A tag that does not match `plugin.json` fails the workflow before anything is
published. To redo a failed release, delete the tag locally and remotely, fix,
and tag again. Never replace the assets of a published release; publish a new
patch version instead.

The `.wsplugin` release is not the whole distribution: the plugin is listed in
Win顺's in-app plugin library. Clone
[LingCore/WinShun-plugins](https://github.com/LingCore/WinShun-plugins) and
generate the entry from the release asset:

```powershell
python tools/entry.py <release asset URL> --unreviewed-native
```

The `--unreviewed-native` flag is mandatory for a native (exe/dll) package; it
sets the entry's `review` field to `unreviewed-native`, which must never be
removed or replaced later. Submit `plugins/ra9fael.binarylab.json` (a PR for
outside contributors; the maintainers of this repository push it directly),
then check the published `index.json` picks it up.

## License

By contributing you agree that your contribution is licensed under the MIT
license of this repository.
