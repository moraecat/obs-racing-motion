"""Assemble a portable, versioned release from validated build outputs."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
import zipfile

ROOT=Path(__file__).resolve().parents[1]
TOOLS=ROOT/'.tools/llvm-mingw-20260826-ucrt-x86_64'

def package():
    version=(ROOT/'VERSION').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+',version):
        raise ValueError('Invalid VERSION')
    if f'({version})' not in (ROOT/'src/plugin.cpp').read_text(encoding='utf-8'):
        raise ValueError('VERSION and plugin log version differ')
    release=ROOT/'release';release.mkdir(exist_ok=True)
    source_name=f'OBS-Racing-Motion-{version}-Source.zip'
    # Only the named source roots are distributed; no git state, downloads,
    # user-specific progress logs, build artifacts or other workspace files.
    source_files=[ROOT/p for p in ['.gitignore','VERSION','LICENSE','README.md']]
    for folder in ['src','data','truck','scripts','tests']:
        source_files += [p for p in (ROOT/folder).rglob('*') if p.is_file()
                         and '__pycache__' not in p.parts and p.suffix!='.pyc']
    source_files += [ROOT/'docs/truck-setup.md']
    source_files += [p for p in (ROOT/'docs/release').rglob('*') if p.is_file()]
    with zipfile.ZipFile(release/source_name,'w',zipfile.ZIP_DEFLATED) as z:
        for p in sorted(source_files):z.write(p,p.relative_to(ROOT).as_posix())
    with tempfile.TemporaryDirectory(prefix='release-stage-',dir=ROOT/'build') as temp:
        stage=Path(temp)
        def copy(src,dest):
            target=stage/dest;target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(ROOT/src,target)
        copy('build/obs-racing-motion.dll','obs-racing-motion/bin/64bit/obs-racing-motion.dll')
        shutil.copytree(ROOT/'data',stage/'obs-racing-motion/data')
        copy('build/racing-motion-scs.dll','truck/racing-motion-scs.dll')
        for dll in [stage/'obs-racing-motion/bin/64bit/obs-racing-motion.dll',stage/'truck/racing-motion-scs.dll']:
            subprocess.run([str(TOOLS/'bin/llvm-strip.exe'),'--strip-debug',str(dll)],check=True)
        copy('truck/SCS-SDK-LICENSE.txt','truck/SCS-SDK-LICENSE.txt')
        copy('docs/truck-setup.md','truck/truck-setup.md')
        for name in ['VERSION','LICENSE','README.md']:copy(name,name)
        for name in ['START-HERE.txt','CHANGELOG.md','THIRD-PARTY-NOTICES.txt']:
            copy('docs/release/'+name,name)
        for name in ['install.ps1','install-game.ps1']:copy('scripts/'+name,name)
        for game in ['OBS','ETS2','ATS']:
            command='install.ps1' if game=='OBS' else 'install-game.ps1'
            argument='' if game=='OBS' else ' -Game '+game
            script=('@echo off\nsetlocal\n'
                    f'powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0{command}"{argument}\n'
                    'set "result=%errorlevel%"\n'
                    'if not "%result%"=="0" echo Installation did not complete. Read the error above.\n'
                    'echo See START-HERE.txt for setup and permissions help.\npause\nexit /b %result%\n')
            (stage/f'Install-{game}.cmd').write_bytes(script.replace('\n','\r\n').encode('ascii'))
        licenses={
            TOOLS/'LICENSE.TXT':'LLVM.txt',
            TOOLS/'x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64.txt':'MinGW.txt',
            TOOLS/'x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt':'MinGW-runtime.txt',
            TOOLS/'x86_64-w64-mingw32/share/mingw32/COPYING.winpthreads.txt':'winpthreads.txt',
            TOOLS/'x86_64-w64-mingw32/share/mingw32/COPYING.winstorecompat.txt':'winstorecompat.txt',
            ROOT/'.deps/obs-studio-32.0.2/COPYING':'OBS.txt',
        }
        for path,name in licenses.items():copy(path,'licenses/'+name)
        copy(release/source_name,'source/'+source_name)
        manifest={p.relative_to(stage).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(stage.rglob('*')) if p.is_file()}
        (stage/'SHA256SUMS.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
        archive=release/f'OBS-Racing-Motion-{version}-Windows-x64.zip'
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
            for p in sorted(stage.rglob('*')):
                if p.is_file():z.write(p,p.relative_to(stage).as_posix())
    for archive in [archive,release/source_name]:
        digest=hashlib.sha256(archive.read_bytes()).hexdigest()
        archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n',encoding='ascii')
    print(f'Packaged {release}')

if __name__=='__main__':package()
