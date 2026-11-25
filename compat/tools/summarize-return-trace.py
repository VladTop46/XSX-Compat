#!/usr/bin/env python3
"""Group observed return failures; an expected capability probe is not a crash cause."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re


def inspect(log):
    events, snapshots, faults, malformed = [], [], [], []
    for number, line in enumerate(log.read_text(errors='replace').splitlines(), 1):
        marker = re.search(r'\[D3D12_XS\] RET (HRESULT_FAILURE|NULL_OUTPUT|UNREADABLE_OUTPUT|NULL_RETURN|RETURN) ', line)
        if marker:
            fields = dict(re.findall(r'(\w+)=([^\s]+)', line[marker.end():]))
            required = {'call', 'end', 'failure', 'tick', 'tid', 'slot', 'method', 'hr', 'caller'}
            kind = re.search(r' tid=\d+ (\w+) slot=', line)
            if not required <= fields.keys() or not kind:
                malformed.append({'line': number, 'text': line.strip()})
                continue
            fields.update(finding=marker[1], line=number)
            try:
                for field in ('call', 'end', 'failure', 'tick', 'tid', 'slot'):
                    fields[field] = int(fields[field])
            except ValueError:
                malformed.append({'line': number, 'text': line.strip()})
                continue
            fields['kind'] = kind[1]
            events.append(fields)
        elif '[D3D12_XS] RET SNAPSHOT' in line:
            snapshots.append({'line': number, 'text': line.strip()})
        if '[D3D12_XS] FIRST-CHANCE' in line:
            faults.append({'line': number, 'text': line.strip()})
    failures = sorted((e for e in events if e.get('failure', 0)), key=lambda e: e['failure'])
    counts = Counter((e['kind'], e.get('slot'), e.get('method'), e.get('hr'),
                      e.get('caller'), e['finding']) for e in failures)
    groups = [dict(kind=k[0], slot=k[1], method=k[2], hr=k[3], caller=k[4], finding=k[5], count=n)
              for k, n in counts.most_common()]
    return {'log': log.name, 'observed_returns': len(events), 'failures': failures,
            'groups': groups, 'snapshots': snapshots, 'faults': faults, 'malformed': malformed,
            'scope': 'Typed guest-facing D3D12 returns; void/unresolved contracts excluded; first failure is not proof of causality.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.log)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / 'return-trace.json').write_text(json.dumps(report, indent=2) + '\n')
    rows = ['# Observed D3D12 return failures', '', report['scope'], '',
            f"Log: `{report['log']}`; failures: {len(report['failures'])}.", '',
            '| Kind / slot | Method | Finding / HRESULT | Count | Caller |',
            '|---|---|---|---:|---|']
    for e in report['groups']:
        rows.append(f"| {e['kind']} / {e['slot']} | {e['method']} | {e['finding']} / {e['hr']} | {e['count']} | `{e['caller']}` |")
    rows.extend(['', '## First observed failures', ''])
    for e in report['failures'][:12]:
        rows.append(f"- Failure {e['failure']}, line {e['line']}: {e['kind']}[{e['slot']}] {e['method']} → {e['finding']} {e['hr']}; caller `{e['caller']}`.")
    rows.extend(['', '## Fault markers', ''])
    rows.extend(f"- Line {e['line']}: `{e['text']}`" for e in report['faults'])
    rows.extend(['', '## Runtime counters', ''])
    rows.extend(f"- Line {e['line']}: `{e['text']}`" for e in report['snapshots'])
    rows.extend(['', f"Damaged/interleaved return lines: {len(report['malformed'])}. Snapshot counters can exceed parsed records when Wine output splits a line."])
    (args.out / 'return-trace.md').write_text('\n'.join(rows) + '\n')
    print(f"Observed returns={report['observed_returns']} failures={len(report['failures'])} groups={len(report['groups'])}")
    if not report['snapshots']:
        print('No trace snapshot: verify the deployed DLL and XSX_TRACE_RETURNS forwarding.')


if __name__ == '__main__':
    main()
