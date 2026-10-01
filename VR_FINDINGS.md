# City Car Driving / Steam Frame stereo investigation

## Scope and status

Investigation date: 2026-10-01. The user reports an unfusable/doubled CCD world while SteamVR dashboard/browser overlays remain correctly fused. Treat this observation as established; it was not independently viewed through the headset.

The leading candidate is an incorrect vertical off-axis projection reconstruction in CCD's OpenVR path. This is supported by binary inspection and an isolated execution of the engine's matrix builder. It is not yet a confirmed explanation of the final displayed stereo image: final shader constants and possible downstream compensation have not been captured.

The DLL-replacement proxy failed CCD startup with a user-observed game-corruption warning and was rolled back. A process-local hook now applies the same correction while leaving the original game DLL files unchanged. Both hook modes passed native SteamVR checks; the launcher reached CCD's main menu, and the enabled session entered free driving. During that live session the user reported that the headset image “seems ok,” but the viewpoint feels low, Up/Down view adjustments have no visible effect, and reset works. Those camera/control observations remain unresolved; no seat-height or pitch adjustment has been added.

## Source publication and private investigation baseline

This source-only repository was exported from verified local checkpoint `5caa9ee24be47e3b82be259d1ad5c3728d7cb581`. It contains the hook/launcher sources, pinned third-party dependencies and their licenses, build/launch scripts, default configuration, and these findings. Game binaries, installed game configuration, build outputs, logs, and the Microsoft toolchain are not distributed.

The branch and historical commit identifiers below refer to the separate local investigation repository, not this source-only Git history. That local history remains intact and was not pushed.

- Git branch: `vr-diagnostics`.
- Original baseline commit: `c0b223e` (`Record original City Car Driving VR baseline`).
- Steam application: `493490`; installed build ID: `23337938`.
- `bin/win32/openvr_api.dll`, `d3d11renderer.dll`, `mangalore.dll`, and `starter.exe` are PE32 x86 binaries.
- The renderer requests `IVRSystem_015`, `IVRCompositor_020`, and `IVROverlay_014`.
- The private baseline tracks the original OpenVR and renderer DLLs and installed `game.ini`, `mangalore.ini`, `settings_ex.ini`, `presets.xml`, and `fixtures.xml`; none of those game files are included here.
- Active user configuration lives outside the repository, under `%USERPROFILE%\Documents\Forward Development\City Car Driving Steam\config`. It is distinct from the installed defaults. The assistant did not edit user-profile configuration or SteamVR settings.

Original SHA-256 fingerprints:

| File under `bin/win32` | SHA-256 |
| --- | --- |
| `openvr_api.dll` | `6b5065d2c6c08aeddbbc6ac8d67880f7450fed344906ee3ea44b022aeb034c37` |
| `d3d11renderer.dll` | `9fda16a4a75504d1efbeb6b1fb5015cedc747a6e68ee2001ab55ec587a34a6c4` |
| `mangalore.dll` | `58ded9d9e15632ae6d3d63b77e459239cb00c4a677282adc360b75db60b6bb5f` |

## Runtime and configuration evidence

- SteamVR logs report version `2.17.10`, the `vrlink` driver, and the Frame's `Deckard MP` model identifier.
- `vrclient_Starter.txt` identifies CCD as `steam.app.493490`, `arch=win32`, `VRApplication_Scene`.
- The client log reports legacy input simulation of a Rift HMD and Oculus Touch controllers. That establishes identity/input simulation, not Rift projection geometry.
- The inspected `steamvr.vrsettings` has no saved per-game FOV/world-scale overrides.
- Active CCD `mangalore.ini` contains `VRHeadShift=0.080000`, `SteamVRForceIterleaved=false`, and `PresetHMD="HMD_NoRender"`. No explicit per-eye projection/IPD correction control was found in the inspected configuration. These existing keys were not changed.
- Both Windows registry views have SteamVR registered as the active OpenXR runtime: `steamxr_win32.json` for Registry32, `steamxr_win64.json` for Registry64. The x86 manifest points to `bin\vrclient.dll`.

