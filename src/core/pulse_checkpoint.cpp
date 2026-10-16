// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <fmt/format.h>

#include "common/host_memory.h"
#include "common/logging.h"
#include "common/pulse_diagnostics.h"
#include "common/pulse_store.h"
#include "core/arm/exclusive_monitor.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_client_port.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_handle_table.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_object_name.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_scoped_scheduler_lock_and_sleep.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_shared_memory.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/pulse_wait_graph.h"
#include "core/hle/service/hid/hid_server.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/ns/platform_service_manager.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/memory.h"
#include "core/pulse_checkpoint.h"
#include "core/pulse_machine_state.h"
#include "hid_core/resource_manager.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace Core::Pulse {
namespace {

using Common::Pulse::Error;
using Common::Pulse::Failure;
using Kernel::KAutoObject;
using Kernel::KMemoryAttribute;
using Kernel::KMemoryPermission;
using Kernel::KMemoryState;
using Kernel::KProcess;
using Kernel::KThread;
using Kernel::ThreadState;

constexpr u64 PageSize = GuestPageSize;
constexpr std::array SectionOrder{ProcessSection::Process, ProcessSection::Objects,
                                  ProcessSection::Threads, ProcessSection::Memory,
                                  ProcessSection::Handles, ProcessSection::Waits,
                                  ProcessSection::Exclusive, ProcessSection::Timing};
constexpr u64 MaxMetadataSection = 64ULL << 20;
constexpr auto ServiceStartTimeout = std::chrono::seconds{5};

[[noreturn]] void Invalid(const char* reason) {
    throw Failure{Error::Invalid, reason};
}

// The emulated DRAM commits a page on its first touch, and the kernel decommits the pages it
// clears, so a game's mapped but unused memory costs the host nothing. Reading it would commit
// it: the memory section stores those pages as zero without touching them, and a restore leaves
// the zero pages it gets alone where the fresh boot never committed them either.
class UntouchedPages {
public:
    bool Contains(const u8* host) {
        if (!host) return false;
        if (!region || host < region->begin || host >= region->end) {
            region = Common::QueryDramRegion(host);
        }
        return region && region->untouched;
    }
    // A write commits the pages it lands on.
    void Wrote() {
        if (region && region->untouched) region.reset();
    }

private:
    std::optional<Common::DramRegion> region;
};

bool IsZero(const u8* data, size_t size) {
    for (size_t i = 0; i + sizeof(u64) <= size; i += sizeof(u64)) {
        u64 word;
        std::memcpy(&word, data + i, sizeof(word));
        if (word) return false;
    }
    for (size_t i = size & ~(sizeof(u64) - 1); i < size; ++i) {
        if (data[i]) return false;
    }
    return true;
}
[[noreturn]] void Incompatible(const std::string& reason) {
    throw Failure{Error::Incompatible, reason.c_str()};
}
[[noreturn]] void Reject(bool retryable, const std::string& reason) {
    throw CheckpointRejected{retryable, "Pulse: " + reason};
}
void Check(Result result, const char* reason) {
    if (result.IsError()) Incompatible(reason);
}

// Every block the guest can see, coalesced so that block-manager merge history (which can differ
// between a captured and a rebuilt process) does not change the comparison.
std::vector<MemoryBlock> EnumerateBlocks(KProcess& process) {
    std::vector<MemoryBlock> blocks;
    auto& table = process.GetPageTable();
    // QueryInfo answers outside the address space with one fake block up to 2^64: walk inside it.
    const u64 start = GetInteger(table.GetAddressSpaceStart());
    const u64 end = start + table.GetAddressSpaceSize();
    for (u64 address = start; address < end;) {
        Kernel::KMemoryInfo info{};
        Kernel::Svc::PageInfo page{};
        Check(table.QueryInfo(std::addressof(info), std::addressof(page), address),
              "Pulse: memory query failed");
        const auto svc_state = static_cast<Kernel::Svc::MemoryState>(static_cast<u32>(info.m_state) & 0xff);
        if (info.m_size == 0 || info.m_address > address || info.m_address + info.m_size <= address) {
            Incompatible("Pulse: inconsistent memory query");
        }
        if (svc_state != Kernel::Svc::MemoryState::Free &&
            svc_state != Kernel::Svc::MemoryState::Inaccessible) {
            MemoryBlock block{info.m_address, info.m_size, static_cast<u32>(info.m_state),
                              static_cast<u32>(info.m_permission),
                              static_cast<u32>(info.m_attribute)};
            if (!blocks.empty() && blocks.back().address + blocks.back().size == block.address &&
                blocks.back().state == block.state && blocks.back().permission == block.permission &&
                blocks.back().attribute == block.attribute) {
                blocks.back().size += block.size;
            } else {
                if (blocks.size() == MaxMemoryBlocks) Reject(false, "too many memory blocks");
                blocks.push_back(block);
            }
        }
        address = info.m_address + info.m_size;
    }
    return blocks;
}

bool UserReadable(u32 permission) {
    return (permission & static_cast<u32>(Kernel::Svc::MemoryPermission::Read)) != 0;
}

u64 Physical(KProcess& process, u64 address) {
    Kernel::KPhysicalAddress physical{};
    if (!process.GetPageTable().GetPhysicalAddress(std::addressof(physical), address)) {
        Reject(false, "unbacked guest page");
    }
    return GetInteger(physical);
}

// A set of DRAM pages, one bit each: a set of every page a game maps costs a few hundred KiB
// where a hash set of them cost tens of MiB at the moment of the save.
class PhysicalPages {
public:
    // False if the page was already in.
    bool Insert(u64 physical) {
        const u64 page = Index(physical);
        if (page / 64 >= words.size()) words.resize(page / 64 + 1);
        u64& word = words[page / 64];
        const u64 bit = u64{1} << (page % 64);
        if (word & bit) return false;
        word |= bit;
        return true;
    }
    bool Contains(u64 physical) const {
        const u64 page = Index(physical);
        return page / 64 < words.size() && (words[page / 64] >> (page % 64)) & 1;
    }

private:
    static u64 Index(u64 physical) {
        if (physical < Core::DramMemoryMap::Base) Reject(false, "guest page outside the DRAM");
        return (physical - Core::DramMemoryMap::Base) / PageSize;
    }
    std::vector<u64> words;
};

// Identities of the objects a captured process reaches, assigned in discovery order. Never the
// KAutoObject address: identities are graph references, not host pointers.
class Registry {
public:
    Registry(Kernel::KernelCore& kernel_, KProcess* process_) : kernel{kernel_}, process{process_} {
        Add(process, ObjectKind::Process, 0);
    }

    u64 AddThread(const KThread* thread) {
        const auto id = next++;
        ids.emplace(thread, id);
        return id;
    }

    u64 Identify(const KAutoObject* object) {
        if (!object) Reject(false, "null object reference");
        if (const auto found = ids.find(object); found != ids.end()) return found->second;
        auto* mutable_object = const_cast<KAutoObject*>(object);
        if (mutable_object->DynamicCast<KThread*>()) {
            Reject(false, "reference to a thread of another process");
        }
        if (auto* event = mutable_object->DynamicCast<Kernel::KEvent*>()) {
            return AddEvent(event).first;
        }
        if (auto* readable = mutable_object->DynamicCast<Kernel::KReadableEvent*>()) {
            if (!readable->GetParent()) Reject(false, "readable event without a kernel event");
            return AddEvent(readable->GetParent()).second;
        }
        if (auto* session = mutable_object->DynamicCast<Kernel::KClientSession*>()) {
            return Add(session, ObjectKind::ClientSession, 0);
        }
        // Their payloads are written after the locks: the owner lookup takes service locks.
        if (auto* shared = mutable_object->DynamicCast<Kernel::KSharedMemory*>()) {
            return Add(shared, ObjectKind::SharedMemory, 0);
        }
        if (auto* transfer = mutable_object->DynamicCast<Kernel::KTransferMemory*>()) {
            return Add(transfer, ObjectKind::TransferMemory, 0);
        }
        Reject(false, std::string{"no serializer for kernel object "} +
                          mutable_object->GetTypeObj().GetName());
    }

    std::vector<ObjectRecord>& Records() {
        return records;
    }
    std::vector<const KAutoObject*>& Objects() {
        return objects;
    }

    // A record with no kernel object behind it (a service interface).
    u64 AddRecord(ObjectKind kind) {
        if (records.size() == MaxKernelObjects) Reject(false, "too many kernel objects");
        const auto id = next++;
        records.push_back({id, kind, 0, 0, {}});
        objects.push_back(nullptr);
        return id;
    }

private:
    u64 Add(const KAutoObject* object, ObjectKind kind, u64 parent) {
        if (records.size() == MaxKernelObjects) Reject(false, "too many kernel objects");
        const auto id = next++;
        ids.emplace(object, id);
        records.push_back({id, kind, 0, parent, {}});
        objects.push_back(object);
        return id;
    }

    // An event a service owns is resolved after the lock, to the interface that exports it.
    std::pair<u64, u64> AddEvent(Kernel::KEvent* event) {
        auto* readable = std::addressof(event->GetReadableEvent());
        if (const auto found = ids.find(event); found != ids.end()) {
            return {found->second, ids.at(readable)};
        }
        // An event its service already closed has no signaller left: the guest's handle is all
        // that keeps it, so it is stored and rebuilt as an application event.
        const auto kind = event->GetOwner() == process ||
                                  Service::KernelHelpers::IsClosedServiceEvent(event)
                              ? ObjectKind::Event
                              : ObjectKind::ServiceEvent;
        const auto event_id = Add(event, kind, 0);
        return {event_id, Add(readable, ObjectKind::ReadableEvent, event_id)};
    }

