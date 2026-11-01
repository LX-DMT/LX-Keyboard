"""Check standalone project boundaries, defaults and retained source hashes."""
import csv
import hashlib
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]
BOARDS=('keyboard','knob','receiver')
SKIP={'artifacts','build','managed_components','__pycache__','.git','.venv','venv','dist'}
ABSOLUTE=re.compile(r'(?<![\w])(?:[a-zA-Z]:[\\/]|/Users/|/home/[^/]+/)')

def number(value):
    value=value.strip()
    if value[-1:].upper() in ('K','M'):
        return int(value[:-1],0)*(1024 if value[-1:].upper()=='K' else 1024*1024)
    return int(value,0)

def verify():
    errors=[];layouts={}
    for board in BOARDS:
        project=ROOT/board
        for required in ('CMakeLists.txt','sdkconfig.defaults','partitions.csv','main/idf_component.yml','dependencies.lock','.vscode/extensions.json','LICENSE','README.md'):
            if not (project/required).is_file():errors.append(f'Missing standalone file: {board}/{required}')
        end=0
        for row in csv.reader((project/'partitions.csv').read_text(encoding='utf8').splitlines()):
            if not row or row[0].lstrip().startswith('#'):continue
            end=max(end,number(row[3])+number(row[4]))
        config=(project/'sdkconfig.defaults').read_text(encoding='utf8')
        for expected in ('CONFIG_ESPTOOLPY_FLASHFREQ_80M=y','CONFIG_IDF_TARGET="esp32s3"'):
            if expected not in config:errors.append(f'Missing {expected}: {board}')
        match=re.search(r'^CONFIG_ESPTOOLPY_FLASHSIZE_(\d+)MB=y$',config,re.M)
        capacity=int(match[1])*1024*1024 if match else 0
        if end>capacity:errors.append('Partition exceeds configured Flash: '+board)
        layouts[board]={'partition_end':hex(end),'flash_mib':capacity//1024**2}
        for p in project.rglob('*'):
            if not p.is_file() or any(x in SKIP or x.startswith('build-') for x in p.relative_to(project).parts):continue
            if p.name not in ('CMakeLists.txt','idf_component.yml','dependencies.lock','sdkconfig.defaults','settings.json'):continue
            text=p.read_text(encoding='utf-8-sig')
            if ABSOLUTE.search(text):errors.append('Workstation path: '+str(p.relative_to(ROOT)))
            if p.name=='idf_component.yml':
                for value in re.findall(r'^\s+(?:override_)?path:\s*["\']?([^"\'\n]+)',text,re.M):
                    target=(p.parent/value.strip()).resolve()
                    if not target.is_dir() or not target.is_relative_to(project.resolve()):errors.append('Dependency outside standalone project: '+str(p.relative_to(ROOT)))
            if p.name=='CMakeLists.txt' and re.search(r'\.\./(?:\.\./)?(?:shared|third_party|keyboard|knob|receiver|components)',text):
                errors.append('Cross-project CMake path: '+str(p.relative_to(ROOT)))
    lock=json.loads((ROOT/'vendored.lock.json').read_text(encoding='utf8'))['files']
    for name,digest in lock.items():
        p=ROOT/name
        if not p.is_file() or hashlib.sha256(p.read_bytes()).hexdigest()!=digest:errors.append('Changed retained dependency: '+name)
    left=ROOT/'keyboard/components';right=ROOT/'receiver/components'
    for name in ('bringup_usb','hid_mouse','input_core','network_link','radio_keys','protocol'):
        a={p.relative_to(left/name).as_posix():p.read_bytes() for p in (left/name).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
        b={p.relative_to(right/name).as_posix():p.read_bytes() for p in (right/name).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
        if a!=b:errors.append('Divergent shared implementation: '+name)
    if (left/'protocol/lx_protocol.py').read_bytes()!=(ROOT/'pc_app/lx_protocol.py').read_bytes():errors.append('PC protocol differs from firmware protocol')
    print(json.dumps({'passed':not errors,'layouts':layouts,'retained_files':len(lock),'errors':errors},ensure_ascii=False,indent=2))
    return not errors

if __name__=='__main__':raise SystemExit(0 if verify() else 1)
