"""Rebuild the native Windows port of Digimon Rumble Arena 2 from your own disc image.

    python reproduce.py "Digimon Rumble Arena 2 (USA).iso" [--work work] [--xdvdfs path] [--installer]

Your image may be a full .iso or a trimmed .xiso. Nothing from the game is in this repository.
The script extracts your image and verifies its files, fetches
XboxRecomp at the commit this work was done against, applies this repository's patch, statically
recompiles the game's XBE with this repository's function seeds, builds the executable with MSVC and
puts a playable copy in <work>/dist. Every step is skipped when its output already exists, so an
interrupted run can simply be started again.

Requirements (see README.md): Windows 10/11 x64, Python 3.10+ with `pip install capstone`, Git,
Visual Studio 2022 or newer with the C++ workload and a Windows SDK, CMake 3.20+, Ninja, and xdvdfs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
REF = json.loads((HERE / 'data/reference.json').read_text(encoding='utf-8'))
UPSTREAM = 'https://github.com/sp00nznet/xboxrecomp.git'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def tree_sha256(root):
    """One hash over every file's path and content: any missing, extra or changed file shows."""
    h = hashlib.sha256()
    for rel in sorted(p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()):
        h.update(f'{rel}\t{sha256(root / rel)}\n'.encode())
    return h.hexdigest()


def run(argv, cwd=None, env=None):
    print('>', ' '.join(str(a) for a in argv), flush=True)
    subprocess.run([str(a) for a in argv], cwd=cwd, env=env, check=True)


