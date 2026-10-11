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
    if (!resolve || state.domain_handlers.size() > 65536 ||
        (!state.is_domain && !state.domain_handlers.empty()) ||
        (state.is_domain && state.domain_handlers.empty())) {
        throw std::invalid_argument("Pulse: invalid service domain checkpoint");
    }
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
