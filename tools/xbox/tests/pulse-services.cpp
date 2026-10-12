// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include "core/hle/service/pulse_session_graph.h"

namespace {
struct Handler { int value; };
template <class F>
void MustReject(F&& function) {
    try { function(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("invalid graph accepted");
}
}

int main() {
    using namespace Service::Pulse;
    try {
        for (int cycle = 0; cycle < 100; ++cycle) {
            SessionGraph state;
            {
                auto a = std::make_shared<Handler>(Handler{1});
                auto b = std::make_shared<Handler>(Handler{2});
                std::vector<std::shared_ptr<Handler>> domains{a, {}, b, a};
                state = CaptureSessionGraph<Handler>(a, domains, true, false, true,
                    [&](const auto& handler) -> u64 { return handler == a ? 7 : 8; });
                MustReject([&] {
                    (void)CaptureSessionGraph<Handler>(a, domains, true, false, true,
                        [](const auto&) -> u64 { return 7; });
                });
                MustReject([&] {
                    (void)CaptureSessionGraph<Handler>(a, domains, true, false, true,
                        [](const auto&) -> u64 { return 0; });
                });
            }
            const auto bytes = EncodeSessionGraph(state);
            state = {};
            state = DecodeSessionGraph(bytes);
            for (size_t size = 0; size < bytes.size(); ++size) {
                MustReject([&] { DecodeSessionGraph(std::span{bytes}.first(size)); });
            }
            for (size_t offset : {size_t{0}, size_t{4}, size_t{8}, size_t{12}}) {
                auto bad = bytes; bad[offset] = 0xff;
                MustReject([&] { DecodeSessionGraph(bad); });
            }
            unsigned calls = 0;
            const std::function<std::shared_ptr<Handler>(u64)> resolve = [&](u64 id) {
                ++calls;
                return std::make_shared<Handler>(Handler{static_cast<int>(id)});
            };
            const auto graph = ResolveSessionGraph<Handler>(state, resolve);
            if (calls != 2 || graph.domain_handlers[1] ||
                graph.domain_handlers[0] != graph.session_handler ||
                graph.domain_handlers[3] != graph.session_handler ||
                graph.domain_handlers[2]->value != 8) {
                throw std::runtime_error("alias, closed slot or owner identity changed");
            }
            MustReject([&] {
                (void)ResolveSessionGraph<Handler>(state, [](u64) { return nullptr; });
            });
            auto merged = std::make_shared<Handler>(Handler{0});
            MustReject([&] {
                (void)ResolveSessionGraph<Handler>(state, [&](u64) { return merged; });
            });
            auto invalid = state;
            invalid.is_domain = false;
            MustReject([&] { (void)ResolveSessionGraph<Handler>(invalid, resolve); });
            invalid = state;
            invalid.domain_handlers.resize(65537);
            MustReject([&] { (void)ResolveSessionGraph<Handler>(invalid, resolve); });
        }
        std::cout << "Pulse services: 100 wire/graph cycles, shared owners, holes, limits and rejection passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
