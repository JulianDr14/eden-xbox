// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "core/arm/exclusive_monitor.h"
#include "core/core.h"
#include "core/core_timing.h"
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
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/pulse_wait_graph.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/memory.h"
#include "core/pulse_checkpoint.h"
#include "core/pulse_machine_state.h"

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
constexpr auto SessionManagerTimeout = std::chrono::seconds{5};

[[noreturn]] void Invalid(const char* reason) {
    throw Failure{Error::Invalid, reason};
}
[[noreturn]] void Incompatible(const char* reason) {
    throw Failure{Error::Incompatible, reason};
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
        Reject(false, std::string{"no serializer for kernel object "} +
                          mutable_object->GetTypeObj().GetName());
    }

    std::vector<ObjectRecord>& Records() {
        return records;
    }
    std::vector<const KAutoObject*>& Objects() {
        return objects;
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

    std::pair<u64, u64> AddEvent(Kernel::KEvent* event) {
        if (event->GetOwner() != process) Reject(false, "event owned by a service process");
        auto* readable = std::addressof(event->GetReadableEvent());
        if (const auto found = ids.find(event); found != ids.end()) {
            return {found->second, ids.at(readable)};
        }
        const auto event_id = Add(event, ObjectKind::Event, 0);
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

// Every session on the sm: port is served by the one SM handler that port's factory returns, so
// the port check (done by the callers) is what proves the handler is SM. The build has no RTTI.
bool IsSmHandler(const Service::SessionRequestHandlerPtr& handler) {
    return handler != nullptr;
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

ProcessCheckpoint ProcessCheckpoint::Capture(Core::System& system) {
    if (!system.IsPaused()) Reject(false, "capture requires a paused system");
    auto& kernel = system.Kernel();
    auto* process = system.ApplicationProcess();
    if (!process || !process->Is64Bit() || process->GetState() != KProcess::State::Running) {
        Reject(false, "no running 64-bit application process");
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
            if (state == ThreadState::Terminated || thread.IsTerminationRequested()) {
                Reject(true, "a guest thread is exiting");
            }
            if (thread.IsInExceptionHandler()) Reject(false, "a guest thread is in an exception handler");
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
            case ThreadState::Initialized:
                record.run = ThreadRun::Initialized;
                break;
            case ThreadState::Runnable:
                if (thread.IsCallingSvc()) Reject(true, "a guest thread is inside an SVC");
                record.run = ThreadRun::Runnable;
                break;
            case ThreadState::Waiting:
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
        }
        for (size_t i = 0; i < registry.Records().size(); ++i) {
            if (registry.Records()[i].kind != ObjectKind::ReadableEvent) continue;
            const auto* readable = static_cast<const Kernel::KReadableEvent*>(registry.Objects()[i]);
            registry.Records()[i].flags = readable->IsSignaled(kernel) ? 1 : 0;
        }
        exclusive = process->GetExclusiveMonitor().CaptureCheckpoint();
    }

    // Sessions: only sm: has a serializer (its whole state is the session graph).
    auto* sm_port = FindSmPort(kernel);
    for (size_t i = 0; i < registry.Records().size(); ++i) {
        auto& record = registry.Records()[i];
        if (record.kind != ObjectKind::ClientSession) continue;
        const auto* client = static_cast<const Kernel::KClientSession*>(registry.Objects()[i]);
        auto* session = client->GetParent();
        if (!session || session->IsServerClosed() || !sm_port || session->GetParent() != sm_port) {
            Reject(false, "IPC session to a service without a serializer");
        }
        const auto manager =
            Service::ServerManager::FindSessionManager(std::addressof(session->GetServerSession()));
        if (!manager) Reject(true, "IPC session is not registered with its server yet");
        if (manager->IsDomain()) Reject(false, "sm: session converted to a domain");
        const auto id = record.id;
        Service::SessionManagerCheckpoint graph;
        try {
            graph = manager->CaptureCheckpoint([&](const Service::SessionRequestHandlerPtr& handler) {
                return IsSmHandler(handler) ? id : u64{0};
            });
        } catch (const std::exception& error) {
            Reject(false, error.what());
        }
        record.payload = Service::Pulse::EncodeSessionGraph(graph);
    }

    // Memory map: what the rebuilt skeleton must reproduce, and what to store.
    auto& table = process->GetPageTable();
    if (table.GetBasePageTable().GetMappedPhysicalMemorySize()) {
        Reject(false, "the application maps physical memory");
    }
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
    std::unordered_set<u64> stored;
    auto& aliases = layout.aliases;
    auto& runs = layout.runs;
    const auto store_page = [&](u64 address) {
        if (!stored.insert(Physical(*process, address)).second) return;
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
            (attribute != KMemoryAttribute::None && attribute != KMemoryAttribute::Locked)) {
            Reject(true, "guest memory is lent to an IPC or device operation");
        }
        const bool normal = state == KMemoryState::Normal;
        if (attribute == KMemoryAttribute::Locked && !normal) {
            Reject(false, "locked memory outside the heap");
        }
        if (normal && (block.address < layout.heap_start || block.address + block.size > heap_end)) {
            Reject(false, "normal memory outside the heap");
        }
        if (state != KMemoryState::Code && state != KMemoryState::CodeData && !normal &&
            state != KMemoryState::Stack && state != KMemoryState::ThreadLocal) {
            Reject(false, "memory state " + std::to_string(block.state & 0xff) + " has no serializer");
        }
        if (attribute == KMemoryAttribute::Locked) {
            for (u64 page = 0; page < block.size; page += PageSize) {
                locked_sources.emplace(Physical(*process, block.address + page), block.address + page);
            }
        }
    }
    for (const auto& block : layout.blocks) {
        const auto state = static_cast<KMemoryState>(block.state);
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
    // A locked heap page with no readable alias cannot be read back: fail closed.
    for (const auto& [physical, address] : locked_sources) {
        if (!stored.contains(physical)) Reject(false, "locked heap page with no readable mapping");
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
            timing_section.bindings.size() == MaxTimingNames || !seen_names.insert(type->name).second) {
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
    auto& memory = process->GetMemory();
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
                               if (!memory.ReadBlockUnsafe(runs[index].address + within,
                                                           block.data() + position, count)) {
                                   Invalid("Pulse: guest memory read failed");
                               }
                               position += count;
                               offset += count;
                               ++index;
                           }
                       });
        }
        write_bytes(entry);
    }
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
        const auto& ends = state->run_ends;
        size_t index = std::upper_bound(ends.begin(), ends.end(), offset) - ends.begin();
        size_t position{};
        u64 cursor = offset;
        while (position < data.size()) {
            const u64 start = index ? ends[index - 1] : 0;
            const u64 within = cursor - start;
            const auto count = static_cast<size_t>(
                std::min<u64>(data.size() - position, state->layout.runs[index].pages * PageSize - within));
            if (!memory.WriteBlockUnsafe(state->layout.runs[index].address + within, data.data() + position,
                                         count)) {
                Invalid("Pulse: guest memory write failed");
            }
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
        if (!process || !process->IsRestoringCheckpoint() || !process->Is64Bit()) {
            Incompatible("Pulse: skeleton loader did not create a pinned process");
        }
        if (process->GetLayoutPin() != layout.pin || process->GetProgramId() != layout.program_id ||
            process->GetProcessId() != layout.process_id) {
            Incompatible("Pulse: rebuilt process identity or layout differs");
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
        for (const auto& alias : layout.aliases) {
            Check(table.MapMemory(alias.destination, alias.source, alias.size),
                  "Pulse: stack mapping rebuild failed");
        }
        break;
    }
    case ProcessSection::Objects: {
        const auto records = DecodeObjects(bytes);
        for (const auto& record : records) {
            switch (record.kind) {
            case ObjectKind::Process:
                break; // bound with the skeleton
            case ObjectKind::Event: {
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
                break;
            }
            case ObjectKind::ReadableEvent: {
                auto* event = resolve(record.parent)->DynamicCast<Kernel::KEvent*>();
                if (!event) Invalid("Pulse: readable event without its event");
                auto* readable = std::addressof(event->GetReadableEvent());
                bind(record.id, readable);
                if (record.flags & 1) Check(readable->Signal(kernel), "Pulse: event signal failed");
                break;
            }
            case ObjectKind::ClientSession: {
                Service::Pulse::SessionGraph graph;
                try {
                    graph = Service::Pulse::DecodeSessionGraph(record.payload);
                } catch (const std::invalid_argument&) {
                    Invalid("Pulse: malformed sm: session graph");
                }
                if (graph.is_domain || graph.session_handler != record.id) {
                    Invalid("Pulse: malformed sm: session graph");
                }
                auto* port = FindSmPort(kernel);
                if (!port) Incompatible("Pulse: sm: port missing");
                Kernel::KClientSession* client{};
                Check(port->CreateSession(kernel, std::addressof(client), process),
                      "Pulse: sm: session rebuild failed");
                state->created.push_back(client);
                bind(record.id, client);
                // The server thread registers the session asynchronously.
                const auto* server = std::addressof(client->GetParent()->GetServerSession());
                const auto deadline = std::chrono::steady_clock::now() + SessionManagerTimeout;
                std::shared_ptr<Service::SessionRequestManager> manager;
                while (!(manager = Service::ServerManager::FindSessionManager(server))) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        Incompatible("Pulse: sm: server did not accept the session");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                if (!manager->HasSessionHandler()) Incompatible("Pulse: sm: handler");
                const auto handler = manager->SessionHandler().shared_from_this();
                manager->RestoreCheckpoint(graph, [&](u64 handler_id) {
                    return handler_id == record.id ? handler : Service::SessionRequestHandlerPtr{};
                });
                break;
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
            // Stopped until the caller runs the system, like every captured thread.
            thread->RequestSuspend(kernel, Kernel::SuspendType::System);
            if (record.flags & 1) thread->RequestSuspend(kernel, Kernel::SuspendType::Thread);
            Check(thread->SetCoreMask(kernel, record.ideal_core, record.affinity),
                  "Pulse: thread core mask rebuild failed");
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
        // The fresh system's own events, by name.
        std::unordered_map<std::string, std::shared_ptr<Core::Timing::EventType>> fresh_types;
        std::vector<Core::Timing::CheckpointEventBinding> fresh_bindings;
        for (const auto& type : timing.CheckpointQueuedEventTypes()) {
            fresh_bindings.push_back({fresh_bindings.size() + 1, type});
            if (type != hardware_timer && !fresh_types.emplace(type->name, type).second) {
                Incompatible("Pulse: duplicated timing event name");
            }
        }
        std::vector<Core::Timing::CheckpointEventBinding> bindings;
        std::unordered_set<std::string> bound;
        u64 next_id{};
        for (const auto& binding : section.bindings) {
            const auto found = fresh_types.find(binding.name);
            if (found == fresh_types.end()) Incompatible("Pulse: timing event missing after rebuild");
            bindings.push_back({binding.id, found->second});
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