    Kernel::KernelCore& kernel;
    KProcess* process;
    std::unordered_map<const KAutoObject*, u64> ids;
    std::vector<ObjectRecord> records;
    std::vector<const KAutoObject*> objects;
    u64 next{1};
};

Kernel::KClientPort* FindSmPort(Kernel::KernelCore& kernel) {
    auto port = Kernel::KObjectName::Find<Kernel::KClientPort>(kernel, "sm:");
    return port.GetPointerUnsafe();
}

Kernel::KClientPort* FindServicePort(Core::System& system, const std::string& name) {
    if (name == "sm:") return FindSmPort(system.Kernel());
    Kernel::KClientPort* port{};
    if (system.ServiceManager().GetServicePort(std::addressof(port), name).IsError()) return nullptr;
    return port;
}

std::string Join(const std::set<std::string>& names) {
    std::string out;
    for (const auto& name : names) out += (out.empty() ? "" : ", ") + name;
    return out;
}

} // namespace

struct ProcessRestore::State {
    ProcessLayout layout;
    std::vector<u64> run_ends;
    std::unordered_map<u64, KAutoObject*> objects;
    std::vector<KAutoObject*> created; // creation references, closed by Finish
    std::vector<std::pair<KThread*, ThreadRun>> threads;
};

// ---------------------------------------------------------------------------------- capture

namespace {

// What the GPU wrote and the host caches still hold goes back to guest memory, which the
// checkpoint stores; the caches themselves are rebuilt after a restore. The exact readback is
// tried first; a texture it cannot read back exactly (rescaled, aliased...) falls back to the
// renderer's flush of every range the GPU can reach, limited to the images the GPU wrote and
// the CPU has not overwritten since.
void FlushGpuWrites(Core::System& system) {
    std::vector<std::pair<DAddr, u64>> ranges;
    if (const auto nvdrv = system.ServiceManager().GetService<Service::Nvidia::NVDRV>("nvdrv:s",
                                                                                     true)) {
        ranges = nvdrv->GetModule()->GetContainer().GetNvMapFile().PulsePinnedRanges();
    }
    auto& gpu = system.GPU();
    std::string failure;
    gpu.RunOnGpuThread([&] {
        auto* const rasterizer = gpu.Renderer().ReadRasterizer();
        if (!rasterizer) return;
        try {
            if (rasterizer->MaterializeCheckpointMemory()) return;
        } catch (const std::exception& inexact) {
            LOG_INFO(Core, "Pulse: GPU writes flushed by region ({})", inexact.what());
        }
        try {
            for (const auto& [address, size] : ranges) {
                rasterizer->FlushCheckpointRegion(address, size);
            }
            rasterizer->FlushAll();
        } catch (const std::exception& error) {
            failure = error.what();
        }
    });
    if (!failure.empty()) Reject(false, "GPU writes cannot reach guest memory: " + failure);
}

// Shared memory is never stored: its owner fills it. A checkpoint names the owner and the restore
// maps the fresh System's memory of that owner at the captured addresses.
enum class SharedMemoryOwner : u32 {
    Font = 1,   // pl:u, refilled with the system font on restore
    Irs = 2,
    Time = 3,   // psc time contexts, written by the fresh time service
    HidBus = 4,
    Hid = 5,    // per applet resource user id (the argument)
};

std::shared_ptr<Service::HID::ResourceManager> HidResources(Core::System& system) {
    const auto hid = system.ServiceManager().GetService<Service::HID::IHidServer>("hid", true);
    return hid ? hid->GetResourceManager() : nullptr;
}

std::pair<SharedMemoryOwner, u64> SharedMemoryKey(Core::System& system,
                                                  const Kernel::KSharedMemory* shared) {
    auto& kernel = system.Kernel();
    if (shared == std::addressof(kernel.GetFontSharedMem())) return {SharedMemoryOwner::Font, 0};
    if (shared == std::addressof(kernel.GetIrsSharedMem())) return {SharedMemoryOwner::Irs, 0};
    if (shared == std::addressof(kernel.GetTimeSharedMem())) return {SharedMemoryOwner::Time, 0};
    if (shared == std::addressof(kernel.GetHidBusSharedMem())) return {SharedMemoryOwner::HidBus, 0};
    if (const auto hid = HidResources(system)) {
        if (const auto aruid = hid->PulseFindSharedMemory(shared)) {
            return {SharedMemoryOwner::Hid, *aruid};
        }
    }
    Reject(false, "shared memory of an owner without a serializer");
}

Kernel::KSharedMemory* ResolveSharedMemory(Core::System& system, u32 owner, u64 argument) {
    auto& kernel = system.Kernel();
    switch (static_cast<SharedMemoryOwner>(owner)) {
    case SharedMemoryOwner::Font:
        if (const auto pl = system.ServiceManager().GetService<Service::NS::IPlatformServiceManager>(
                "pl:u", true)) {
            pl->PulseFillSharedFont();
        }
        return std::addressof(kernel.GetFontSharedMem());
    case SharedMemoryOwner::Irs:
        return std::addressof(kernel.GetIrsSharedMem());
    case SharedMemoryOwner::Time:
        return std::addressof(kernel.GetTimeSharedMem());
    case SharedMemoryOwner::HidBus:
        return std::addressof(kernel.GetHidBusSharedMem());
    case SharedMemoryOwner::Hid:
        if (const auto hid = HidResources(system)) return hid->PulseSharedMemory(argument);
        return nullptr;
    default:
        Invalid("Pulse: unknown shared memory owner");
    }
}

void PutLe(std::vector<u8>& out, u64 value, size_t width) {
    for (size_t i = 0; i < width; ++i) out.push_back(static_cast<u8>(value >> (i * 8)));
}

} // Anonymous namespace

ProcessCheckpoint ProcessCheckpoint::CaptureAtBarrier(Core::System& system,
                                                    std::chrono::milliseconds budget,
                                                    u32& retries,
                                                    const std::function<void(const char*)>& phase) {
    const auto step = [&](const char* name) {
        if (phase) phase(name);
    };
    auto& kernel = system.Kernel();
    kernel.SetCheckpointBarrier(true);
    struct Lower {
        Kernel::KernelCore& kernel;
        ~Lower() {
            kernel.SetCheckpointBarrier(false);
        }
    } lower{kernel};
    const auto deadline = std::chrono::steady_clock::now() + budget;
    // A first short run lets the calls already inside the kernel return and park on the next one.
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    for (;;) {
        void(system.Pause());
        try {
            // With the guest parked nothing new is queued: the GPU finishes what it has, so the
            // channels are between command lists when nvdrv serializes them.
            step("idle");
            system.GPU().WaitForIdle();
            step("process");
            auto checkpoint = Capture(system);
            step("gpu");
            FlushGpuWrites(system);
            return checkpoint;
        } catch (const CheckpointRejected& rejected) {
            if (!rejected.retryable || std::chrono::steady_clock::now() >= deadline) {
                void(system.Run());
                throw;
            }
            ++retries;
        }
        void(system.Run());
        std::this_thread::sleep_for(std::chrono::microseconds{500});
    }
}

