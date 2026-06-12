# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# eden-uwp: the headless GATE-2 boot frontend for the Xbox/UWP AppContainer.
# A CoreApplication IFrameworkView that brings up Core::System with the Null renderer and runs a
# homebrew NRO through the JIT. Only built for the WindowsStore (UWP) target.

add_executable(eden-uwp WIN32
    headless_emu_window.h
    uwp_boot.cpp
    vma_impl.cpp
)

# core transitively pulls common/video_core/audio_core/hid_core/dynarmic etc.; name the ones we touch
# directly so the dependency graph is explicit. frontend_common provides PlayTimeManager (pulled by
# core's NS query service).
target_link_libraries(eden-uwp PRIVATE common core video_core audio_core frontend_common)

# WinRT (CoreApplication, IFrameworkView, ApplicationData) comes from the Store umbrella import lib.
target_link_libraries(eden-uwp PRIVATE WindowsApp.lib)

# Win32 networking used by core's network_interface (ConvertLengthToIpv4Mask etc.).
target_link_libraries(eden-uwp PRIVATE iphlpapi ws2_32)

# The HostMemory *FromApp path keys off this for the common headers it includes here too.
target_compile_definitions(eden-uwp PRIVATE YUZU_UWP_APPCONTAINER=1)

# C++/WinRT needs C++20 + exceptions.
target_compile_features(eden-uwp PRIVATE cxx_std_20)
if (MSVC)
    target_compile_options(eden-uwp PRIVATE /EHsc)
    # AppContainer executable: required for a sideloadable UWP package.
    target_link_options(eden-uwp PRIVATE /APPCONTAINER)
    # STOPGAP (track for removal): the bundled OpenSSL (libcrypto_static.lib) is a prebuilt /MT static
    # lib and injects /DEFAULTLIB:LIBCMT, which the dynamic store CRT doesn't provide. Ignoring it lets
    # OpenSSL's CRT symbols resolve against the app's /MD MSVCRT (single CRT in one binary). The proper
    # fix is building bundled OpenSSL /MD for the UWP target (YUZU_USE_BUNDLED_OPENSSL — DEVOPS); drop
    # this line once that lands.
    target_link_options(eden-uwp PRIVATE /NODEFAULTLIB:LIBCMT)
endif()

# ── STATUS / remaining work (WIP, not ready to merge) ─────────────────────────────────────────────
# This target COMPILES against the uwp-x64 toolchain; the final exe LINK is not yet clean. Findings
# from the first UWP exe link (the first ever done — library builds don't resolve these):
#   1. CRT conflict: bundled OpenSSL (libcrypto_static.lib) is built /MT and injects /DEFAULTLIB:LIBCMT,
#      colliding with the /MD app. Real fix: build bundled OpenSSL /MD for the UWP target
#      (YUZU_USE_BUNDLED_OPENSSL — DEVOPS/bundled-deps), not a /NODEFAULTLIB hack.
#   2. Link-completeness: still-unresolved externals point at deps yuzu_cmd pulls that need adding here
#      — frontend_common (PlayTime), the VMA implementation TU (GPUOpen::VulkanMemoryAllocator is PUBLIC
#      on video_core but its impl symbols don't resolve under the UWP build — needs investigation),
#      and platform libs (iphlpapi, ws2_32). To finalize once the toolchain env is correct.
#   3. Toolchain env: a true Store-CRT link needs the x64_uwp vcvars environment; the dev box's VS
#      install is incomplete (vswhere missing → vcvarsall falls back to desktop x64). VS-install repair
#      (DEVOPS, likely needs a reboot) is the gating item for a deployable Store binary.
