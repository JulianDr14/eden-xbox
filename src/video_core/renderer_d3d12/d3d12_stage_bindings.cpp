// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bitset>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "video_core/renderer_d3d12/d3d12_stage_bindings.h"

namespace D3D12 {
namespace {

template <typename Destination, typename Source>
void CopyDescriptors(Destination& destination, const Source& source) {
    destination.assign(source.begin(), source.end());
}

void Combine(size_t& seed, size_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

template <typename Descriptors>
void HashDescriptors(size_t& seed, const Descriptors& descriptors) {
    Combine(seed, descriptors.size());
    for (const auto& desc : descriptors) {
        Combine(seed, desc.count);
        if constexpr (requires { desc.cbuf_index; }) {
            Combine(seed, (size_t(desc.cbuf_index) << 32) | desc.cbuf_offset);
        } else {
            Combine(seed, desc.index);
        }
    }
}

size_t Hash(const StageBindings& bindings) {
    size_t seed = 0;
    HashDescriptors(seed, bindings.constant_buffer_descriptors);
    HashDescriptors(seed, bindings.storage_buffers_descriptors);
    HashDescriptors(seed, bindings.texture_buffer_descriptors);
    HashDescriptors(seed, bindings.image_buffer_descriptors);
    HashDescriptors(seed, bindings.texture_descriptors);
    HashDescriptors(seed, bindings.image_descriptors);
    Combine(seed, std::hash<std::bitset<512>>{}(bindings.loads.mask));
    Combine(seed, std::hash<std::bitset<512>>{}(bindings.stores.mask));
    Combine(seed, bindings.uses_render_area);
    return seed;
}

struct Interner {
    std::mutex mutex;
    std::unordered_multimap<size_t, std::weak_ptr<const StageBindings>> entries;
    size_t inserts_since_sweep{};
};

Interner& GetInterner() {
    static Interner interner;
    return interner;
}

} // Anonymous namespace

bool StageBindings::operator==(const StageBindings& other) const {
    return constant_buffer_descriptors == other.constant_buffer_descriptors &&
           storage_buffers_descriptors == other.storage_buffers_descriptors &&
           texture_buffer_descriptors == other.texture_buffer_descriptors &&
           image_buffer_descriptors == other.image_buffer_descriptors &&
           texture_descriptors == other.texture_descriptors &&
           image_descriptors == other.image_descriptors && loads.mask == other.loads.mask &&
           stores.mask == other.stores.mask && uses_render_area == other.uses_render_area;
}

std::shared_ptr<const StageBindings> InternStage(const Shader::Info& info) {
    auto bindings = std::make_shared<StageBindings>();
    CopyDescriptors(bindings->constant_buffer_descriptors, info.constant_buffer_descriptors);
    CopyDescriptors(bindings->storage_buffers_descriptors, info.storage_buffers_descriptors);
    CopyDescriptors(bindings->texture_buffer_descriptors, info.texture_buffer_descriptors);
    CopyDescriptors(bindings->image_buffer_descriptors, info.image_buffer_descriptors);
    CopyDescriptors(bindings->texture_descriptors, info.texture_descriptors);
    CopyDescriptors(bindings->image_descriptors, info.image_descriptors);
    bindings->loads = info.loads;
    bindings->stores = info.stores;
    bindings->uses_render_area = info.uses_render_area;
    const size_t hash = Hash(*bindings);

    Interner& interner = GetInterner();
    std::scoped_lock lock{interner.mutex};
    const auto [first, last] = interner.entries.equal_range(hash);
    for (auto it = first; it != last; ++it) {
        if (auto shared = it->second.lock(); shared && *shared == *bindings) {
            return shared;
        }
    }
    // Entries of destroyed pipelines expire; drop them once the map has doubled since the sweep.
    if (++interner.inserts_since_sweep > (std::max)(interner.entries.size(), size_t(1024))) {
        std::erase_if(interner.entries, [](const auto& entry) { return entry.second.expired(); });
        interner.inserts_since_sweep = 0;
    }
    interner.entries.emplace(hash, bindings);
    return bindings;
}

} // namespace D3D12