ProcessCheckpoint ProcessCheckpoint::Capture(Core::System& system) {
    if (!system.IsPaused()) Reject(false, "capture requires a paused system");
    auto& kernel = system.Kernel();
    auto* process = system.ApplicationProcess();
    // A32 and A64 share the context (ArmDynarmic32 maps r0-r15 and the CPSR onto it), the
    // exclusive monitor and the r0/r1 results of the waits a restore completes.
    if (!process || process->GetState() != KProcess::State::Running) {
        Reject(false, "no running application process");
    }

    ProcessCheckpoint checkpoint;
    checkpoint.process = process;
    Registry registry{kernel, process};
    std::vector<ThreadRecord> threads;
    std::vector<KThread*> waiting;
    Kernel::HandleTableCheckpoint handles;
    std::vector<Kernel::CheckpointWaitingThread> waits;
    ExclusiveMonitorCheckpoint exclusive;
    {
        Kernel::KScopedLightLock list_lock{process->GetListLock()};
        Kernel::KScopedSchedulerLock scheduler_lock{kernel};
        for (auto& thread : process->GetThreadList()) {
            if (threads.size() == MaxGuestThreads) Reject(false, "too many guest threads");
            const auto state = thread.GetState();
            // Exited and signaled is final; still unwinding (or asked to exit) settles soon.
            const bool exited = state == ThreadState::Terminated && thread.IsSignaled(kernel);
            if (!exited && (state == ThreadState::Terminated || thread.IsTerminationRequested())) {
                Reject(true, "a guest thread is exiting");
            }
            // IsInExceptionHandler() is not checked: Eden sets it on every thread at creation and
            // never clears it, so it carries no state (rebuilt threads get it the same way).
            if (thread.GetStackParameters().is_pinned) Reject(true, "a guest thread is pinned");
            for (auto type : {Kernel::SuspendType::Process, Kernel::SuspendType::Debug,
                              Kernel::SuspendType::Backtrace}) {
                if (thread.IsSuspendRequested(type)) {
                    Reject(false, "a guest thread is suspended by a debugger or the process");
                }
            }
            ThreadRecord record{};
            record.id = registry.AddThread(std::addressof(thread));
            record.thread_id = thread.GetThreadId();
            record.tls = GetInteger(thread.GetTlsAddress());
            record.flags = thread.IsSuspendRequested(Kernel::SuspendType::Thread) ? 1 : 0;
            record.base_priority = thread.GetBasePriority();
            record.context = EncodeContext(thread.GetContext());
            Check(thread.GetCoreMask(kernel, std::addressof(record.ideal_core),
                                     std::addressof(record.affinity)),
                  "Pulse: thread core mask");
            switch (state) {
            case ThreadState::Terminated:
                record.run = ThreadRun::Terminated;
                break;
            case ThreadState::Initialized:
                record.run = ThreadRun::Initialized;
                break;
            case ThreadState::Runnable:
                if (thread.IsCallingSvc()) Reject(true, "a guest thread is inside an SVC");
                record.run = ThreadRun::Runnable;
                break;
            case ThreadState::Waiting:
                if (thread.IsCheckpointParked()) {
                    // Held by the barrier before its SVC ran: resumed, it issues the call again.
                    record.run = ThreadRun::Runnable;
                    break;
                }
                if (!thread.IsCallingSvc() ||
                    thread.GetWaitReasonForDebugging() == Kernel::ThreadWaitReasonForDebugging::IPC) {
                    Reject(true, "a guest thread is waiting on IPC or a kernel lock");
                }
                record.run = ThreadRun::Waiting;
                waiting.push_back(std::addressof(thread));
                break;
            default:
                Reject(false, "a guest thread is in an unknown state");
            }
            if (!record.tls) Reject(false, "a guest thread has no thread-local region");
            threads.push_back(record);
        }
        if (threads.empty()) Reject(false, "the application has no threads");

        const Kernel::IdentifyCheckpointObject identify = [&](const KAutoObject* object) {
            return registry.Identify(object);
        };
        handles = process->GetHandleTable().CaptureCheckpoint(kernel, identify);
        try {
            waits = Kernel::CaptureCheckpointWaitGraph(
                kernel, waiting, [&](const Kernel::KSynchronizationObject* object) {
                    return registry.Identify(object);
                });
        } catch (const std::invalid_argument& error) {
            // A wait the graph cannot express yet (a kernel lock inside an SVC): transient.
            Reject(true, error.what());
        } catch (const std::runtime_error& error) {
            std::string reasons;
            for (const auto* thread : waiting) {
                reasons += fmt::format(" {}", static_cast<u32>(thread->GetWaitReasonForDebugging()));
            }
            Reject(false, std::string{error.what()} + " (wait reasons" + reasons + ")");
        }
        for (size_t i = 0; i < registry.Records().size(); ++i) {
            if (registry.Records()[i].kind != ObjectKind::ReadableEvent) continue;
            const auto* readable = static_cast<const Kernel::KReadableEvent*>(registry.Objects()[i]);
            registry.Records()[i].flags = readable->IsSignaled(kernel) ? 1 : 0;
        }
        exclusive = process->GetExclusiveMonitor().CaptureCheckpoint();
    }

    // Memory objects the process uses without holding a handle: the shared memory it maps (a
    // handle closed after mapping) and transfer memory only a service still references.
    for (auto* shared : process->GetMappedSharedMemory()) registry.Identify(shared);
    for (auto* object : kernel.ObjectListContainer().GetOwned(process)) {
        if (auto* transfer = object->DynamicCast<Kernel::KTransferMemory*>()) {
            registry.Identify(transfer);
        }
    }
    std::unordered_map<u64, size_t> shared_by_physical; // physical start -> record index
    PhysicalPages lent_pages;                           // physical pages of transfer memory
    for (size_t i = 0; i < registry.Records().size(); ++i) {
        auto& record = registry.Records()[i];
        if (record.kind == ObjectKind::TransferMemory) {
            const auto* transfer = static_cast<const Kernel::KTransferMemory*>(registry.Objects()[i]);
            if (transfer->GetOwner() != process) Reject(false, "transfer memory of another process");
            if (transfer->IsMapped()) Reject(false, "transfer memory mapped by another process");
            const u64 address = GetInteger(transfer->GetSourceAddress());
            const u64 size = transfer->GetSize();
            PutLe(record.payload, address, 8);
            PutLe(record.payload, size, 8);
            PutLe(record.payload, static_cast<u32>(transfer->GetOwnerPermission()), 4);
            for (u64 page = 0; page < size; page += PageSize) {
                lent_pages.Insert(Physical(*process, address + page));
            }
        } else if (record.kind == ObjectKind::SharedMemory) {
            const auto* shared = static_cast<const Kernel::KSharedMemory*>(registry.Objects()[i]);
            const auto [owner, argument] = SharedMemoryKey(system, shared);
            PutLe(record.payload, static_cast<u32>(owner), 4);
            PutLe(record.payload, 0, 4); // mapping count, set with the memory map
            PutLe(record.payload, argument, 8);
            shared_by_physical.emplace(GetInteger(shared->GetPhysicalAddress()), i);
        }
    }

    // Sessions and the service interfaces behind them. Every interface the guest can reach needs
    // a serializer; the rejection names all that are missing at once.
    struct HandlerEntry {
        size_t record;
        Service::SessionRequestHandlerPtr handler;
        Service::ServerManager* server;
        std::string port; // set for a root: the port of the session that starts with it
    };
    std::vector<HandlerEntry> handlers;
    std::unordered_map<const Service::SessionRequestHandler*, size_t> handler_index;
    std::unordered_map<Service::ServerManager*, std::string> server_ports;
    std::unordered_map<const Service::SessionRequestManager*, u64> managers; // -> owning session
    std::set<std::string> missing;
    std::set<std::string> refused;
    bool refused_retryable = true;
    const auto add_handler = [&](const Service::SessionRequestHandlerPtr& handler,
                                 Service::ServerManager* server) -> u64 {
        if (const auto found = handler_index.find(handler.get()); found != handler_index.end()) {
            return registry.Records()[handlers[found->second].record].id;
        }
        const auto id = registry.AddRecord(ObjectKind::ServiceHandler);
        handler_index.emplace(handler.get(), handlers.size());
        handlers.push_back({registry.Records().size() - 1, handler, server, {}});
        if (handler->PulseKind().empty()) missing.insert(handler->PulseName());
        return id;
    };
    auto* sm_port = FindSmPort(kernel);
    // Port sessions first, so a clone always names the session that connected.
    std::vector<size_t> session_order;
    for (const bool ported : {true, false}) {
        for (size_t i = 0; i < registry.Records().size(); ++i) {
            if (registry.Records()[i].kind != ObjectKind::ClientSession) continue;
            const auto* client = static_cast<const Kernel::KClientSession*>(registry.Objects()[i]);
            const auto* session = client->GetParent();
            if ((session && session->GetParent()) == ported) session_order.push_back(i);
        }
    }
    for (const size_t i : session_order) {
        const auto* client = static_cast<const Kernel::KClientSession*>(registry.Objects()[i]);
        auto* session = client->GetParent();
        if (!session || session->IsServerClosed()) Reject(false, "IPC session whose server is closed");
        const auto* port = session->GetParent();
        Service::Pulse::SessionRecord session_record;
        if (port) {
            session_record.port = port == sm_port ? "sm:" : system.ServiceManager().FindServiceName(port);
            if (session_record.port.empty()) Reject(false, "IPC session to an unregistered port");
        }
        const auto manager =
            Service::ServerManager::FindSessionManager(std::addressof(session->GetServerSession()));
        if (!manager) Reject(true, "IPC session is not registered with its server yet");
        if (const auto owner = managers.find(manager.get()); owner != managers.end()) {
            if (port) Reject(false, "two port sessions share one service session");
            session_record.clone_of = owner->second;
            registry.Records()[i].payload = Service::Pulse::EncodeSessionRecord(session_record);
            continue;
        }
        managers.emplace(manager.get(), registry.Records()[i].id);
        auto* server = std::addressof(manager->GetServerManager());
        if (port) server_ports.try_emplace(server, session_record.port);
        Service::SessionManagerCheckpoint graph;
        try {
            graph = manager->CaptureCheckpoint([&](const Service::SessionRequestHandlerPtr& handler) {
                return add_handler(handler, server);
            });
        } catch (const std::exception& error) {
            Reject(false, error.what());
        }
        if (port && graph.session_handler) {
            auto& root = handlers[handler_index.at(std::addressof(manager->SessionHandler()))];
            if (!root.port.empty() && root.port != session_record.port) {
                Reject(false, "one service interface is the root of two ports");
            }
            root.port = session_record.port;
        }
        session_record.graph = Service::Pulse::EncodeSessionGraph(graph);
        registry.Records()[i].payload = Service::Pulse::EncodeSessionRecord(session_record);
    }
    // Interface state, in discovery order; a reference discovers the interface it names.
    std::unordered_map<const Kernel::KEvent*, std::pair<u64, u32>> exported;
    for (size_t h = 0; h < handlers.size(); ++h) {
        const auto handler = handlers[h].handler;
        auto* server = handlers[h].server;
        const auto record_index = handlers[h].record;
        if (handler->PulseKind().empty()) continue;
        Service::Pulse::HandlerCapture capture{[&](const Service::SessionRequestHandlerPtr& other) {
            return add_handler(other, server);
        }};
        try {
            handler->PulseCapture(capture);
        } catch (const Service::Pulse::ServiceRejected& error) {
            refused.insert(std::string{handler->PulseName()} + ": " + error.what());
            refused_retryable = refused_retryable && error.retryable;
            continue;
        } catch (const std::exception& error) {
            refused.insert(std::string{handler->PulseName()} + ": " + error.what());
            refused_retryable = false;
            continue;
        }
        Service::Pulse::HandlerRecord handler_record;
        handler_record.kind = handler->PulseKind();
        handler_record.root = !handlers[h].port.empty();
        handler_record.port = handlers[h].port;
        if (handler_record.port.empty()) {
            if (const auto found = server_ports.find(server); found != server_ports.end()) {
                handler_record.port = found->second;
            } else if (const auto names = server->PulsePortNames(); !names.empty()) {
                handler_record.port = names.front();
            } else {
                Reject(false, std::string{"service interface without a port: "} + handler->PulseName());
            }
        }
        handler_record.state = std::move(capture.state.bytes);
        if (handler_record.state.size() > Service::Pulse::MaxHandlerState) {
            Reject(false, std::string{"service state too large: "} + handler->PulseName());
        }
        const auto id = registry.Records()[record_index].id;
        registry.Records()[record_index].payload = Service::Pulse::EncodeHandlerRecord(handler_record);
        for (const auto& event : capture.events) {
            if (!exported.emplace(event.event, std::make_pair(id, event.slot)).second) {
                Reject(false, "a service event exported twice");
            }
        }
    }
    std::set<std::string> unexported;
    for (size_t i = 0; i < registry.Records().size(); ++i) {
        auto& record = registry.Records()[i];
        if (record.kind != ObjectKind::ServiceEvent) continue;
        const auto* event = static_cast<const Kernel::KEvent*>(registry.Objects()[i]);
        const auto found = exported.find(event);
        if (found == exported.end()) {
            auto name = Service::KernelHelpers::EventName(event);
            if (name.empty()) {
                const auto* owner = event->GetOwner();
                name = !owner             ? std::string{"unnamed event without owner"}
                       : owner->GetName()[0] ? std::string{owner->GetName()}
                                             : fmt::format("unnamed event of process {}",
                                                           owner->GetProcessId());
            }
            unexported.insert(std::move(name));
            continue;
        }
        record.parent = found->second.first;
        const u32 slot = found->second.second;
        record.payload = {static_cast<u8>(slot), static_cast<u8>(slot >> 8),
                          static_cast<u8>(slot >> 16), static_cast<u8>(slot >> 24)};
    }
    if (!missing.empty() || !refused.empty() || !unexported.empty()) {
        std::string reason;
        if (!missing.empty()) reason += "no serializer for service interfaces " + Join(missing);
        if (!refused.empty()) reason += (reason.empty() ? "" : "; ") + Join(refused);
        if (!unexported.empty()) {
            reason += (reason.empty() ? "" : "; ") +
                      std::string{"events of service processes no interface exports: "} + Join(unexported);
        }
        // A refused interface exports none of its events, so they only make the rejection final
        // when nothing was refused.
        Reject(missing.empty() && (unexported.empty() || !refused.empty()) && refused_retryable,
               reason);
    }

    // Memory map: what the rebuilt skeleton must reproduce, and what to store.
    auto& table = process->GetPageTable();
    // svcMapPhysicalMemory memory: Normal blocks in the alias region, rebuilt by mapping the same
    // ranges again. The layout needs no field for it; their total must match the kernel's.
    const u64 alias_start = GetInteger(table.GetAliasRegionStart());
    const u64 alias_end = alias_start + table.GetAliasRegionSize();
    const u64 physical_size = table.GetBasePageTable().GetMappedPhysicalMemorySize();
    u64 physical_found{};
    ProcessLayout layout;
    layout.program_id = process->GetProgramId();
    layout.process_id = process->GetProcessId();
    layout.pin = process->GetLayoutPin();
    layout.heap_start = GetInteger(table.GetHeapRegionStart());
    layout.heap_size = table.GetBasePageTable().GetCurrentHeapSize();
    layout.main_stack_address = GetInteger(process->GetMainStackAddress());
    layout.main_stack_size = process->GetMainStackSize();
    layout.blocks = EnumerateBlocks(*process);
    const u64 heap_end = layout.heap_start + layout.heap_size;
    const u64 main_stack_end = layout.main_stack_address + layout.main_stack_size;
    std::unordered_map<u64, u64> locked_sources; // physical page -> heap address
    PhysicalPages stored;
    auto& aliases = layout.aliases;
    auto& runs = layout.runs;
    const auto store_page = [&](u64 address) {
        if (!stored.Insert(Physical(*process, address))) return;
        if (!runs.empty() && runs.back().address + runs.back().pages * PageSize == address) {
            ++runs.back().pages;
        } else {
            if (runs.size() == MaxContentRuns) Reject(false, "guest memory too fragmented");
            runs.push_back({address, 1});
        }
    };
    for (const auto& block : layout.blocks) {
        const auto state = static_cast<KMemoryState>(block.state);
        const auto attribute = static_cast<KMemoryAttribute>(block.attribute);
        if (state == KMemoryState::Ipc || state == KMemoryState::NonSecureIpc ||
            state == KMemoryState::NonDeviceIpc ||
            True(attribute & KMemoryAttribute::IpcLocked)) {
            Reject(true, fmt::format("guest memory is lent to an IPC operation "
                                     "(block {:#x}+{:#x} state {:#x} attribute {:#x})",
                                     block.address, block.size, block.state, block.attribute));
        }
        // PermissionLocked (libnx locks its RELRO page) and Uncached (svcSetMemoryAttribute on GPU
        // buffers) are replayed by the restore, DeviceShared by the nvdrv serializer re-sharing its
        // nvmap handles.
        if (True(attribute & ~(KMemoryAttribute::Locked | KMemoryAttribute::PermissionLocked |
                               KMemoryAttribute::DeviceShared | KMemoryAttribute::Uncached))) {
            Reject(false, fmt::format("memory attribute {:#x} has no serializer", block.attribute));
        }
        const bool normal = state == KMemoryState::Normal;
        if (True(attribute & KMemoryAttribute::DeviceShared) && !normal) {
            Reject(false, fmt::format("device-shared memory outside the heap (block {:#x}+{:#x} "
                                      "state {:#x})",
                                      block.address, block.size, block.state));
        }
        // svcMapMemory (a thread stack) and transfer memory lock their source, which may be static
        // data (.bss, CodeData) as well as the heap: the restore replays the same calls.
        if (True(attribute & KMemoryAttribute::Locked) && !normal &&
            state != KMemoryState::CodeData) {
            Reject(false, fmt::format("locked memory outside the heap and static data (block "
                                      "{:#x}+{:#x} state {:#x})",
                                      block.address, block.size, block.state));
        }
        if (normal && (block.address < layout.heap_start || block.address + block.size > heap_end)) {
            if (block.address < alias_start || block.address + block.size > alias_end) {
                Reject(false, "normal memory outside the heap and the alias region");
            }
            physical_found += block.size;
        }
        if (state != KMemoryState::Code && state != KMemoryState::CodeData && !normal &&
            state != KMemoryState::Stack && state != KMemoryState::ThreadLocal &&
            state != KMemoryState::Shared) {
            Reject(false, "memory state " + std::to_string(block.state & 0xff) + " has no serializer");
        }
        if (state == KMemoryState::Shared) {
            // Adjacent mappings may share one coalesced block: walk it object by object.
            if (block.attribute != 0) Reject(false, "shared memory with attributes");
            for (u64 address = block.address; address < block.address + block.size;) {
                const auto found = shared_by_physical.find(Physical(*process, address));
                if (found == shared_by_physical.end()) Reject(false, "shared memory of an unknown object");
                auto& record = registry.Records()[found->second];
                const auto* shared =
                    static_cast<const Kernel::KSharedMemory*>(registry.Objects()[found->second]);
                const u64 size = shared->GetSize();
                if (!size || address + size > block.address + block.size) {
                    Reject(false, "partial shared memory mapping");
                }
                PutLe(record.payload, address, 8);
                PutLe(record.payload, size, 8);
                PutLe(record.payload,
                      block.permission & static_cast<u32>(KMemoryPermission::UserMask), 4);
                u32 count{};
                std::memcpy(&count, record.payload.data() + 4, 4);
                ++count;
                std::memcpy(record.payload.data() + 4, &count, 4);
                address += size;
            }
        }
        if (True(attribute & KMemoryAttribute::Locked)) {
            for (u64 page = 0; page < block.size; page += PageSize) {
                locked_sources.emplace(Physical(*process, block.address + page), block.address + page);
            }
        }
    }
    if (physical_found != physical_size) {
        Reject(false, fmt::format("physical memory mappings ({:#x}) differ from the kernel's ({:#x})",
                                  physical_found, physical_size));
    }
    for (const auto& block : layout.blocks) {
        const auto state = static_cast<KMemoryState>(block.state);
        if (state == KMemoryState::Shared) continue; // its owner refills it
        for (u64 page = 0; page < block.size; page += PageSize) {
            const u64 address = block.address + page;
            if (state == KMemoryState::Stack &&
                !(address >= layout.main_stack_address && address < main_stack_end)) {
                // A thread stack libnx allocated on the heap and mapped with svcMapMemory.
                const auto source = locked_sources.find(Physical(*process, address));
                if (source == locked_sources.end()) Reject(false, "stack without a heap source");
                if (!aliases.empty() && aliases.back().destination + aliases.back().size == address &&
                    aliases.back().source + aliases.back().size == source->second) {
                    aliases.back().size += PageSize;
                } else if (aliases.size() == MaxStackAliases) {
                    Reject(false, "too many stack mappings");
                } else {
                    aliases.push_back({address, source->second, PageSize});
                }
            }
            if (UserReadable(block.permission)) store_page(address);
        }
    }
    // A locked heap page with no readable alias cannot be read back: fail closed. Transfer memory
    // the guest cannot read is a service's work memory: rebuilt locked, its content not kept.
    for (const auto& [physical, address] : locked_sources) {
        if (!stored.Contains(physical) && !lent_pages.Contains(physical)) {
            Reject(false, "locked heap page with no readable mapping");
        }
    }
    if (runs.empty()) Reject(false, "the application has no memory to store");
    checkpoint.runs = runs;
    checkpoint.run_ends.reserve(runs.size());
    for (const auto& run : runs) {
        checkpoint.memory_size += run.pages * PageSize;
        checkpoint.run_ends.push_back(checkpoint.memory_size);
    }

    // Timing: bind event types by name. Each fresh System creates the same named events.
    auto& timing = system.CoreTiming();
    const auto types = timing.CheckpointQueuedEventTypes();
    const auto& hardware_timer = kernel.HardwareTimer().GetEventType();
    std::vector<Core::Timing::CheckpointEventBinding> bindings;
    TimingSection timing_section;
    std::unordered_set<std::string> seen_names;
    u64 hardware_id{};
    for (const auto& type : types) {
        const u64 id = bindings.size() + 1;
        bindings.push_back({id, type});
        if (type == hardware_timer) {
            hardware_id = id;
            continue;
        }
        if (type->name.empty() || type->name.size() > MaxTimingNameLength ||
            timing_section.bindings.size() == MaxTimingNames || !seen_names.insert(type->name).second ||
            Core::Timing::FindLiveEvent(type->name) != type) {
            Reject(false, "timing event '" + type->name + "' cannot be bound by name");
        }
        timing_section.bindings.push_back({id, type->name});
    }
    const auto timing_state = timing.CaptureCheckpoint(bindings);
    timing_section.multicore = timing_state.multicore;
    timing_section.time_ns = timing_state.time_ns;
    timing_section.clock_ticks = timing_state.clock_ticks;
    timing_section.gpu_ticks = timing_state.gpu_ticks;
    timing_section.cpu_ticks = timing_state.cpu_ticks;
    timing_section.downcount = timing_state.downcount;
    timing_section.pause_end_time = timing_state.pause_end_time;
    timing_section.next_fifo_id = timing_state.next_fifo_id;
    for (const auto& event : timing_state.events) {
        // The hardware timer is rebuilt from the waits' deadlines, never from the queue.
        if (event.binding_id == hardware_id) continue;
        timing_section.events.push_back(
            {event.binding_id, event.time_ns, event.fifo_order, event.period_ns});
    }

    const auto exclusive_bytes = EncodeExclusiveState(exclusive);
    auto& sections = checkpoint.sections;
    sections.push_back({ProcessSection::Process, EncodeProcessLayout(layout)});
    sections.push_back({ProcessSection::Objects, EncodeObjects(registry.Records())});
    sections.push_back({ProcessSection::Threads, EncodeThreads(threads)});
    sections.push_back({ProcessSection::Handles, Kernel::EncodeHandleTableCheckpoint(handles)});
    sections.push_back({ProcessSection::Waits, EncodeWaits(waits)});
    sections.push_back({ProcessSection::Exclusive, {exclusive_bytes.begin(), exclusive_bytes.end()}});
    sections.push_back({ProcessSection::Timing, EncodeTiming(timing_section)});

    auto& stats = checkpoint.stats;
    stats.threads = static_cast<u32>(threads.size());
    stats.waiting_threads = static_cast<u32>(waits.size());
    stats.objects = static_cast<u32>(registry.Records().size());
    stats.handles = static_cast<u32>(std::count_if(handles.entries.begin(), handles.entries.end(),
                                                   [](const auto& entry) { return entry.object != 0; }));
    stats.memory_blocks = static_cast<u32>(layout.blocks.size());
    stats.memory_bytes = checkpoint.memory_size;
    stats.timing_events = static_cast<u32>(timing_section.events.size());
    for (const auto& section : sections) stats.metadata_bytes += section.bytes.size();
    return checkpoint;
}

