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

# Win32 networking used by core's network_interface (ConvertLengthToIpv4Mask etc.), plus crypt32 +
# user32 which the from-source OpenSSL pulls (cert store + its fatal-error/service-detection code).
target_link_libraries(eden-uwp PRIVATE iphlpapi ws2_32 crypt32 user32)

# The HostMemory *FromApp path keys off this for the common headers it includes here too.
target_compile_definitions(eden-uwp PRIVATE YUZU_UWP_APPCONTAINER=1)

# C++/WinRT needs C++20 + exceptions.
target_compile_features(eden-uwp PRIVATE cxx_std_20)
if (MSVC)
    target_compile_options(eden-uwp PRIVATE /EHsc)
    # AppContainer executable: required for a sideloadable UWP package.
    target_link_options(eden-uwp PRIVATE /APPCONTAINER)
    # CRT bridge for the two bundled prebuilt deps that are /MT and have NO from-source path on Windows:
    # ffmpeg (Eden disables YUZU_USE_EXTERNAL_FFMPEG on WIN32) and SDL3. They inject /DEFAULTLIB:LIBCMT,
    # which the /MD Store-CRT app can't satisfy; ignoring it forces their CRT symbols onto the app's
    # single /MD MSVCRT. OpenSSL + sirit are already proper from-source /MD (CMakeLists WindowsStore
    # blocks), so this redirect now covers only ffmpeg/SDL3. PROPER FIX (DEVOPS hardening): a /MD ffmpeg
    # for UWP (+ from-source /MD SDL3) so this line can be dropped.
    target_link_options(eden-uwp PRIVATE /NODEFAULTLIB:LIBCMT)
    # core/internal_network/wifi_scanner.cpp statically imports the WiFi extension API set
    # (ext-ms-win-networking-wlanapi), which the Xbox AppContainer does NOT provide -> a hard import of
    # it makes the app fail to ACTIVATE before any code runs (observed: ~1s close, no log, no dump).
    # The headless boot never scans WiFi, so delay-load it: the loader resolves it only on first call
    # (which never happens here), so activation succeeds. delayimp.lib provides the delay-load helper.
    target_link_options(eden-uwp PRIVATE "/DELAYLOAD:ext-ms-win-networking-wlanapi-l1-1-0.dll")
    # delayimp.lib (the /DELAYLOAD helper) ships only in the desktop lib dir, not the Store-CRT LIB
    # path the uwp env points at, so reference it by full path via vcvars' VCToolsInstallDir.
    if (DEFINED ENV{VCToolsInstallDir})
        target_link_libraries(eden-uwp PRIVATE "$ENV{VCToolsInstallDir}lib/x64/delayimp.lib")
    else()
        target_link_libraries(eden-uwp PRIVATE delayimp.lib)
    endif()
endif()
