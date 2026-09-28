// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Headless GATE-2 boot frontend for the Xbox/UWP AppContainer target.
//
// Brings up Core::System with the Null renderer + null audio sink, loads a homebrew NRO staged in the
// app's sandboxed local storage, runs it through the dynarmic JIT, and emits a deterministic
// JIT-liveness marker. No audio device; input is the Xbox gamepad and/or a boot.cfg script
// (uwp_input.h).
//
// The Core::System bring-up is modeled on the proven desktop boot in src/yuzu_cmd/yuzu.cpp; the
// WinRT IFrameworkView wrapper is the UWP entry point that drives it.
//
// JIT-LIVENESS CONTRACT (agreed with AGENT QA for the GATE-2 checklist): GATE 2 means "Eden executed
// guest code via the JIT", not "the process didn't crash". The homebrew NRO (NO keys/firmware/ROM —
// house rule) issues svcOutputDebugString with the exact sentinel below; Eden's SVC handler logs
// OutputDebugString, so observing this line is positive proof the JIT decoded + executed guest code.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "common/logging.h"
#include "common/settings.h"
#include "common/windows/timer_resolution.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/kernel/svc/svc_debug_string.h" // Kernel::Svc::SetDebugStringObserver
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "hid_core/hid_core.h"
#include "video_core/gpu.h"

#include "eden_uwp/headless_emu_window.h"
#include "eden_uwp/uwp_input.h"

namespace D3D12 {
// renderer_d3d12.h; its includes need Mesa's headers, which only video_core sees.
void SetTracedFrame(u32 frame);
void SetArrayPadding(bool enabled); // d3d12_texture_cache.h
} // namespace D3D12

namespace {
void WriteDiag(const std::string& msg); // defined with the UWP entry point below
std::string MemoryReport();             // likewise
} // namespace

