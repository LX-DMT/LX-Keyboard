"""Check first-party documentation, package scope and optional build provenance.

This is a structural check, not a legal audit or a guarantee that no secret exists.
No device commands are sent. Reports are written under ignored artifacts/.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import unquote

from package_source import included

ROOT = Path(__file__).resolve().parents[1]


def check(builds=False):
    errors = []
    files = [p for p in ROOT.rglob('*') if included(p)]
    for required in ('README.md', 'LICENSE', 'IDF_VERSION', 'THIRD_PARTY.md',
                     'third_party.lock.json', 'VERSION', 'pc_app/README.md'):
        if not (ROOT / required).is_file():
            errors.append('Missing required file: ' + required)
    pc = [p.relative_to(ROOT).as_posix() for p in files if p.relative_to(ROOT).parts[0] == 'pc_app']
    for name in ('main.py', 'README.md', 'requirements.txt', 'requirements-build.txt',
                 'LXKeyboardSetup.spec', 'assets/lx.ico', 'assets/tray.ico'):
        if not (ROOT / 'pc_app' / name).is_file():
            errors.append('Missing PC app file: ' + name)
    if any(Path(p).suffix in ('.exe', '.dll', '.pyc') for p in pc):
        errors.append('PC app binaries/cache must remain outside the source package')
    linked = 0
    for p in files:
        rel = p.relative_to(ROOT)
        if rel.parts[0] == 'third_party' or p.suffix != '.md':
            continue
        content = p.read_text(encoding='utf-8-sig')
        for raw in re.findall(r'\[[^\]]*\]\(([^)]+)\)', content):
            target = raw.split(' "')[0].strip('<>')
            if '://' in target or target.startswith(('#', 'mailto:')):
                continue
            target = unquote(target.split('#')[0])
            if target and not (p.parent / target).exists():
                errors.append(f'Broken local link: {rel.as_posix()} -> {target}')
            linked += 1
    oversized = [p.relative_to(ROOT).as_posix() for p in files if p.stat().st_size >= 100 * 1024**2]
    errors.extend('File exceeds GitHub 100 MiB limit: ' + p for p in oversized)
    report = {'passed': False, 'files': len(files), 'local_links_checked': linked,
              'pc_app': pc, 'largest_file_bytes': max(p.stat().st_size for p in files),
              'scope': 'local file links (not anchors/remote URLs), package scope and optional build provenance',
              'builds': {}, 'errors': errors}
    if builds:
        for board, app in (('keyboard', 'lx_keyboard'), ('knob', 'lx_knob_idf'), ('receiver', 'lx_receiver')):
            build = ROOT / 'firmware' / board / 'build'
            try:
                meta = json.loads((build / 'project_description.json').read_text(encoding='utf8'))
                sdk = Path(meta['idf_path']).resolve()
                paths = [Path(p).resolve() for p in meta['build_component_paths'] if p]
                outside = [str(p) for p in paths if not (p.is_relative_to(ROOT) or p.is_relative_to(sdk))]
                if meta['git_revision'] != 'v5.5.6' or outside:
                    errors.append('Unexpected SDK or outside component path: ' + board)
                binary = build / (app + '.bin')
                boot = build / 'bootloader/bootloader.bin'
                config = (ROOT / 'firmware' / board / 'sdkconfig').read_text(encoding='utf8')
                headers = {'bootloader': boot.read_bytes()[:4].hex(), 'application': binary.read_bytes()[:4].hex()}
                if 'CONFIG_ESPTOOLPY_FLASHFREQ_80M=y' not in config or any(bytes.fromhex(h)[3] & 15 != 15 for h in headers.values()):
                    errors.append('Generated config/image is not 80 MHz: ' + board)
                report['builds'][board] = {'sdk': meta['git_revision'], 'components': len(paths),
                    'flash_mhz': 80, 'image_headers': headers,
                    'outside_components': outside, 'application_bytes': binary.stat().st_size,
                    'application_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
            except (OSError, KeyError, ValueError) as exc:
                errors.append(f'Cannot audit {board}: {exc}')
    report['passed'] = not errors
    out = ROOT / 'artifacts/validation'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'release-check.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf8')
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return report['passed']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--builds', action='store_true', help='Also audit all three existing local builds')
    args = parser.parse_args()
    raise SystemExit(0 if check(args.builds) else 1)
