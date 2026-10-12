// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"

namespace Service::Pulse {

struct SessionGraph {
    bool is_domain{};
    bool convert_to_domain{};
    bool initialized_for_sm{};
    u64 session_handler{};
    std::vector<u64> domain_handlers;
};

inline void ValidateSessionGraph(const SessionGraph& state) {
    if (state.domain_handlers.size() > 65536 ||
        (!state.is_domain && !state.domain_handlers.empty()) ||
        (state.is_domain && state.domain_handlers.empty())) {
        throw std::invalid_argument("Pulse: invalid service domain checkpoint");
    }
}

// Section version is explicit; no native bools, pointers or STL layouts on disk.
inline std::vector<u8> EncodeSessionGraph(const SessionGraph& state) {
    ValidateSessionGraph(state);
    std::vector<u8> bytes(24 + state.domain_handlers.size() * 8);
    size_t offset{};
    const auto put = [&](u64 value, size_t width) {
        for (size_t i = 0; i < width; ++i) bytes[offset++] = static_cast<u8>(value >> (i * 8));
    };
    put(1, 4);
    put((state.is_domain ? 1 : 0) | (state.convert_to_domain ? 2 : 0) |
        (state.initialized_for_sm ? 4 : 0), 4);
    put(state.domain_handlers.size(), 4);
    put(0, 4);
    put(state.session_handler, 8);
    for (auto id : state.domain_handlers) put(id, 8);
    return bytes;
}

inline SessionGraph DecodeSessionGraph(std::span<const u8> bytes) {
    if (bytes.size() < 24 || bytes.size() > 24 + 65536 * 8) {
        throw std::invalid_argument("Pulse: service graph section size");
    }
    size_t offset{};
    const auto get = [&](size_t width) {
        u64 value{};
        for (size_t i = 0; i < width; ++i) value |= u64{bytes[offset++]} << (i * 8);
        return value;
    };
    const auto version = get(4);
    const auto flags = get(4);
    const auto count = get(4);
    const auto reserved = get(4);
    const auto session = get(8);
    if (version != 1 || flags > 7 || reserved || count > 65536 || bytes.size() != 24 + count * 8 ||
        (!(flags & 1) && count) || ((flags & 1) && !count)) {
        throw std::invalid_argument("Pulse: malformed service graph section");
    }
    SessionGraph state{bool(flags & 1), bool(flags & 2), bool(flags & 4), session, {}};
    state.domain_handlers.reserve(static_cast<size_t>(count));
    for (u64 i = 0; i < count; ++i) state.domain_handlers.push_back(get(8));
    return state;
}

template <class Handler>
struct ResolvedSessionGraph {
    std::shared_ptr<Handler> session_handler;
    std::vector<std::shared_ptr<Handler>> domain_handlers;
};

// The graph coordinator supplies IDs only for explicitly supported service serializers.
// No names, vtable addresses or host pointers are persisted in SessionGraph.
template <class Handler>
SessionGraph CaptureSessionGraph(
    const std::shared_ptr<Handler>& session, std::span<const std::shared_ptr<Handler>> domains,
    bool is_domain, bool convert_to_domain, bool initialized_for_sm,
    const std::function<u64(const std::shared_ptr<Handler>&)>& identify) {
    if (!identify || domains.size() > 65536 || (!is_domain && !domains.empty()) ||
        (is_domain && domains.empty())) {
        throw std::invalid_argument("Pulse: invalid service graph or domain size");
    }
    std::unordered_map<const Handler*, u64> identities;
    std::unordered_map<u64, const Handler*> owners;
    const auto reference = [&](const std::shared_ptr<Handler>& handler) -> u64 {
        if (!handler) return 0;
        if (auto found = identities.find(handler.get()); found != identities.end()) {
            return found->second;
        }
        const auto id = identify(handler);
        if (id == 0) throw std::runtime_error("Pulse: service handler has no serializer");
        if (!owners.emplace(id, handler.get()).second) {
            throw std::invalid_argument("Pulse: duplicate service graph identity");
        }
        identities.emplace(handler.get(), id);
        return id;
    };
    SessionGraph state{is_domain, convert_to_domain, initialized_for_sm, reference(session), {}};
    state.domain_handlers.reserve(domains.size());
    for (const auto& handler : domains) state.domain_handlers.push_back(reference(handler));
    return state;
}

template <class Handler>
ResolvedSessionGraph<Handler> ResolveSessionGraph(
    const SessionGraph& state, const std::function<std::shared_ptr<Handler>(u64)>& resolve) {
    ValidateSessionGraph(state);
    if (!resolve) throw std::invalid_argument("Pulse: missing service graph resolver");
    std::unordered_map<u64, std::shared_ptr<Handler>> owners;
    std::unordered_map<const Handler*, u64> identities;
    const auto reference = [&](u64 id) -> std::shared_ptr<Handler> {
        if (id == 0) return {};
        if (auto found = owners.find(id); found != owners.end()) return found->second;
        auto handler = resolve(id);
        if (!handler) throw std::invalid_argument("Pulse: unresolved service graph reference");
        if (!identities.emplace(handler.get(), id).second) {
            throw std::invalid_argument("Pulse: distinct service IDs resolved to the same owner");
        }
        owners.emplace(id, handler);
        return handler;
    };
    ResolvedSessionGraph<Handler> rebuilt{reference(state.session_handler), {}};
    rebuilt.domain_handlers.reserve(state.domain_handlers.size());
    for (auto id : state.domain_handlers) rebuilt.domain_handlers.push_back(reference(id));
    return rebuilt;
}
} // namespace Service::Pulse