namespace EdenXbox {

constexpr const char* JIT_LIVENESS_SENTINEL = "EDEN_XBOX_JIT_ALIVE";
// Emitted by the payload after its framebuffer loop, so the boot keeps presenting until then.
constexpr const char* GFX_DONE_SENTINEL = "EDEN_XBOX_GFX_DONE";

/// Where the renderer presents: the CoreWindow (as IUnknown*) and its size in physical pixels.
/// A null window selects the Null renderer.
struct BootSurface {
    void* core_window{};
    u32 width{1920};
    u32 height{1080};
};

// Force the device-light configuration the boot needs.
static void ApplyHeadlessBootSettings(const BootSurface& surface) {
    Settings::values.renderer_backend = surface.core_window != nullptr
                                            ? Settings::RendererBackend::Direct3D12
                                            : Settings::RendererBackend::Null;
    Settings::values.sink_id = Settings::AudioEngine::Null;              // audio_core/sink/null_sink
    Settings::values.cpuopt_fastmem = false;        // Phase-2: bounds-checked page-table path
    Settings::values.cpuopt_fastmem_exclusives = false;
    // The on-console failure mode is a hard crash with no eden_log.txt; the default 4 KiB write
    // buffering loses exactly the lines that say where it died. Flush every line instead.
    Settings::values.log_flush_line = true;
    // Player 1 is a connected Pro Controller bound to the virtual_gamepad engine (uwp_input.h).
    auto& player = Settings::values.players.GetValue()[0];
    player.connected = true;
    player.controller_type = Settings::ControllerType::ProController;
    // memory_layout_mode stays at its default until the Series-S budget is measured on-console; the
    // DRAM clamp is a separate reservation follow-up, not here.
}

/// Optional boot.cfg next to boot.nro (written by package-appx.ps1 -RunSeconds).
struct BootConfig {
    /// > 0: the payload does not emit the sentinels (deko3d examples, games, ...); run it this long.
    u32 run_seconds{};
    /// D3D12 debug layer (renderer_debug); PC only, the console has no SDK layers.
    bool debug_layer{};
    /// File name of a game in LocalState\games to boot instead of boot.nro (package-appx.ps1 -Game).
    std::string game;
    /// Eden's log filter (e.g. "*:Info HW.GPU:Debug"); empty keeps the default.
    std::string log_filter;
    /// Null renderer even with a window, to tell GPU hangs from CPU ones.
    bool null_renderer{};
    /// Presented frame whose draws the D3D12 renderer logs; 0 keeps its default.
    u32 traced_frame{};
    /// Diagnostic: D3D12 color array images get a power of two layer count.
    bool array_pad{};
    /// Buttons to press at given times ("input=25:L+R" lines).
    std::vector<InputStep> input_script;
};

/// Games never emit the sentinels; without an explicit time they run this long.
constexpr u32 DEFAULT_GAME_RUN_SECONDS = 120;

// Returns a process exit-style status. 0 == boot reached the run phase cleanly.
int RunHeadlessBoot(const std::string& nro_path, const BootSurface& surface,
                    const BootConfig& config) {
    if (!config.log_filter.empty()) {
        Settings::values.log_filter = config.log_filter;
    }
    Common::Log::Initialize();
    // As yuzu_cmd: the default 15.6 ms timer resolution makes the emulated vsync (and any sleep in
    // the core) tick every 15.6 ms and drop one frame in ten, visible as a stutter.
    const auto timer_resolution = Common::Windows::SetCurrentTimerResolutionToMaximum();
    WriteDiag("step: timer resolution " +
              std::to_string(std::chrono::duration<double, std::milli>(timer_resolution).count()) +
              " ms");
    ApplyHeadlessBootSettings(config.null_renderer ? BootSurface{} : surface);
    D3D12::SetTracedFrame(config.traced_frame);
    D3D12::SetArrayPadding(config.array_pad);
    if (config.debug_layer) {
        Settings::values.renderer_debug = true;
        WriteDiag("step: D3D12 debug layer requested");
    }
    WriteDiag(surface.core_window != nullptr && !config.null_renderer
                  ? "step: logging up, renderer Direct3D12 on a " + std::to_string(surface.width) +
                        "x" + std::to_string(surface.height) + " CoreWindow"
                  : std::string("step: logging up, renderer Null"));

    Core::System system{};
    WriteDiag("step: Core::System constructed");
    system.Initialize();
    WriteDiag("step: system.Initialize() done");
    system.ApplySettings();
    WriteDiag("step: system.ApplySettings() done");

    HeadlessEmuWindow emu_window{surface.core_window, surface.width, surface.height};

    // Before Load, so HID starts with player 1 connected and bound to the gamepad engine.
    GamepadInput input{config.input_script};
    system.HIDCore().ReloadInputDevices();
    const auto shutdown = [&] {
        input.Stop();
        Kernel::Svc::SetDebugStringObserver(nullptr); // detach before teardown
        void(system.Pause());
        system.ShutdownMainProcess();
        system.HIDCore().UnloadInputDevices();
        WriteDiag("step: shutdown complete");
    };

    // Filesystem + content plumbing, as yuzu_cmd does it, plus the manual content provider the Qt
    // and Android frontends fill from their game lists: a game dump loaded straight from its file
    // is only found through it, and without it the game has no control data (NACP), so no save
    // data sizes, and aborts.
    FileSys::ManualContentProvider manual_provider;
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                   &manual_provider);
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();
    if (const auto file = system.GetFilesystem()->OpenFile(nro_path, FileSys::OpenMode::Read);
        file != nullptr && manual_provider.AddEntriesFromContainer(file)) {
        WriteDiag("step: game contents registered");
    }
    WriteDiag("step: filesystem factories created");

