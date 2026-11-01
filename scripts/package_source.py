"""Create a clean portable source ZIP, never include build/device backups."""
import datetime
import hashlib
import json
from pathlib import Path
import zipfile
from verify_portability import ROOT, verify

def included(p):
    rel=p.relative_to(ROOT)
    if rel.parts[0]=='pc_app' and any(n in {'dist','build','.venv','venv'} for n in rel.parts[1:-1]):return False
    if any(n in {'artifacts','managed_components','__pycache__','.git'} for n in rel.parts[:-1]):return False
    if rel.parts[0]!='third_party' and any(n in {'.vscode','.idea'} for n in rel.parts[:-1]):return False
    if rel.parts[0]!='third_party' and any(n=='build' or n.startswith('build-') for n in rel.parts[:-1]):return False
    if p.name in ('sdkconfig','sdkconfig.old','dependencies.lock','Thumbs.db','.DS_Store'):return False
    if rel.parts[0]!='third_party' and (p.name=='.env' or p.name.startswith('.env.') or p.suffix in ('.pyc','.log','.bak')):return False
    return p.is_file()

if __name__=='__main__':
    if not verify():raise SystemExit('Dependency/path validation failed')
    out=ROOT/'artifacts/releases';out.mkdir(parents=True,exist_ok=True)
    archive=out/f'lx-keyboard-firmware-idf556-source-{datetime.datetime.now():%Y%m%d-%H%M%S}.zip'
    manifest={}
    with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=5) as z:
        for p in sorted(ROOT.rglob('*')):
            if not included(p):continue
            name='lx-keyboard-firmware/'+p.relative_to(ROOT).as_posix();data=p.read_bytes()
            manifest[name]=hashlib.sha256(data).hexdigest();z.writestr(name,data)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        for name,digest in manifest.items():assert hashlib.sha256(z.read(name)).hexdigest()==digest,name
    archive.with_suffix('.json').write_text(json.dumps({'files':manifest,'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'verified':True},indent=2),encoding='utf8')
    print(archive)
