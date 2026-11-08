"""Validate the release identity; shared by packaging and CI (stdlib only)."""
from __future__ import annotations
import json
import re
from pathlib import Path


def read(root: Path, tag: str | None = None) -> tuple[dict, dict]:
    manifest = json.loads((root / 'plugin.json').read_text(encoding='utf-8'))
    release = json.loads((root / 'release.json').read_text(encoding='utf-8'))
    variant = release.get('variant')
    if variant not in ('gdi', 'qml'):
        raise ValueError('release.json variant must be gdi or qml')
    if not isinstance(release.get('prerelease'), bool):
        raise ValueError('release.json prerelease must be boolean')
    if manifest.get('id') != 'ra9fael.binarylab':
        raise ValueError('Both variants must use id ra9fael.binarylab for replacement installs')
    version = manifest.get('version', '')
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('plugin.json version must be X.Y.Z')
    expected = f'{variant}-v{version}'
    if tag is not None and tag != expected:
        raise ValueError(f'tag {tag!r} does not match {expected!r}')
    if manifest.get('icon') != 'app.ico':
        raise ValueError('plugin.json must declare icon: app.ico')
    runtime = manifest.get('runtime', {})
    if runtime.get('program') != 'binarylab.exe':
        raise ValueError('runtime.program must be binarylab.exe')
    if variant == 'qml':
        if runtime.get('qt') != 'host':
            raise ValueError('QML must declare runtime.qt: host')
        qt = release.get('qtVersion', '')
        if not re.fullmatch(r'\d+\.\d+\.\d+', qt) or runtime.get('qtVersion') != qt:
            raise ValueError('QML build and manifest must pin the same Qt version')
        source = release.get('hostSource', {})
        if not re.fullmatch(r'[0-9a-f]{40}', source.get('ref', '')):
            raise ValueError('QML host source must pin a full commit SHA')
        if not re.fullmatch(r'[\w.-]+/[\w.-]+', source.get('repository', '')):
            raise ValueError('Invalid host source repository')
    elif 'qt' in runtime or 'qtVersion' in runtime:
        raise ValueError('GDI must not request Qt')
    return manifest, release


def changelog(root: Path, version: str, filename: str) -> str:
    lines = (root / filename).read_text(encoding='utf-8').splitlines()
    body: list[str] = []
    inside = False
    for line in lines:
        if line.startswith('## '):
            if inside:
                break
            inside = re.match(r'^## \[' + re.escape(version) + r'\](?:\s|$)', line) is not None
        elif inside:
            body.append(line)
    text = '\n'.join(body).strip()
    if not text:
        raise ValueError(f'{filename} has no nonempty ## [{version}] section')
    return text
