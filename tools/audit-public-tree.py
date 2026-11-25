#!/usr/bin/env python3
"""Audit the selected source tree and verify its explicit publication inventory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys


MANIFEST = 'PUBLIC_SOURCE_MANIFEST.json'
MANIFEST_SCOPE = 'Authored UTF-8 source and explicitly reviewed screenshot; excludes .git'
# The one existing visual evidence artifact was inspected; replacements require review.
REVIEWED_ASSETS = {'screenshots/player_select_first_screen.png':
                   '82f06fbb9e8096326450a4c3a565adf86db5c88738b586abdd89da213e72f803'}
ROOT_FILES = {'.gitignore', '.gitattributes', 'README.md', 'THIRD_PARTY.md',
              'source-provenance.json', MANIFEST}
SOURCE_SUFFIXES = {'.c', '.cpp', '.h', '.inc', '.def', '.py', '.sh', '.hlsl', '.xml', '.Config', '.s', '.spec', '.conf'}
BLOCKED_PARTS = {'sdk', 'PGDK-extracted', 'game_dlls', 'sandbox', 'drive_c',
                 'dosdevices', '.aws', '.codex', '.agents', '__pycache__',
                 'node_modules', 'build', 'logs', 'data', 'isa', 'shader-cache', 'analysis'}
BLOCKED_SUFFIXES = {'.exe', '.dll', '.rpf', '.xvd', '.xvc', '.xsp', '.dxbc', '.dxil',
                    '.fxcp', '.fssm', '.bin', '.blob', '.src', '.dst', '.dmp', '.log',
                    '.reg', '.pem', '.key', '.pfx', '.p12', '.zip', '.7z', '.rar',
                    '.png', '.jpg', '.jpeg', '.mp4', '.bik', '.bk2', '.pdb'}
PATTERNS = {
    'private host home path': re.compile(r'(?<![\w])/(?:home|Users)/(?!(?:game|user|host-user)(?:[/\s:"\']|$))[^/\s"\'<>]+'),
    'private mounted-media path': re.compile(r'/(?:run/media|media|mnt)/[^\s"\'<>]+'),
    'private Windows account path': re.compile(r'[A-Za-z]:\\Users\\[^\\\s]+', re.I),
    'private key material': re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----'),
    'GitHub credential shape': re.compile(r'\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{30,})\b'),
    'AWS access key shape': re.compile(r'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
    'credential-bearing URL': re.compile(r'https?://[^\s/@:]+:[^\s/@]+@', re.I),
    'assigned credential literal': re.compile(r'''(?ix)\b(?:api[_-]?key|access[_-]?token|password|client[_-]?secret)\s*[:=]\s*["'][A-Za-z0-9_+/=.-]{16,}["']'''),
    'large embedded byte array': re.compile(r'(?:0x[0-9a-fA-F]{2}\s*,\s*){256,}'),
    'large encoded payload': re.compile(r'[A-Za-z0-9+/]{4096,}={0,2}'),
}


def allowed(path):
    if path in {'native-gfx/build.sh', 'native-gfx/README.md', 'native-gfx/EXPERIMENTS.md',
                'local-agent-work/build-dll.sh', 'local-agent-work/README.md'}:
        return True
    if path in REVIEWED_ASSETS:
        return True
    if path in ROOT_FILES:
        return True
    p = Path(path)
    if path.startswith('docs/'):
        return p.suffix == '.md'
    if path in {'tools/audit-public-tree.py', 'tools/sync-from-workspace.py'}:
        return True
    if path in {'compat/runtime/xcrdapi/third_party/miniz/LICENSE',
                'compat/runtime/xcrdapi/third_party/miniz/README.txt'}:
        return True
    # compat/sandbox is source for the isolation scripts, not the root deployment.
    parts = set(p.parts)
    if path.startswith('compat/sandbox/'):
        parts.discard('sandbox')
    if parts & BLOCKED_PARTS or p.suffix.lower() in BLOCKED_SUFFIXES:
        return False
    if p.name in {'gamedata.path', 'MicrosoftGame.Config', 'game-imports.generated.h',
                  'native-vs-data.h', 'native-cs-data.h', 'target-profile.json'}:
        return False
    return (path.startswith(('compat/', 'tests/', 'native-gfx/src/', 'local-agent-work/src/d3d12_xs/')) and
            (p.suffix in SOURCE_SUFFIXES or p.name in {'README.md', 'scope.txt'}))


def inspect(root):
    files, problems = [], []
    for current, directories, names in os.walk(root, followlinks=False):
        for directory in list(directories):
            p = Path(current, directory)
            if p == root / '.git':
                directories.remove(directory)
            elif p.is_symlink():
                problems.append((p.relative_to(root).as_posix(), 'symlink directory'))
                directories.remove(directory)
        for name in names:
            p = Path(current, name)
            rel = p.relative_to(root).as_posix()
            if rel == '.git':
                continue
            if p.is_symlink():
                problems.append((rel, 'symlink file'))
                continue
            if not allowed(rel):
                problems.append((rel, 'file is outside the publication selection'))
                continue
            data = p.read_bytes()
            if rel in REVIEWED_ASSETS:
                digest = hashlib.sha256(data).hexdigest()
                if digest != REVIEWED_ASSETS[rel] or not data.startswith(b'\x89PNG\r\n\x1a\n'):
                    problems.append((rel, 'reviewed asset changed; inspect before updating its pinned hash'))
                else:
                    files.append({'path': rel, 'size': len(data), 'sha256': digest})
                continue
            if data.startswith((b'MZ', b'\x7fELF')) or b'\0' in data:
                problems.append((rel, 'binary signature or NUL byte'))
                continue
            try:
                content = data.decode('utf-8-sig')
            except UnicodeDecodeError:
                problems.append((rel, 'not UTF-8 text'))
                continue
            for label, pattern in PATTERNS.items():
                if pattern.search(content):
                    problems.append((rel, label))
            if rel != MANIFEST:
                files.append({'path': rel, 'size': len(data),
                              'sha256': hashlib.sha256(data).hexdigest()})
    return sorted(files, key=lambda f: f['path']), problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--write-manifest', action='store_true')
    action.add_argument('--verify-manifest', action='store_true')
    args = parser.parse_args()
    root = args.root.resolve()
    files, problems = inspect(root)
    if problems:
        for path, rule in problems:
            print(f'[FAIL] {path}: {rule}', file=sys.stderr)
        return 1
    target = root / MANIFEST
    if args.write_manifest:
        target.write_text(json.dumps({'schema': 1, 'scope': MANIFEST_SCOPE,
                                      'files': files}, indent=2) + '\n')
    else:
        try:
            expected = json.loads(target.read_text())
            if expected.get('schema') != 1:
                raise ValueError('unsupported manifest schema')
            recorded = expected['files']
            if recorded != files:
                before = {f['path']: f for f in recorded}
                after = {f['path']: f for f in files}
                for path in sorted(before.keys() | after.keys()):
                    if before.get(path) != after.get(path):
                        print(f'[FAIL] {path}: added, removed, or changed since inventory', file=sys.stderr)
                return 1
        except (OSError, ValueError, KeyError, TypeError):
            print('[FAIL] Missing or invalid publication manifest', file=sys.stderr)
            return 1
    print(f'[PASS] {len(files)} inventoried files (source and reviewed assets); {sum(f["size"] for f in files)} bytes; no selection/scan findings')
    print('[PASS] manifest ' + ('written' if args.write_manifest else 'verified'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
