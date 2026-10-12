// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"
#include "core/hle/service/nvnflinger/hos_binder_driver_server.h"

namespace Service::Nvnflinger {

HosBinderDriverServer::HosBinderDriverServer() = default;
HosBinderDriverServer::~HosBinderDriverServer() = default;

s32 HosBinderDriverServer::RegisterBinder(std::shared_ptr<android::IBinder>&& binder) {
    std::scoped_lock lk{lock};

    last_id++;

    binders[last_id] = std::move(binder);
    refcounts[last_id] = {}; // strong = 1, weak = 0

    return last_id;
}

void HosBinderDriverServer::UnregisterBinder(s32 binder_id) {
    std::scoped_lock lk{lock};

    binders.erase(binder_id);
    refcounts.erase(binder_id);
}

void HosBinderDriverServer::AdjustRefcount(s32 binder_id, s32 delta, bool is_weak) {
    std::scoped_lock lk{lock};

    auto search_rc = refcounts.find(binder_id);
    if (search_rc == refcounts.end()) {
        LOG_WARNING(Service_VI, "AdjustRefcount called for unknown binder id {}", binder_id);
        return;
    }

    auto& rc = search_rc->second;
    s32& counter = is_weak ? rc.weak : rc.strong;
    counter += delta;

    if (counter < 0)
        counter = 0;

    if (rc.strong == 0 && rc.weak == 0) {
        binders.erase(binder_id);
        refcounts.erase(search_rc);
    }
}

std::vector<s32> HosBinderDriverServer::PulseBinderIds() const {
    std::scoped_lock lk{lock};
    std::vector<s32> ids;
    for (const auto& [id, binder] : binders) {
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::pair<s32, s32> HosBinderDriverServer::PulseRefcounts(s32 binder_id) const {
    std::scoped_lock lk{lock};
    const auto found = refcounts.find(binder_id);
    return found == refcounts.end() ? std::pair<s32, s32>{} : std::pair{found->second.strong, found->second.weak};
}

void HosBinderDriverServer::PulseSetRefcounts(s32 binder_id, s32 strong, s32 weak) {
    std::scoped_lock lk{lock};
    if (strong <= 0 && weak <= 0) {
        binders.erase(binder_id);
        refcounts.erase(binder_id);
        return;
    }
    if (const auto found = refcounts.find(binder_id); found != refcounts.end()) {
        found->second = {(std::max)(strong, 0), (std::max)(weak, 0)};
    }
}

s32 HosBinderDriverServer::PulseLastId() const {
    std::scoped_lock lk{lock};
    return last_id;
}

void HosBinderDriverServer::PulseSetLastId(s32 id) {
    std::scoped_lock lk{lock};
    last_id = id;
}

std::shared_ptr<android::IBinder> HosBinderDriverServer::TryGetBinder(s32 id) const {
    std::scoped_lock lk{lock};

    if (auto search = binders.find(id); search != binders.end()) {
        return search->second;
    }

    return {};
}

} // namespace Service::Nvnflinger