## Live legacy-OpenVR geometry sample

A separate 32-bit PowerShell/C# background client used the original game DLL and `FnTable:IVRSystem_015`. Initialization and interface lookup both returned error `0`. The client submitted no frames and changed no settings. These are runtime values from the diagnostic client, not an in-game capture.

Recommended render target: `2644 x 2644`.

| Eye | Raw left | Raw right | Raw top | Raw bottom |
| --- | ---: | ---: | ---: | ---: |
| Left | -1.65569317 | 1.21572745 | -1.74098039 | 1.16517377 |
| Right | -1.20432019 | 1.67948031 | -1.70426273 | 1.18643951 |

Both eye-to-head rotation blocks were identity. X translations were `-0.0347802676` and `+0.0347802676` metres: separation approximately `69.5605352 mm`. Thus unhandled eye cant is not supported by this sample. The per-eye frusta are substantially asymmetric; asymmetry alone is normal and is not a SteamVR defect.

OpenVR projection matrices, row-major, near `0.1`, far `1000`:

```text
Left:
 0.6965193  0           -0.153222308  0
 0          0.688194752 -0.198133543  0
 0          0           -1.0000999   -0.100009993
 0          0           -1           0

Right:
 0.693529248 0            0.164768726  0
 0           0.691873431 -0.179134071  0
 0           0           -1.0000999   -0.100009993
 0           0           -1           0
```

## Inspected game reconstruction and numerical experiment

Relative virtual addresses below apply only to the fingerprinted binaries; they are evidence, not patch instructions.

- `d3d11renderer.dll` RVA `0x34D80`: queries `IVRSystem_015::GetProjectionRaw` for each eye during initialization and converts the tangents to absolute angular extents for the engine camera.
- `mangalore.dll` RVA `0xE9E00`: reconstructs near-plane edges from those camera angles.
- `mangalore.dll` RVA `0xEA280`: builds the off-center projection matrix.
- The inspected scene submission path calls `IVRCompositor_020::Submit` with eye indices 0 and 1, null texture bounds and flags 0. This alone does not prove the correct image is associated with each index.

The off-center builder was executed in an isolated x86 process using the effective near-plane inputs reconstructed from the live raw sample. Its output was transposed into OpenVR's matrix convention for comparison:

| Eye | Native OpenVR vertical center `P[1][2]` | CCD reconstruction | Corrected-input reconstruction |
| --- | ---: | ---: | ---: |
| Left | -0.198133543 | +0.198133543 | -0.198133543 |
| Right | -0.179134071 | +0.179134056 | -0.179134056 |

Candidate CCD-specific raw-extent compensation:

```text
left_out   = left_native
right_out  = right_native
top_out    = -bottom_native
bottom_out = -top_native
```

With that compensation, maximum absolute differences in the first two matrix rows versus native OpenVR were `5.2e-8` (left) and `3.1e-8` (right). CCD's reversed-depth rows were deliberately excluded; they are not an angular-projection error.

[INFERENCE] This vertical convention mismatch is a strong correction target. It does not establish that all perceived misalignment has the same cause. An application-specific raw-extent workaround deliberately compensates a nonconforming consumer; it is not a generally correct OpenVR projection override and must not be installed globally. Overriding only `GetProjectionMatrix` would miss the inspected raw-projection path.

## Compatibility alternatives

- SteamVR Field of View and Override World Scale are real controls, not general repairs for an incorrect projection-center sign.
- The installed tooltip for Use Legacy Reprojection Mode says it disables asynchronous reprojection; it is not a parallel-projection switch.
- Pupil Perspective Compensation adjusts IPD/per-eye perspective with gaze. Its relevance is unproven; a controlled A/B is relevant only if the symptom varies with gaze.
- Simulate HMD changes reported device information. No beneficial Vive-specific CCD branch has been demonstrated.
- OpenComposite's OpenXR branch supplies a Windows x86 DLL, and this installation already has x86 SteamVR OpenXR registration. No working integration with CCD's corruption handling has been demonstrated. Its inspected geometry path preserves asymmetric FOV rather than automatically correcting CCD's reconstruction.
- Arbitrary symmetric/Vive-like matrices would require matching submission metadata or image conversion. Valve describes supplying custom projection metadata via `VRTextureWithDepth_t`, with a null depth handle permitted. That is different from compensating CCD so its resulting angular projection matches the native geometry.

