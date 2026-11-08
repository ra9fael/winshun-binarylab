"""Packaging regressions: incomplete QML, cross-variant tags and stray DLLs."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from package_variant import package


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        self.out = self.root / 'dist'
        self.manifest = {'id': 'ra9fael.binarylab', 'version': '1.2.3', 'icon': 'app.ico',
                         'runtime': {'program': 'binarylab.exe'}}
        self.release = {'variant': 'gdi', 'prerelease': False}
        for name in ('README.md', 'README.en.md', 'LICENSE', 'app.ico'):
            (self.root / name).write_text('fixture', encoding='utf-8')
        for name in ('CHANGELOG.md', 'CHANGELOG.en.md'):
            (self.root / name).write_text('## [1.2.3]\n\nA tested release.\n', encoding='utf-8')
        (self.build / 'binarylab.exe').write_bytes(b'MZfixture executable')
        (self.root / '.gitignore').write_text('build/\ndist/\n', encoding='ascii')
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        self.save()

    def save(self):
        (self.root / 'plugin.json').write_text(json.dumps(self.manifest), encoding='utf-8')
        (self.root / 'release.json').write_text(json.dumps(self.release), encoding='utf-8')
        subprocess.run(['git', '-c', 'core.autocrlf=false', 'add', '.'], cwd=self.root, check=True)

    def qml(self):
        self.release.update(variant='qml', prerelease=True, qtVersion='6.12.0',
                            hostSource={'repository': 'ra9fael/WinShun', 'ref': 'a' * 40})
        self.manifest['runtime'].update(qt='host', qtVersion='6.12.0')
        (self.root / 'icons').mkdir()
        for size in (16, 20, 24, 32, 40, 48, 64, 256):
            (self.root / f'icons/app-{size}.png').write_bytes(b'PNGfixture')
        self.save()

    def test_gdi_excludes_stray_and_tracked_dlls(self):
        (self.build / 'BinaryLabApp.dll').write_bytes(b'MZstale')
        (self.root / 'stray.dll').write_bytes(b'MZstale')
        self.save()
        target = package(self.root, self.build, self.out, 'gdi-v1.2.3')
        self.assertEqual(target.name, 'ra9fael.binarylab-1.2.3.wsplugin')
        with zipfile.ZipFile(target) as archive:
            self.assertIn('binarylab.exe', archive.namelist())
            self.assertFalse(any(n.lower().endswith('.dll') for n in archive.namelist()))
            self.assertEqual(json.loads(archive.read('plugin.json'))['id'], 'ra9fael.binarylab')

    def test_qml_requires_dll_before_writing(self):
        self.qml()
        with self.assertRaisesRegex(ValueError, 'BinaryLabApp.dll not found'):
            package(self.root, self.build, self.out, 'qml-v1.2.3')
        self.assertFalse(self.out.exists())

    def test_qml_contains_dll_icons_and_preview_notes(self):
        self.qml()
        (self.build / 'BinaryLabApp.dll').write_bytes(b'MZfixture dll')
        target = package(self.root, self.build, self.out, 'qml-v1.2.3')
        with zipfile.ZipFile(target) as archive:
            self.assertIn('BinaryLabApp.dll', archive.namelist())
            self.assertIn('icons/app-256.png', archive.namelist())
        self.assertIn('Preview', (self.out / 'release-notes.md').read_text(encoding='utf-8'))

    def test_rejects_other_variant_and_legacy_tags(self):
        for tag in ('qml-v1.2.3', 'v1.2.3', 'gdi-v1.2.4'):
            with self.assertRaisesRegex(ValueError, 'tag'):
                package(self.root, self.build, self.out, tag)
        self.assertFalse(self.out.exists())

    def test_deterministic_archive_and_checksum(self):
        target = package(self.root, self.build, self.out)
        original = target.read_bytes()
        package(self.root, self.build, self.out)
        self.assertEqual(original, target.read_bytes())
        self.assertEqual((self.out / (target.name + '.sha256')).read_text().split()[0],
                         hashlib.sha256(original).hexdigest())

    def test_missing_translation_leaves_no_partial_package(self):
        (self.root / 'CHANGELOG.en.md').write_text('## [Unreleased]\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'CHANGELOG.en.md'):
            package(self.root, self.build, self.out)
        self.assertFalse(self.out.exists())


if __name__ == '__main__':
    unittest.main()