def find_vcvars():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / \
        'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.exists():
        sys.exit('Visual Studio not found (vswhere.exe missing). Install Visual Studio with the C++ workload.')
    root = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                           'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                          capture_output=True, text=True).stdout.strip()
    vcvars = Path(root) / 'VC/Auxiliary/Build/vcvarsall.bat'
    if not root or not vcvars.exists():
        sys.exit('No Visual Studio with the C++ x64 tools was found.')
    return vcvars


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('iso', type=Path, help='your dump of the game disc (.iso or .xiso)')
    ap.add_argument('--work', type=Path, default=HERE / 'work')
    ap.add_argument('--xdvdfs', default='xdvdfs', help='xdvdfs executable (https://github.com/antangelo/xdvdfs)')
    ap.add_argument('--installer', action='store_true', help='also build a setup .exe with Inno Setup (private use)')
    ap.add_argument('--version', default='local', help='version shown by the setup program')
    a = ap.parse_args()
    work = a.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    for tool in ('git', 'cmake', 'ninja'):
        if not shutil.which(tool):
            sys.exit(f'{tool} is not on PATH.')
    xdvdfs = shutil.which(a.xdvdfs) or (a.xdvdfs if Path(a.xdvdfs).is_file() else None)
    if xdvdfs:
        xdvdfs = str(Path(xdvdfs).resolve())    # CreateProcess wants an absolute Windows path
    else:
        sys.exit('xdvdfs not found: put it on PATH or pass --xdvdfs <path to xdvdfs.exe>.')

    # 1. Extract your image (a full .iso or a trimmed .xiso hold the same files), then check the
    #    files themselves: the USA release this work was done against, unmodified.
    game = work / 'game'
    if not (game / 'default.xbe').exists():
        run([xdvdfs, 'unpack', a.iso.resolve(), game])
    redo = f' Delete "{game}" and run again with your own dump of {REF["game_name"]}.'
    if sha256(game / 'default.xbe') != REF['xbe_sha256']:
        sys.exit('This is not ' + REF['game_name'] + ', the only release this port supports. '
                 'For other releases see OTHER-RELEASES.md.' + redo)
    print('Checking the extracted files...', flush=True)
    if tree_sha256(game) != REF['game_files_sha256']:
        sys.exit('The extracted files differ from a clean dump (modified, damaged or incomplete).' + redo)

    # 3. XboxRecomp at the commit this work used, plus this repository's patch.
    kit = work / 'xboxrecomp'
    if not (kit / '.dra2-patched').exists():
        if not kit.exists():
            run(['git', 'clone', UPSTREAM, kit])
        run(['git', '-C', kit, 'checkout', '--quiet', REF['xboxrecomp_commit']])
        run(['git', '-C', kit, 'apply', '--whitespace=nowarn', HERE / 'patches/xboxrecomp-193e2995-dra2.patch'])
        (kit / '.dra2-patched').write_text('ok')

    # 4. Static recompilation: analysis, then C generation, seeded with the function entries this
    #    work recovered (data/seeds.json).
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
               PYTHONPATH=os.pathsep.join(filter(None, [str(kit), os.environ.get('PYTHONPATH')])))
    py = [sys.executable, '-B', '-X', 'utf8', '-m']
    xbe, pipe = game / 'default.xbe', work / 'pipeline'
    if not (pipe / 'abi').exists():
        pipe.mkdir(exist_ok=True)
        run(py + ['tools.xbe_parser', xbe, '--json', pipe / 'analysis.json'], cwd=kit, env=env)
        run(py + ['tools.disasm', xbe, '-o', pipe / 'disasm', '--analysis-json', pipe / 'analysis.json',
                  '--seed-functions', HERE / 'data/seeds.json'], cwd=kit, env=env)
        run(py + ['tools.func_id', xbe, '--functions', pipe / 'disasm/functions.json', '--strings',
                  pipe / 'disasm/strings.json', '--xrefs', pipe / 'disasm/xrefs.json', '-o', pipe / 'func_id'],
            cwd=kit, env=env)
        run(py + ['tools.abi_analysis', xbe, '--disasm-dir', pipe / 'disasm', '--func-id-dir', pipe / 'func_id',
                  '--output-dir', pipe / 'abi'], cwd=kit, env=env)
    project = work / 'project'
    gen = project / 'src/recomp/gen'
    if not (gen / 'recomp_dispatch.c').exists():
        shutil.copytree(HERE / 'project', project, dirs_exist_ok=True)
        gen.mkdir(parents=True, exist_ok=True)
        run(py + ['tools.recomp', xbe, '--all', '--split', '250', '--gen-dir', gen, '--disasm-dir',
                  pipe / 'disasm', '--func-id-dir', pipe / 'func_id', '--abi-dir', pipe / 'abi',
                  '-o', pipe / 'recomp',
                  # cold chunks the detector split from their owner (ENGINEERING.md)
                  '--coalesce-functions', HERE / 'data/coalesce.json'], cwd=kit, env=env)

    # 5. SDL3 (controllers): the official release, checked against its published hash.
    sdl_zip = work / 'SDL3-devel-VC.zip'
    if not sdl_zip.exists():
        print('> download', REF['sdl3_url'], flush=True)
        urllib.request.urlretrieve(REF['sdl3_url'], sdl_zip)
    if sha256(sdl_zip) != REF['sdl3_sha256']:
        sys.exit(f'{sdl_zip} does not match the SDL3 release hash; delete it and run again.')
    sdl_dir = work / 'sdl3'
    if not sdl_dir.exists():
        zipfile.ZipFile(sdl_zip).extractall(sdl_dir)
    sdl_cmake = next(sdl_dir.glob('SDL3-*/cmake'))

    # 5b. The official SDL3.dll has no Switch 2 controller driver: SDL compiles it only with libusb.
    #     Rebuild the same SDL3 version from its source release with libusb 1.0.30's official build.
    #     The .7z is opened with Windows' own tar.exe (bsdtar).
    for key in ('sdl3_src', 'libusb_bin', 'libusb_src'):
        f = work / REF[key + '_url'].rsplit('/', 1)[1]
        if not f.exists():
            print('> download', REF[key + '_url'], flush=True)
            urllib.request.urlretrieve(REF[key + '_url'], f)
        if sha256(f) != REF[key + '_sha256']:
            sys.exit(f'{f} does not match its release hash; delete it and run again.')
        out = work / key
        if not out.exists():
            out.mkdir()
            run([Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/tar.exe', '-xf', f, '-C', out])
    sdl_src = next((work / 'sdl3_src').glob('SDL3-*'))
    libusb = work / 'libusb_bin/VS2022/MS64/dll'
    sdl_build = work / 'sdl3_build'

    # 6. Build with MSVC (C11, Release): SDL3 with libusb, then the game.
    build = work / 'build'
    exe = build / 'dra2_recomp.exe'
    if not exe.exists() or not (sdl_build / 'SDL3.dll').exists():
        cmd = work / 'build.cmd'
        cmd.write_text('\r\n'.join([
            '@echo off', f'call "{find_vcvars()}" x64 || exit /b 1',
            # CMAKE_NINJA_CMCLDEPS_RC=OFF: CMake's rc wrapper fails (RC1107) on non-English MSVC output.
            f'cmake -S "{sdl_src}" -B "{sdl_build}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl.exe'
            ' -DCMAKE_NINJA_CMCLDEPS_RC=OFF -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF'
            f' -DSDL_STATIC=OFF -DSDL_HIDAPI_LIBUSB=ON -DLibUSB_LIBRARY="{(libusb / "libusb-1.0.lib").as_posix()}"'
            f' -DLibUSB_INCLUDE_PATH="{(work / "libusb_bin/include").as_posix()}" || exit /b 1',
            f'cmake --build "{sdl_build}" || exit /b 1',
            f'cmake -S "{project}" -B "{build}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl.exe'
            f' -DXBOXRECOMP_DIR="{kit.as_posix()}" -DSDL3_DIR="{sdl_cmake.as_posix()}" || exit /b 1',
            f'cmake --build "{build}" --parallel || exit /b 1']) + '\r\n')
        run(['cmd.exe', '/d', '/c', cmd])

    # 7. A playable copy: the executable, SDL3.dll (with libusb) and libusb-1.0.dll with their licenses,
    #    and the game data in a "game" folder beside them.
    dist = work / 'dist'
    (dist / 'licenses').mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, dist / 'DigimonRumbleArena2.exe')
    shutil.copy2(sdl_build / 'SDL3.dll', dist / 'SDL3.dll')
    shutil.copy2(libusb / 'libusb-1.0.dll', dist / 'libusb-1.0.dll')
    shutil.copy2(sdl_src / 'LICENSE.txt', dist / 'licenses/SDL3-LICENSE.txt')
    shutil.copy2(next((work / 'libusb_src').glob('libusb-*/COPYING')), dist / 'licenses/libusb-COPYING.txt')
    if not (dist / 'game' / 'default.xbe').exists():
        shutil.copytree(game, dist / 'game', dirs_exist_ok=True)
    print(f'\nDone. Play: {dist / "DigimonRumbleArena2.exe"}')

    if a.installer:
        iscc = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Inno Setup 6/ISCC.exe'
        if not iscc.exists():
            sys.exit('Inno Setup 6 is not installed; the playable copy in dist/ does not need it.')
        vcrt = next(Path(find_vcvars()).parents[2].glob('Redist/MSVC/*/x64/Microsoft.VC*.CRT/vcruntime140.dll'))
        shutil.copy2(vcrt, dist / 'vcruntime140.dll')
        (dist / 'README.txt').write_text((HERE / 'installer/README.txt').read_text(encoding='utf-8'), encoding='utf-8')
        icon = HERE / 'project/res/dra2.ico'   # private: the game's own artwork, when you add it
        run([iscc, f'/DStageDir={dist}', f'/DGameDir={dist / "game"}', f'/DVersion={a.version}',
             *([f'/DIconFile={icon}'] if icon.exists() else []), HERE / 'installer/dra2.iss'])


if __name__ == '__main__':
    main()