    // As yuzu_cmd. Without the Application applet id, AM never sends the focus messages and a game
    // waits for them forever after its first ReceiveMessage (homebrew does not wait).
    Service::AM::FrontendAppletParameters load_parameters{
        .applet_id = Service::AM::AppletId::Application,
    };
    const Core::SystemResultStatus load_result = system.Load(emu_window, nro_path, load_parameters);
    WriteDiag("step: system.Load() returned status " +
              std::to_string(static_cast<int>(load_result)) + " | " + MemoryReport());
    if (load_result != Core::SystemResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Headless boot: failed to load {} (status {})", nro_path,
                     static_cast<int>(load_result));
        // A failed Load leaves kernel objects behind; without ShutdownMainProcess the kernel's
        // teardown in ~System reads freed memory and crashes.
        shutdown();
        return 2;
    }

    // Install the JIT-liveness observer BEFORE running any guest code: it watches every guest
    // svcOutputDebugString chunk for the sentinel and signals the wait below. Cheap no-op for any
    // write that isn't the sentinel; zero cost in builds that never install an observer.
    std::mutex live_mutex;
    std::condition_variable live_cv;
    std::atomic<bool> jit_alive{false};
    std::atomic<bool> gfx_done{false};
    Kernel::Svc::SetDebugStringObserver([&](std::string_view chunk) {
        if (chunk.find(JIT_LIVENESS_SENTINEL) != std::string_view::npos) {
            jit_alive.store(true, std::memory_order_release);
            live_cv.notify_all();
        }
        if (chunk.find(GFX_DONE_SENTINEL) != std::string_view::npos) {
            gfx_done.store(true, std::memory_order_release);
            live_cv.notify_all();
        }
    });

    // Start the GPU host thread (null renderer — no device) and release the CPU manager.
    system.GPU().Start();
    WriteDiag("step: GPU host thread started");
    system.GetCpuManager().OnGpuReady();

    // Run the guest. CpuManager spins up guest threads; dynarmic compiles + executes their code.
    void(system.Run());
    input.Start();

    if (config.run_seconds > 0) {
        // Timed mode: nothing to wait for but the clock. The payload counts as having run if the
        // process is still alive when the time is up; the log says what it drew.
        WriteDiag("step: system.Run() issued, running for " + std::to_string(config.run_seconds) +
                  " s | " + MemoryReport());
        for (u32 second = 1; second <= config.run_seconds; ++second) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (second % 10 == 0) {
                WriteDiag("step: running, " + std::to_string(second) + " s | " + MemoryReport());
            }
        }
        shutdown();
        LOG_INFO(Frontend, "Headless boot: timed run of {} s finished.", config.run_seconds);
        return 0;
    }
    WriteDiag("step: system.Run() issued, waiting for the JIT sentinel | " + MemoryReport());

    // Headless: no window event loop. Wait for the guest to execute through the JIT and emit the
    // sentinel; the timeout is only a backstop (a hung/failed boot), not the success path.
    constexpr auto kLivenessTimeout = std::chrono::seconds(30);
    {
        std::unique_lock lock(live_mutex);
        live_cv.wait_for(lock, kLivenessTimeout,
                         [&] { return jit_alive.load(std::memory_order_acquire); });
    }
    const bool alive = jit_alive.load(std::memory_order_acquire);
    WriteDiag(std::string(alive ? "step: sentinel observed"
                                : "step: sentinel NOT observed within timeout") +
              " | " + MemoryReport());

    // With a real renderer the payload goes on to draw frames; keep the guest running so they reach
    // the screen, until it reports the loop finished (or the backstop expires).
    if (alive && surface.core_window != nullptr) {
        constexpr auto kGfxTimeout = std::chrono::seconds(30);
        {
            std::unique_lock lock(live_mutex);
            live_cv.wait_for(lock, kGfxTimeout,
                             [&] { return gfx_done.load(std::memory_order_acquire); });
        }
        WriteDiag(std::string(gfx_done.load() ? "step: framebuffer loop finished"
                                              : "step: framebuffer loop NOT finished in time") +
                  " | " + MemoryReport());
    }

    shutdown();

    if (alive) {
        LOG_INFO(Frontend, "Headless boot: JIT liveness CONFIRMED ('{}' observed).",
                 JIT_LIVENESS_SENTINEL);
        return 0;
    }
    LOG_CRITICAL(Frontend, "Headless boot: JIT-liveness sentinel '{}' not observed within timeout.",
                 JIT_LIVENESS_SENTINEL);
    return 3;
}

} // namespace EdenXbox

// ============================================================================================
// UWP entry point: a CoreApplication IFrameworkView whose Run() drives RunHeadlessBoot() against the
// homebrew NRO bundled in the package install location (Package.InstalledLocation\boot.nro). Reading
// the fixed GATE-2 payload from the read-only install dir keeps the MSIX self-contained — no
// Device-Portal file-push or LocalState chicken-and-egg. (Eden's log still writes to the writable
// LocalFolder; see common/fs/path_util.cpp under YUZU_UWP_APPCONTAINER.)
// ============================================================================================
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <windows.h> // OutputDebugStringA/W + ::Sleep (sets the target-arch macros winnt.h needs)

#include "common/dynamic_library.h"

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

