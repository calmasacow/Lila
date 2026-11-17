#!/usr/bin/env python3
"""Bundle a built x86_64 Linux runtime, then produce DEB/RPM/Arch/AppImage packages."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
# Host libc, graphics loaders and GPU drivers must match the target system.
HOST_LIB = re.compile(r"^(ld-linux|lib(c|m|dl|pthread|rt|resolv|util)\.so|lib(GL|EGL|OpenGL|GLES|vulkan|drm|gbm|nvidia|cuda))")


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def elf(path):
    if not path.is_file():
        return False
    with path.open('rb') as source:
        return source.read(4) == b'\x7fELF'


def write(path, content, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    if executable:
        path.chmod(0o755)


def stage(build, config, python, output):
    binary = build / 'bin' / config
    app = output / 'root/opt/lila'
    if (output / 'root').exists():
        raise SystemExit(f'Refusing to overwrite staging directory: {output / "root"}')
    for required in [binary / 'lila', binary / 'lilabatch', python / 'bin/python3']:
        if not required.is_file():
            raise SystemExit(f'Missing build input: {required}')
    app.mkdir(parents=True)
    for name in ['lila', 'lilabatch']:
        shutil.copy2(binary / name, app / name)
    for path in binary.glob('*.so*'):
        shutil.copy2(path, app / path.name, follow_symlinks=False)
    for name in ['extensions', 'shaders', 'python-site']:
        if (binary / name).is_dir():
            shutil.copytree(binary / name, app / name, symlinks=True)
    for name in ['resources', 'python']:
        shutil.copytree(ROOT / name, app / name, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    shutil.copytree(python, app / 'python-runtime', symlinks=True,
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc', 'include', 'pkgconfig'))
    cache = (build / 'CMakeCache.txt').read_text()
    triplet = re.search(r'^VCPKG_TARGET_TRIPLET:[^=]+=(.+)$', cache, re.M)[1]
    vcpkg = ROOT / 'vcpkg_installed' / triplet
    plugins = vcpkg / 'Qt6/plugins'
    if not (plugins / 'platforms/libqxcb.so').is_file():
        raise SystemExit(f'Qt XCB plugin missing: {plugins}')
    for name in ['platforms', 'imageformats', 'iconengines', 'tls', 'xcbglintegrations']:
        if (plugins / name).is_dir():
            shutil.copytree(plugins / name, app / 'plugins' / name, symlinks=True)
    run('cmake', f'-DSOURCE_DIR={ROOT}', f'-DSHARE_DIR={vcpkg / "share"}',
        f'-DPYTHON_HOME={python}', f'-DNANOBIND_DIR={build / "_deps/nanobind-src"}',
        f'-DUFBX_DIR={build / "_deps/ufbx-src"}', f'-DOUT_DIR={app}', '-P', ROOT / 'cmake/LilaLicenses.cmake')
    library = app / 'lib'
    library.mkdir()
    env = os.environ | {'LD_LIBRARY_PATH': ':'.join(map(str, [binary, vcpkg / 'lib', python / 'lib', library]))}
    queue = [p for p in app.rglob('*') if elf(p)]
    checked = set()
    while queue:
        path = queue.pop()
        resolved = path.resolve()
        if resolved in checked:
            continue
        checked.add(resolved)
        result = run('ldd', path, env=env, capture_output=True, text=True)
        if '=> not found' in result.stdout:
            raise SystemExit(f'Unresolved runtime dependency for {path}:\n{result.stdout}')
        for line in result.stdout.splitlines():
            match = re.search(r'=> (/\S+)', line)
            if not match:
                continue
            source = Path(match[1])
            if HOST_LIB.match(source.name) or (app / source.name).exists():
                continue
            target = library / source.resolve().name
            if not target.exists():
                shutil.copy2(source.resolve(), target)
                queue.append(target)
            alias = library / source.name
            if alias != target and not alias.exists():
                alias.symlink_to(target.name)
    # Python wheels retain their own relative RPATHs (e.g. numpy.libs).
    for path in app.rglob('*'):
        if 'python-runtime' not in path.parts and not path.is_symlink() and elf(path):
            run('patchelf', '--remove-rpath', path)
            run('strip', '--strip-debug', path)
    launcher = '''#!/bin/sh
set -eu
app="$(CDPATH= cd -- "$(dirname -- "$(readlink -f -- "$0")")/../../opt/lila" && pwd)"
export LD_LIBRARY_PATH="$app:$app/lib:$app/python-runtime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$app/plugins"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export LILA_PYTHON_HOME="$app/python-runtime"
exec "$app/PROGRAM" "$@"
'''
    for name in ['lila', 'lilabatch']:
        write(output / f'root/usr/bin/{name}', launcher.replace('PROGRAM', name), True)
    write(output / 'root/usr/share/applications/lila.desktop', '''[Desktop Entry]
Type=Application
Name=Lila
Comment=3D modeling and animation
Exec=lila %F
Icon=lila
Terminal=false
Categories=Graphics;3DGraphics;
''')
    icon = output / 'root/usr/share/icons/hicolor/256x256/apps/lila.png'
    icon.parent.mkdir(parents=True)
    shutil.copy2(ROOT / 'resources/icons/lila_color.png', icon)
    return output / 'root'


def packages(root, output, version, formats, appimagetool=None):
    stem = f'Lila-{version}-linux-x86_64'
    libc = os.confstr('CS_GNU_LIBC_VERSION').split()[-1]
    size = sum(p.stat().st_size for p in root.rglob('*') if p.is_file())
    if 'deb' in formats:
        write(root / 'DEBIAN/control', f'''Package: lila
Version: {version}
Architecture: amd64
Maintainer: Lila contributors <noreply@lila-3d.com>
Installed-Size: {(size + 1023) // 1024}
Depends: libc6 (>= {libc}), libgl1, libegl1, libvulkan1
Section: graphics
Priority: optional
Description: Lila 3D modeling and animation
''')
        run('dpkg-deb', '--root-owner-group', '--build', root, output / f'{stem}.deb')
        shutil.rmtree(root / 'DEBIAN')
    if 'rpm' in formats:
        top = output / 'rpm'
        for directory in ['BUILD', 'BUILDROOT', 'RPMS', 'SOURCES', 'SPECS', 'SRPMS']:
            (top / directory).mkdir(parents=True, exist_ok=True)
        spec = top / 'SPECS/lila.spec'
        write(spec, f'''Name: lila
Version: {version}
Release: 1
Summary: Lila 3D modeling and animation
License: LicenseRef-Lila
BuildArch: x86_64
AutoReqProv: no
Requires: glibc >= {libc}, libglvnd-glx, libglvnd-egl, vulkan-loader
%description
Lila 3D modeling and animation, with bundled Qt and Python.
%install
mkdir -p "%{{buildroot}}"
cp -a "{root}/." "%{{buildroot}}/"
%files
/opt/lila
/usr/bin/lila
/usr/bin/lilabatch
/usr/share/applications/lila.desktop
/usr/share/icons/hicolor/256x256/apps/lila.png
''')
        run('rpmbuild', '-bb', '--define', f'_topdir {top}', '--define', '__os_install_post %{nil}', '--define', '_binary_payload w3.zstdio', spec)
        shutil.copy2(next((top / 'RPMS').rglob('*.rpm')), output / f'{stem}.rpm')
    if 'arch' in formats:
        write(root / '.PKGINFO', f'''pkgname = lila
pkgbase = lila
pkgver = {version}-1
pkgdesc = Lila 3D modeling and animation
url = https://github.com/lila-3d/Lila
builddate = {os.environ.get('SOURCE_DATE_EPOCH', str(int(time.time())))}
packager = Lila contributors
size = {size}
arch = x86_64
license = custom:Lila
depend = glibc>={libc}
depend = libglvnd
depend = vulkan-icd-loader
''')
        run('bsdtar', '--zstd', '--uid', '0', '--gid', '0', '-cf', output / f'{stem}.pkg.tar.zst', '-C', root, '.')
        (root / '.PKGINFO').unlink()
    if 'appimage' in formats:
        if not appimagetool:
            raise SystemExit('--appimagetool is required for AppImage packaging')
        shutil.copy2(root / 'usr/share/applications/lila.desktop', root / 'lila.desktop')
        shutil.copy2(root / 'usr/share/icons/hicolor/256x256/apps/lila.png', root / 'lila.png')
        write(root / 'AppRun', '#!/bin/sh\nset -eu\nappdir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"\nexec "$appdir/usr/bin/lila" "$@"\n', True)
        run(appimagetool, root, output / f'{stem}.AppImage', env=os.environ | {'ARCH': 'x86_64', 'APPIMAGE_EXTRACT_AND_RUN': '1'})
    assets = [p for p in output.iterdir() if p.is_file() and p.name.startswith(stem)]
    write(output / 'SHA256SUMS', ''.join(f'{hashlib.file_digest(p.open("rb"), "sha256").hexdigest()}  {p.name}\n' for p in sorted(assets)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--config', default='Release')
    parser.add_argument('--python-home', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--formats', nargs='+', choices=['deb', 'rpm', 'arch', 'appimage'], default=['deb', 'rpm', 'arch', 'appimage'])
    parser.add_argument('--appimagetool', type=Path)
    args = parser.parse_args()
    version = re.search(r'project\(Lila VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text())[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = stage(args.build_dir.resolve(), args.config, args.python_home.resolve(), output)
    packages(root, output, version, args.formats, args.appimagetool.resolve() if args.appimagetool else None)
    print(f'Packages and SHA256SUMS: {output}')


if __name__ == '__main__':
    main()
