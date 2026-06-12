// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Headless GATE-2 boot frontend for the Xbox/UWP AppContainer target.
//
// Brings up Core::System with the Null renderer + null audio sink, loads a homebrew NRO staged in the
// app's sandboxed local storage, runs it through the dynarmic JIT, and emits a deterministic
// JIT-liveness marker. No GPU device, no input, no audio device.
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
#include <mutex>
#include <string>
#include <string_view>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/kernel/svc/svc_debug_string.h" // Kernel::Svc::SetDebugStringObserver
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "video_core/gpu.h"

#include "eden_uwp/headless_emu_window.h"

namespace EdenXbox {

constexpr const char* JIT_LIVENESS_SENTINEL = "EDEN_XBOX_JIT_ALIVE";

// Force the renderer-independent, device-light configuration the headless boot needs.
static void ApplyHeadlessBootSettings() {
    Settings::values.renderer_backend = Settings::RendererBackend::Null; // no Vk/GL device created
    Settings::values.sink_id = Settings::AudioEngine::Null;              // audio_core/sink/null_sink
    Settings::values.cpuopt_fastmem = false;        // Phase-2: bounds-checked page-table path
    Settings::values.cpuopt_fastmem_exclusives = false;
    // memory_layout_mode stays at its default until the Series-S budget is measured on-console; the
    // DRAM clamp is a separate reservation follow-up, not here.
}

// Returns a process exit-style status. 0 == boot reached the run phase cleanly.
int RunHeadlessBoot(const std::string& nro_path) {
    Common::Log::Initialize();
    ApplyHeadlessBootSettings();

    Core::System system{};
    system.Initialize();
    system.ApplySettings();

    HeadlessEmuWindow emu_window{};

    // Filesystem + content plumbing, exactly as yuzu_cmd does it.
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();

    Service::AM::FrontendAppletParameters load_parameters{};
    const Core::SystemResultStatus load_result = system.Load(emu_window, nro_path, load_parameters);
    if (load_result != Core::SystemResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Headless boot: failed to load NRO {} (status {})", nro_path,
                     static_cast<int>(load_result));
        return 2;
    }

    // Install the JIT-liveness observer BEFORE running any guest code: it watches every guest
    // svcOutputDebugString chunk for the sentinel and signals the wait below. Cheap no-op for any
    // write that isn't the sentinel; zero cost in builds that never install an observer.
    std::mutex live_mutex;
    std::condition_variable live_cv;
    std::atomic<bool> jit_alive{false};
    Kernel::Svc::SetDebugStringObserver([&](std::string_view chunk) {
        if (chunk.find(JIT_LIVENESS_SENTINEL) != std::string_view::npos) {
            jit_alive.store(true, std::memory_order_release);
            live_cv.notify_all();
        }
    });

    // Start the GPU host thread (null renderer — no device) and release the CPU manager.
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();

    // Run the guest. CpuManager spins up guest threads; dynarmic compiles + executes their code.
    void(system.Run());

    // Headless: no window event loop. Wait for the guest to execute through the JIT and emit the
    // sentinel; the timeout is only a backstop (a hung/failed boot), not the success path.
    constexpr auto kLivenessTimeout = std::chrono::seconds(30);
    {
        std::unique_lock lock(live_mutex);
        live_cv.wait_for(lock, kLivenessTimeout,
                         [&] { return jit_alive.load(std::memory_order_acquire); });
    }
    const bool alive = jit_alive.load(std::memory_order_acquire);

    Kernel::Svc::SetDebugStringObserver(nullptr); // detach before teardown
    void(system.Pause());
    system.ShutdownMainProcess();

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
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

namespace {

struct BootView : implements<BootView, IFrameworkViewSource, IFrameworkView> {
    IFrameworkView CreateView() {
        return *this;
    }
    void Initialize(CoreApplicationView const&) {}
    void SetWindow(CoreWindow const&) {}
    void Load(hstring const&) {}
    void Uninitialize() {}

    void Run() {
        // Read the bundled NRO from the package install location (read-only, always present once the
        // appx is installed). UTF-8 the WinRT path for Eden's filesystem layer.
        const auto install_path =
            Windows::ApplicationModel::Package::Current().InstalledLocation().Path();
        const std::string nro_path = winrt::to_string(install_path) + "\\boot.nro";
        EdenXbox::RunHeadlessBoot(nro_path);
    }
};

} // namespace

int __stdcall wWinMain(void*, void*, wchar_t*, int) {
    winrt::init_apartment();
    CoreApplication::Run(winrt::make<BootView>());
    return 0;
}
