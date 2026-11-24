# Import audit probe

This tool reads a locally supplied LLVM COFF report, generates an import list,
then builds a probe that loads libraries and resolves the listed exports.

```bash
bash tests/05-game/build.sh /path/to/local/exe-report.txt
XSX_GRAPHICS=1 WINEDEBUG=-all ./compat/run-test.sh \
  tests/05-game/game-import-probe.exe --dll-dir 'tests\03-fidelityfx'
```

The input report, generated target profile/header, executable, and runtime DLLs
are local files excluded from this source publication. Exit 0 establishes export
availability for the selected inputs. It does not validate function ABI or game
startup. DLL loading can execute library initializers.

Use `--module xmem.dll` to select one module. The historical target audit counted
68 import descriptors and 587 symbols; these are observations about that input,
not a hardcoded compatibility guarantee.