namespace {

// Best-effort startup diagnostics that survive an early crash (before Eden's own logging is up):
// append to a pullable file in the app's LocalFolder AND emit on the debugger channel. This is how we
// see *where* the headless boot fails on-console when no eden_log.txt and no crash dump are produced.
//
// Every line carries milliseconds since launch, so a hang (steps stop, heartbeats continue) reads
// differently from a crash (steps stop, a CRASH line follows, or nothing at all).
//
// The diag path is resolved once, on the UI thread before the boot worker exists, and cached: the
// crash handlers below must not call into WinRT from a dying process.
std::string g_diag_path;
std::mutex g_diag_mutex;

unsigned long long ElapsedMs() {
    static const ULONGLONG start = GetTickCount64();
    return GetTickCount64() - start;
}

void WriteDiag(const std::string& msg) {
    const std::string line = "[eden-uwp] [+" + std::to_string(ElapsedMs()) + "ms] " + msg + "\n";
    OutputDebugStringA(line.c_str());
    try {
        std::scoped_lock lock{g_diag_mutex};
        if (g_diag_path.empty()) {
            g_diag_path =
                winrt::to_string(Windows::Storage::ApplicationData::Current().LocalFolder().Path()) +
                "\\eden_uwp_diag.txt";
        }
        std::ofstream f(g_diag_path, std::ios::app);
        f << line;
    } catch (...) {
        OutputDebugStringW(L"[eden-uwp] WriteDiag: could not write diag file\n");
    }
}

// The title's memory budget as the OS enforces it: Microsoft documents Game-mode UWP titles as capped
// (5 GB on the Xbox resource page), and exceeding it makes allocations fail rather than paging.
std::string MemoryReport() {
    try {
        using winrt::Windows::System::MemoryManager;
        return "app memory " + std::to_string(MemoryManager::AppMemoryUsage() >> 20) + " MiB of " +
               std::to_string(MemoryManager::AppMemoryUsageLimit() >> 20) + " MiB limit";
    } catch (...) {
        return "app memory: MemoryManager unavailable";
    }
}

// Last-chance writer for the crash handlers: no WinRT, no lock (the faulting thread may hold it),
// no allocation beyond what fopen needs.
void WriteDiagRaw(const char* line, bool debugger_channel = true) {
    if (debugger_channel) {
        OutputDebugStringA(line); // raises DBG_PRINTEXCEPTION_C internally: never from the VEH
    }
    if (g_diag_path.empty()) {
        return;
    }
    std::FILE* f = nullptr;
    if (fopen_s(&f, g_diag_path.c_str(), "a") == 0 && f != nullptr) {
        std::fputs(line, f);
        std::fclose(f);
    }
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

// Unhandled SEH (access violation, illegal instruction, breakpoint from a soft assert, ...) on any
// thread. /EHsc catch(...) does not see these, which is why the boot worker's handlers stay silent.
// Reports the faulting address as an RVA into eden-uwp.exe so it can be resolved with the build's PDB.
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
    const auto addr = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);

    char where[64];
    if (addr >= base && addr < base + nt->OptionalHeader.SizeOfImage) {
        std::snprintf(where, sizeof(where), "eden-uwp.exe+0x%llx",
                      static_cast<unsigned long long>(addr - base));
    } else {
        std::snprintf(where, sizeof(where), "0x%llx (outside eden-uwp.exe: JIT code or a DLL)",
                      static_cast<unsigned long long>(addr));
    }

    char line[320];
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        const char* what = kind == 0 ? "read of" : kind == 1 ? "write to" : "execute at";
        std::snprintf(line, sizeof(line),
                      "[eden-uwp] [+%llums] CRASH: access violation at %s (%s 0x%llx), thread %lu\n",
                      ElapsedMs(), where, what,
                      static_cast<unsigned long long>(rec->ExceptionInformation[1]),
                      GetCurrentThreadId());
    } else {
        std::snprintf(line, sizeof(line),
                      "[eden-uwp] [+%llums] CRASH: exception 0x%08lx at %s, thread %lu\n",
                      ElapsedMs(), static_cast<unsigned long>(rec->ExceptionCode), where,
                      GetCurrentThreadId());
    }
    WriteDiagRaw(line);
    Common::Log::Stop(); // flush eden_log.txt
    return EXCEPTION_CONTINUE_SEARCH; // let the OS finish the crash (and WER take its dump)
}

// Eden's fatal ASSERT/UNREACHABLE and the default std::terminate both end in abort(). SIGABRT is
// process-wide in the UCRT, so this sees it from any thread.
void OnAbort(int) {
    char line[200];
    std::snprintf(line, sizeof(line),
                  "[eden-uwp] [+%llums] CRASH: abort() on thread %lu - fatal ASSERT/UNREACHABLE "
                  "(see eden\\log\\eden_log.txt) or an exception escaped a thread\n",
                  ElapsedMs(), GetCurrentThreadId());
    WriteDiagRaw(line);
}