u64 ProcessCheckpoint::DecodedSize() const {
    return memory_size + stats.metadata_bytes;
}

u64 ProcessCheckpoint::MaxEncodedSize() const {
    const auto section = [](u64 size) {
        return 24 + (size + Common::Pulse::BlockSize - 1) / Common::Pulse::BlockSize * 32 + size;
    };
    u64 total = 136 + section(memory_size) + 24 + 8;
    for (const auto& entry : sections) total += section(entry.bytes.size());
    return total;
}

void ProcessCheckpoint::Write(Common::Pulse::Writer& writer) const {
    if (Common::Pulse::Diagnostics()) {
        LOG_INFO(Core, "Pulse: GPU 0x400000000+256 KiB as written is {}",
                 Common::DescribeWatchedPages());
    }
    auto& memory = process->GetMemory();
    // The writer reads several blocks at once, from its own threads; the game is paused.
    std::atomic<u64> untouched_bytes{};
    const auto read_guest = [&](u64 address, u8* out, size_t size) {
        UntouchedPages untouched;
        for (size_t done = 0; done < size;) {
            const auto count = static_cast<size_t>(
                std::min<u64>(size - done, PageSize - ((address + done) & (PageSize - 1))));
            if (untouched.Contains(memory.GetPointerSilent(address + done))) {
                std::memset(out + done, 0, count);
                untouched_bytes.fetch_add(count, std::memory_order_relaxed);
            } else if (!memory.ReadBlockUnsafe(address + done, out + done, count)) {
                Invalid("Pulse: guest memory read failed");
            }
            done += count;
        }
    };
    const auto write_bytes = [&](const SectionBytes& entry) {
        writer.Add({static_cast<u32>(entry.id), ProcessSectionVersion, entry.bytes.size()},
                   [&](u64 offset, std::span<u8> block) {
                       std::memcpy(block.data(), entry.bytes.data() + offset, block.size());
                   });
    };
    for (const auto& entry : sections) {
        if (entry.id == ProcessSection::Handles) {
            writer.Add({static_cast<u32>(ProcessSection::Memory), ProcessSectionVersion, memory_size},
                       [&](u64 offset, std::span<u8> block) {
                           size_t index = std::upper_bound(run_ends.begin(), run_ends.end(), offset) -
                                          run_ends.begin();
                           size_t position{};
                           while (position < block.size()) {
                               const u64 start = index ? run_ends[index - 1] : 0;
                               const u64 within = offset - start;
                               const auto count = static_cast<size_t>(std::min<u64>(
                                   block.size() - position, runs[index].pages * PageSize - within));
                               read_guest(runs[index].address + within, block.data() + position,
                                          count);
                               position += count;
                               offset += count;
                               ++index;
                           }
                       });
        }
        write_bytes(entry);
    }
    LOG_INFO(Core, "Pulse: {} of {} MiB of guest memory never committed, stored as zero",
             untouched_bytes.load() >> 20, memory_size >> 20);
}

