"""Deterministic variant packages. Only tracked sources and required binaries ship."""
from __future__ import annotations
import argparse
import hashlib
from io import BytesIO
from pathlib import Path
import subprocess
import sys
import zipfile
from release_metadata import read, changelog

ROOT = Path(__file__).resolve().parent.parent
MAX_BYTES = 256 << 20


def package(root: Path, build: Path, out: Path, tag: str | None = None) -> Path:
    manifest, release = read(root, tag)
    variant, version = release['variant'], manifest['version']
    # Validate everything before writing output, including bilingual notes.
    notes = '\n\n---\n\n'.join(changelog(root, version, name)
        for name in ('CHANGELOG.md', 'CHANGELOG.en.md'))
    required = ['binarylab.exe'] + (['BinaryLabApp.dll'] if variant == 'qml' else [])
    binaries = {}
    for name in required:
        file = build / name
        if not file.is_file():
            raise ValueError(f'{name} not found in {build}; build the {variant} target first')
        data = file.read_bytes()
        if not data.startswith(b'MZ'):
            raise ValueError(f'{file} is not a Windows PE executable')
        binaries[name] = data
    tracked = subprocess.run(['git', 'ls-files', '-z'], cwd=root, check=True,
                             capture_output=True).stdout.decode('utf-8').split('\0')
    files = {}
    for name in tracked:
        if not name or any(p.startswith('.') for p in Path(name).parts):
            continue
        if name.startswith(('tools/verify/', 'build/', 'dist/')):
            continue
        if Path(name).suffix.lower() in ('.exe', '.dll', '.pdb', '.obj', '.lib'):
            continue
        files[name] = (root / name).read_bytes()
    for name in ('plugin.json', 'release.json', 'app.ico', 'README.md', 'README.en.md', 'LICENSE'):
        if name not in files:
            raise ValueError(f'{name} must be git-tracked before packaging')
    if variant == 'qml':
        for size in (16, 20, 24, 32, 40, 48, 64, 256):
            name = f'icons/app-{size}.png'
            if name not in files:
                raise ValueError(f'{name} must be git-tracked before packaging')
    files.update(binaries)
    if sum(map(len, files.values())) > MAX_BYTES:
        raise ValueError('Unpacked package exceeds 256 MiB')
    buffer = BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    data = buffer.getvalue()
    if len(data) > MAX_BYTES:
        raise ValueError('Archive exceeds 256 MiB')
    filename = f"{manifest['id']}-{variant}-{version}.wsplugin"
    digest = hashlib.sha256(data).hexdigest()
    out.mkdir(parents=True, exist_ok=True)
    target = out / filename
    target.write_bytes(data)
    (out / (filename + '.sha256')).write_text(f'{digest}  {filename}\n', encoding='ascii')
    title = f'Binary Lab {variant.upper()} {version}'
    if release['prerelease']:
        title += ' — Preview / 预览版'
    (out / 'release-notes.md').write_text(
        f'# {title}\n\n{notes}\n\nSHA256: `{digest}`  `{filename}`\n', encoding='utf-8')
    print(f'{filename}: {len(files)} files, {len(data)} bytes, SHA256 {digest}')
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', help='Must match <variant>-v<plugin.json version>')
    parser.add_argument('--build-dir', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, default=ROOT / 'dist')
    args = parser.parse_args()
    try:
        package(ROOT, args.build_dir.resolve(), args.out.resolve(), args.tag)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    return 0
