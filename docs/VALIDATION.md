# Validation of the public source snapshot

Checks below were performed against the exported source during publication
preparation on November 24, 2026. Generated test executables and DLLs were written
outside the publication directory and are not included.

| Check | Result |
|---|---|
| Synthetic shader-record parser tests | 5 passed |
| Resource-provenance tests without game fixtures | 3 passed |
| Independent synthetic PE export fixture | 5 passed |
| Native APU memory test | 70 checks, 4,000 threaded allocations, live allocations 0 |
| Python source syntax | Parsed successfully |
| Shell script syntax | `bash -n` passed |
| Markdown | Local links, README anchors, code fences, and fenced Bash syntax checked |
| Source provenance | Exported source SHA-256 hashes matched the provenance records |
| Publication audit rejection cases | Binary/NUL input, private paths, credential-shaped text, private config, and symlinks rejected |

MinGW compilation was checked for current D3D12, XMem, XCRDAPI, GameRuntime proxy,
AcpHal, XG, and GameInput DLLs; the graphics regression executable; and the
sanitized sandbox probe. The frozen graphics adapter and earlier XMem alias
source were also compiled separately.

For the Game Running checkpoint refresh, the exported D3D12, XMem, AcpHal, and
XCRDAPI adapters were compiled again, including the bundled miniz C object.
The updated XMem alias probe and storage probe also compiled. Runtime results
were read from the existing development evidence; this export did not rerun Wine.

These are compilation and source-only test results. The export preparation does
not claim a new Wine/GPU run, a new successful native FFX context, or a game
frame. The older 62/0 graphics, 13/13 XMem v1, 19-check APU, and 139-check network
results retain the build/fixture boundaries described in [STATUS.md](STATUS.md).
In particular, the current resource/presentation adapters and AcpHal FIFO do not
inherit the frozen graphics or earlier APU channel tests' runtime validation.
The newer 16/16 XMem and 20/20 storage results are recorded from the development
checkpoint, together with Game Running / frame 82 and diagnostic Present S_OK.

The final inventory is `PUBLIC_SOURCE_MANIFEST.json`. Recheck the selected tree:

```bash
python3 tools/audit-public-tree.py --verify-manifest
```

The audit excludes existing `.git` metadata. Runtime logs, game content, local
prefixes, and generated build outputs remain outside the published source tree.