// ---------------------------------------------------------------------------------- restore

ProcessRestore::ProcessRestore(Core::System& system_, LoadSkeleton load_)
    : system{system_}, load{std::move(load_)}, state{std::make_unique<State>()} {
    if (!load) Invalid("Pulse: restore requires a skeleton loader");
}

ProcessRestore::~ProcessRestore() {
    if (finished || !state) return;
    // Failed restore: drop our creation references; the caller destroys the System.
    auto& kernel = system.Kernel();
    for (auto it = state->created.rbegin(); it != state->created.rend(); ++it) {
        (*it)->Close(kernel);
    }
}

void ProcessRestore::Consume(Common::Pulse::Section section, u64 offset, std::span<const u8> data) {
    if (finished) Invalid("Pulse: restore already finished");
    if (offset == 0) {
        if (next_section >= SectionOrder.size() ||
            section.id != static_cast<u32>(SectionOrder[next_section])) {
            Incompatible("Pulse: unexpected process section");
        }
        if (section.version != ProcessSectionVersion) Incompatible("Pulse: process section version");
        if (!section.size) Invalid("Pulse: empty process section");
        buffer.clear();
        if (Common::Pulse::Diagnostics()) {
            LOG_INFO(Core, "Pulse: restore section {} begins",
                     static_cast<u32>(SectionOrder[next_section]));
        }
        if (SectionOrder[next_section] == ProcessSection::Memory) {
            if (section.size != state->layout.ContentSize()) Invalid("Pulse: memory section size");
            VerifyLayout();
        } else {
            if (section.size > MaxMetadataSection) Invalid("Pulse: process section too large");
            buffer.reserve(static_cast<size_t>(section.size));
        }
    } else if (next_section >= SectionOrder.size() ||
               section.id != static_cast<u32>(SectionOrder[next_section])) {
        Invalid("Pulse: interleaved process section");
    }
    const auto id = SectionOrder[next_section];
    if (data.size() > section.size || offset > section.size - data.size()) {
        Invalid("Pulse: process section bounds");
    }
    if (id == ProcessSection::Memory) {
        auto& memory = process->GetMemory();
        UntouchedPages untouched;
        const auto write_guest = [&](u64 address, const u8* in, size_t size) {
            for (size_t done = 0; done < size;) {
                const auto count = static_cast<size_t>(
                    std::min<u64>(size - done, PageSize - ((address + done) & (PageSize - 1))));
                if (!IsZero(in + done, count) ||
                    !untouched.Contains(memory.GetPointerSilent(address + done))) {
                    if (!memory.WriteBlockUnsafe(address + done, in + done, count)) {
                        Invalid("Pulse: guest memory write failed");
                    }
                    untouched.Wrote();
                }
                done += count;
            }
        };
        const auto& ends = state->run_ends;
        size_t index = std::upper_bound(ends.begin(), ends.end(), offset) - ends.begin();
        size_t position{};
        u64 cursor = offset;
        while (position < data.size()) {
            const u64 start = index ? ends[index - 1] : 0;
            const u64 within = cursor - start;
            const auto count = static_cast<size_t>(
                std::min<u64>(data.size() - position, state->layout.runs[index].pages * PageSize - within));
            write_guest(state->layout.runs[index].address + within, data.data() + position, count);
            position += count;
            cursor += count;
            ++index;
        }
        stats.memory_bytes += data.size();
    } else {
        if (offset != buffer.size()) Invalid("Pulse: process section out of order");
        buffer.insert(buffer.end(), data.begin(), data.end());
    }
    if (offset + data.size() == section.size) {
        if (id != ProcessSection::Memory) {
            stats.metadata_bytes += buffer.size();
            Complete(id, buffer);
        }
        buffer.clear();
        if (Common::Pulse::Diagnostics()) {
            LOG_INFO(Core, "Pulse: restore section {} done", static_cast<u32>(id));
        }
        // The last section completes the rebuild here, inside the Store's rebuild callback, so a
        // failure leaves the published state unconsumed.
        if (++next_section == SectionOrder.size()) Finish();
    }
}

