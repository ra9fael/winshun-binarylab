"""Export validated release metadata to GitHub Actions without running a shell."""
import os
from pathlib import Path
from release_metadata import read, changelog

root = Path(__file__).resolve().parent.parent
tag = os.environ.get('RELEASE_TAG') or None
manifest, release = read(root, tag)
for name in ('CHANGELOG.md', 'CHANGELOG.en.md'):
    changelog(root, manifest['version'], name)
source = release.get('hostSource', {})
values = {
    'variant': release['variant'], 'version': manifest['version'],
    'prerelease': str(release['prerelease']).lower(),
    'qt': release.get('qtVersion', ''),
    'host_repository': source.get('repository', ''), 'host_ref': source.get('ref', ''),
}
with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
    for name, value in values.items():
        output.write(f'{name}={value}\n')
