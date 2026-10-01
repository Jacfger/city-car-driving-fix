# City Car Driving — Steam Frame VR projection fix

An unofficial, application-local workaround for **double vision / an unfusable driving scene in City Car Driving on Steam Frame**, when SteamVR's dashboard and overlays look correct.

The fix was exercised on the Windows Steam version of CCD with Steam Frame. The player reported that the corrected headset image “seems ok.” This is a tested workaround for that installation, **not a guarantee for every headset or game/runtime version**.

> **Keep the original game DLLs.** Do not replace `bin/win32/openvr_api.dll` or copy this hook into `bin/win32`. This repository contains source code, not a prebuilt installer or the game itself.

## What changed, and why

CCD asks legacy OpenVR for each eye's raw projection extents, then constructs its own projection matrix. In the inspected game build, that reconstruction gave the vertical projection center the opposite sign from SteamVR's native matrix. The difference matters with asymmetric per-eye projections such as those observed on Steam Frame.

The correction changes only the values returned by `IVRSystem_015::GetProjectionRaw` to CCD:

```text
left_out   = left_native
right_out  = right_native
top_out    = -bottom_native
bottom_out = -top_native
```

These are values from your running SteamVR system, not hard-coded headset measurements. With the corrected inputs, CCD's reconstructed angular projection matched the native projection within floating-point tolerance in the diagnostic experiment.

An initial replacement `openvr_api.dll` passed numerical checks but caused CCD startup failures and a game-corruption warning. That approach was abandoned. The current implementation instead:

1. Starts a **new CCD process** with a small x86 hook DLL loaded before the game begins.
2. Uses Microsoft Detours to intercept the original OpenVR initialization, interface lookup, and shutdown functions **in that process's memory**.
3. Wraps only the legacy C++ `IVRSystem_015` interface and applies the raw vertical-extent correction above.

The original game DLL files stay unchanged. Camera position, head tracking, eye-to-head transforms, IPD, native projection-matrix calls, compositor submission, keyboard bindings, and global SteamVR settings are not changed. Game licensing/validation code is not patched. The correction is CCD-specific; **do not install it globally or use it as a general OpenVR replacement**.

## Requirements

