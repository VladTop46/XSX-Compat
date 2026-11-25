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


## November26 synchronization

The current source selection includes the main runtime, separate graphics fork, native ISA probes and Wine unixlib. This refresh preserves the existing source-only publication boundary and regenerates original/export hashes and the final manifest. Current export checks are recorded below; historical GPU/Wine results above remain associated with their original builds. No original game shader payloads are included.

| Current export check | Result |
|---|---|
| Python syntax | 33 files parsed |
| Shell syntax | 38 scripts passed `bash -n` |
| Markdown | Local file links and code fences passed |
| Publication rejection cases | 8 passed: binary, private path, credential, native data, fork cache, DLL, symlink, changed reviewed screenshot |
| `provenance` | PASS |
| `records` | PASS |
| `pe-export-fixture` | PASS |
| `guest-footprint-test` | PASS |
| `gfx-buffer-descriptor-cpu` | PASS |
| `apu-memory-test` | PASS |
| `frame-clock-test` | PASS |
| `return-trace-test` | PASS |
| `xgr-proxy` | PASS |
| `xmem` | PASS |
| `gameinput` | PASS |
| `acphal` | PASS |
| `native-gfx-unixlib-build` | PASS |
| `xcrdapi` | PASS |
| `task-trace-cpu` | PASS |
| `d3d12-main` | PASS |
| `d3d12-fork` | PASS |
| `ffx-xbox3-heap-abi` | exit 8 |

All generated executables, DLLs, Unix libraries and raw logs were kept in a temporary validation directory outside this export. GPU experiments and game runs were not repeated during synchronization. The FFX row records the new smoke attempt separately; a failure before entry is not a native-context result.

The rebuilt exported FFX harness was run with the locally supplied libraries using `--xbox 3 --heap-abi`. It entered FFX, retained resource/heap/layout behavior, reached `FSR2-LUM_PYRAMID`, then returned `0x8000000D` with process exit8. This is the expected native-PSO boundary, not a successful native context. Original DLLs were not replaced.

A screenshot appeared in the public directory during synchronization. Its account-selection label, `PLAYER` and button prompts were inspected; the existing bytes were retained and allowed as one hash-pinned asset. Later private game logs were also inspected for native XMem, physical storage and refreshed vertex/index data; they are not published. They do not establish complete menu/gameplay correctness.

The fork was synchronized once more after a concurrent resource-Map edit and rebuilt successfully. The main adapter and unchanged probes retain their immediately preceding build/test results.

The final exported fork, including the default-off watchdog debugging helper, compiled successfully. Its source SHA-256 is `42696f2a206d8308baab1b73524810a7f292c78f81d9c941032b985a0fb22ae8`. The workspace comparison reported zero source differences immediately after this build.
