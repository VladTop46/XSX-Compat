# Source publication and workspace synchronization

This directory is a separate Git repository selected from a larger development workspace. It contains authored source and one explicitly reviewed visual evidence artifact. It is not a second full installation. The November26 synchronization brings in the runtime, separate VKD3D graphics fork, direct RDNA2 experiments and Wine unixlib without copying their private inputs or generated outputs.

## Included

- Current authored compatibility DLL source/exports, build and diagnostic scripts.
- Main `compat/d3d12_xs` and separate `local-agent-work/src/d3d12_xs` source.
- `native-gfx/src`, its build script and authored experiment notes.
- Native PM4 probe source, authored compute/fetch assembly, portable tests and parsers.
- Null audio configuration and current sandbox/deployment source.
- The previously published historical 62-check graphics and XMem v1 source checkpoints.
- The existing `screenshots/player_select_first_screen.png`, visually inspected and pinned by SHA-256 in the auditor; replacement images require review.
- Bundled miniz C/header and permission notice, public documentation, provenance and audit tools.

## Excluded

Game executables/libraries/content; SDK/header and runtime distributions; original or translated game shader payloads; extracted metadata/ISA, generated fixture headers and caches; native PSO captures; raw logs/dumps/disassembly and unreviewed screenshots; prefixes/registries/saves; machine accounts, private content paths, credentials and agent handoff; old source backups and build outputs.

API identifiers, research callsite RVAs, inferred contracts and observed counts remain in authored prose/source. They are not the referenced binary payloads.

## Repeatable synchronization

From this public root, with the separate workspace as its parent:

```bash
python3 tools/sync-from-workspace.py --workspace ..
python3 tools/sync-from-workspace.py --workspace .. --apply
python3 tools/sync-from-workspace.py --workspace .. --check
python3 tools/audit-public-tree.py --verify-manifest
```

Default is dry-run. The explicit source families and suffixes are defined in the tool. It uses existing provenance for older selected files, retains historical report source whose private originals are gone, checks for local exported-source edits before overwriting, rejects symlinks/private payloads and checks source stability before writing. New private data/capture/cache directories and source backups are excluded by selection.

The tool updates selected source, `source-provenance.json` and `PUBLIC_SOURCE_MANIFEST.json`. It does not remove unrelated files, stage/commit, change remotes, push or make any network request. Public README/status/build documents require editorial review when evidence advances; source synchronization does not invent new runtime validation.

## Export adjustments

- Keep the existing sandbox probe parameterized by `XSX_PROBE_HOST_USER`; a changed private probe requires review of that sanitization.
- Create output directories in main/fork builders for a fresh checkout.
- Prefix the historical native branch README with the current source/runtime boundary.
- Curate public build/status/publication documents independently of private handoff and raw evidence.

Every copied file has original/export SHA-256 values and adjustment notes in provenance. Paths are relative to the development root. The manifest inventories final published text, including public documentation/tools. The working development tree is unchanged by export.

## Checking the selection

```bash
python3 tools/audit-public-tree.py --verify-manifest
git diff --check
git status --short
```

The audit scans the whole working tree outside `.git`, including ignored artifacts. The exact reviewed PNG is inventoried as a binary asset; every other image remains excluded. It rejects unselected files, binaries/NULs, symlinks, private account paths, credential-shaped text, oversized encoded payloads and inventory differences. Build products are ignored by Git but still fail this source-publication audit; use a clean source checkout or build separately for publication checks.

After an intentional documentation/tool edit, regenerate the inventory explicitly with `python3 tools/audit-public-tree.py --write-manifest`, then verify it. A passing mechanical scan is not a license grant. External provenance/notices remain in [THIRD_PARTY.md](../THIRD_PARTY.md); no project-wide license has been selected.
