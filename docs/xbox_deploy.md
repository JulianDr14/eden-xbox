# Packaging and deploying the headless boot appx to an Xbox Series X|S

This is the step that turns the UWP build from [`docs/uwp_build.md`](uwp_build.md) into something
that actually runs on a console. It covers **GATE 2**: proving Eden executed guest code through the
dynarmic JIT inside the Xbox AppContainer.

> **What this does _not_ do yet:** there is no D3D12 renderer (Phase 1/3). The boot selects the
> **Null renderer** — no GPU device, no image, no input, no audio. Success looks like a log line,
> not a game.

## What you need

**On the console**
- An Xbox Series X|S with **Dev Mode** activated (the *Xbox Dev Mode Activation* app, which requires
  a Partner Center developer account).
- Device Portal reachable at `https://<xbox-ip>:11443`.

**On the PC**
- Everything in [`docs/uwp_build.md`](uwp_build.md) (VS 2022 + the **C++ (v143) UWP tools**
  component, a Windows 10/11 SDK, CMake, Ninja, native perl + nasm, vswhere on PATH).
- The Windows SDK also supplies `MakeAppx.exe` and `SignTool.exe`, which the packaging script finds
  on its own.

**The payload**
- **devkitPro** with the `switch-dev` package, to build the `boot.nro` in
  [`tools/xbox/boot_nro/`](../tools/xbox/boot_nro/). That is ordinary homebrew, built from source
  here: no keys, no firmware, no commercial ROMs — house rule, and nothing it calls needs them.

Build the payload from that directory, in **PowerShell or cmd** (not a Git Bash shell — msys2's
make resolves paths differently there and the compile fails):

```
C:\devkitPro\msys2\usr\bin\make.exe
```

It runs a loop, checks the result, and emits `EDEN_XBOX_JIT_ALIVE` only if the arithmetic came out
right — a miscompiling JIT reports `EDEN_XBOX_JIT_MISCOMPILE` instead of silently passing the
gate. Observing the sentinel is what makes GATE 2 positive proof rather than "it didn't crash".

## Build, package, sign

From a **`vcvarsall.bat x64 uwp`** shell with CMake + Ninja on PATH (see the build doc — all three
env steps matter, and `echo %LIB% | findstr x64\store` must match):

```bat
cmake --preset uwp-x64
cmake --build --preset uwp-x64 --target eden-uwp
```

Then, from PowerShell at the repo root:

```powershell
.\tools\xbox\package-appx.ps1 -BootNro C:\path\to\boot.nro
```

The script stages the layout, packs it, mints (or reuses) a self-signed code-signing certificate
whose subject matches the manifest's `Publisher`, signs the package, and exports the `.cer`:

```
build-uwp\package\eden-xbox.appx
build-uwp\package\eden-xbox.cer
```

## Deploy

1. Open `https://<xbox-ip>:11443` and accept the self-signed certificate warning.
2. **Add** → upload `eden-xbox.appx` **and** `eden-xbox.cer` (the console must trust the signer, or
   installation fails with a generic error).
3. Set the app to **Game mode**, not App mode. App mode caps a UWP app at roughly a gigabyte of
   memory; Game mode is what makes the emulated DRAM reservation viable — especially on Series S.
4. Launch it.

## Reading the result

The boot writes two things into the app's **LocalFolder**, pullable through the Device Portal's file
explorer:

- **`eden_uwp_diag.txt`** — coarse startup breadcrumbs written before Eden's logging is up
  (`BootView::Run entered`, the resolved NRO path, `RunHeadlessBoot returned <rc>`, or the exception
  that killed it). This is the file to read when the app flashes and closes.
- **`eden_log.txt`** — Eden's own log, once logging initialises. GATE 2 passes on:
  `Headless boot: JIT liveness CONFIRMED ('EDEN_XBOX_JIT_ALIVE' observed).`

Return codes from `RunHeadlessBoot`: `0` liveness confirmed · `2` the NRO failed to load ·
`3` the app ran but the sentinel never appeared within the 30 s backstop.

## Manifest notes

[`dist/uwp/AppxManifest.xml`](../dist/uwp/AppxManifest.xml) is deliberately minimal, but two things
in it are load-bearing:

- **`<Capability Name="codeGeneration" />`** — this is what unlocks `VirtualProtectFromApp`,
  `CreateFileMappingFromApp` and `MapViewOfFileFromApp` for the AppContainer. Without it dynarmic
  cannot make JIT pages executable and `common/host_memory.cpp` cannot map the emulated DRAM. Drop
  this line and nothing about the port works.
- **`Identity/@Publisher` must equal the signing certificate's subject** character for character.
  A mismatch is the most common sideload rejection; `package-appx.ps1` checks it before packing.

`EntryPoint="eden-uwp.App"` is the conventional moniker for a plain C++/WinRT `CoreApplication` app
(`wWinMain` + `IFrameworkView`, see [`src/eden_uwp/uwp_boot.cpp`](../src/eden_uwp/uwp_boot.cpp));
there is no activatable WinRT class to name.

## When it does not activate

A UWP app that fails **activation** dies before any of your code runs — no log, no dump, no diag
file. The usual causes, in the order worth checking:

- **A hard import on a DLL the Xbox sandbox does not provide.** `src/eden_uwp/CMakeLists.txt`
  already delay-loads the known set (WLAN API set, setupapi, hid, winmm, imm32, user32…). A new
  dependency that pulls a desktop-only DLL reintroduces this. Inspect with
  `dumpbin /imports eden-uwp.exe`.
- **Missing `codeGeneration`**, which fails later, at the first JIT page protect.
- **App mode instead of Game mode** — shows up as an out-of-memory death during DRAM reservation
  rather than at activation.