// Terminate handlers are per-thread in the MSVC runtime; installed on the boot worker, this names the
// exception that escaped (e.g. std::bad_alloc from the 4 GiB backing reservation).
[[noreturn]] void OnTerminate() {
    char line[320];
    const char* what = "unknown (not a std::exception)";
    std::string msg;
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            msg = ex.what();
            what = msg.c_str();
        } catch (...) {
        }
    } else {
        what = "no active exception (std::terminate called directly)";
    }
    std::snprintf(line, sizeof(line), "[eden-uwp] [+%llums] CRASH: std::terminate: %s\n",
                  ElapsedMs(), what);
    WriteDiagRaw(line);
    std::abort();
}

// ---- First-chance logging ------------------------------------------------------------------
// The unhandled filter above never runs for a fault inside JIT code when the unwinder cannot walk
// back out of it: the process just disappears (observed on-console: log stops ~300 ms after
// system.Run(), no CRASH line, no heartbeat). A vectored handler runs BEFORE any unwinding, so it
// sees every fault. It is installed LAST in the chain, so the demand-commit handlers (DRAM backing,
// VirtualBuffer) resolve their own expected faults first and never reach it.
std::atomic<int> g_first_chance_lines{0};
constexpr int MAX_FIRST_CHANCE_LINES = 32;

const char* ProtectName(DWORD protect) {
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:          return "NOACCESS";
    case PAGE_READONLY:          return "R";
    case PAGE_READWRITE:         return "RW";
    case PAGE_WRITECOPY:         return "WC";
    case PAGE_EXECUTE:           return "X";
    case PAGE_EXECUTE_READ:      return "RX";
    case PAGE_EXECUTE_READWRITE: return "RWX";
    case PAGE_EXECUTE_WRITECOPY: return "XWC";
    default:                     return "?";
    }
}

// "eden-uwp.exe+0xRVA" inside the image; otherwise the region's state/type/protection, which is what
// tells JIT code (private RX), a code page left RW (W^X failure) and unmapped memory apart.
void DescribeAddress(std::uintptr_t addr, char* out, std::size_t out_size) {
    const auto base = reinterpret_cast<std::uintptr_t>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
    if (addr >= base && addr < base + nt->OptionalHeader.SizeOfImage) {
        std::snprintf(out, out_size, "eden-uwp.exe+0x%llx",
                      static_cast<unsigned long long>(addr - base));
        return;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
        std::snprintf(out, out_size, "0x%llx [VirtualQuery failed]",
                      static_cast<unsigned long long>(addr));
        return;
    }
    const char* state = mbi.State == MEM_COMMIT    ? "commit"
                        : mbi.State == MEM_RESERVE ? "reserved"
                                                   : "free";
    const char* type = mbi.Type == MEM_PRIVATE ? "private"
                       : mbi.Type == MEM_MAPPED ? "mapped"
                       : mbi.Type == MEM_IMAGE  ? "image"
                                                : "-";
    std::snprintf(out, out_size, "0x%llx [%s %s %s, alloc base 0x%llx]",
                  static_cast<unsigned long long>(addr), state, type,
                  mbi.State == MEM_COMMIT ? ProtectName(mbi.Protect) : "-",
                  static_cast<unsigned long long>(
                      reinterpret_cast<std::uintptr_t>(mbi.AllocationBase)));
}

// C++ throws are not failures by themselves, but one that escapes a thread other than the boot
// worker ends the process through a per-thread terminate handler nobody installed: no CRASH line,
// just a WER event. Name them (type and what()) so the log says what was thrown.
std::atomic<int> g_cxx_throw_lines{0};
constexpr int MAX_CXX_THROW_LINES = 16;

void LogCxxThrow(const EXCEPTION_RECORD* rec) {
    // MSVC x64 throw: [1] = thrown object, [2] = ThrowInfo, [3] = image base of the RVAs.
    if (rec->NumberParameters < 4 ||
        g_cxx_throw_lines.fetch_add(1, std::memory_order_relaxed) >= MAX_CXX_THROW_LINES) {
        return;
    }
    const auto object = static_cast<std::uintptr_t>(rec->ExceptionInformation[1]);
    const auto* throw_info = reinterpret_cast<const s32*>(rec->ExceptionInformation[2]);
    const auto image = static_cast<std::uintptr_t>(rec->ExceptionInformation[3]);
    if (throw_info == nullptr || image == 0) {
        return;
    }
    // ThrowInfo { attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray }
    const auto* types = reinterpret_cast<const s32*>(image + throw_info[3]);
    const char* first_name = "?";
    const char* what = "";
    for (s32 i = 0; i < types[0]; ++i) {
        // CatchableType { properties, pType, mdisp, pdisp, vdisp, sizeOrOffset, copyFunction }
        const auto* type = reinterpret_cast<const s32*>(image + types[1 + i]);
        // TypeDescriptor { pVFTable, spare, name[] }
        const char* name = reinterpret_cast<const char*>(image + type[1] + 2 * sizeof(void*));
        if (i == 0) {
            first_name = name;
        }
        if (std::string_view{name} == ".?AVexception@std@@" && object != 0) {
            what = reinterpret_cast<const std::exception*>(object + type[2])->what();
        }
    }
    char line[512];
    std::snprintf(line, sizeof(line), "[eden-uwp] [+%llums] C++ throw %s: %s | thread %lu\n",
                  ElapsedMs(), first_name, what, GetCurrentThreadId());
    WriteDiagRaw(line, /*debugger_channel=*/false);
}

