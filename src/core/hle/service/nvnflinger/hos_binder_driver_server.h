// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "common/container/unordered_map.h"

#include "common/common_types.h"
#include "core/hle/service/nvnflinger/binder.h"

namespace Core {
class System;
}

namespace Service::Nvnflinger {

class HosBinderDriverServer final {
public:
    explicit HosBinderDriverServer();
    ~HosBinderDriverServer();

    s32 RegisterBinder(std::shared_ptr<android::IBinder>&& binder);
    void UnregisterBinder(s32 binder_id);

    std::shared_ptr<android::IBinder> TryGetBinder(s32 id) const;

    void AdjustRefcount(s32 binder_id, s32 delta, bool is_weak);

    // Pulse: live binder ids, their reference counts, and the id the next binder follows.
    std::vector<s32> PulseBinderIds() const;
    std::pair<s32, s32> PulseRefcounts(s32 binder_id) const;
    void PulseSetRefcounts(s32 binder_id, s32 strong, s32 weak);
    s32 PulseLastId() const;
    void PulseSetLastId(s32 id);

private:
    struct RefCounts {
        s32 strong{1};
        s32 weak{0};
    };

    mutable std::mutex lock;
    s32 last_id = 0;
    ::Common::unordered_map<s32, std::shared_ptr<android::IBinder>> binders;
    ::Common::unordered_map<s32, RefCounts> refcounts;
};

} // namespace Service::Nvnflinger
