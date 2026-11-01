"""Verify portable project paths and the exact vendored dependency snapshot."""
import hashlib
import json
from pathlib import Path
import re
import csv

ROOT=Path(__file__).resolve().parents[1]
SKIP={'artifacts','build','build-idf55','managed_components','__pycache__','.git'}
ABSOLUTE=re.compile(r'(?<![\w])(?:[a-zA-Z]:[\\/]|/Users/|/home/[^/]+/)')

def verify():
    errors=[];paths=0;layouts={}
    def number(value):
        value=value.strip()
        if value[-1:].upper() in ('K','M'):
            return int(value[:-1],0)*(1024 if value[-1:].upper()=='K' else 1024*1024)
        return int(value,0)
    for board in ('keyboard','knob','receiver'):
        project=ROOT/'firmware'/board;end=0
        for row in csv.reader((project/'partitions.csv').read_text(encoding='utf8').splitlines()):
            if not row or row[0].lstrip().startswith('#'):continue
            end=max(end,number(row[3])+number(row[4]))
        config=(project/'sdkconfig.defaults').read_text(encoding='utf8')
        if not re.search(r'^CONFIG_ESPTOOLPY_FLASHFREQ_80M=y$',config,re.M):
            errors.append('Missing explicit 80 MHz Flash baseline: '+board)
        match=re.search(r'^CONFIG_ESPTOOLPY_FLASHSIZE_(\d+)MB=y$',config,re.M)
        if not match:errors.append('Missing explicit flash size: '+board);continue
        capacity=int(match[1])*1024*1024
        layouts[board]={'partition_end':hex(end),'configured_flash_mb':int(match[1]),'minimum_layout_flash_mb':(1<<(end-1).bit_length())//(1024*1024)}
        if end>capacity:errors.append('Partition exceeds configured Flash: '+board)
    for folder in ('firmware','shared','scripts','pc_app'):
        for p in (ROOT/folder).rglob('*'):
            if not p.is_file() or set(p.relative_to(ROOT).parts)&SKIP:continue
            if folder=='pc_app' and set(p.relative_to(ROOT/'pc_app').parts)&{'dist','build','.venv','venv'}:continue
            if p.name in ('dependencies.lock','sdkconfig','sdkconfig.old'):continue
            if p.name not in ('CMakeLists.txt','idf_component.yml','sdkconfig.defaults','settings.json','.clangd') and p.suffix not in ('.py','.cmake','.spec'):continue
            text=p.read_text(encoding='utf-8-sig')
            if p.name!='verify_portability.py' and ABSOLUTE.search(text):errors.append(f'Workstation path: {p.relative_to(ROOT)}')
            if p.name=='idf_component.yml':
                for value in re.findall(r'^\s+(?:override_)?path:\s*["\']?([^"\'\n]+)',text,re.M):
                    target=(p.parent/value.strip()).resolve();paths+=1
                    if not target.is_dir() or not target.is_relative_to(ROOT):errors.append(f'Missing/outside dependency: {p.relative_to(ROOT)} -> {value}')
    lock=json.loads((ROOT/'third_party.lock.json').read_text(encoding='utf8'))
    actual={p.relative_to(ROOT/'third_party').as_posix():p for p in (ROOT/'third_party').rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    expected={f'{name}/{rel}':digest for name,entry in lock['components'].items() for rel,digest in entry['files'].items()}
    expected.update(lock['root_files'])
    for rel in sorted(expected.keys()|actual.keys()):
        if rel not in actual:errors.append('Missing dependency file: '+rel)
        elif rel not in expected:errors.append('Unexpected dependency file: '+rel)
        elif hashlib.sha256(actual[rel].read_bytes()).hexdigest()!=expected[rel]:errors.append('Changed dependency file: '+rel)
    print(json.dumps({'passed':not errors,'local_dependency_paths':paths,'components':len(lock['components']),'dependency_files':len(expected),'layouts':layouts,'errors':errors},indent=2))
    return not errors

if __name__=='__main__':raise SystemExit(0 if verify() else 1)