## Current process-local hook

**Do not replace `bin/win32/openvr_api.dll`.** It must remain the original stock file. The rejected replacement implementation is retained only in local investigation commit `f8e8b2a`; the distributed source uses a separate launcher and hook DLL.

### Installation, launch, and A/B control

Install the Steam version of City Car Driving first. Copy this repository's **`vr-shim` directory into the game installation root, alongside `bin`**, then build it there using the instructions below. Keep that directory layout: `launch.cmd` locates `bin\win32\Starter.exe` relative to its own directory. No game files or prebuilt binaries are bundled.

1. Have Steam running and signed in, with SteamVR and the headset connected.
2. Fully exit any existing CCD instance.
3. Run `vr-shim\launch.cmd` from this installation.

The launcher starts this installation's x86 `Starter.exe` with the hook loaded before game startup. It refuses an existing instance instead of attaching to it. Steam application IDs are set only in the launcher/child environment; no global Steam configuration or game validation/licensing code is patched.

The active configuration is **`vr-shim/build/ccd_vr_projection.ini`**:

```ini
[Projection]
Enabled=1
```

`1` applies the signed vertical-extent exchange; `0` preserves native raw extents. Both modes retain the process-local hooks. Fully exit and relaunch CCD after changing it: the setting is read once, and CCD caches projection data during initialization. The source INI at `vr-shim/ccd_vr_projection.ini` is the default copied by the build only when the active INI does not already exist.

An ordinary Steam launch does **not** load this correction. To return to the untouched game path, exit the hooked session and launch CCD normally through Steam.

Diagnostics append to `vr-shim/build/ccd_vr_projection.log`: process/native DLL paths, mode, init/shutdown, and each eye's first raw-frustum sample per session. There is no per-frame logging.

### Implementation and build

- `vr-shim/openvr_hook.cpp`: retains the typed MSVC x86 `IVRSystem_015` wrapper and the CCD-specific raw-extent correction.
- `vr-shim/launcher.cpp` and `launch.cmd`: create only a new CCD process using `DetourCreateProcessWithDllExW`. No arbitrary-PID attachment, elevation request, service, or automatic restart.
- Microsoft Detours v4.0.1, commit `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`, is vendored under `vr-shim/vendor/detours/` with its MIT license.
- `stock_openvr.def` imports the three native functions by name from the original `openvr_api.dll`. `openvr_hook.def` supplies Detours' required helper export at ordinal 1.
- The injected DLL restores Detours' temporary in-memory executable import-table changes, then detours `VR_InitInternal`, `VR_GetGenericInterface`, and `VR_ShutdownInternal` inside that child process. The hook is pinned until process exit.
- Configuration/logging are deferred until the first intercepted API call, not performed in `DllMain`.

Only C++ `IVRSystem_015` is wrapped. Other interface versions, compositor/overlay interfaces, and `FnTable:*` pass through. Only `GetProjectionRaw` changes: left/right remain native, `top = -nativeBottom`, and `bottom = -nativeTop`. `GetProjectionMatrix`, eye-to-head transforms, tracking/poses, submission, IPD, headset identity, and keyboard bindings are not changed. The correction changes vertical projection framing; it is not a camera-position or seat-height adjustment.

Build with CCD closed, using a configured x86 MSVC environment or an installed Visual Studio Desktop development with C++ workload:

```bat
vr-shim\build.cmd
```

The Microsoft compiler and Windows SDK are prerequisites, not bundled dependencies. The local investigation used an isolated toolchain under an ignored build directory; a fresh checkout should use its own installed x86 MSVC toolchain.