void ProcessRestore::VerifyLayout() {
    if (!process) Invalid("Pulse: memory before process");
    if (EnumerateBlocks(*process) != state->layout.blocks) {
        Incompatible("Pulse: rebuilt memory map differs from the captured one");
    }
    stats.memory_blocks = static_cast<u32>(state->layout.blocks.size());
    layout_verified = true;
}

void ProcessRestore::Complete(ProcessSection id, std::span<const u8> bytes) {
    auto& kernel = system.Kernel();
    const auto resolve = [&](u64 object_id) -> KAutoObject* {
        const auto found = state->objects.find(object_id);
        return found != state->objects.end() ? found->second : nullptr;
    };
    const auto bind = [&](u64 object_id, KAutoObject* object) {
        if (!object_id || !state->objects.emplace(object_id, object).second) {
            Invalid("Pulse: duplicate or null object identity");
        }
    };

    switch (id) {
    case ProcessSection::Process: {
        auto& layout = state->layout;
        layout = DecodeProcessLayout(bytes);
        for (const auto& run : layout.runs) {
            state->run_ends.push_back((state->run_ends.empty() ? 0 : state->run_ends.back()) +
                                      run.pages * PageSize);
        }

        // The skeleton: the same program, at the same random layout, never run.
        if (system.ApplicationProcess()) Invalid("Pulse: restore requires a fresh system");
        load(layout.pin);
        process = system.ApplicationProcess();
        if (!process || !process->IsRestoringCheckpoint()) {
            Incompatible("Pulse: skeleton loader did not create a pinned process");
        }
        if (process->GetLayoutPin() != layout.pin || process->GetProgramId() != layout.program_id ||
            process->GetProcessId() != layout.process_id) {
            Incompatible("Pulse: rebuilt process identity or layout differs");
        }
        // Services start on their own threads, several on guest cores that only run while the
        // system runs (vi blocks on one of them before it creates its composition event). The
        // skeleton has no threads yet, so running here runs the services alone.
        if (!kernel.AreAllServersRunning()) {
            void(system.Run());
            const auto deadline = std::chrono::steady_clock::now() + ServiceStartTimeout;
            while (!kernel.AreAllServersRunning()) {
                if (std::chrono::steady_clock::now() > deadline) {
                    void(system.Pause());
                    Incompatible("Pulse: system services did not start");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        if (!system.IsPaused()) void(system.Pause());
        bind(ProcessObjectId, process);
        Check(process->RunForCheckpointRestore(kernel, layout.main_stack_address,
                                               layout.main_stack_size),
              "Pulse: main stack rebuild failed");
        auto& table = process->GetPageTable();
        if (GetInteger(table.GetHeapRegionStart()) != layout.heap_start) {
            Incompatible("Pulse: rebuilt heap region differs");
        }
        if (layout.heap_size) {
            Kernel::KProcessAddress heap{};
            Check(table.SetHeapSize(std::addressof(heap), layout.heap_size), "Pulse: heap rebuild failed");
            if (GetInteger(heap) != layout.heap_start) Incompatible("Pulse: rebuilt heap address differs");
        }
        // Normal memory outside the heap was mapped with svcMapPhysicalMemory (checked at capture).
        const u64 heap_end = layout.heap_start + layout.heap_size;
        for (const auto& block : layout.blocks) {
            if (static_cast<KMemoryState>(block.state) != KMemoryState::Normal ||
                (block.address >= layout.heap_start && block.address + block.size <= heap_end)) {
                continue;
            }
            // No guest thread runs here, so the owner is passed instead of the current process.
            Check(table.MapPhysicalMemory(block.address, block.size, process->GetProcessId()),
                  "Pulse: physical memory rebuild failed");
        }
        for (const auto& alias : layout.aliases) {
            Check(table.MapMemory(alias.destination, alias.source, alias.size),
                  "Pulse: stack mapping rebuild failed");
        }
        // Runtime reprotection (libnx makes its RELRO read-only, then locks it): replay the
        // captured permissions on the loader's data, then the permission locks.
        for (const auto& block : layout.blocks) {
            const auto block_state = static_cast<KMemoryState>(block.state);
            if ((block_state != KMemoryState::CodeData && block_state != KMemoryState::Normal) ||
                (block.attribute & ~static_cast<u32>(KMemoryAttribute::DeviceShared |
                                                     KMemoryAttribute::Uncached)) != 0) {
                continue;
            }
            for (u64 address = block.address; address < block.address + block.size;) {
                Kernel::KMemoryInfo info{};
                Kernel::Svc::PageInfo page{};
                Check(table.QueryInfo(std::addressof(info), std::addressof(page), address),
                      "Pulse: memory query failed");
                const u64 end = std::min(info.m_address + info.m_size, block.address + block.size);
                if (static_cast<u32>(info.m_permission) != block.permission) {
                    Check(table.SetMemoryPermission(
                              address, end - address,
                              static_cast<Kernel::Svc::MemoryPermission>(
                                  block.permission & static_cast<u32>(KMemoryPermission::UserMask))),
                          "Pulse: permission rebuild failed");
                }
                address = end;
            }
        }
        for (const auto& block : layout.blocks) {
            if (False(static_cast<KMemoryAttribute>(block.attribute) & KMemoryAttribute::Uncached)) {
                continue;
            }
            Check(table.SetMemoryAttribute(block.address, block.size, KMemoryAttribute::Uncached,
                                           KMemoryAttribute::Uncached),
                  "Pulse: uncached memory rebuild failed");
        }
        for (const auto& block : layout.blocks) {
            if (False(static_cast<KMemoryAttribute>(block.attribute) &
                      KMemoryAttribute::PermissionLocked)) {
                continue;
            }
            const auto block_state = static_cast<KMemoryState>(block.state);
            if (block_state != KMemoryState::CodeData && block_state != KMemoryState::Normal) {
                Invalid("Pulse: permission lock outside data memory");
            }
            Check(table.SetMemoryPermission(
                      block.address, block.size,
                      static_cast<Kernel::Svc::MemoryPermission>(
                          block.permission & static_cast<u32>(KMemoryPermission::UserMask))),
                  "Pulse: permission rebuild failed");
            Check(table.SetMemoryAttribute(block.address, block.size,
                                           KMemoryAttribute::PermissionLocked,
                                           KMemoryAttribute::PermissionLocked),
                  "Pulse: permission lock rebuild failed");
        }
        break;
    }
    case ProcessSection::Objects: {
        const auto records = DecodeObjects(bytes);
        std::unordered_map<u64, const ObjectRecord*> by_id;
        for (const auto& record : records) by_id.emplace(record.id, std::addressof(record));
        const auto kind_of = [&](u64 object_id) {
            const auto found = by_id.find(object_id);
            return found != by_id.end() ? found->second->kind : ObjectKind{};
        };

        // Application events.
        for (const auto& record : records) {
            if (record.kind == ObjectKind::Event) {
                Kernel::KScopedResourceReservation reservation{
                    kernel, process, Kernel::LimitableResource::EventCountMax};
                if (!reservation.Succeeded()) Incompatible("Pulse: event limit reached");
                auto* event = Kernel::KEvent::Create(kernel);
                if (!event) Incompatible("Pulse: out of kernel events");
                event->Initialize(kernel, process);
                reservation.Commit();
                state->created.push_back(std::addressof(event->GetReadableEvent()));
                state->created.push_back(event);
                Kernel::KEvent::Register(kernel, event);
                bind(record.id, event);
            } else if (record.kind == ObjectKind::ReadableEvent &&
                       kind_of(record.parent) == ObjectKind::Event) {
                auto* event = resolve(record.parent)->DynamicCast<Kernel::KEvent*>();
                if (!event) Invalid("Pulse: readable event without its event");
                auto* readable = std::addressof(event->GetReadableEvent());
                bind(record.id, readable);
                if (record.flags & 1) Check(readable->Signal(kernel), "Pulse: event signal failed");
            }
        }

        // Transfer memory: the heap range locked again with the owner's permission.
        for (const auto& record : records) {
            if (record.kind != ObjectKind::TransferMemory) continue;
            Service::Pulse::StateReader in{record.payload};
            const u64 address = in.U64();
            const u64 size = in.U64();
            const u32 permission = in.U32();
            Kernel::KScopedResourceReservation reservation{
                kernel, process, Kernel::LimitableResource::TransferMemoryCountMax};
            if (!reservation.Succeeded()) Incompatible("Pulse: transfer memory limit reached");
            auto* transfer = Kernel::KTransferMemory::Create(kernel);
            if (!transfer) Incompatible("Pulse: out of kernel transfer memory");
            state->created.push_back(transfer);
            Check(transfer->Initialize(kernel, process, address, size,
                                       static_cast<Kernel::Svc::MemoryPermission>(permission)),
                  "Pulse: transfer memory rebuild failed");
            reservation.Commit();
            Kernel::KTransferMemory::Register(kernel, transfer);
            bind(record.id, transfer);
        }

        // Sessions to named ports: the fresh service accepts them and gives each its root.
        struct PortContext {
            Service::SessionRequestHandlerPtr root;
            Service::ServerManager* server{};
        };
        std::unordered_map<std::string, PortContext> ports;
        std::unordered_map<u64, std::shared_ptr<Service::SessionRequestManager>> managers;
        std::unordered_map<u64, Service::SessionRequestHandlerPtr> roots; // handler id -> fresh root
        std::unordered_map<u64, Service::Pulse::SessionRecord> sessions;
        const auto open_port = [&](const std::string& name) {
            auto* port = FindServicePort(system, name);
            if (!port) Incompatible("Pulse: service port '" + name + "' missing");
            Kernel::KClientSession* client{};
            Check(port->CreateSession(kernel, std::addressof(client), process),
                  "Pulse: service session rebuild failed");
            state->created.push_back(client);
            // The server thread registers the session asynchronously.
            const auto* server = std::addressof(client->GetParent()->GetServerSession());
            const auto deadline = std::chrono::steady_clock::now() + ServiceStartTimeout;
            std::shared_ptr<Service::SessionRequestManager> manager;
            while (!(manager = Service::ServerManager::FindSessionManager(server))) {
                if (std::chrono::steady_clock::now() > deadline) {
                    Incompatible("Pulse: service '" + name + "' did not accept the session");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (!manager->HasSessionHandler()) Incompatible("Pulse: service '" + name + "' has no handler");
            ports.try_emplace(name, PortContext{manager->SessionHandler().shared_from_this(),
                                                std::addressof(manager->GetServerManager())});
            return std::make_pair(client, manager);
        };
        for (const auto& record : records) {
            if (record.kind != ObjectKind::ClientSession) continue;
            auto& session = sessions[record.id];
            try {
                session = Service::Pulse::DecodeSessionRecord(record.payload);
                if (!session.clone_of) Service::Pulse::DecodeSessionGraph(session.graph);
            } catch (const std::invalid_argument&) {
                Invalid("Pulse: malformed session record");
            }
            if (session.port.empty()) continue;
            const auto graph = Service::Pulse::DecodeSessionGraph(session.graph);
            auto [client, manager] = open_port(session.port);
            bind(record.id, client);
            managers.emplace(record.id, manager);
            if (graph.session_handler) {
                if (kind_of(graph.session_handler) != ObjectKind::ServiceHandler) {
                    Invalid("Pulse: session root is not a service interface");
                }
                roots.try_emplace(graph.session_handler, manager->SessionHandler().shared_from_this());
            }
        }

        // Interfaces, each after the ones it references.
        std::unordered_map<u64, Service::SessionRequestHandlerPtr> handlers;
        std::unordered_set<u64> restoring;
        std::function<Service::SessionRequestHandlerPtr(u64)> restore_handler;
        const auto handler_port = [&](u64 handler_id) -> PortContext& {
            if (kind_of(handler_id) != ObjectKind::ServiceHandler) {
                Invalid("Pulse: reference to a missing service interface");
            }
            Service::Pulse::HandlerRecord handler;
            try {
                handler = Service::Pulse::DecodeHandlerRecord(by_id.at(handler_id)->payload);
            } catch (const std::invalid_argument&) {
                Invalid("Pulse: malformed service interface record");
            }
            if (!ports.contains(handler.port)) open_port(handler.port);
            return ports.at(handler.port);
        };
        restore_handler = [&](u64 handler_id) -> Service::SessionRequestHandlerPtr {
            if (const auto found = handlers.find(handler_id); found != handlers.end()) return found->second;
            if (!restoring.insert(handler_id).second) Invalid("Pulse: service interfaces reference in a cycle");
            auto& context = handler_port(handler_id);
            const auto handler = Service::Pulse::DecodeHandlerRecord(by_id.at(handler_id)->payload);
            Service::Pulse::HandlerRestore restore{system, process, handler.state, restore_handler};
            Service::SessionRequestHandlerPtr rebuilt;
            try {
                if (handler.root) {
                    const auto root = roots.find(handler_id);
                    if (root == roots.end()) Invalid("Pulse: root interface without its session");
                    rebuilt = root->second->PulseRestore(handler.kind, restore);
                    if (rebuilt != root->second) {
                        Incompatible("Pulse: service '" + handler.port + "' root changed kind");
                    }
                } else {
                    rebuilt = context.root->PulseRestore(handler.kind, restore);
                    // One server hosts several ports (audio...) and the capture names the first
                    // it saw, not the one that made this interface: try the server's other ports,
                    // those already open first.
                    if (!rebuilt && context.server) {
                        auto names = context.server->PulsePortNames();
                        std::ranges::stable_partition(
                            names, [&](const std::string& name) { return ports.contains(name); });
                        for (const auto& name : names) {
                            if (name == handler.port) continue;
                            if (!ports.contains(name)) open_port(name);
                            Service::Pulse::HandlerRestore retry{system, process, handler.state,
                                                                 restore_handler};
                            rebuilt = ports.at(name).root->PulseRestore(handler.kind, retry);
                            if (rebuilt) break;
                        }
                    }
                }
            } catch (const std::invalid_argument& error) {
                Incompatible(error.what());
            }
            if (!rebuilt) {
                Incompatible("Pulse: service '" + handler.port + "' cannot rebuild " + handler.kind);
            }
            restoring.erase(handler_id);
            handlers.emplace(handler_id, rebuilt);
            return rebuilt;
        };
        for (const auto& record : records) {
            if (record.kind == ObjectKind::ServiceHandler) restore_handler(record.id);
        }

        // Session graphs. A session a request created is registered with its service's server.
        for (const auto& record : records) {
            if (record.kind != ObjectKind::ClientSession) continue;
            const auto& session = sessions.at(record.id);
            if (session.clone_of) continue;
            const auto graph = Service::Pulse::DecodeSessionGraph(session.graph);
            std::shared_ptr<Service::SessionRequestManager> manager;
            Kernel::KSession* created_session{};
            if (session.port.empty()) {
                u64 any{graph.session_handler};
                for (const auto domain : graph.domain_handlers) any = any ? any : domain;
                if (!any) Invalid("Pulse: session without an interface");
                auto& context = handler_port(any);
                Kernel::KScopedResourceReservation reservation{
                    kernel, process, Kernel::LimitableResource::SessionCountMax};
                if (!reservation.Succeeded()) Incompatible("Pulse: session limit reached");
                created_session = Kernel::KSession::Create(kernel);
                if (!created_session) Incompatible("Pulse: out of kernel sessions");
                created_session->Initialize(kernel, nullptr, 0, process);
                reservation.Commit();
                Kernel::KSession::Register(kernel, created_session);
                state->created.push_back(std::addressof(created_session->GetClientSession()));
                bind(record.id, std::addressof(created_session->GetClientSession()));
                manager = std::make_shared<Service::SessionRequestManager>(kernel, *context.server);
                managers.emplace(record.id, manager);
            } else {
                manager = managers.at(record.id);
            }
            try {
                manager->RestoreCheckpoint(graph, [&](u64 handler_id) {
                    return kind_of(handler_id) == ObjectKind::ServiceHandler ? handlers.at(handler_id)
                                                                             : nullptr;
                });
            } catch (const std::invalid_argument& error) {
                Invalid(error.what());
            }
            if (created_session) {
                Check(handler_port(graph.session_handler ? graph.session_handler
                                                         : graph.domain_handlers.front())
                          .server->RegisterSession(std::addressof(created_session->GetServerSession()),
                                                   manager),
                      "Pulse: service session registration failed");
            }
        }

        // Clones: a fresh session served by the manager of the session they were cloned from.
        for (const auto& record : records) {
            if (record.kind != ObjectKind::ClientSession) continue;
            const auto& session = sessions.at(record.id);
            if (!session.clone_of) continue;
            const auto owner = sessions.find(session.clone_of);
            if (owner == sessions.end() || owner->second.clone_of) {
                Invalid("Pulse: session clone without its original");
            }
            const auto manager = managers.at(session.clone_of);
            Kernel::KScopedResourceReservation reservation{
                kernel, process, Kernel::LimitableResource::SessionCountMax};
            if (!reservation.Succeeded()) Incompatible("Pulse: session limit reached");
            auto* clone = Kernel::KSession::Create(kernel);
            if (!clone) Incompatible("Pulse: out of kernel sessions");
            clone->Initialize(kernel, nullptr, 0, process);
            reservation.Commit();
            Kernel::KSession::Register(kernel, clone);
            state->created.push_back(std::addressof(clone->GetClientSession()));
            bind(record.id, std::addressof(clone->GetClientSession()));
            Check(manager->GetServerManager().RegisterSession(std::addressof(clone->GetServerSession()),
                                                              manager),
                  "Pulse: service session registration failed");
        }

        // Service events, from the interfaces that exported them, with the captured signal.
        for (const auto& record : records) {
            if (record.kind == ObjectKind::ServiceEvent) {
                const u32 slot = record.payload[0] | (u32{record.payload[1]} << 8) |
                                 (u32{record.payload[2]} << 16) | (u32{record.payload[3]} << 24);
                auto* event = handlers.at(record.parent)->PulseEvent(slot);
                if (!event) Incompatible("Pulse: a service interface lost an exported event");
                bind(record.id, event);
            } else if (record.kind == ObjectKind::ReadableEvent &&
                       kind_of(record.parent) == ObjectKind::ServiceEvent) {
                auto* event = resolve(record.parent)->DynamicCast<Kernel::KEvent*>();
                if (!event) Invalid("Pulse: readable event without its event");
                auto* readable = std::addressof(event->GetReadableEvent());
                bind(record.id, readable);
                Check((record.flags & 1) ? readable->Signal(kernel) : readable->Clear(kernel),
                      "Pulse: service event signal failed");
            }
        }

        // Shared memory, after the interfaces (HID creates an applet resource's memory): the fresh
        // owner's memory, mapped where the guest had it, before the memory map is compared.
        for (const auto& record : records) {
            if (record.kind != ObjectKind::SharedMemory) continue;
            Service::Pulse::StateReader in{record.payload};
            const u32 owner = in.U32();
            const u32 count = in.U32();
            const u64 argument = in.U64();
            if (count != (record.payload.size() - SharedMemoryPayloadHeader) / SharedMappingSize) {
                Invalid("Pulse: malformed shared memory record");
            }
            auto* shared = ResolveSharedMemory(system, owner, argument);
            if (!shared) Incompatible("Pulse: a shared memory owner did not rebuild its memory");
            bind(record.id, shared);
            for (u32 i = 0; i < count; ++i) {
                const u64 address = in.U64();
                const u64 size = in.U64();
                const auto permission = static_cast<Kernel::Svc::MemoryPermission>(in.U32());
                if (size != shared->GetSize()) Incompatible("Pulse: shared memory size differs");
                Check(process->AddSharedMemory(kernel, shared, address, size),
                      "Pulse: shared memory rebuild failed");
                if (shared->Map(*process, address, size, permission).IsError()) {
                    process->RemoveSharedMemory(kernel, shared, address, size);
                    Incompatible("Pulse: shared memory mapping rebuild failed");
                }
            }
        }
        stats.objects = static_cast<u32>(records.size());
        break;
    }

    case ProcessSection::Threads: {
        const auto records = DecodeThreads(bytes);
        for (const auto& record : records) {
            const auto context = DecodeContext(record.context);
            Kernel::KScopedResourceReservation reservation{
                kernel, process, Kernel::LimitableResource::ThreadCountMax};
            if (!reservation.Succeeded()) Incompatible("Pulse: thread limit reached");
            auto* thread = KThread::Create(kernel);
            if (!thread) Incompatible("Pulse: out of kernel threads");
            state->created.push_back(thread);
            const s32 core = record.ideal_core >= 0 ? record.ideal_core : process->GetIdealCoreId();
            {
                Kernel::KScopedLightLock lock{process->GetStateLock()};
                Check(KThread::InitializeCheckpointUserThread(system, thread, record.thread_id,
                                                              record.tls, context.sp,
                                                              record.base_priority, core, process),
                      "Pulse: thread rebuild failed");
            }
            reservation.Commit();
            KThread::Register(kernel, thread);
            thread->GetContext() = context;
            const bool terminated = record.run == ThreadRun::Terminated;
            if (!terminated) {
                // Stopped until the caller runs the system, like every captured thread.
                thread->RequestSuspend(kernel, Kernel::SuspendType::System);
                if (record.flags & 1) thread->RequestSuspend(kernel, Kernel::SuspendType::Thread);
            }
            Check(thread->SetCoreMask(kernel, record.ideal_core, record.affinity),
                  "Pulse: thread core mask rebuild failed");
            if (terminated) {
                Kernel::KScopedSchedulerLock lock{kernel};
                thread->RestoreTerminated(kernel);
            }
            bind(record.id, thread);
            state->threads.emplace_back(thread, record.run);
        }
        stats.threads = static_cast<u32>(records.size());
        break;
    }
    case ProcessSection::Handles: {
        Kernel::HandleTableCheckpoint handles;
        try {
            handles = Kernel::DecodeHandleTableCheckpoint(bytes);
        } catch (const std::invalid_argument&) {
            Invalid("Pulse: malformed handle table");
        }
        Kernel::KScopedSchedulerLock lock{kernel};
        process->GetHandleTable().RestoreCheckpoint(kernel, handles, resolve);
        stats.handles = static_cast<u32>(std::count_if(handles.entries.begin(), handles.entries.end(),
                                                       [](const auto& e) { return e.object != 0; }));
        break;
    }
    case ProcessSection::Waits: {
        const auto waits = DecodeWaits(bytes);
        size_t expected{};
        for (const auto& [thread, run] : state->threads) expected += run == ThreadRun::Waiting;
        if (waits.size() != expected) Invalid("Pulse: waits do not match waiting threads");
        Kernel::KScopedSchedulerLock lock{kernel};
        Kernel::RestoreCheckpointWaitGraph(kernel, waits, [&](u64 object_id) {
            auto* object = resolve(object_id);
            return object ? object->DynamicCast<Kernel::KSynchronizationObject*>() : nullptr;
        });
        stats.waiting_threads = static_cast<u32>(waits.size());
        break;
    }
    case ProcessSection::Exclusive:
        process->GetExclusiveMonitor().RestoreCheckpoint(DecodeExclusiveState(bytes));
        break;
    case ProcessSection::Timing: {
        const auto section = DecodeTiming(bytes);
        auto& timing = system.CoreTiming();
        const auto& hardware_timer = kernel.HardwareTimer().GetEventType();
        // Services start on their own host threads, so a captured event's owner may not exist
        // yet. Wait for each one; a looping event must also be queued already, or its owner would
        // schedule it again on top of the restored queue.
        std::unordered_set<u64> looping;
        for (const auto& event : section.events) {
            if (event.period_ns) looping.insert(event.binding_id);
        }
        std::unordered_map<std::string, std::shared_ptr<Core::Timing::EventType>> fresh_types;
        std::vector<Core::Timing::CheckpointEventBinding> fresh_bindings;
        std::vector<Core::Timing::CheckpointEventBinding> bindings;
        const auto deadline = std::chrono::steady_clock::now() + ServiceStartTimeout;
        for (;;) {
            // The fresh system's own events, by name.
            fresh_types.clear();
            fresh_bindings.clear();
            for (const auto& type : timing.CheckpointQueuedEventTypes()) {
                fresh_bindings.push_back({fresh_bindings.size() + 1, type});
                if (type != hardware_timer && !fresh_types.emplace(type->name, type).second) {
                    Incompatible("Pulse: duplicated timing event name");
                }
            }
            bindings.clear();
            const std::string* missing{};
            for (const auto& binding : section.bindings) {
                const auto found = fresh_types.find(binding.name);
                auto type = found != fresh_types.end()  ? found->second
                            : looping.contains(binding.id) ? nullptr
                                                           : Core::Timing::FindLiveEvent(binding.name);
                if (!type || type == hardware_timer) {
                    missing = std::addressof(binding.name);
                    break;
                }
                bindings.push_back({binding.id, std::move(type)});
            }
            if (!missing) break;
            if (std::chrono::steady_clock::now() > deadline) {
                Incompatible("Pulse: timing event '" + *missing + "' missing after rebuild");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        std::unordered_set<std::string> bound;
        u64 next_id{};
        for (const auto& binding : section.bindings) {
            bound.insert(binding.name);
            next_id = std::max(next_id, binding.id);
        }
        Core::Timing::TimingCheckpoint merged{};
        merged.multicore = section.multicore;
        merged.time_ns = section.time_ns;
        merged.clock_ticks = section.clock_ticks;
        merged.gpu_ticks = section.gpu_ticks;
        merged.cpu_ticks = section.cpu_ticks;
        merged.downcount = section.downcount;
        merged.pause_end_time = section.pause_end_time;
        merged.next_fifo_id = section.next_fifo_id;
        for (const auto& event : section.events) {
            merged.events.push_back({event.binding_id, event.time_ns, event.fifo_order, event.period_ns});
        }
        // Events only the fresh system queued keep their phase relative to the restored now.
        const auto fresh = timing.CaptureCheckpoint(fresh_bindings);
        std::unordered_map<u64, u64> added; // fresh binding -> merged binding
        for (const auto& event : fresh.events) {
            const auto& type = fresh_bindings[event.binding_id - 1].type;
            if (type == hardware_timer || bound.contains(type->name)) continue;
            auto [entry, inserted] = added.emplace(event.binding_id, next_id + 1);
            if (inserted) bindings.push_back({++next_id, type});
            merged.events.push_back({entry->second,
                                     merged.time_ns + std::max<s64>(0, event.time_ns - fresh.time_ns),
                                     merged.next_fifo_id++, event.period_ns});
        }
        timing.RestoreCheckpoint(merged, bindings);
        kernel.HardwareTimer().RearmAfterCheckpointRestore();
        stats.timing_events = static_cast<u32>(merged.events.size());
        break;
    }
    default:
        Invalid("Pulse: unexpected process section");
    }
}

void ProcessRestore::Finish() {
    if (finished) return;
    if (next_section != SectionOrder.size() || !layout_verified) Invalid("Pulse: missing process sections");
    if (Common::Pulse::Diagnostics()) {
        // What each restored page holds before the guest runs, and the watch on the shader code.
        Common::Pulse::ClearRestoredPages();
        auto& memory = process->GetMemory();
        UntouchedPages untouched;
        for (const auto& run : state->layout.runs) {
            for (u64 page = 0; page < run.pages; ++page) {
                const u8* const host = memory.GetPointerSilent(run.address + page * PageSize);
                if (!host) continue;
                Common::Pulse::NoteRestoredPage(host, untouched.Contains(host));
            }
        }
        LOG_INFO(Core, "Pulse: restored GPU 0x400000000+256 KiB is {}",
                 Common::DescribeWatchedPages());
        Common::ArmDramWriteWatch();
    }
    auto& kernel = system.Kernel();
    for (const auto& [thread, run] : state->threads) {
        if (run == ThreadRun::Runnable) {
            Check(thread->Run(kernel), "Pulse: thread start failed");
        } else if (run == ThreadRun::Waiting) {
            Kernel::KScopedSchedulerLock lock{kernel};
            thread->ActivateRestoredWaiter(kernel);
        }
    }
    finished = true;
    // The handle table, running and waiting references now own every object.
    for (auto it = state->created.rbegin(); it != state->created.rend(); ++it) (*it)->Close(kernel);
    state->created.clear();
}

} // namespace Core::Pulse
