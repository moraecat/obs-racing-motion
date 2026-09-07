"""Developer-only build; delivered OBS/game DLLs do not require Python."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / '.tools/llvm-mingw-20260826-ucrt-x86_64/bin'
OBS = ROOT / '.deps/obs-studio-32.0.2'
BUILD = ROOT / 'build'

def run(args):
    result = subprocess.run([str(x) for x in args], cwd=ROOT, text=True)
    if result.returncode:
        raise SystemExit(result.returncode)

def compiler():
    override = os.environ.get('RACING_CXX')
    return Path(override) if override else TOOLS / 'clang++.exe'

def flags():
    return [compiler(), '-std=c++17', '-O2', '-g', '-Wall', '-Wextra',
            '-Werror=return-type', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
            '-D_WIN32_WINNT=0x0A00', '-Isrc', '-static']

def import_lib(obs_bin):
    output = subprocess.check_output([str(TOOLS/'llvm-readobj.exe'), '--coff-exports', str(obs_bin/'obs.dll')], text=True)
    names = re.findall(r'^\s+Name: (\S+)', output, re.M)
    if not names:
        raise SystemExit('No OBS exports found')
    definition = BUILD / 'obs.def'
    definition.write_text('LIBRARY obs.dll\nEXPORTS\n'+'\n'.join(names)+'\n')
    run([TOOLS/'llvm-dlltool.exe', '-m', 'i386:x86-64', '-d', definition, '-l', BUILD/'libobs.a'])

def obs_flags():
    return [f'-I{OBS / "libobs"}', f'-I{BUILD}']

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--test', nargs='?', const='all', choices=['all','motion','telemetry','receiver','truck','monitor'])
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--obs-bin', type=Path, default=Path('C:/Program Files/obs-studio/bin/64bit'))
    args = parser.parse_args()
    BUILD.mkdir(exist_ok=True)
    if not compiler().exists():
        raise SystemExit('Run scripts/bootstrap.py first (or set RACING_CXX).')
    if args.test:
        cases = ['telemetry','motion','receiver','truck','monitor'] if args.test=='all' else [args.test]
        for case in cases:
            source = ROOT/f'tests/{case}_test.cpp'
            extra = {'motion':['src/motion.cpp'], 'telemetry':['src/telemetry.cpp'],
                     'receiver':['src/telemetry.cpp','src/receiver.cpp'],
                     'truck':[], 'monitor':['src/monitor_data.cpp']}[case]
            more = [f'-I{ROOT / ".deps/scs-sdk/include"}'] if case=='truck' else []
            if case=='truck':
                run(flags()+more+['-shared','truck/truck_telemetry.cpp','truck/truck_telemetry.def','-o',BUILD/'racing-motion-scs.dll'])
            run(flags()+more+[source]+extra+['-lws2_32','-o',BUILD/f'{case}_test.exe'])
            run([BUILD/f'{case}_test.exe']+([BUILD/'racing-motion-scs.dll'] if case=='truck' else []))
        return
    (BUILD/'obsconfig.h').write_text('#pragma once\n#define OBS_DATA_PATH "../../data"\n#define OBS_INSTALL_PREFIX ""\n#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n')
    import_lib(args.obs_bin)
    sources = ['src/plugin.cpp','src/motion.cpp','src/telemetry.cpp','src/receiver.cpp','src/monitor_data.cpp','src/telemetry_monitor.cpp']
    run(flags()+obs_flags()+['-shared']+sources+[BUILD/'libobs.a','-lws2_32','-luser32','-lgdi32','-o',BUILD/'obs-racing-motion.dll'])
    if args.smoke:
        run(flags()+obs_flags()+['tests/obs_smoke.cpp',BUILD/'libobs.a','-lws2_32','-o',BUILD/'obs_smoke.exe'])
        env = os.environ.copy()
        env['PATH'] = str(args.obs_bin)+os.pathsep+env.get('PATH','')
        result = subprocess.run([str(BUILD/'obs_smoke.exe'),str(BUILD/'obs-racing-motion.dll'),str(ROOT/'data'),str(args.obs_bin)],cwd=ROOT,env=env)
        raise SystemExit(result.returncode)
    truck = ROOT/'truck/truck_telemetry.cpp'
    if truck.exists():
        run(flags()+[f'-I{ROOT/".deps/scs-sdk/include"}', '-shared', truck,'truck/truck_telemetry.def','-o',BUILD/'racing-motion-scs.dll'])
    dist = ROOT/'dist'
    dist.mkdir(exist_ok=True)
    # Only remove generated product directories, after resolving containment.
    for name in ['obs-racing-motion','truck']:
        generated=(dist/name).resolve()
        if generated.parent != dist.resolve(): raise SystemExit('Unsafe staging path')
        if generated.exists(): shutil.rmtree(generated)
    stage = ROOT/'dist/obs-racing-motion'
    dll_dir = stage/'bin/64bit'
    dll_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(BUILD/'obs-racing-motion.dll', dll_dir)
    shutil.copytree(ROOT/'data',stage/'data',dirs_exist_ok=True)
    (ROOT/'dist/truck').mkdir(exist_ok=True)
    if (BUILD/'racing-motion-scs.dll').exists():
        shutil.copy2(BUILD/'racing-motion-scs.dll',ROOT/'dist/truck')
    for file in ['truck/SCS-SDK-LICENSE.txt','docs/truck-setup.md']:
        if (ROOT/file).exists(): shutil.copy2(ROOT/file,ROOT/'dist/truck')
    for src,dst in [(ROOT/'README.md',ROOT/'dist/README.md'),(ROOT/'LICENSE',ROOT/'dist/LICENSE'),(ROOT/'scripts/install.ps1',ROOT/'dist/install.ps1')]:
        if src.exists(): shutil.copy2(src,dst)
    manifest = {str(p.relative_to(ROOT/'dist')):hashlib.sha256(p.read_bytes()).hexdigest()
                for p in (ROOT/'dist').rglob('*') if p.is_file() and p.suffix not in ['.zip'] and p.name!='SHA256SUMS.json'
                and (p.relative_to(dist).parts[0] in ['obs-racing-motion','truck','README.md','LICENSE','install.ps1'])}
    (ROOT/'dist/SHA256SUMS.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    archive = ROOT/'dist/OBS-Racing-Motion-Windows-x64.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for p in (ROOT/'dist').rglob('*'):
            relative=str(p.relative_to(dist))
            if p.is_file() and (relative in manifest or p.name=='SHA256SUMS.json'):
                z.write(p,p.relative_to(dist))
    print(f'Built {archive}')

if __name__=='__main__': main()
