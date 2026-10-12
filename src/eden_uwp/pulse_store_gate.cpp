// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <stop_token>
#include <string>

#include "common/fs/path_util.h"
#include "common/pulse_store.h"
#include "eden_uwp/pulse_store_gate.h"

namespace EdenXbox {
namespace {

using namespace Common::Pulse;

// Publish/RestoreAndConsume boundaries the gate ends the process at, in order. Points 0-3 fail a
// publication at its fault step, 4 dies halfway through writing the archive, 5-6 die while a
// restore consumes the published state.
constexpr unsigned TerminationPoints = 7;
constexpr u64 Payload = 1024;

void Check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(std::string{"Pulse store gate: "} + what);
}

template <typename F>
void Reject(F run, const char* what) {
    try {
        run();
    } catch (const Failure&) {
        return;
    }
    throw std::runtime_error(std::string{"Pulse store gate: accepted "} + what);
}

void Serialize(Writer& writer) {
    writer.Add({1, 1, Payload}, [](auto, auto block) { std::ranges::fill(block, u8{42}); });
}

Identity Base() {
    return Identity{};
}

Identity Next() {
    Identity next{};
    next.content[0] = 1;
    return next;
}

std::string BaseKey() {
    return std::string(48, '0');
}

// The in-process part: everything the desktop harness checks except the process terminations.
void RunSuite(const std::filesystem::path& root) {
    const auto key = BaseKey();
    const auto identity = Base();
    const auto second = Next();
    Store store{root};
    Check(store.List().empty(), "store not empty at start");
    for (unsigned i = 0; i < 100; ++i) {
        store.Publish(key, identity, 4096, Payload, Serialize);
        store.Verify(key, identity, Payload);
        Check(!std::filesystem::exists(root / (key + ".next")), "temporary left after publish");
    }
    {
        const auto entries = store.List();
        Check(entries.size() == 1 && entries[0].state == EntryState::Available &&
                  entries[0].header_valid && entries[0].identity == identity && entries[0].bytes > 136,
              "catalogue after publications");
    }
    // A failure before publication keeps the old generation readable.
    for (auto point : {PublishStep::Write, PublishStep::Verify, PublishStep::Sync, PublishStep::Replace}) {
        Store failing{root, DefaultQuota, [=](auto step) {
                          if (step == point) throw Failure{Error::Io, "injected publication failure"};
                      }};
        Reject([&] { failing.Publish(key, second, 4096, Payload, Serialize); }, "an injected fault");
        store.Verify(key, identity, Payload);
    }
    // The quota counts the published generation; the admitted maximum bounds every written byte.
    Store full{root, 4096};
    Reject([&] { full.Publish(key, second, 4096, Payload, Serialize); }, "a publication over quota");
    store.Verify(key, identity, Payload);
    Reject([&] { store.Publish(key, second, 140, Payload, Serialize); }, "output past its admission");
    store.Verify(key, identity, Payload);
    Reject([&] { store.Publish(key, second, 4096, Payload - 1, Serialize); }, "an oversized payload");
    store.Verify(key, identity, Payload);
    std::stop_source stop;
    Reject([&] {
        store.Publish(key, second, 4096, Payload, [&](Writer& writer) {
            Serialize(writer);
            stop.request_stop();
        }, stop.get_token());
    }, "a cancelled publication");
    store.Verify(key, identity, Payload);
    Reject([&] { store.Discard("../../saves"); }, "a path as key");
    auto wrong_user = identity;
    wrong_user.user[0] = 1;
    Reject([&] { store.Publish(key, wrong_user, 4096, Payload, Serialize); }, "another user's key");
    // An incomplete temporary from a lost process.
    {
        std::ofstream temp{root / (key + ".next")};
        temp << "partial";
    }
    Store restarted{root};
    restarted.Verify(key, identity, Payload);
    restarted.Publish(key, second, 4096, Payload, Serialize);
    restarted.Verify(key, second, Payload);
    Reject([&] { restarted.Verify(key, identity, Payload); }, "a replaced identity");
    unsigned rebuilt{};
    Reject([&] {
        restarted.RestoreAndConsume(key, identity, Payload, [&](auto, auto, auto) { ++rebuilt; });
    }, "a restore with the wrong identity");
    Check(rebuilt == 0, "rebuild ran before validation");
    for (auto point : {PublishStep::Rebuild, PublishStep::Consume}) {
        Store failing{root, DefaultQuota, [=](auto step) {
                          if (step == point) throw Failure{Error::Io, "injected restore failure"};
                      }};
        Reject([&] { failing.RestoreAndConsume(key, second, Payload, [](auto, auto, auto) {}); },
               "an injected restore fault");
        restarted.Verify(key, second, Payload);
    }
    restarted.RestoreAndConsume(key, second, Payload, [&](auto section, auto offset, auto block) {
        Check(section.id == 1 && section.version == 1 && offset == 0, "restored section");
        Check(std::ranges::all_of(block, [](u8 byte) { return byte == 42; }), "restored bytes");
        rebuilt += static_cast<unsigned>(block.size());
    });
    Check(rebuilt == Payload, "restored size");
    Reject([&] { restarted.Verify(key, second, Payload); }, "a consumed state as available");
    Store after_restore{root};
    Reject([&] { after_restore.RestoreAndConsume(key, second, Payload, [](auto, auto, auto) {}); },
           "a consumed state after restart");
    // Payload corruption is caught before any rebuild callback.
    after_restore.Publish(key, second, 4096, Payload, Serialize);
    {
        std::fstream file{root / (key + ".pulse"), std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(-33, std::ios::end);
        file.put('\0');
    }
    unsigned callbacks{};
    Reject([&] {
        after_restore.RestoreAndConsume(key, second, Payload, [&](auto, auto, auto) { ++callbacks; });
    }, "a corrupt archive");
    Check(callbacks == 0, "rebuild ran on a corrupt archive");
    after_restore.Discard(key);
    // Games and users are independent; listing never removes an unknown format.
    auto other_game = identity;
    other_game.program_id = 0x123456789abcdef0ULL;
    auto other_profile = identity;
    other_profile.user[0] = 0xab;
    const auto game_key = StorageKey(other_game);
    const auto profile_key = StorageKey(other_profile);
    after_restore.Publish(game_key, other_game, 4096, Payload, Serialize);
    after_restore.Publish(profile_key, other_profile, 4096, Payload, Serialize);
    Check(after_restore.List().size() == 2, "two independent entries");
    after_restore.Discard(game_key);
    after_restore.Verify(profile_key, other_profile, Payload);
    {
        std::fstream file{root / (profile_key + ".pulse"), std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(6);
        file.put('\2');
    }
    const auto entries = after_restore.List();
    Check(entries.size() == 1 && entries[0].state == EntryState::UnknownFormat && !entries[0].header_valid,
          "unknown format in the catalogue");
    after_restore.Discard(profile_key);
    Check(std::filesystem::is_empty(root), "store not empty after discards");
}

// Ends the process at one boundary. Returns only if the boundary never came (a gate failure).
void Terminate(const std::filesystem::path& root, unsigned point, const std::function<void()>& restart) {
    const auto key = BaseKey();
    Store child{root, DefaultQuota, [&](PublishStep step) {
                    if (static_cast<unsigned>(step) == point) restart();
                }};
    if (point >= 5) {
        child.RestoreAndConsume(key, Base(), Payload, [](auto, auto, auto) {});
    } else if (point == 4) {
        child.Publish(key, Next(), 4ULL << 20, 2ULL << 20, [&](Writer& writer) {
            writer.Add({1, 1, 2 * BlockSize}, [&](auto offset, auto block) {
                if (offset) restart();
                std::ranges::fill(block, u8{42});
            });
        });
    } else {
        child.Publish(key, Next(), 4096, Payload, Serialize);
    }
}

// After the process died at a point: the previous generation (or, past consumption, nothing) and
// no orphan in the way of the next publication.
void Recover(const std::filesystem::path& root, unsigned point) {
    const auto key = BaseKey();
    Store recovered{root};
    if (point == 6) {
        Reject([&] { recovered.Verify(key, Base(), Payload); }, "a state consumed before the loss");
        const auto consumed = recovered.List();
        Check(consumed.size() == 1 && consumed[0].state == EntryState::Consumed,
              "consumed state after the loss");
    } else {
        recovered.Verify(key, Base(), Payload);
    }
    recovered.Publish(key, Base(), 4096, Payload, Serialize);
}

unsigned ReadStage(const std::filesystem::path& path) {
    std::ifstream file{path};
    unsigned stage{};
    if (!(file >> stage)) return 0;
    return stage;
}

void WriteStage(const std::filesystem::path& path, unsigned stage) {
    const auto temp = std::filesystem::path{path}.concat(".tmp");
    {
        std::ofstream file{temp, std::ios::trunc};
        file << stage;
        file.flush();
        Check(file.good(), "stage file not written");
    }
    std::filesystem::rename(temp, path);
}

} // namespace

PulseStoreGateResult RunPulseStoreGate(const PulseStoreGateOptions& options) {
    const auto& diag = options.diag;
    const auto pulse = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "pulse";
    const auto root = pulse / "store-gate";
    const auto stage_path = pulse / "store-gate.stage";
    try {
        std::filesystem::create_directories(pulse);
        unsigned stage = std::filesystem::exists(stage_path) ? ReadStage(stage_path) : 0;
        if (stage == 0) {
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root);
            diag("PULSE store: suite in " + root.string());
            RunSuite(root);
            Store{root}.Publish(BaseKey(), Base(), 4096, Payload, Serialize);
            diag("PULSE store: suite passed (100 publications, injected faults, quota, cancellation, "
                 "consume, corruption, catalogue, discard)");
        } else {
            if (stage > TerminationPoints) throw std::runtime_error("Pulse store gate: bad stage file");
            Recover(root, stage - 1);
            diag("PULSE store: recovered after the process ended at boundary " +
                 std::to_string(stage - 1));
        }
        if (stage < TerminationPoints) {
            WriteStage(stage_path, stage + 1);
            diag("PULSE store: ending the process at boundary " + std::to_string(stage) + " of " +
                 std::to_string(TerminationPoints));
            Terminate(root, stage, options.restart);
            throw std::runtime_error("Pulse store gate: boundary " + std::to_string(stage) +
                                     " never reached");
        }
        Store{root}.Discard(BaseKey());
        Check(std::filesystem::is_empty(root), "store not empty at the end");
        std::filesystem::remove_all(root);
        std::filesystem::remove(stage_path);
        diag("PULSE store gate PASS: suite and " + std::to_string(TerminationPoints) +
             " process terminations recovered in LocalState");
        return PulseStoreGateResult::Pass;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(stage_path, ignored);
        diag(std::string{"PULSE store gate FAIL: "} + error.what());
        return PulseStoreGateResult::Fail;
    }
}

} // namespace EdenXbox
