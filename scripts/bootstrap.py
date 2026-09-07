"""Fetch pinned build dependencies locally; no machine-wide installation."""
from pathlib import Path
import hashlib
import json
import urllib.request
import zipfile

ROOT=Path(__file__).resolve().parents[1]
ITEMS=[
('.tools/llvm.zip','.tools','https://github.com/mstorsjo/llvm-mingw/releases/download/20260826/llvm-mingw-20260826-ucrt-x86_64.zip'),
('.deps/obs.zip','.deps','https://codeload.github.com/obsproject/obs-studio/zip/refs/tags/32.0.2'),
('.deps/scs.zip','.deps/scs-sdk','https://download.eurotrucksimulator2.com/scs_sdk_1_14.zip'),
]
checksums={}
for archive, dest, url in ITEMS:
    path=ROOT/archive;path.parent.mkdir(exist_ok=True)
    if not path.exists():
        print('Downloading',url,flush=True)
        urllib.request.urlretrieve(url,path)
    checksums[archive]={'url':url,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
    if archive=='.deps/scs.zip' and checksums[archive]['sha256']!='c6c1f7376b7324994d9f9c567f3c4141fbbf305b6bf803bc4cfeef2437b2023a':
        raise SystemExit('SCS SDK checksum does not match pinned 1.14 archive')
    with zipfile.ZipFile(path) as z:
        if archive=='.deps/scs.zip':
            prefix=next(n[:-len('include/scssdk.h')] for n in z.namelist() if n.endswith('include/scssdk.h'))
            for member in z.infolist():
                name=member.filename[len(prefix):] if member.filename.startswith(prefix) else ''
                if not name or member.is_dir(): continue
                out=ROOT/dest/name
                if not out.exists():
                    out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(z.read(member))
            continue
        top=z.namelist()[0].split('/')[0]
        if not (ROOT/dest/top).exists(): z.extractall(ROOT/dest)
(ROOT/'.deps/downloads.json').write_text(json.dumps(checksums,indent=2))
print('OBS headers, LLVM toolchain and SCS SDK ready.')