LONG NTAPI FirstChanceLogger(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    switch (rec->ExceptionCode) {
    case 0xE06D7363: // C++ throw
        LogCxxThrow(rec);
        return EXCEPTION_CONTINUE_SEARCH;
    case 0x406D1388: // SetThreadDescription-by-exception (thread naming)
    case 0x40010006: // DBG_PRINTEXCEPTION_C (OutputDebugStringA)
    case 0x4001000A: // DBG_PRINTEXCEPTION_WIDE_C (OutputDebugStringW)
        return EXCEPTION_CONTINUE_SEARCH;
    default:
        break;
    }
    if (g_first_chance_lines.fetch_add(1, std::memory_order_relaxed) >= MAX_FIRST_CHANCE_LINES) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    char at[160];
    DescribeAddress(reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress), at, sizeof(at));

    char line[512];
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        const char* what = kind == 0 ? "read of" : kind == 1 ? "write to" : "execute at";
        char target[160];
        DescribeAddress(static_cast<std::uintptr_t>(rec->ExceptionInformation[1]), target,
                        sizeof(target));
        std::snprintf(line, sizeof(line),
                      "[eden-uwp] [+%llums] FIRST-CHANCE access violation: %s %s | at %s | "
                      "thread %lu\n",
                      ElapsedMs(), what, target, at, GetCurrentThreadId());
    } else if ((rec->ExceptionCode == 0xE0DA0001 || rec->ExceptionCode == 0xE0DA0002) &&
               rec->NumberParameters >= 4) {
        std::snprintf(line, sizeof(line),
                      "[eden-uwp] [+%llums] JIT MEMORY: %s failed, GetLastError=%llu, base 0x%llx, "
                      "size 0x%llx, protection %s | thread %lu\n",
                      ElapsedMs(),
                      rec->ExceptionCode == 0xE0DA0001 ? "VirtualProtectFromApp"
                                                       : "VirtualAllocFromApp(MEM_COMMIT)",
                      static_cast<unsigned long long>(rec->ExceptionInformation[0]),
                      static_cast<unsigned long long>(rec->ExceptionInformation[1]),
                      static_cast<unsigned long long>(rec->ExceptionInformation[2]),
                      ProtectName(static_cast<DWORD>(rec->ExceptionInformation[3])),
                      GetCurrentThreadId());
    } else {
        std::snprintf(line, sizeof(line),
                      "[eden-uwp] [+%llums] FIRST-CHANCE exception 0x%08lx | at %s | params %lu "
                      "[0x%llx 0x%llx] | thread %lu\n",
                      ElapsedMs(), static_cast<unsigned long>(rec->ExceptionCode), at,
                      static_cast<unsigned long>(rec->NumberParameters),
                      static_cast<unsigned long long>(
                          rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : 0),
                      static_cast<unsigned long long>(
                          rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0),
                      GetCurrentThreadId());
    }
    WriteDiagRaw(line, /*debugger_channel=*/false);
    return EXCEPTION_CONTINUE_SEARCH; // observe only
}

