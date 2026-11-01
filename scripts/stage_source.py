"""Stage the curated source snapshot in a local Git index; never commit or push.

Force-add only enumerated third-party files: upstream ignore rules otherwise
omit required lockfile entries. Run after reviewing the release checklist.
"""
from pathlib import Path
import subprocess

from package_source import included
from verify_portability import verify

ROOT = Path(__file__).resolve().parents[1]

if __name__ == '__main__':
    if not verify():
        raise SystemExit('Dependency validation failed; nothing staged')
    if not (ROOT / '.git').exists():
        raise SystemExit('Initialize a local Git repository in this directory first')
    names = [p.relative_to(ROOT).as_posix() for p in sorted(ROOT.rglob('*')) if included(p)]
    for force in (False, True):
        subset = [p for p in names if p.startswith('third_party/') == force]
        command = ['git', '-C', str(ROOT), 'add'] + (['--force'] if force else [])
        command += ['--pathspec-from-file=-', '--pathspec-file-nul']
        if subset:
            result = subprocess.run(command, input=('\0'.join(subset) + '\0').encode('utf8'), capture_output=True)
            if result.returncode:
                raise SystemExit(result.stderr.decode('utf8', errors='replace'))
            # Refresh already staged blobs if attribute policy changed.
            result = subprocess.run(command[:command.index('add')+1] + ['--renormalize'] + command[command.index('add')+1:],
                                    input=('\0'.join(subset) + '\0').encode('utf8'), capture_output=True)
            if result.returncode:
                raise SystemExit(result.stderr.decode('utf8', errors='replace'))
    print(f'Staged {len(names)} source files locally. No commit, remote or upload performed.')
