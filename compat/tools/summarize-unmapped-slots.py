#!/usr/bin/env python3
"""Index logged UNMAPPED observations by interface/slot/caller, never by VA alone."""
import argparse
import csv
import glob
import json
import re
from collections import defaultdict
from pathlib import Path

PATTERN = re.compile(
    r'(?P<interface>DEVICE|device|queue|list|heap|pso|allocator|fence) slot\s+'
    r'(?P<slot>\d+) \(\+0x(?P<offset>[0-9a-fA-F]+)\) UNMAPPED call#(?P<count>\d+) '
    r'(?P<arguments>rcx=.*?) caller=(?P<caller>[^\s\[]+)')

# Evidence-backed annotations only. Unidentified slots stay unidentified.
# These statuses describe the current source; old logs also contain fixed slots.
KNOWN = {
    ('device', 55): ('Xbox PSO updates', 'E_NOTIMPL/null; base PSO + tagged updates; disasm/pso-cache-and-slot55.txt'),
    ('device', 57): ('Xbox native PSO', 'E_NOTIMPL/null; IID PipelineState/out proven; disasm/slot57-output-cache.txt'),
    ('device', 61): ('Xbox sampler blob', 'implemented placeholder: zero 16-byte blob; no native sampler descriptor'),
    ('device', 62): ('Xbox native compute PSO', 'existing valid empty compute PSO; BVH does not execute'),
    ('device', 70): ('Xbox extension; semantics unresolved', 'S_OK/log; signature unresolved, outside PC Device base'),
    ('device', 74): ('Xbox heap from VA range', 'DEFAULT heap placeholder; guest memory not shared with GPU'),
    ('device', 78): ('Xbox SetFrameIntervalX', 'S_OK/no-op; local sample frame-pacing contract'),
    ('device', 79): ('Xbox ScheduleFrameEventX', 'S_OK/no-op; local sample frame-pacing contract'),
    ('device', 80): ('Xbox WaitFrameEventX', 'S_OK/log, token unwritten; local DeviceResources.cpp and disasm/present32-waitframe80.txt'),
    ('device', 81): ('Xbox GetFrameStatisticsX', 'verified call shape in FramePacing.cpp:1317; S_FALSE means unavailable, concurrent queue-info experiment unverified'),
    ('device', 86): ('Xbox native PSO', 'existing valid empty compute PSO; native game shader not executed'),
    ('device', 108): ('PC method at shifted Xbox slot', 'PC56 GetResourceAllocationInfo1-like output; adapted/tested'),
    ('device', 127): ('Xbox extension; semantics unresolved', 'existing game-specific pool hook; S_OK, exact method unidentified'),
    ('device', 133): ('Xbox placed resource from address', 'committed fallback; game memory not used'),
    ('queue', 18): ('PC method at shifted Xbox slot', 'PC16 GetTimestampFrequency; implemented, GPU probe passed'),
    ('queue', 32): ('Xbox PresentX', 'S_OK/log, no native presentation; local DeviceResources.cpp and disasm/present32-waitframe80.txt'),
    ('queue', 35): ('Xbox ExecuteCommandListsX', 'unwrap lists -> PC10 ExecuteCommandLists; implemented, GPU probe passed'),
    ('list', 52): ('Xbox operation, not PC BeginQuery', 'explicit unsupported skip; raw descriptor data; disasm/list52-incompatible.txt'),
    ('list', 70): ('PC method at shifted Xbox slot candidate', 'depth bounds candidate; game0x33390 uses FP regs, integer register values are not its arguments'),
    ('list', 71): ('Xbox extension candidate', 'S_OK/log; shared address/write operation, exact contract unresolved'),
    ('list', 72): ('Xbox timestamp/write extension candidate', 'S_OK/log; local PerformanceTimersXbox.cpp, GPU VA mapping missing'),
    ('list', 74): ('Xbox extension; semantics unresolved', 'S_OK/log; descriptor/ring setup path, no PC equivalent proven'),
    ('list', 75): ('Xbox extension or shifted view-mask method', 'S_OK/log; boolean argument from view state; exact contract unresolved'),
    ('list', 126): ('Xbox extension; semantics unresolved', 'S_OK/log; graphics state setup, not a PC core slot'),
    ('list', 131): ('Xbox extension; semantics unresolved', 'S_OK/log; observed register/ring constants, outside native list core'),
    ('list', 132): ('Xbox extension; semantics unresolved', 'S_OK/log; observed size setup, outside native list core'),
    ('list', 139): ('Xbox ResetX candidate', 'S_OK/log; (allocator,0,0) at game0x2474cc0; native reset not performed'),
    ('list', 142): ('Xbox extension; semantics unresolved', 'S_OK/log; graphics state mask, exact contract unresolved'),
    ('list', 146): ('Xbox render-target binding extension candidate', 'S_OK/log; count/views in render-target helper; native descriptor model missing'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='*', help='Input paths/globs (default: compat/logs/sandbox/game-*.log)')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    paths = sorted({Path(p) for pattern in (args.logs or ['compat/logs/sandbox/game-*.log'])
                    for p in glob.glob(pattern)}, key=lambda p: (p.stat().st_mtime_ns, str(p)))
    rows = {}
    malformed = []
    observations = 0
    for path in paths:
        for lineno, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
            if 'UNMAPPED' not in line:
                continue
            matches = list(PATTERN.finditer(line))
            if not matches:
                malformed.append({'log': str(path), 'line': lineno, 'text': line[:512]})
            for match in matches:
                fields = match.groupdict()
                interface, slot = fields['interface'].lower(), int(fields['slot'])
                offset = int(fields['offset'], 16)
                if offset != slot * 8:
                    raise ValueError(f'{path}:{lineno}: slot/offset mismatch')
                key = (interface, slot, fields['caller'])
                if key not in rows:
                    category, status = KNOWN.get((interface, slot), ('unidentified', 'S_OK/log; semantics unresolved'))
                    rows[key] = dict(interface=interface, slot=slot, offset=f'0x{offset:x}', caller=fields['caller'],
                        observations=0, max_logged_counter=0, logs=set(), category=category, status=status)
                row = rows[key]
                row['observations'] += 1
                row['max_logged_counter'] = max(row['max_logged_counter'], int(fields['count']))
                row['logs'].add(str(path))
                row.update(arguments=fields['arguments'], latest_log=str(path), latest_line=lineno)
                observations += 1
    ordered = sorted(rows.values(), key=lambda r: (r['interface'], r['slot'], r['caller']))
    callers = defaultdict(list)
    for row in ordered:
        row['logs'] = sorted(row['logs'])
        callers[row['caller']].append({'interface': row['interface'], 'slot': row['slot'],
                                      'offset': row['offset'], 'observations': row['observations']})
    report = {'scope': 'Logged observations, NOT invocation counts: logging is throttled and some lines interleave. '
                        'caller is the return address, NOT necessarily function entry. Historical and current builds are mixed.',
              'summary': {'logs': len(paths), 'observations': observations, 'slot_callers': len(ordered),
                          'interface_slots': len({(r['interface'], r['slot']) for r in ordered}),
                          'callers': len(callers), 'malformed_lines': len(malformed)},
              'rows': ordered, 'by_caller': dict(sorted(callers.items())), 'malformed': malformed}
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / 'unmapped-slots.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    fields = ['interface', 'slot', 'offset', 'caller', 'arguments', 'observations', 'max_logged_counter',
              'category', 'status', 'latest_log', 'latest_line']
    with (args.out / 'unmapped-slots.csv').open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        writer.writeheader(); writer.writerows(ordered)
    md = ['# UNMAPPED: interface × slot × caller', '', report['scope'], '',
          'Счётчики означают наблюдения в логах, а не число вызовов. '
          'По одному номеру слота семантика не назначается.', '',
          f"Logs={len(paths)}, observations={observations}, interface slots={report['summary']['interface_slots']}, "
          f"slot/callers={len(ordered)}, malformed/interleaved={len(malformed)}.", '',
          '| Interface | Slot | Offset | Caller (return RVA) | Latest arguments | Category / current status | Observations |',
          '|---|---:|---|---|---|---|---:|']
    for row in ordered:
        cells = [row['interface'], str(row['slot']), row['offset'], row['caller'], row['arguments'],
                 row['category'] + ': ' + row['status'], str(row['observations'])]
        md.append('| ' + ' | '.join(c.replace('|', '\\|') for c in cells) + ' |')
    md += ['', '## Группировка по caller', '', '| Caller | Interface.slot (observations) |', '|---|---|']
    for caller, entries in sorted(callers.items()):
        md.append('| ' + caller + ' | ' + ', '.join(f"{r['interface']}.{r['slot']} ({r['observations']})" for r in entries) + ' |')
    (args.out / 'unmapped-slots.md').write_text('\n'.join(md) + '\n')
    print(json.dumps(report['summary'], ensure_ascii=False))


if __name__ == '__main__':
    main()
