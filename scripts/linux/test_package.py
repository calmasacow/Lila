"""Focused checks: python3 scripts/linux/test_package.py (uses available native package tools)."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

from package import packages, write

with tempfile.TemporaryDirectory(prefix='lila-package-check-') as scratch:
    output = Path(scratch)
    root = output / 'root'
    for name in ['opt/lila/lila', 'usr/bin/lila', 'usr/bin/lilabatch',
                 'usr/share/applications/lila.desktop', 'usr/share/icons/hicolor/256x256/apps/lila.png']:
        write(root / name, 'package fixture\n')
    formats = [kind for kind, tool in [('deb', 'dpkg-deb'), ('rpm', 'rpmbuild'), ('arch', 'bsdtar')]
               if shutil.which(tool)]
    assert formats, 'Install at least one package tool: dpkg-deb, rpmbuild, or bsdtar'
    packages(root, output, '1.2.3', formats)
    for line in (output / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ')
        assert hashlib.sha256((output / name).read_bytes()).hexdigest() == digest
    assert not (root / 'DEBIAN').exists()
    assert not (root / '.PKGINFO').exists()
    if 'deb' in formats:
        package = next(output.glob('*.deb'))
        metadata = subprocess.check_output(['dpkg-deb', '-f', str(package)], text=True)
        assert 'Architecture: amd64' in metadata and 'Version: 1.2.3' in metadata
    if 'rpm' in formats:
        package = next(output.glob('*.rpm'))
        files = subprocess.check_output(['rpm', '-qpl', str(package)], text=True)
        assert '/opt/lila/lila' in files
    if 'arch' in formats:
        package = next(output.glob('*.pkg.tar.zst'))
        metadata = subprocess.check_output(['bsdtar', '-xOf', str(package), './.PKGINFO'], text=True)
        assert 'arch = x86_64' in metadata and 'pkgver = 1.2.3-1' in metadata
    print('Native package metadata and checksums passed:', ', '.join(formats))