// Resolved by name, never imported: importing the errorhandling-l1-1-1 api-set makes the app fail
// to activate on-console (see host_memory.cpp).
void InstallFirstChanceLogger() {
    using PFN_AddVectoredExceptionHandler = PVOID(WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    static Common::DynamicLibrary kernelbase("Kernelbase");
    PFN_AddVectoredExceptionHandler add_veh{};
    if (kernelbase.IsOpen() && kernelbase.GetSymbol("AddVectoredExceptionHandler", &add_veh) &&
        add_veh(/*first=*/0, FirstChanceLogger) != nullptr) {
        WriteDiag("first-chance exception logger installed");
    } else {
        WriteDiag("WARNING: could not install the first-chance exception logger");
    }
}

void InstallCrashHandlers() {
    InstallFirstChanceLogger();
    SetUnhandledExceptionFilter(OnUnhandledException);
    std::signal(SIGABRT, OnAbort);
    std::set_terminate(OnTerminate);
}

// ---- User data seeding ------------------------------------------------------------------------
// The user's own keys, firmware and game dumps reach the console inside a local-only package
// (package-appx.ps1 -Keys/-Firmware/-Game, never the repo), under InstalledLocation\userdata. Eden
// reads keys and firmware from its writable user dir, so they are copied into LocalState once;
// LocalState survives package updates, so later packages can leave them out.
//   userdata\keys\*      -> LocalState\eden\keys
//   userdata\firmware\*  -> LocalState\eden\nand\system\Contents\registered
//   userdata\games\*     -> LocalState\games
// A file is copied when it is missing or its size differs.
void SeedDirectory(const std::filesystem::path& from, const std::filesystem::path& to,
                   const char* what) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(from, ec)) {
        return;
    }
    fs::create_directories(to, ec);
    u32 copied = 0;
    u32 kept = 0;
    u64 bytes = 0;
    const ULONGLONG start = GetTickCount64();
    for (const fs::directory_entry& entry : fs::directory_iterator(from, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const fs::path target = to / entry.path().filename();
        const u64 size = entry.file_size(ec);
        std::error_code size_ec;
        if (fs::exists(target, size_ec) && fs::file_size(target, size_ec) == size && !size_ec) {
            ++kept;
            continue;
        }
        std::error_code copy_ec;
        fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            WriteDiag(std::string("seed ") + what + ": FAILED copying " +
                      entry.path().filename().string() + ": " + copy_ec.message());
            continue;
        }
        ++copied;
        bytes += size;
    }
    WriteDiag(std::string("seed ") + what + ": " + std::to_string(copied) + " copied (" +
              std::to_string(bytes >> 20) + " MiB in " + std::to_string(GetTickCount64() - start) +
              " ms), " + std::to_string(kept) + " already there");
}

void SeedUserData(const std::filesystem::path& install, const std::filesystem::path& local) {
    const std::filesystem::path userdata = install / "userdata";
    SeedDirectory(userdata / "keys", local / "eden" / "keys", "keys");
    SeedDirectory(userdata / "firmware", local / "eden" / "nand" / "system" / "Contents" / "registered",
                  "firmware");
    SeedDirectory(userdata / "games", local / "games", "games");
}

struct BootView : implements<BootView, IFrameworkViewSource, IFrameworkView> {
    IFrameworkView CreateView() {
        return *this;
    }
    void Initialize(CoreApplicationView const&) {}
    void SetWindow(CoreWindow const& window) {
        m_window = window;
    }
    void Load(hstring const&) {}
    void Uninitialize() {}

