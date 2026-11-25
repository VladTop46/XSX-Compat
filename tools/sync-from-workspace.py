#!/usr/bin/env python3
"""Compare or refresh the public source selection from a separate workspace.

Default: dry run. --apply updates selected sources, provenance and inventory.
No game inputs, generated payloads, Git operations or network calls are copied.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import sys

# Dry-run/check must not create import caches in the source selection.
sys.dont_write_bytecode = True

SOURCE_SUFFIXES = {'.c', '.cpp', '.h', '.inc', '.def', '.py', '.sh', '.s', '.spec', '.conf'}
FAMILIES = ('compat/d3d12_xs', 'compat/runtime', 'compat/stubs', 'compat/tests',
            'compat/tools', 'compat/sandbox', 'local-agent-work/src/d3d12_xs', 'native-gfx/src')
BLOCKED = {'__pycache__', 'build', 'logs', 'data', 'isa', 'shader-cache', 'analysis', '.git'}
EXTRA = ('local-agent-work/build-dll.sh', 'native-gfx/build.sh', 'native-gfx/README.md',
         'native-gfx/EXPERIMENTS.md', 'compat/tests/native-isa/README.md')
NATIVE_NOTE = '> Current integration audit, November 26, 2026: game/UI draws still use selected\n> translated DXIL through our d3d12_xs adapter fork and VKD3D-Proton.\n> Native AMDGPU BO-backed XMem is exercised by game runs. Original Xbox ISA\n> executes directly in separate Linux/Wine probes; live game command-list\n> integration is M3. The statement below about VKD3D serving only display is\n> the intended architecture, not the current game renderer.\n> See [verified paths](../docs/RENDERING_PATHS.md) and [current status](../docs/STATUS.md).\n\n'

def sha(data):
    return hashlib.sha256(data).hexdigest()

def safe(root, rel):
    q=Path(rel)
    if q.is_absolute() or '..' in q.parts:
        raise ValueError('unsafe source path')
    p=root
    for part in q.parts:
        p=p/part
        if p.is_symlink():
            raise ValueError(f'{rel}: symlink input is excluded')
    return p

def adjusted(rel, data, previous, exported):
    notes=[]
    if rel=='compat/tests/sandbox-probe.cpp':
        # Retain the reviewed, parameterized export while its original is unchanged.
        # A changed private probe requires a fresh review of that adjustment.
        if not previous or sha(data)!=previous['source_sha256'] or sha(exported)!=previous['export_sha256']:
            raise ValueError(f'{rel}: sanitized probe requires manual review')
        return exported, previous['adjustments']
    if rel=='compat/build-d3d12_xs.sh':
        text=data.decode(); marker='cd "$(dirname "$0")"\n'
        if 'mkdir -p build ../tests/03-fidelityfx' not in text:
            if marker not in text: raise ValueError('graphics builder changed; review directory adjustment')
            text=text.replace(marker,marker+'mkdir -p build ../tests/03-fidelityfx\n',1)
            notes=['Create build output directories for a fresh source checkout']
        data=text.encode()
    if rel=='local-agent-work/build-dll.sh':
        text=data.decode(); marker='cd "$(dirname "$0")"\n'
        if 'mkdir -p build' not in text:
            if marker not in text: raise ValueError('fork builder changed; review directory adjustment')
            text=text.replace(marker,marker+'mkdir -p build\n',1)
            notes=['Create the isolated fork build directory for a fresh source checkout']
        data=text.encode()
    if rel=='native-gfx/README.md':
        data=(NATIVE_NOTE+data.decode()).encode()
        notes=['Prefix the historical roadmap with current source/runtime evidence boundaries']
    return data, notes

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--workspace',type=Path,help='Separate development root (default: parent of public checkout)')
    ap.add_argument('--snapshot-date',default=datetime.date.today().isoformat())
    mode=ap.add_mutually_exclusive_group(); mode.add_argument('--apply',action='store_true');mode.add_argument('--check',action='store_true')
    args=ap.parse_args(); pub=Path(__file__).resolve().parents[1]
    workspace=(args.workspace or pub.parent).resolve()
    if workspace==pub or not (workspace/'XSX_COMPAT_HANDOFF.md').is_file():
        ap.error('workspace must be the separate development root containing XSX_COMPAT_HANDOFF.md')
    previous=json.loads((pub/'source-provenance.json').read_text())
    old={row['path']:row for row in previous['files']}
    selected=set(old)
    for base in FAMILIES:
        directory=safe(workspace,base)
        if not directory.is_dir(): raise ValueError(f'{base}: missing source family')
        for f in directory.rglob('*'):
            rel=f.relative_to(workspace)
            if set(rel.parts)&BLOCKED or any(x.startswith('prefix') for x in rel.parts):continue
            if f.is_file() and f.suffix in SOURCE_SUFFIXES:selected.add(rel.as_posix())
    selected.update(f.relative_to(workspace).as_posix() for f in (workspace/'compat').glob('*.sh'))
    selected.update(EXTRA)
    spec=importlib.util.spec_from_file_location('public_audit',pub/'tools/audit-public-tree.py')
    audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
    pending={}; rows=[]; changed=[]; preserved=[]
    for rel in sorted(selected):
        src=safe(workspace,rel); dst=safe(pub,rel); prior=old.get(rel)
        current=dst.read_bytes() if dst.is_file() else b''
        if prior and sha(current)!=prior['export_sha256']:
            raise ValueError(f'{rel}: local exported source changed; review before synchronization')
        if not src.is_file():
            if prior and rel.startswith('compat/reports/'):
                rows.append(dict(prior,preserved_historical=True));preserved.append(rel);continue
            raise ValueError(f'{rel}: missing original source')
        raw=src.read_bytes(); data, notes=adjusted(rel,raw,prior,current)
        if not audit.allowed(rel): raise ValueError(f'{rel}: outside publication selection')
        content=data.decode('utf-8-sig')
        if data.startswith((b'MZ',b'\x7fELF')) or b'\0' in data:raise ValueError(f'{rel}: binary input')
        for label, pattern in audit.PATTERNS.items():
            if pattern.search(content):raise ValueError(f'{rel}: {label}')
        rows.append({'path':rel,'source_sha256':sha(raw),'export_sha256':sha(data),'adjustments':notes})
        if data!=current:
            changed.append(('update' if dst.exists() else 'add',rel))
            pending[rel]=(data,0o755 if src.stat().st_mode&0o111 else 0o644)
    for action,rel in changed:print(f'[{action}] {rel}')
    print(f'[summary] sources={len(rows)} changed={len(changed)} retained_historical={len(preserved)}')
    # Refuse to silently mix file revisions if development is still active.
    for row in rows:
        if row.get('preserved_historical'):continue
        if sha(safe(workspace,row['path']).read_bytes())!=row['source_sha256']:
            raise ValueError(f'{row["path"]}: source changed during snapshot; retry')
    if args.check:return int(bool(changed))
    if not args.apply:
        print('[dry-run] No files changed; use --apply after reviewing the selection.');return 0
    for rel,(data,mode) in pending.items():
        dst=safe(pub,rel);dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(data);dst.chmod(mode)
    provenance={'schema':1,'snapshot_date':args.snapshot_date,
                'scope':'Selected authored source; paths relative to the separate development root; no binary inputs included',
                'snapshot_note':'Runtime, VKD3D graphics fork, direct RDNA2 experiments and Wine unixlib. Historical results belong to their original builds; M2 source is experimental. Game payloads and raw evidence stay private.',
                'files':rows}
    (pub/'source-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
    files,problems=audit.inspect(pub)
    if problems:raise ValueError('publication audit failed: '+repr(problems))
    (pub/audit.MANIFEST).write_text(json.dumps({'schema':1,'scope':audit.MANIFEST_SCOPE,'files':files},indent=2)+'\n')
    print(f'[PASS] synchronized; inventory={len(files)} files; Git index/history/remotes untouched')
    return 0

if __name__=='__main__':
    try:raise SystemExit(main())
    except (OSError,ValueError,KeyError) as error:
        print(f'[FAIL] {error}',file=sys.stderr);raise SystemExit(1)
