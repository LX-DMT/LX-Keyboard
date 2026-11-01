"""Package factory BINs from audited builds; never read or write a device."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT=Path(__file__).resolve().parents[1]
APPS={'keyboard':'lx_keyboard','knob':'lx_knob_idf','receiver':'lx_receiver'}

def digest(data):return hashlib.sha256(data).hexdigest()

def package(out):
    import esptool
    if not esptool.__version__.startswith('4.'):
        raise RuntimeError('Run with the ESP-IDF 5.5.6 Python environment (esptool 4.x)')
    archive=out.parent/(out.name+'.zip')
    if out.exists() or archive.exists():raise FileExistsError(out)
    out.mkdir(parents=True)
    manifest={'version':(ROOT/'VERSION').read_text().strip(),'idf':'5.5.6','flash_mhz':80,'purpose':'factory installation; erases configuration',
              'tool':'esptool '+esptool.__version__,'boards':{}}
    for board,name in APPS.items():
        build=ROOT/'firmware'/board/'build'
        meta=json.loads((build/'project_description.json').read_text(encoding='utf8'))
        assert meta['git_revision']=='v5.5.6' and meta['target']=='esp32s3',board
        config=json.loads((build/'flasher_args.json').read_text(encoding='utf8'))
        size='8MB' if board=='receiver' else '16MB'
        assert config['flash_settings']=={'flash_mode':'dio','flash_size':size,'flash_freq':'80m'},board
        segments=[]
        for offset,relative in sorted(config['flash_files'].items(),key=lambda p:int(p[0],0)):
            path=(build/relative).resolve()
            assert path.is_relative_to(build.resolve()) and path.is_file()
            data=path.read_bytes()
            segments.append((int(offset,0),path,data))
        app=(build/config['app']['file']).read_bytes()
        assert app[0]==0xe9 and int.from_bytes(app[12:14],'little')==9,board
        assert app[0x20+48:0x20+80].split(b'\0')[0].decode()==name,board
        assert app[0x20+112:0x20+144].split(b'\0')[0]==b'v5.5.6',board
        assert app[3]&15==15,board
        binary=out/f'{board}-factory.bin'
        cmd=[sys.executable,'-m','esptool','--chip','esp32s3','merge_bin','-o',str(binary),
             '--flash_mode','dio','--flash_freq','80m','--flash_size',size]
        for offset,path,data in segments:cmd.extend([hex(offset),str(path)])
        subprocess.run(cmd,check=True)
        merged=binary.read_bytes();cursor=0;items=[]
        for offset,path,data in segments:
            assert offset>=cursor and merged[cursor:offset]==b'\xff'*(offset-cursor)
            assert merged[offset:offset+len(data)]==data,(board,path.name)
            cursor=offset+len(data)
            items.append({'offset':hex(offset),'file':path.relative_to(build.resolve()).as_posix(),
                          'size':len(data),'sha256':digest(data)})
        assert cursor==len(merged) and len(merged)<=int(size[:-2])*1024**2
        manifest['boards'][board]={'file':binary.name,'address':'0x0','flash_size':size,
                                  'bytes':len(merged),'sha256':digest(merged),'segments':items}
    shutil.copy2(ROOT/'docs/PREBUILT_FLASH.md',out/'README.md')
    shutil.copy2(ROOT/'LICENSE',out/'LICENSE')
    shutil.copy2(ROOT/'THIRD_PARTY.md',out/'THIRD_PARTY.md')
    (out/'docs').mkdir();shutil.copy2(ROOT/'docs/LICENSING.md',out/'docs/LICENSING.md')
    (out/'pc_app').mkdir();shutil.copy2(ROOT/'pc_app/THIRD_PARTY.md',out/'pc_app/THIRD_PARTY.md')
    shutil.copytree(ROOT/'pc_app/licenses',out/'pc_app/licenses')
    for p in (ROOT/'third_party').rglob('*'):
        if p.is_file() and p.name.lower().startswith(('license','licence','copying','notice')):
            dest=out/p.relative_to(ROOT);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf8')
    files={p.relative_to(out).as_posix():digest(p.read_bytes()) for p in sorted(out.rglob('*')) if p.is_file()}
    (out/'SHA256SUMS.txt').write_text(''.join(f'{sha}  {name}\n' for name,sha in files.items()),encoding='utf8')
    with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for p in sorted(out.rglob('*')):
            if p.is_file():z.write(p,out.name+'/'+p.relative_to(out).as_posix())
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        for name,sha in files.items():assert digest(z.read(out.name+'/'+name))==sha
    archive.with_suffix('.sha256').write_text(digest(archive.read_bytes())+'  '+archive.name+'\n')
    print(archive)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True,help='New output directory; ZIP written alongside it')
    args=parser.parse_args()
    package(args.output.resolve())
