# Selected-program VKD3D graphics fork

This directory contains the separate working graphics source from the development workspace. It retains the existing `local-agent-work/src/d3d12_xs/` layout so the integration builders and relative includes resolve without merging the fork into the main adapter.

```bash
bash local-agent-work/build-dll.sh
```

The output is `local-agent-work/build/d3d12_xs.dll`. The export only adds creation of the missing build directory. Private `data/`, translated `shader-cache/`, captures, analysis logs and built binaries are excluded.

Despite its historical `XSX_DEV_NATIVE_GFX` flag, this fork uses selected instruction-translated shaders through PC D3D12/VKD3D. Direct original-ISA execution lives separately in `native-gfx/`.

The source contains corrected pointer-first slot57 descriptions, owned cached PSOs, per-draw buffer snapshots and GPU-fence retention, descriptor provenance, selected SRV/RTV tracking, RECTLIST expansion, submit-time buffer refresh and Present inspection. Later refresh differs from the draw-time snapshot probe; see [status](../docs/STATUS.md).

To build/run integration probes, separately provide the private captures and frozen program manifest expected by `compat/build-ui-shader-probe.sh` and `compat/build-native-ui-bridge-probe.sh`. The ordinary `compat/sandbox/run-game.sh` deploys the main adapter. `compat/run-native-ui-game.sh` explicitly deploys this fork after normal dependencies.

Historical standalone512-pixel and bridge112-check passes do not establish a correct current game image. The inspected game path created42 selected PSOs and reached real draws; black output and a later watchdog remain unresolved.
