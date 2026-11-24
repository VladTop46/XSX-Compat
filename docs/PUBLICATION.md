# Source publication contents

This directory is prepared as the root of the public XSX-Compat repository.
It contains the implementation and documentation selected from a larger private
development workspace. Copying the entire parent workspace is not the export
procedure.

## Included

- Current project-authored compatibility DLL source and export definitions.
- Build, deployment, sandbox, and diagnostic scripts.
- Probe source, synthetic HLSL, synthetic package layout, and synthetic service
  configuration.
- Shader-container parsers, limited instruction translator, and PE/trace tools.
- The frozen 62-check graphics source and earlier 13-check XMem source.
- Curated public status/build documentation and external dependency notes.
- An inventory of exported file sizes and SHA-256 hashes.

## Excluded

- Game executable, libraries, archives, scripts, textures, audio, video, and other
  content, whether original, extracted, decrypted, or decompressed.
- SDK/header distributions, GameOS images, native CRT/GameRuntime distributions,
  Wine/VKD3D/DXVK binaries, and other external dependency trees.
- Original or translated game shader payloads, extracted metadata blobs,
  generated shader byte arrays, native PSO captures, and storage input/output.
- Raw logs, dumps, disassembly, screenshots, caches, and temporary artifacts.
- Wine prefixes/registries, saves, local game configuration, private content
  paths, environment files, credential directories, and machine account files.
- The working agent instructions/handoff, old source backups, and private game
  import reports/generated headers.

Module names, API identifiers, diagnostic callsite RVAs, observed counts, and
inferred interface contracts remain in the authored research source and prose.
No referenced game payload is supplied by those descriptions.

## Export-only adjustments

The runtime development tree is unchanged by this export. The snapshot includes
the grouped-scatter correction observed during preparation. Further ongoing
changes in the private workspace are not synchronized automatically.

The published copy:

- Requires a build-time host login for the sandbox probe, replacing a literal
  developer login.
- Creates output directories in the graphics build script for a fresh checkout.
- Replaces the stale import-probe README with source-only usage instructions.
- Adds public documentation, ignore rules, attributes, and the audit tool.

`source-provenance.json` records relative source paths and hashes for copied
implementation files. It also identifies export adjustments. It contains no
absolute development-directory names. The main manifest covers the final
published bytes, including documentation and these adjustments.

## Inspecting the snapshot

From this repository root:

```bash
python3 tools/audit-public-tree.py --verify-manifest
git status --short
git ls-files
```

The audit checks the working tree, not merely files Git chooses to track. It
rejects unlisted files, symlinks, non-UTF-8/binary payloads, forbidden extensions,
credential-like patterns, private account paths, and manifest differences. It
prints file names and rule labels instead of matching secret values.

The `.git` directory is excluded from the artifact audit. An existing Git
history or remote configuration has its own contents and is not made safe by a
working-tree scan. This preparation does not create commits, set remotes, upload,
or rewrite history. No network publication is performed.

New local build outputs and fixtures are ignored by `.gitignore`, but an ignore
file is not a publication guarantee. Keep the public manifest explicit and
review additions before updating it. To inventory an intentionally revised
source tree:

```bash
python3 tools/audit-public-tree.py --write-manifest
python3 tools/audit-public-tree.py --verify-manifest
```

The preparation checks are recorded in [VALIDATION.md](VALIDATION.md).

The scanner is a mechanical check of the selected tree. It is not a license
grant or an automatic determination of redistribution rights. The project
license remains unspecified; external components are documented in
[THIRD_PARTY.md](../THIRD_PARTY.md).
