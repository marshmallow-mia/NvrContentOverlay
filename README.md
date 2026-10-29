# NvrContentOverlay

A nEVR plugin that serves a **content pack**'s game data in place of the stock files
while Echo VR runs. No game file is changed: with the plugin off (or not listed), the
game is stock.

It is the service behind mods that add or grow resources: new gear, new scripts, larger
tables. NvrAssetPatches can't do that, since it only overwrites a resource the game
already loads, in place, at its size or smaller. The first pack is EchoCombat
(github.com/marshmallow-mia/EchoCombat, `evrtool pack`).

## What it does

With `{"active": "<id>"}` it reads `<root>/content/<id>/content.json`, checks the pack
and the stock files, then:

| The game opens | It gets |
|---|---|
| `<data dir>/manifests/<pkg>` | the pack's manifest (the stock one with the pack's resources added) |
| `<data dir>/packages/<pkg>_<n>`, n ≥ the stock package count | the pack's package |
| a script through its module loader (`echovr.exe+0xEB0F0`): `bin/win10/scripts/<hash>.dll` | the pack's DLL, when it has one |

Echo runs script code from `bin/win10/scripts`, not from the package, so a pack carries
its scripts as DLLs. Opens for writing and every other file are left alone. The `\\?\`
form the game uses and `/` separators are matched too.

## When it does nothing

It refuses the pack, logs why and returns nonzero from init, so nEVR unloads it and the
game runs stock, when:

- **a stock manifest isn't the one the pack was built from** (its sha256 is in
  content.json). For example, an old installer repacked the game: restore the stock
  files first.
- **a pack file is missing or changed**: every file is listed with its sha256 and checked
  at start.
- **content.json is malformed**: a path that is absolute, climbs out (`..`) or names
  anything but plain files; a package below the stock count, which would shadow a stock
  package; a script not named `scripts/<16 hex>.dll`; an id that isn't `[a-z0-9._-]`.
- **it was loaded too late**: the game has opened its data already. It holds its packages
  open from the moment it reads the manifests, so a stock package that can't be opened
  without sharing is the sign. Serving then would hand the game the pack's scripts with
  stock data.
- **the pack has scripts and this isn't echovr.exe 631547**: PE timestamp `0x6452DFF6`
  and the loader's first 16 bytes are checked. A pack without scripts works on any build
  whose stock manifests match.

## Loading it early: nEVR's `early: true`

Echo reads its manifests while the first command-line pass is still running, before
nEVR's normal plugin load (after the graphics device on a client). The plugin has to be
listed with `early: true`, which needs a nEVR with the early load pass
(marshmallow-mia/nevr-runtime `feature/early-plugins`). Without it, the plugin loads too
late and refuses (see above).

```yaml
plugins:
  - name: "NvrContentOverlay"
    file: "NvrContentOverlay.dll"
    early: true
    args: {"active": "echocombat"}
```

| Arg | |
|---|---|
| `active` | the pack to serve (`<root>/content/<active>/`). Absent: nothing to do |
| `logging` | `verbose`: also log every file the game opens and every module it loads (the first 4000), as a probe |

The plugin writes no log file of its own. Its lines go to nEVR's run log, as
`[NEVR.PLUGIN] plugin=NvrContentOverlay <message>`. It has no `log_dir` arg. It logs at Warning when it refuses or
degrades (a refused pack, a failed hook), at Info for the pack it checks and the state it ends in,
and at Debug for the first 50 opens and scripts it serves (all of them with `logging: verbose`)
and for each open or module it probes. Its lines reach the host's log when the context the
host passes carries the log function (its `ctx_size` covers `ctx->log`: plugin API 6 or newer);
from an older host they go to stderr as `<prefix> <LEVEL> <message>`, which that host does not
collect. The plugin reports the `NEVR_PLUGIN_API_VERSION` of the SDK headers it is built with (8);
a host older than that warns and loads it anyway. It is built with `-DNEVR_SDK_INCLUDE=<nevr-runtime>/sdk/include`.

`<root>` is the folder above the `plugins` folder the plugin was loaded from. From nEVR 6.0 on,
plugins load from nEVR's root, `%LOCALAPPDATA%\nevr\echovr`, so the pack is there and nothing is
written in the game folder. A nEVR before 6.0 loads plugins from `bin/win10/plugins`,
so `<root>` is `bin/win10`, as in 1.0.0. The game's own files (the stock manifests and packages,
`bin/win10/scripts`) are still read where the game keeps them.

One pack is served at a time. Stacking packs would need their manifests merged; nothing
does that yet.

## content.json

```json
{
  "format": 1,
  "id": "echocombat",
  "name": "EchoCombat",
  "version": "1.0.0",
  "data_dir": "_data/5932408047/rad15/win10",
  "manifests": [
    {
      "package": "48037dc70b0ecab2",
      "stock_sha256": "<sha256 of the stock manifest it was built from>",
      "stock_packages": 3,
      "file": "manifests/48037dc70b0ecab2",
      "packages": ["packages/48037dc70b0ecab2_3"]
    }
  ],
  "scripts": ["scripts/190ed14fe6c74dd3.dll"],
  "files": { "manifests/48037dc70b0ecab2": "<sha256>", "packages/48037dc70b0ecab2_3": "<sha256>",
             "scripts/190ed14fe6c74dd3.dll": "<sha256>" }
}
```

Paths are relative to the pack folder (`/`, plain names). `data_dir` is relative to the
game folder. `files` lists every file the other keys name.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake \
      -DNEVR_SDK_INCLUDE=<nevr-runtime>/sdk/include   # required: configure fails without it
cmake --build build                                     # build/NvrContentOverlay.dll
x86_64-w64-mingw32-g++ -std=c++17 -O1 -static -I<nevr-runtime>/sdk/include -o build/host.exe test/host.cpp
```

On Windows: `powershell -ExecutionPolicy Bypass -File test\run.ps1 -Dll build\NvrContentOverlay.dll
-HostExe build\host.exe`. It runs the plugin in a stand-in game, one fake install per
case: no pack, serving, each refusal above, and shutdown. The script loader hook needs
the real echovr.exe and is tested in the game.

The plugin is built against nEVR's plugin SDK headers (`nevr/plugin_interface.h`, `nevr/plugin_logger.h`,
from nEVR's `sdk/include`), and reports the API version those headers give. It is built static with
MinHook 1.3.4, pinned by hash, and nlohmann/json (third_party).
Capabilities: `ALTERS_GAMEPLAY | HOOKS_ENGINE`.
