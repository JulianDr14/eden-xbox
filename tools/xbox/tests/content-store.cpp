// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <source_location>
#include <cstdio>
#include <fstream>
#include <windows.h>
#include "eden_uwp/content/content_store.h"
using namespace EdenXbox::Content;
void Check(bool value, const std::source_location at = std::source_location::current()) { if (!value) { std::fprintf(stderr, "FAIL line %u\n", at.line()); std::exit(1); } }
Revision Item(char id, unsigned version, Kind kind = Kind::Update) {
    Revision r;
    r.id = std::string(64, id); r.application = 0x0100123400000000ULL;
    r.title = r.application | (kind == Kind::Update ? 0x800 : 0x1001);
    r.version = version; r.kind = kind; r.verified = true;
    r.source = "eden-usb:/eden-games-test/content.nsp";
    return r;
}
int main() {
    std::vector<Revision> entries;
    auto first = Item('a', 10); ActivateNew(entries, first, true); entries.push_back(first);
    auto older = Item('b', 5); ActivateNew(entries, older, true); Check(!older.enabled); entries.push_back(older);
    auto newer = Item('c', 20); ActivateNew(entries, newer, true); Check(newer.enabled && !entries[0].enabled); entries.push_back(newer);
    Check(Select(entries, older.id, true) == Error::None && entries[1].enabled && !entries[2].enabled);
    Check(EffectiveVersion(entries, first.application, 0) == 5);
    Check(PlanImport(entries, first) == ImportAction::Duplicate);
    auto conflict = first; conflict.id = std::string(64, 'd');
    Check(PlanImport(entries, conflict) == ImportAction::Conflict);
    auto orphan = Item('e', 1, Kind::Dlc); ActivateNew(entries, orphan, false); Check(!orphan.enabled);
    orphan.verified = false; entries.push_back(orphan);
    Check(Select(entries, orphan.id, true) == Error::HashMismatch);
    Check(!ValidId("../outside") && !ValidId(std::string(64, 'g')));

    const auto temp = std::filesystem::temp_directory_path();
    const auto root = temp / ("eden-content-gate-" + std::to_string(GetCurrentProcessId()));
    Check(std::filesystem::equivalent(root.parent_path(), temp) && root.filename().string().starts_with("eden-content-gate-"));
    std::filesystem::create_directory(root);
    ContentStore store{root}; Catalog catalog;
    Check(store.Load(catalog) == Error::None && catalog.revisions.empty());
    catalog.revisions = entries; catalog.managed_applications.push_back(first.application);
    Check(store.Save(catalog) == Error::None && catalog.generation == 1);
    Catalog loaded; Check(store.Load(loaded) == Error::None && loaded.revisions.size() == 4);
    for (auto stage : {ContentStore::PublishStep::WriteNext, ContentStore::PublishStep::ValidateNext,
                      ContentStore::PublishStep::WriteBackup, ContentStore::PublishStep::ReplaceBackup,
                      ContentStore::PublishStep::ReplaceCatalog}) {
        ContentStore failing{root, [stage](auto at) { return at == stage; }};
        Catalog candidate = catalog; candidate.revisions.clear();
        Check(failing.Save(candidate) != Error::None && candidate.generation == catalog.generation);
        Check(store.Load(loaded) == Error::None && loaded.revisions.size() == 4);
    }
    // A torn/invalid primary recovers the previous complete generation.
    Check(store.Save(catalog) == Error::None);
    { std::ofstream out{root / "catalog.json", std::ios::trunc}; out << "{broken"; }
    Check(store.Load(loaded) == Error::None && loaded.generation == 1);
    Check(store.Save(loaded) == Error::None);
    // Cleanup protects objects in either generation and only generated names.
    loaded.revisions[0].internal = true;
    Check(store.Save(loaded) == Error::None);
    std::filesystem::create_directories(store.Object(first.id));
    std::filesystem::create_directories(store.Temporary(newer.id));
    std::filesystem::create_directories(root / "pending" / "user-folder");
    Check(store.Remove(loaded, first.id) == Error::None);
    Check(std::filesystem::exists(store.Object(first.id)) && !std::filesystem::exists(store.Temporary(newer.id)));
    Check(std::filesystem::exists(root / "pending" / "user-folder"));
    Check(store.Save(loaded) == Error::None); store.Cleanup(loaded);
    Check(!std::filesystem::exists(store.Object(first.id)));
    for (const auto& r : std::vector<Revision>{loaded.revisions}) Check(store.Remove(loaded, r.id) == Error::None);
    Check(store.Load(loaded) == Error::None && loaded.revisions.empty() && loaded.managed_applications.size() == 1);
    // Corruption of both generations must never reset the user's decisions silently.
    { std::ofstream{root / "catalog.json", std::ios::trunc} << "[]";
      std::ofstream{root / "catalog.previous.json", std::ios::trunc} << "{}"; }
    Check(store.Load(loaded) == Error::Registry);
    std::filesystem::remove_all(root);
    std::puts("PASS content: versions/conflicts/downgrade/orphans, five publication faults, recovery, cleanup, tombstone");
}