Outputs are `vr-shim/build/ccd_vr_hook.dll` and `ccd_vr_launcher.exe`. They remain local ignored build artifacts; source, pinned dependencies, build/launch scripts, and the default configuration are tracked. No build or launch step copies a DLL into `bin/win32/`. MSVC x86 is required for the legacy C++ ABI, not interchangeable with MinGW or x64.

## Original proxy numerical checks (historical)

Initial DLL-replacement build and native smoke verification: 2026-10-02, preserved with the rejected experiment in private local investigation commit `f8e8b2a`.

- Built with the Microsoft 14.43.34808 x86 toolset and Windows SDK 10.0.22621.0, using `/W4 /WX /O2 /MT`. The previously discovered Visual Studio installation was no longer present at build time. Microsoft packages were unpacked under the ignored `vr-shim/build/msvc/` directory instead of installing system-wide software.
- The build uses `LIB` to generate the export object before linking. This avoids LINK's C-forwarder DEF validation failure; there are no fake forwarding stubs.
- Parsed the resulting PE32 binary: all 18 original export names and ordinals match; 15 are native PE forwarders and three are intercepted entrypoints.
- Ran a temporary x86 C++ diagnostic against the actual installed SteamVR runtime and connected `Deckard MP`, once with `Enabled=1` and once with `Enabled=0`. Both exited successfully.
- Each mode exercised two initialization/shutdown cycles, stable repeated `IVRSystem_015` queries, native initialization tokens, scalar/string returns, and large-structure returns. All 16 native projection-matrix terms and all 12 eye-transform terms were preserved.
- `FnTable:IVRSystem_015`, `IVRSystem_019`, `IVRCompositor_020`, and `IVROverlay_016` retained their native interface pointers. Unsupported C++ and function-table requests retained error 105 and null results.
- The diagnostic used `VRApplication_Background`, submitted no textures, and made no tracking-origin or headset-configuration changes. It tested DLL loading from a working directory different from the DLL directory.

Observed CCD-reconstructed vertical projection centers:

| Eye | Native `P12` | Proxy disabled | Proxy enabled |
| --- | ---: | ---: | ---: |
| Left | -0.198133543 | +0.198133543 | -0.198133543 |
| Right | -0.179134071 | +0.179134056 | -0.179134056 |

The first two reconstructed projection rows agree with native geometry when enabled, within floating-point tolerance. This is numerical evidence, not a headset-fusion result.

Tested proxy SHA-256: `1910ef713ecf079db73a06320a7abded2161ca4ff909c420a349572b9decec33`. This binary is not installed in the game.

## Original DLL-replacement startup failure (historical)

Game-path verification: 2026-10-02.

The installed proxy reached a successful Scene initialization (`application_type=1`, `error=0`) and intercepted the renderer's `IVRSystem_015` request. Its game-side log recorded both corrected frusta:

```text
left  native=[-1.65569317 1.21572745 -1.74098039 1.16517377]
      return=[-1.65569317 1.21572745 -1.16517377 1.74098039]
right native=[-1.20432019 1.67948031 -1.70426273 1.18643951]
      return=[-1.20432019 1.67948031 -1.18643951 1.70426273]
```

Reaching this call path was not sufficient for a usable game launch:

| Installed OpenVR DLL | Observed game result |
| --- | --- |
| Original, byte-exact stock DLL | Reached the visible main menu; remained alive through a 60-second debugger observation, then was closed |
| Projection proxy, `Enabled=1` | Startup access violation |
| Projection proxy, `Enabled=0` | Same startup access violation |
| Transparent 18-export forwarding DLL, no wrapper or projection changes | Same startup access violation |
| Forwarding control with a normal static import of the original DLL | Same startup access violation |

During these launches, the user reported a warning that the game was corrupted. This observation is accepted as evidence; the exact dialog text was not captured. DLL-replacement tests were stopped and the original DLL restored.

