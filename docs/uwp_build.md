# Building Eden's core stack for the Xbox UWP / AppContainer target

This is the **Phase 2** build target: it compiles Eden's runtime libraries
(`common`, `core`, `dynarmic`, `video_core`, `audio_core`, `hid_core`, `network`,
`shader_recompiler`) for the **UWP/WindowsStore (AppContainer)** toolchain — the
console-side target. Desktop frontends (Qt, the CLI, room server, cubeb, libusb,
web services, OpenGL) are turned off, and `dynarmic` builds in its W^X +
`Virtual*FromApp` mode for the sandbox.

It is **headless**: the Null renderer (`video_core/renderer_null/`) is selected at
runtime, so no GPU device is created at boot (Vulkan stays compiled). This is the
build CORE's JIT/memory work and the eventual headless-boot frontend validate against.

> Status: the full core stack **compiles and links** for this target today (store
> CRT, `CMAKE_SYSTEM_NAME=WindowsStore`). Producing a runnable `.appx` (AppContainer
> link + `codeGeneration` manifest) and the headless-boot frontend are the follow-on
> steps toward GATE 2.

## Prerequisites

- Visual Studio 2022 with the **"C++ (v143) Universal Windows Platform tools"**
  component (`Microsoft.VisualStudio.ComponentGroup.UWP.VC`) and a Windows 10/11 SDK.
- CMake ≥ 3.25 and Ninja (both ship with VS 2022 under
  `Common7/IDE/CommonExtensions/Microsoft/CMake/`).

## Why Ninja (not the Visual Studio generator)

We use **Ninja**, not `-G "Visual Studio 17 2022"`, on purpose: the VS generator
queries the VS Installer for a registered instance, which fails if the install is
flagged incomplete. Ninja just uses the `cl.exe`/`link.exe` on `PATH` — so it only
needs the **UWP VC environment**, set by `vcvarsall.bat x64 uwp`. That environment
points `INCLUDE`/`LIB` at the **Store CRT** and is what makes this a UWP build.

## Configure + build

Set up a **UWP VC environment** and make sure CMake + Ninja are on `PATH`, then use
the preset. From `cmd` at the repo root (adjust the VS edition/path as needed):

```bat
set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 uwp
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"

cmake --preset uwp-x64
cmake --build --preset uwp-x64 --target core
```

`vcvarsall x64 uwp` points the compiler at the Store CRT; the `PATH` line puts the
VS-bundled `cmake.exe` + `ninja.exe` on `PATH` (a plain `vcvarsall` shell does not add
them, and `cmake --preset` needs Ninja discoverable). CI does the same two steps.

`--target core` pulls the whole runtime graph (common, dynarmic, video_core, etc.).
Scope to a single library with `--target dynarmic` / `--target common` while iterating.

The `uwp-x64` preset (see `CMakePresets.json`) pins the option set, the WindowsStore
system name, and `DYNARMIC_UWP_APPCONTAINER=ON` (which forces dynarmic's W^X path and
routes its JIT alloc/protect through `VirtualAllocFromApp`/`VirtualProtectFromApp`).

## Notes / known follow-ups

- **Partition strictness:** this build uses the default `WINAPI_FAMILY_DESKTOP_APP`
  partition (both desktop + app APIs visible), which is why `host_memory.cpp` — which
  resolves `VirtualAlloc2`/`MapViewOfFile3` via `GetProcAddress` — compiles as-is. The
  runtime AppContainer still requires the `*FromApp` variants; that swap is tracked in
  CORE's HostMemory work. A stricter `WINAPI_FAMILY_APP` per-target pass can be layered
  on later to catch direct desktop-API calls at compile time.
- **The desktop build is unaffected** — this is a separate preset/binary dir; the normal
  desktop configure is unchanged.
