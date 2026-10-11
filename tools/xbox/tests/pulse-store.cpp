// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/pulse_store.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <windows.h>

using namespace Common::Pulse;
void Check(bool ok) { if (!ok) std::abort(); }
template <typename F> void Reject(F run) {
    try { run(); } catch (const Failure&) { return; }
    std::abort();
}
int wmain(int argc, wchar_t** argv) {
    const std::string key(48, '0');
    auto serialize = [](Writer& writer) {
        writer.Add({1, 1, 1024}, [](auto, auto block) { std::fill(block.begin(), block.end(), std::uint8_t{42}); });
    };
    if (argc == 4 && std::wstring_view{argv[1]} == L"--terminate") {
        const auto point = static_cast<PublishStep>(_wtoi(argv[3]));
        Store child{std::filesystem::path{argv[2]}, DefaultQuota, [=](auto step) {
            if (step == point) std::_Exit(73); // No unwinding/cleanup: actual process loss.
        }};
        Identity next{};
        next.content[0] = 1;
        if (_wtoi(argv[3]) >= 5) {
            child.RestoreAndConsume(key, Identity{}, 1024, [](auto, auto, auto) {});
        } else if (_wtoi(argv[3]) == 4) {
            child.Publish(key, next, 4ULL << 20, 2ULL << 20, [](Writer& writer) {
                writer.Add({1, 1, 2 * BlockSize}, [](auto offset, auto block) {
                    if (offset) std::_Exit(73);
                    std::fill(block.begin(), block.end(), std::uint8_t{42});
                });
            });
        } else {
            child.Publish(key, next, 4096, 1024, serialize);
        }
        return 1;
    }
    const auto root = std::filesystem::temp_directory_path() /
        ("eden-pulse-gate-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(std::filesystem::create_directory(root));
    Identity identity{};
    auto second = identity;
    second.content[0] = 1;
    Store store{root};
    Check(store.List().empty());
    for (unsigned i = 0; i < 100; ++i) {
        store.Publish(key, identity, 4096, 1024, serialize);
        store.Verify(key, identity, 1024);
        Check(!std::filesystem::exists(root / (key + ".next")));
    }
    {
        const auto entries = store.List();
        Check(entries.size() == 1 && entries[0].state == EntryState::Available &&
              entries[0].header_valid && entries[0].identity == identity && entries[0].bytes > 136);
    }
    // Failures before publication must leave the old identity/data readable.
    for (auto point : {PublishStep::Write, PublishStep::Verify, PublishStep::Sync, PublishStep::Replace}) {
        Store failing{root, DefaultQuota, [=](auto step) {
            if (step == point) throw Failure{Error::Io, "injected publication failure"};
        }};
        Reject([&] { failing.Publish(key, second, 4096, 1024, serialize); });
        store.Verify(key, identity, 1024);
    }
    // Terminate a separate process at each durable publication boundary, then
    // verify the previous generation and recover the orphan without a destructor.
    for (unsigned point = 0; point < 7; ++point) {
        std::wstring command = L"\"" + std::wstring{argv[0]} + L"\" --terminate \"" + root.wstring() +
            L"\" " + std::to_wstring(point);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        Check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &startup, &process) != FALSE);
        Check(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
        DWORD status{};
        Check(GetExitCodeProcess(process.hProcess, &status) && status == 73);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        Store recovered{root};
        if (point == 6) {
            Reject([&] { recovered.Verify(key, identity, 1024); });
            const auto consumed = recovered.List();
            Check(consumed.size() == 1 && consumed[0].state == EntryState::Consumed);
        } else {
            recovered.Verify(key, identity, 1024);
        }
        recovered.Publish(key, identity, 4096, 1024, serialize);
    }
    // Reserved space includes the currently published generation.
    Store full{root, 4096};
    Reject([&] { full.Publish(key, second, 4096, 1024, serialize); });
    store.Verify(key, identity, 1024);
    // Actual output cannot exceed the admitted maximum, including headers/footer.
    Reject([&] { store.Publish(key, second, 140, 1024, serialize); });
    store.Verify(key, identity, 1024);
    Reject([&] { store.Publish(key, second, 4096, 1023, serialize); });
    store.Verify(key, identity, 1024);
    std::stop_source stop;
    Reject([&] { store.Publish(key, second, 4096, 1024, [&](Writer& writer) {
        serialize(writer); stop.request_stop();
    }, stop.get_token()); });
    store.Verify(key, identity, 1024);
    Reject([&] { store.Discard("../../saves"); });
    auto wrong_user = identity;
    wrong_user.user[0] = 1;
    Reject([&] { store.Publish(key, wrong_user, 4096, 1024, serialize); });
    Check(StorageKey(wrong_user).size() == 48 && StorageKey(wrong_user) != key);
    // Simulate restart after process termination with an incomplete temporary.
    { std::ofstream temp{root / (key + ".next")}; temp << "partial"; }
    Store restarted{root};
    restarted.Verify(key, identity, 1024);
    restarted.Publish(key, second, 4096, 1024, serialize);
    restarted.Verify(key, second, 1024);
    Reject([&] { restarted.Verify(key, identity, 1024); });
    unsigned rebuilt{};
    // Identity and all bytes are checked before calling any reconstruction code.
    Reject([&] { restarted.RestoreAndConsume(key, identity, 1024, [&](auto, auto, auto) { ++rebuilt; }); });
    Check(rebuilt == 0);
    for (auto point : {PublishStep::Rebuild, PublishStep::Consume}) {
        Store failing{root, DefaultQuota, [=](auto step) {
            if (step == point) throw Failure{Error::Io, "injected restore failure"};
        }};
        Reject([&] { failing.RestoreAndConsume(key, second, 1024, [](auto, auto, auto) {}); });
        restarted.Verify(key, second, 1024);
    }
    std::stop_source restore_stop;
    Reject([&] { restarted.RestoreAndConsume(key, second, 1024, [&](auto, auto, auto block) {
        if (!block.empty()) restore_stop.request_stop();
    }, restore_stop.get_token()); });
    restarted.Verify(key, second, 1024);
    restarted.RestoreAndConsume(key, second, 1024, [&](auto section, auto offset, auto block) {
        Check(section.id == 1 && section.version == 1 && offset == 0);
        for (auto byte : block) Check(byte == 42);
        rebuilt += static_cast<unsigned>(block.size());
    });
    Check(rebuilt == 1024);
    Reject([&] { restarted.Verify(key, second, 1024); });
    {
        const auto entries = restarted.List();
        Check(entries.size() == 1 && entries[0].state == EntryState::Consumed && entries[0].header_valid);
    }
    // A fresh Store after process restart must not offer consumed data again.
    Store after_restore{root};
    Reject([&] { after_restore.RestoreAndConsume(key, second, 1024, [](auto, auto, auto) {}); });
    after_restore.Publish(key, second, 4096, 1024, serialize);
    after_restore.Verify(key, second, 1024);
    // Catalogue only reads a header. Payload corruption is caught before rebuild.
    {
        const auto path = root / (key + ".pulse");
        std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(-33, std::ios::end);
        file.put('\0');
        file.close();
        unsigned callbacks{};
        Reject([&] { after_restore.RestoreAndConsume(key, second, 1024,
            [&](auto, auto, auto) { ++callbacks; }); });
        Check(callbacks == 0);
    }
    restarted.Discard(key);
    // Different games/users have independent objects and explicit discard scope.
    auto other_game = identity;
    other_game.program_id = 0x123456789abcdef0ULL;
    auto other_profile = identity;
    other_profile.user[0] = 0xab;
    const auto game_key = StorageKey(other_game);
    const auto profile_key = StorageKey(other_profile);
    Check(game_key.substr(0, 16) == "123456789abcdef0");
    after_restore.Publish(game_key, other_game, 4096, 1024, serialize);
    after_restore.Publish(profile_key, other_profile, 4096, 1024, serialize);
    Check(after_restore.List().size() == 2);
    after_restore.Discard(game_key);
    after_restore.Verify(profile_key, other_profile, 1024);
    {
        const auto path = root / (profile_key + ".pulse");
        std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(6);
        file.put('\2');
        file.close();
        const auto entries = after_restore.List();
        Check(entries.size() == 1 && entries[0].state == EntryState::UnknownFormat && !entries[0].header_valid);
        Check(std::filesystem::exists(path)); // Listing never removes incompatible data.
    }
    after_restore.Discard(profile_key);
    Check(std::filesystem::is_empty(root));
    Check(std::filesystem::remove(root));
    std::cout << "Pulse store: 100 publications, 7 process terminations, catalogue, two-pass restore/consume, fault preservation, quota, cancellation and discard passed\n";
}