The debugger observed exception `0xC0000005` at `gui_ccd.dll` RVA `0xB218A`, dereferencing `0xFFFFFFFF`, with a caller at `Starter.exe` RVA `0xAA85F`. Disassembly shows an object-deletion path that subsequently stores `0xFFFFFFFF`. This crash location is not proof of the underlying corruption check; no GUI code or validation checks were patched.

[INFERENCE] The replacement DLL triggers CCD compatibility/integrity handling independently of the projection correction: a fully native forwarding control fails too. The exact check has not been traced. The static-import control also failed, so changing original-DLL initialization order did not resolve this experiment. The standalone diagnostic had loaded the original before the proxy; its passing result must not be presented as game compatibility.

That DLL-replacement route remains rejected. The current process-local route below avoids it without patching validation code. OpenComposite has not been validated here and also replaces `openvr_api.dll`; it must not be presented as an established workaround for the warning.

An earlier first-hand report, [openvr_fsr issue #40](https://github.com/fholger/openvr_fsr/issues/40), describes the same corruption warning after replacing CCD's OpenVR DLL. That discussion also identifies a 64-bit/32-bit mismatch in the packaged mod; our proxy is verified PE32 and successfully initializes SteamVR, so that explanation cannot simply be carried over. The issue does not document a validated workaround.

At that rollback, the installed DLL was restored to SHA-256 `6b5065d2c6c08aeddbbc6ac8d67880f7450fed344906ee3ea44b022aeb034c37`, matching the original baseline. The failing diagnostic instances were closed. Temporary forwarding controls, debugger programs, dumps, and the extra proxy configuration/log/backup files in `bin/win32/` were removed.

## Process-local verification and headset feedback

Verification: 2026-10-02.

- Built the hook, launcher, and pinned Detours source with MSVC 14.43.34808 x86 and Windows SDK 10.0.22621.0, with `/W4 /WX /O2 /MT`.
- A temporary real-SteamVR diagnostic loaded the original DLL, saved native interfaces, then loaded the hook. Queries through the **same cached original export address** subsequently returned the typed wrapper, demonstrating actual in-memory interception rather than a forwarding-only test.
- `Enabled=0` and `Enabled=1` each completed two initialization/shutdown cycles with zero failed checks. Raw left/right values were bit-exact; vertical values were native in mode 0 and exactly `[-nativeBottom, -nativeTop]` in mode 1.
- All 16 projection-matrix elements at two clip ranges and all 12 eye-transform elements remained bit-exact. Recommended target size was 2644×2644; the background client reported connected `Deckard MP`, HMD class 1.
- Non-target interface pointers and errors remained native. Unsupported interfaces returned null/error 105; queries after shutdown returned null/error 109. Init tokens advanced normally.
- Hidden-area-mesh small-structure returns were exercised for both eyes and all three mesh types. This headset returned empty meshes, so non-empty mesh contents were not exercised.
- Enabled CCD angular reconstruction matched native projection scales/centers within floating-point tolerance; disabled mode retained the opposite vertical-center sign. The background diagnostic submitted no textures and changed no tracking origin.
- The process-local launcher with `Enabled=0` reached the visible CCD main menu (PID 1564), then closed normally with exit code 0. A second launch while that instance was running was correctly refused with Win32 error 183. Missing launcher arguments returned error 160 without starting a game.
- The enabled launcher session (PID 30120) reached the actual main menu, then the existing Free Driving preset: Old city / Test track, manual car, summer/clear/daytime. Automated UI inspection reached the in-session pause menu. The desktop view did not independently establish rendered-world stereo quality.
- The enabled game log recorded `integration=detours imports_restored=1`, native DLL `bin/win32/openvr_api.dll`, Scene initialization with error 0, `IVRSystem_015` interception, and both corrected frusta. No replacement DLL was installed.
- During the enabled live session, the user reported through the headset that the image “seems ok.” This is user-observed improvement, not a claim that every fusion/depth/coverage condition has been validated.
- Game automation stopped when the user began assessing the headset. The live session was left alone; no further camera input, reset, configuration change, or shutdown was performed.

The installed OpenVR and renderer DLLs still match their original baseline hashes. Final built artifact SHA-256 values:

| Artifact | SHA-256 |
| --- | --- |
| `bin/win32/openvr_api.dll` | `6b5065d2c6c08aeddbbc6ac8d67880f7450fed344906ee3ea44b022aeb034c37` |
| `bin/win32/d3d11renderer.dll` | `9fda16a4a75504d1efbeb6b1fb5015cedc747a6e68ee2001ab55ec587a34a6c4` |
| `vr-shim/build/ccd_vr_hook.dll` | `ff9bdb762929bcbfdb0b1c775a9059e101edd1ea6dcddc9802aea59a7052f011` |
| `vr-shim/build/ccd_vr_launcher.exe` | `73c92979fcd48b5a5356ce968213d2269123bdb5019273aacc042c4b93072720` |

### Unresolved camera/control observations

The user reports a low viewpoint/view angle, ineffective Up/Down adjustments in the settings, and a working reset. These observations are accepted without rerunning them. No input bindings, seat-height offset, or head-pose adjustment was implemented. Whether these control symptoms existed before the hook has not been established.

The next useful distinction is **physical seat height versus downward viewing pitch/framing** after reset in the user's normal seated posture. An independent camera offset should not be added blindly to a now-apparently-fusable image. The exact Up/Down binding semantics in CCD's tracked VR camera remain to be investigated.

## Disable, rollback, and visual acceptance

For the current implementation, exit CCD and launch normally through Steam to omit all hooks. Alternatively, set the active build-directory INI to `Enabled=0` and relaunch through `launch.cmd` for a native-raw comparison. The hook is process-local and cannot be removed from an already-running session.

The current hook does not replace game DLL files, so disabling it requires no file restoration. If an old DLL-replacement experiment was installed separately, close CCD and use Steam's **Properties → Installed Files → Verify integrity of game files** to restore the installed game. The private baseline commit and its recovery files are not part of this source-only repository.

For any further A/B assessment, fully restart CCD and use the same parked driving scene. Check fusion of a distant landmark and the steering wheel/dashboard, correct depth at both distances, image orientation/coverage, slow head rotation/translation, and gaze changes. Stop if it causes strain. The current low-viewpoint/control observations are separate unresolved items; do not label them normal based only on the numerical projection checks.

## Primary references

- [Valve historical OpenVR v1.0.7 header (`IVRSystem_015`)](https://github.com/ValveSoftware/openvr/blob/v1.0.7/headers/openvr.h)
- [Valve historical C# function-table ABI](https://github.com/ValveSoftware/openvr/blob/v1.0.7/headers/openvr_api.cs#L15-L41)
- [Valve `GetProjectionRaw` reconstruction formula](https://github.com/ValveSoftware/openvr/wiki/IVRSystem::GetProjectionRaw)
- [Valve custom projection submission guidance](https://github.com/ValveSoftware/openvr/issues/1798#issuecomment-1832661721)
- [SteamVR 2.17 announcement, including 32-bit OpenXR support](https://store.steampowered.com/news/app/250820/view/679635225181946198)
- [Steam Frame per-application preferences](https://partner.steamgames.com/doc/steamhardware/steamframe/vrpreferences)
- [OpenComposite OpenXR installation and configuration](https://gitlab.com/znixian/OpenOVR/-/blob/openxr/README.md)
- [Earlier CCD custom-OpenVR corruption report and architecture caveat](https://github.com/fholger/openvr_fsr/issues/40)
- [Microsoft Detours pinned source and license](https://github.com/microsoft/Detours/tree/e4bfd6b03e50de46b47abfbd1e46b384f0c5f833)
- [Microsoft early process-local DLL injection API](https://github.com/microsoft/Detours/wiki/DetourCreateProcessWithDllEx)
- [Microsoft documented detour attachment and restoration pattern](https://github.com/microsoft/Detours/wiki/Using-Detours)