- A Windows PC with the Steam version of **City Car Driving**, app ID `493490`, installed. The expected game executable is `bin\win32\Starter.exe`.
- Steam running and signed in, plus SteamVR with the Steam Frame headset connected before launching CCD.
- **Visual Studio 2022 or Visual Studio Build Tools 2022**, with the **Desktop development with C++** workload, MSVC x86/x64 build tools, and a Windows SDK. Microsoft provides a [Build Tools 2022 installer](https://aka.ms/vs/17/release/vs_buildtools.exe) and [C++ workload installation instructions](https://learn.microsoft.com/en-us/cpp/build/vscpp-step-0-installation?view=msvc-170).
- Permission to write to the installation's `vr-shim` directory for building, configuration, and logging.

The game and hook are **32-bit/x86**, even on a 64-bit Windows PC. The build script selects x86 automatically. An x64 DLL or MinGW build is not a substitute for the required MSVC C++ ABI. This is a Windows PC/SteamVR workflow, not a standalone headset installation; Linux/Proton and other game editions have not been validated.

## Install, build, and play

### 1. Download the source

Use **Code → Download ZIP** on this repository and extract it, or clone it:

```sh
git clone https://github.com/Jacfger/city-car-driving-fix.git
```

Git is optional if you use the ZIP. The OpenVR header and Detours sources are already included; no submodule checkout is needed.

### 2. Copy the correct folder into the game

Fully exit CCD before installing or updating the hook.

In Steam, right-click **City Car Driving → Manage → Browse local files**. Copy the downloaded repository's **`vr-shim` folder** into that game installation directory, alongside `bin`:

```text
City Car Driving/
├── bin/
│   └── win32/
│       ├── Starter.exe
│       └── openvr_api.dll          (original game file; leave it alone)
└── vr-shim/
    ├── build.cmd
    ├── launch.cmd
    ├── launcher.cpp
    ├── openvr_hook.cpp
    ├── openvr_hook.def
    ├── stock_openvr.def
    ├── ccd_vr_projection.ini
    └── vendor/
```

**Do not add an extra repository directory between the game and `vr-shim`.** `launch.cmd` locates `..\bin\win32\Starter.exe` relative to its own folder. Copy the whole `vr-shim` directory, including `vendor`, not just a `.cpp` file.

If you previously installed a replacement-OpenVR mod, undo that mod and restore the stock game files first. Do not combine this procedure with OpenComposite or another replacement `openvr_api.dll`.

### 3. Build once

Open **Windows Command Prompt**, change to the game installation directory, then run the build. For example, if your Steam library is on `G:`:

```bat
cd /d "G:\SteamLibrary\steamapps\common\City Car Driving"
vr-shim\build.cmd
```

Replace the example installation path with yours. Run these commands in Windows, not a Linux shell. Keeping the terminal open lets you read any compiler error.

`build.cmd` selects the latest installed Visual Studio with the C++ tools and configures an x86 build. If you have multiple Visual Studio versions, use an **x86 Native Tools Command Prompt for VS 2022** to select that toolchain explicitly; the script preserves an already configured x86 environment. It does not install anything into `bin\win32`.

A successful build produces:

```text
vr-shim/build/ccd_vr_launcher.exe
vr-shim/build/ccd_vr_hook.dll
vr-shim/build/ccd_vr_projection.ini
```

The default is `Enabled=1`. If the build fails, resolve the reported error before launching; an older executable may still exist after a failed rebuild. Close CCD before rebuilding, since the running process keeps the hook DLL loaded.

### 4. Start SteamVR, then use the custom launcher

1. Start Steam and sign in. Connect Steam Frame and make sure SteamVR recognizes it.
2. Ensure CCD is fully closed. The launcher refuses to attach to an existing game instance.
3. From the game directory, run:

   ```bat
   vr-shim\launch.cmd
   ```

   You can also double-click the installed `launch.cmd` or create a Windows shortcut to it. If it exits with an error, run it in Command Prompt to keep the message visible.

4. If CCD is still configured for monitor-only output, select its VR/headset output in the game's settings. English versions use labels such as **Output device → VR Goggles** or **Monitor + VR Goggles**. After changing this, fully exit and relaunch through `vr-shim\launch.cmd`, rather than relying on an automatic restart. The hook does not enable the game's VR setting for you.
5. Use the desktop game menu to start **Free Driving** or a driving mission. **CCD's VR output starts with the driving session, not at the main menu.** See the [CCD VR setup discussion](https://steamcommunity.com/app/493490/discussions/0/2561864094345525344/#c3783625216787231335) for that menu behavior. This fix uses the tested SteamVR path for Steam Frame; Oculus/Air Link advice in older discussions is not this setup procedure.
6. Start parked and check the image in the headset. Look at both the dashboard and a distant object, then slowly move your head. Stop and revert if the image remains doubled, distorted, or uncomfortable.

**Use `vr-shim\launch.cmd` every time you want the correction.** The usual Steam Play button and original game shortcut do not load it. The launcher's “Created and resumed” message confirms process creation, not successful VR rendering.

### 5. Confirm the hook is active

After entering a driving session, open:

```text
vr-shim/build/ccd_vr_projection.log
```

The log appends across launches. Inspect the latest `process pid=` block, not an older successful run. A normal corrected launch should include:

- `architecture=x86 integration=detours imports_restored=1`
- A `native=` path pointing to the game's original `bin\win32\openvr_api.dll`
- `mode=CCD raw vertical correction Enabled=1`
- Scene initialization with `init application_type=1 error=0`
- `intercepted=IVRSystem_015`
- Entries for `eye=left` and `eye=right`, each with `native_raw` and `returned_raw` values

These markers establish that the correction reached the intended API path. They do not replace checking stereo comfort in the headset. The hook logs the first raw-frustum sample per eye/session, not every frame.

## Configuration and A/B comparison

Edit **the file next to the built hook DLL**:

```text
vr-shim/build/ccd_vr_projection.ini
```

```ini
[Projection]
Enabled=1
```

| Value | Behavior |
| --- | --- |
| `Enabled=1` | Apply the CCD-specific vertical projection correction. |
| `Enabled=0` | Return native raw projection values unchanged; the hook remains loaded. |

**Fully exit CCD and relaunch through `launch.cmd` after each change.** Configuration is read once per process, and the game caches projection data. Resetting the view or restarting only SteamVR is not a replacement for restarting CCD.

The separate file `vr-shim/ccd_vr_projection.ini` is only the build-time default. The build copies it into `build` if no active INI exists; it deliberately preserves an existing runtime setting on rebuild.

For comparison, use the same parked scene and seated position. The distant scene and nearby dashboard should both be comfortable to fuse. Do not try to hide a projection problem by guessing new IPD, world-scale, or camera-height values.

## Disable or uninstall

- **Run without any hook:** fully exit CCD, then launch it normally through Steam.
- **Compare without the correction:** use `Enabled=0` and restart through the custom launcher. This is not the same as removing the hook.
- **Uninstall:** close CCD and remove only the `vr-shim` folder you installed and any shortcut you created for it. No game DLL restoration or global SteamVR reset is needed for this implementation.

If an earlier, separate experiment replaced a stock DLL, use **Steam → City Car Driving → Properties → Installed Files → Verify integrity of game files** after closing the game. This source-only repository does not contain replacement copies of the original game files.

## Tested setup and known limitations

| Component | Exercised setup |
| --- | --- |
| Game | Windows Steam CCD, x86, Steam build ID `23337938` |
| Headset/runtime | Steam Frame (`Deckard MP`), SteamVR `2.17.10`, `vrlink` driver |
| Build tools | MSVC `14.43.34808` x86, Windows SDK `10.0.22621.0` |
| Native checks | Both enabled and pass-through modes; initialization/shutdown, raw projection, native matrices/eye transforms, and non-target interface forwarding |
| Game check | Main-menu startup and enabled Free Driving session; player reported improved headset image |

This was not a broad compatibility test. Other headsets, future game/runtime updates, and every depth/gaze/comfort condition have not been validated. Only the C++ `IVRSystem_015` path is corrected; different interfaces pass through.

**Still unresolved in the tested session:**

- The viewpoint/view angle felt low.
- Up/Down view adjustments in the settings had no visible effect.
- Reset worked.

These observations are not declared normal or fixed. No seat-height, pitch, or input-binding fix was added. The projection correction changes vertical framing, so a relationship to the perceived low view has not been ruled out.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| `vswhere.exe` missing, no C++ tools, or compiler/SDK errors | Install the Desktop development with C++ workload and Windows SDK. Visual Studio Code alone is not the compiler. Use an x86 Native Tools prompt if automatic discovery is unsuitable. |
| Missing `ccd_vr_launcher.exe` | Complete the build successfully; copying source alone does not create binaries. |
| `Starter.exe` cannot be opened/found | Check the directory layout. `vr-shim` must sit beside the game's `bin` folder, not inside an extra cloned-repository folder. |
| CCD is already running / Win32 error 183 | Exit the existing game, then use `launch.cmd`. This launcher does not attach to an existing process. |
| Steam not running | Start Steam and sign in in the same Windows session before launching. |
| No log or no `IVRSystem_015` interception | Confirm you used the custom launcher, enabled CCD's VR output, started a driving session, and can write to `vr-shim/build`. Do not assume the hook is active merely because a game window opened. |
| Correction appears disabled | Check the latest log block and **`build/ccd_vr_projection.ini`**, then fully restart CCD. Rebuilding does not overwrite an existing active INI. |
| Corruption warning, injection failure, or a new crash | Stop that run and return to stock Steam launch. Check for leftover DLL-replacement mods and restore game files if needed. Record the exact error and latest log; do not bypass validation checks or disable security software to force it to run. |
| View still low / Up/Down still ineffective | These are unresolved camera/control observations, not capabilities of this projection fix. |

For an issue report, include the CCD build, SteamVR version, headset, whether you used `launch.cmd`, the active `Enabled` value, the exact error, and the latest process block from the hook log. **Redact personal paths/usernames before posting logs.** State whether the problem also happens after a full exit and ordinary Steam launch.

## Technical notes and third-party code

[VR_FINDINGS.md](VR_FINDINGS.md) contains the numerical evidence, original-DLL fingerprints, rejected replacement-DLL experiment, actual verification results, and remaining uncertainties. Historical baseline commits mentioned there belong to the separate local investigation repository; proprietary game files and that history were not published.

- Valve OpenVR **v1.0.7** header for the legacy ABI: [BSD 3-Clause license](vr-shim/vendor/LICENSE.openvr).
- Microsoft Detours **v4.0.1**, pinned at `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`: [MIT license](vr-shim/vendor/detours/LICENSE.md).

City Car Driving, SteamVR, and the Microsoft compiler/SDK are separate prerequisites and are not redistributed here.