    void Run() {
        WriteDiag("BootView::Run entered"); // also resolves + caches the diag path
        WriteDiag(MemoryReport());
        InstallCrashHandlers();

        // A UWP app MUST activate its CoreWindow and pump the dispatcher, or the OS terminates it a
        // couple seconds after launch (no crash, no dump - exactly the "flashes then closes" symptom).
        // The hello-world/triangle apps survive because they render (activate + pump); this headless
        // boot did neither. Activate the (blank, Null-renderer) window, run the blocking boot on a
        // worker thread, and keep the UI thread pumping so the OS sees an activated, responsive app.
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();
        // On Xbox the gamepad's B also raises "back"; left unhandled at the root it can send the
        // app to the background. B is a game button here (uwp_input.h).
        SystemNavigationManager::GetForCurrentView().BackRequested(
            [](auto&&, BackRequestedEventArgs const& args) { args.Handled(true); });

        // The swapchain is sized in physical pixels: CoreWindow bounds are in view pixels (DIPs).
        EdenXbox::BootSurface surface{};
        if (m_window) {
            surface.core_window = winrt::get_abi(m_window); // an IInspectable, so a valid IUnknown*
            try {
                const auto bounds = m_window.Bounds();
                const double scale = Windows::Graphics::Display::DisplayInformation::
                                         GetForCurrentView()
                                             .RawPixelsPerViewPixel();
                surface.width = static_cast<u32>(bounds.Width * scale + 0.5);
                surface.height = static_cast<u32>(bounds.Height * scale + 0.5);
            } catch (...) {
                WriteDiag("could not read the window size, assuming 1920x1080");
            }
        }

        std::atomic<bool> done{false};
        std::thread worker([&done, surface]() {
            std::set_terminate(OnTerminate); // per-thread in the MSVC runtime
            std::string nro_path;
            EdenXbox::BootConfig config{};
            try {
                // Bundled NRO from the read-only package install location.
                const std::string install_path = winrt::to_string(
                    Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
                nro_path = install_path + "\\boot.nro";
                WriteDiag("resolved NRO path: " + nro_path);
                std::ifstream cfg{install_path + "\\boot.cfg"};
                for (std::string line; std::getline(cfg, line);) {
                    constexpr std::string_view key = "run_seconds=";
                    if (line.starts_with(key)) {
                        config.run_seconds =
                            static_cast<u32>(std::strtoul(line.c_str() + key.size(), nullptr, 10));
                        WriteDiag("boot.cfg: run " + std::to_string(config.run_seconds) + " s");
                    } else if (line == "debug_layer=1") {
                        config.debug_layer = true;
                        WriteDiag("boot.cfg: D3D12 debug layer");
                    } else if (line.starts_with("log_filter=")) {
                        config.log_filter = line.substr(11);
                        WriteDiag("boot.cfg: log filter " + config.log_filter);
                    } else if (line.starts_with("trace_frame=")) {
                        config.traced_frame =
                            static_cast<u32>(std::strtoul(line.c_str() + 12, nullptr, 10));
                        WriteDiag("boot.cfg: trace frame " + std::to_string(config.traced_frame));
                    } else if (line == "array_pad=1") {
                        config.array_pad = true;
                        WriteDiag("boot.cfg: D3D12 color arrays padded to a power of two layers");
                    } else if (line == "renderer=null") {
                        config.null_renderer = true;
                        WriteDiag("boot.cfg: Null renderer");
                    } else if (line.starts_with("input=")) {
                        if (auto step = EdenXbox::ParseInputStep(line.substr(6))) {
                            config.input_script.push_back(std::move(*step));
                            WriteDiag("boot.cfg: input " + line.substr(6));
                        } else {
                            WriteDiag("boot.cfg: IGNORED bad input line " + line.substr(6));
                        }
                    } else if (line.starts_with("game=")) {
                        config.game = line.substr(5);
                        WriteDiag("boot.cfg: game " + config.game);
                    }
                }
                const std::string local_path = winrt::to_string(
                    Windows::Storage::ApplicationData::Current().LocalFolder().Path());
                SeedUserData(std::filesystem::path{winrt::to_hstring(install_path).c_str()},
                             std::filesystem::path{winrt::to_hstring(local_path).c_str()});
                if (!config.game.empty()) {
                    nro_path = local_path + "\\games\\" + config.game;
                    if (config.run_seconds == 0) {
                        config.run_seconds = EdenXbox::DEFAULT_GAME_RUN_SECONDS;
                    }
                    WriteDiag("resolved game path: " + nro_path + ", running " +
                              std::to_string(config.run_seconds) + " s");
                }
            } catch (...) {
                WriteDiag("FAILED resolving Package.InstalledLocation");
            }
            // Capture any early throw to the diag file instead of a silent exit.
            try {
                WriteDiag("calling RunHeadlessBoot");
                const int rc = EdenXbox::RunHeadlessBoot(nro_path, surface, config);
                WriteDiag("RunHeadlessBoot returned " + std::to_string(rc));
            } catch (winrt::hresult_error const& e) {
                WriteDiag("winrt::hresult_error: " + winrt::to_string(e.message()));
            } catch (std::exception const& e) {
                WriteDiag(std::string("std::exception: ") + e.what());
            } catch (...) {
                WriteDiag("unknown exception in RunHeadlessBoot");
            }
            done.store(true);
        });

        // Heartbeat every 10 s: if steps stop but heartbeats continue, the boot hung rather than
        // crashed, and the last step names where.
        CoreDispatcher dispatcher = window.Dispatcher();
        ULONGLONG next_heartbeat = GetTickCount64() + 10'000;
        while (!done.load()) {
            dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            ::Sleep(50);
            if (GetTickCount64() >= next_heartbeat) {
                WriteDiag("heartbeat: boot worker still running | " + MemoryReport());
                next_heartbeat += 10'000;
            }
        }
        worker.join();
        WriteDiag("boot worker joined; exiting");
    }

private:
    CoreWindow m_window{nullptr};
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment();
    CoreApplication::Run(winrt::make<BootView>());
    return 0;
}
