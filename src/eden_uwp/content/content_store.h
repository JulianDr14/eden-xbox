// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include "eden_uwp/content/content_model.h"

namespace EdenXbox::Content {
struct Catalog {
    std::vector<Revision> revisions;
    std::vector<std::uint64_t> managed_applications;
    std::uint64_t generation{};
};
// Only the serial import worker writes this store. Readers get an immutable copy.
class ContentStore {
public:
    enum class PublishStep { WriteNext, ValidateNext, WriteBackup, ReplaceBackup, ReplaceCatalog };
    using Failure = std::function<bool(PublishStep)>;
    explicit ContentStore(std::filesystem::path root_, Failure failure_ = {})
        : root(std::move(root_)), failure(std::move(failure_)) {}
    Error Load(Catalog& catalog) const;
    Error Save(Catalog& catalog) const;
    Error Remove(Catalog& catalog, const std::string& id) const;
    void Cleanup(const Catalog& catalog) const;
    std::filesystem::path Object(const std::string& id) const;
    std::filesystem::path Temporary(const std::string& id) const;
    const std::filesystem::path& Root() const { return root; }
private:
    std::filesystem::path root;
    Failure failure;
};
} // namespace EdenXbox::Content
