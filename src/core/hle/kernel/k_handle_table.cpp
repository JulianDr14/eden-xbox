// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_map>
#include "common/scope_exit.h"
#include "core/hle/kernel/k_handle_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"

namespace Kernel {

HandleTableCheckpoint KHandleTable::CaptureCheckpoint(
    KernelCore& kernel, const IdentifyCheckpointObject& identify) const {
    if (!KScheduler::IsSchedulerLockedByCurrentThread(kernel) || !identify) {
        throw std::invalid_argument("Pulse: handle capture requires stopped kernel");
    }
    HandleTableCheckpoint state{};
    std::array<KAutoObject*, MaxTableSize> objects{};
    u16 count{};
    {
        KScopedSpinLock lk{m_lock};
        if (!m_table_size || m_table_size > MaxTableSize) {
            throw std::invalid_argument("Pulse: uninitialized handle table");
        }
        state.entries.resize(m_table_size);
        state.next_linear_id = m_next_linear_id;
        state.max_count = m_max_count;
        state.free_head = static_cast<s16>(m_free_head_index);
        count = m_count;
        for (size_t i = 0; i < m_table_size; ++i) {
            objects[i] = m_objects[i];
            if (objects[i]) state.entries[i].linear_id = m_entry_infos[i].linear_id;
            else state.entries[i].next_free = m_entry_infos[i].next_free_index;
        }
    }
    // The barrier keeps objects alive. Do not call the graph registry under a
    // handle-table spin lock: it may need locks belonging to other subsystems.
    std::unordered_map<u64, const KAutoObject*> identities;
    std::unordered_map<const KAutoObject*, u64> references;
    size_t occupied{};
    for (size_t i = 0; i < state.entries.size(); ++i) {
        if (!objects[i]) continue;
        const auto id = identify(objects[i]);
        if (!id) throw std::invalid_argument("Pulse: handle object has no serializer");
        const auto [entry, inserted] = identities.emplace(id, objects[i]);
        const auto [reference, first] = references.emplace(objects[i], id);
        if ((!inserted && entry->second != objects[i]) || (!first && reference->second != id)) {
            throw std::invalid_argument("Pulse: inconsistent handle object identity");
        }
        state.entries[i].object = id;
        ++occupied;
    }
    if (occupied != count) throw std::invalid_argument("Pulse: handle reservation is still pending");
    ValidateHandleTableCheckpoint(state);
    return state;
}

void KHandleTable::RestoreCheckpoint(KernelCore& kernel, const HandleTableCheckpoint& state,
                                     const ResolveCheckpointObject& resolve) {
    if (!KScheduler::IsSchedulerLockedByCurrentThread(kernel) || !resolve) {
        throw std::invalid_argument("Pulse: handle restore requires stopped kernel");
    }
    ValidateHandleTableCheckpoint(state);
    std::array<KAutoObject*, MaxTableSize> objects{};
    std::unordered_map<u64, KAutoObject*> references;
    std::unordered_map<const KAutoObject*, u64> identities;
    for (size_t i = 0; i < state.entries.size(); ++i) {
        const auto id = state.entries[i].object;
        if (!id) continue;
        if (auto found = references.find(id); found != references.end()) {
            objects[i] = found->second;
        } else {
            auto* object = resolve(id);
            if (!object || !identities.emplace(object, id).second) {
                throw std::invalid_argument("Pulse: unresolved or aliased handle object");
            }
            references.emplace(id, object);
            objects[i] = object;
        }
    }
    size_t opened{};
    SCOPE_EXIT {
        for (size_t i = 0; i < opened; ++i) if (objects[i]) objects[i]->Close(kernel);
    };
    // Each live handle owns a reference, including duplicate handles to one object.
    for (size_t i = 0; i < state.entries.size(); ++i) {
        if (objects[i] && !objects[i]->Open(kernel)) {
            throw std::invalid_argument("Pulse: dead handle object");
        }
        opened = i + 1;
    }
    {
        KScopedSpinLock lk{m_lock};
        if (m_count || (m_table_size && m_table_size != state.entries.size()) ||
            std::any_of(m_objects.begin(), m_objects.end(), [](auto* object) { return object != nullptr; })) {
            throw std::invalid_argument("Pulse: handle restore requires a fresh empty table");
        }
        m_table_size = static_cast<u16>(state.entries.size());
        m_next_linear_id = state.next_linear_id;
        m_max_count = state.max_count;
        m_free_head_index = state.free_head;
        m_count = 0;
        for (size_t i = 0; i < m_table_size; ++i) {
            m_objects[i] = objects[i];
            if (objects[i]) { m_entry_infos[i].linear_id = state.entries[i].linear_id; ++m_count; }
            else m_entry_infos[i].next_free_index = state.entries[i].next_free;
        }
    }
    opened = 0; // Ownership transferred to the reconstructed table.
}

void KHandleTable::Finalize(KernelCore& kernel) {
    // Get the table and clear our record of it.
    u16 saved_table_size = 0;
    {
        KScopedDisableDispatch dd{kernel};
        KScopedSpinLock lk(m_lock);

        std::swap(m_table_size, saved_table_size);
    }

    // Close and free all entries.
    for (size_t i = 0; i < saved_table_size; i++)
        if (KAutoObject* obj = m_objects[i]; obj != nullptr)
            obj->Close(kernel);
}

bool KHandleTable::Remove(KernelCore& kernel, Handle handle) {
    // Don't allow removal of a pseudo-handle.
    if (Svc::IsPseudoHandle(handle)) [[unlikely]] {
        return false;
    }

    // Handles must not have reserved bits set.
    const auto handle_pack = HandlePack(handle);
    if (handle_pack.reserved != 0) [[unlikely]] {
        return false;
    }

    // Find the object and free the entry.
    KAutoObject* obj = nullptr;
    {
        KScopedDisableDispatch dd{kernel};
        KScopedSpinLock lk(m_lock);

        if (this->IsValidHandle(handle)) [[likely]] {
            const auto index = handle_pack.index;

            obj = m_objects[index];
            this->FreeEntry(index);
        } else {
            return false;
        }
    }

    // Close the object.
    kernel.UnregisterInUseObject(obj);
    obj->Close(kernel);
    return true;
}

Result KHandleTable::Add(KernelCore& kernel, Handle* out_handle, KAutoObject* obj) {
    KScopedDisableDispatch dd{kernel};
    KScopedSpinLock lk(m_lock);

    // Never exceed our capacity.
    R_UNLESS(m_count < m_table_size, ResultOutOfHandles);

    // Allocate entry, set output handle.
    {
        const auto linear_id = this->AllocateLinearId();
        const auto index = this->AllocateEntry();

        m_entry_infos[index].linear_id = linear_id;
        m_objects[index] = obj;

        obj->Open(kernel);

        *out_handle = EncodeHandle(static_cast<u16>(index), linear_id);
    }

    R_SUCCEED();
}

KScopedAutoObject<KAutoObject> KHandleTable::GetObjectForIpc(KernelCore& kernel, Handle handle, KThread* cur_thread) const {
    // Handle pseudo-handles.
    ASSERT(cur_thread != nullptr);
    if (handle == Svc::PseudoHandle::CurrentProcess) {
        auto* const cur_process = cur_thread->GetOwnerProcess();
        ASSERT(cur_process != nullptr);
        return {kernel, cur_process};
    }
    if (handle == Svc::PseudoHandle::CurrentThread) {
        return {kernel, cur_thread};
    }
    return GetObjectForIpcWithoutPseudoHandle(kernel, handle);
}

Result KHandleTable::Reserve(KernelCore& kernel, Handle* out_handle) {
    KScopedDisableDispatch dd{kernel};
    KScopedSpinLock lk(m_lock);

    // Never exceed our capacity.
    R_UNLESS(m_count < m_table_size, ResultOutOfHandles);

    *out_handle = EncodeHandle(static_cast<u16>(this->AllocateEntry()), this->AllocateLinearId());
    R_SUCCEED();
}

void KHandleTable::Unreserve(KernelCore& kernel, Handle handle) {
    KScopedDisableDispatch dd{kernel};
    KScopedSpinLock lk(m_lock);

    // Unpack the handle.
    const auto handle_pack = HandlePack(handle);
    const auto index = handle_pack.index;
    const auto linear_id = handle_pack.linear_id;
    const auto reserved = handle_pack.reserved;
    ASSERT(reserved == 0);
    ASSERT(linear_id != 0);

    if (index < m_table_size) [[likely]] {
        // NOTE: This code does not check the linear id.
        ASSERT(m_objects[index] == nullptr);
        this->FreeEntry(index);
    }
}

void KHandleTable::Register(KernelCore& kernel, Handle handle, KAutoObject* obj) {
    KScopedDisableDispatch dd{kernel};
    KScopedSpinLock lk(m_lock);

    // Unpack the handle.
    const auto handle_pack = HandlePack(handle);
    const auto index = handle_pack.index;
    const auto linear_id = handle_pack.linear_id;
    const auto reserved = handle_pack.reserved;
    ASSERT(reserved == 0);
    ASSERT(linear_id != 0);

    if (index < m_table_size) [[likely]] {
        // Set the entry.
        ASSERT(m_objects[index] == nullptr);

        m_entry_infos[index].linear_id = static_cast<u16>(linear_id);
        m_objects[index] = obj;

        obj->Open(kernel);
    }
}

} // namespace Kernel
