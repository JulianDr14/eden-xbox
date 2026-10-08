// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <mutex>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/fiber.h"

#include <boost/context/detail/fcontext.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common {

#ifdef __OPENORBIS__
constexpr size_t DEFAULT_STACK_SIZE = 128 * 4096;
#else
constexpr size_t DEFAULT_STACK_SIZE = 512 * 4096;
#endif

#ifdef _WIN32
/// A fiber stack the OS grows on demand, as it does a thread's: the whole size reserved, only
/// INITIAL_COMMIT committed at the top, with a guard page below it. Each guest thread has a fiber,
/// and a game with 126 of them committed 504 MiB of stacks it barely touched, against the Xbox's
/// 5 GiB app limit.
class FiberStack {
public:
    static constexpr size_t PAGE = 4096;
    static constexpr size_t INITIAL_COMMIT = 64 * 1024;

    explicit FiberStack(size_t size_) : size{size_} {
#ifdef YUZU_UWP_APPCONTAINER
        const auto alloc = VirtualAllocFromApp;
#else
        const auto alloc = VirtualAlloc;
#endif
        base = static_cast<u8*>(alloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE));
        ASSERT_MSG(base, "reserving a fiber stack failed");
        u8* const committed = Top() - INITIAL_COMMIT;
        const bool ok = alloc(committed, INITIAL_COMMIT, MEM_COMMIT, PAGE_READWRITE) != nullptr &&
                        alloc(committed - PAGE, PAGE, MEM_COMMIT,
                              PAGE_READWRITE | PAGE_GUARD) != nullptr;
        ASSERT_MSG(ok, "committing a fiber stack failed");
    }
    ~FiberStack() {
        VirtualFree(base, 0, MEM_RELEASE);
    }
    FiberStack(const FiberStack&) = delete;
    FiberStack& operator=(const FiberStack&) = delete;

    [[nodiscard]] u8* Top() const noexcept {
        return base + size;
    }
    [[nodiscard]] u8* Bottom() const noexcept {
        return base;
    }
    /// The lowest committed byte above the guard page: the TIB's StackLimit for this stack.
    [[nodiscard]] u8* CommittedLimit() const noexcept {
        return Top() - INITIAL_COMMIT;
    }

private:
    size_t size;
    u8* base{};
};

#if defined(_M_X64) || defined(__x86_64__)
/// boost.context's Windows x64 make_fcontext stores the stack's TIB fields in the context it
/// returns (make_x86_64_ms_pe_masm.asm), which jump_fcontext loads into the TIB on every switch.
/// It sets StackLimit to the bottom of the stack, as if all of it were committed; then __chkstk
/// would not probe the pages of a large frame, and one could skip the guard page. So StackLimit
/// becomes the committed limit, and the OS moves it down as the guard page is hit.
void SetGrowableStackLimit(boost::context::detail::fcontext_t context, const FiberStack& stack) {
    constexpr size_t DEALLOCATION_OFFSET = 0xb8;
    constexpr size_t LIMIT_OFFSET = 0xc0;
    constexpr size_t BASE_OFFSET = 0xc8;
    auto* const data = static_cast<u8*>(context);
    const auto field = [data](size_t offset) { return reinterpret_cast<u8**>(data + offset); };
    // Only with the layout this was written for (boost 1.90); with another, nothing is patched.
    if (*field(BASE_OFFSET) == stack.Top() && *field(LIMIT_OFFSET) == stack.Bottom() &&
        *field(DEALLOCATION_OFFSET) == stack.Bottom()) {
        *field(LIMIT_OFFSET) = stack.CommittedLimit();
    } else {
        ASSERT_MSG(false, "unexpected boost.context layout: fiber stack limit left unchanged");
    }
}
#endif
#else
/// Elsewhere the stack is committed as it is touched anyway.
class FiberStack {
public:
    explicit FiberStack(size_t size) : memory(size) {}
    [[nodiscard]] u8* Top() noexcept {
        return memory.data() + memory.size();
    }

private:
    std::vector<u8> memory;
};
#endif

struct Fiber::FiberImpl {
    FiberImpl() {}

    /// None for a thread's own fiber, which runs on the thread's stack.
    std::unique_ptr<FiberStack> stack;
    boost::context::detail::fcontext_t context{};

    std::mutex guard;
    std::function<void()> entry_point;
    std::function<void()> rewind_point;
    std::shared_ptr<Fiber> previous_fiber;

    bool is_thread_fiber = false;
    bool released = false;
};

void Fiber::SetRewindPoint(std::function<void()>&& rewind_func) {
    impl->rewind_point = std::move(rewind_func);
}

Fiber::Fiber(std::function<void()>&& entry_point_func) : impl{std::make_unique<FiberImpl>()} {
    impl->entry_point = std::move(entry_point_func);
    impl->stack = std::make_unique<FiberStack>(DEFAULT_STACK_SIZE);
    impl->context = boost::context::detail::make_fcontext(impl->stack->Top(), DEFAULT_STACK_SIZE, [](boost::context::detail::transfer_t transfer) -> void {
        auto* fiber = static_cast<Fiber*>(transfer.data);
        ASSERT(fiber && fiber->impl && fiber->impl->previous_fiber && fiber->impl->previous_fiber->impl);
        fiber->impl->previous_fiber->impl->context = transfer.fctx;
        fiber->impl->previous_fiber->impl->guard.unlock();
        fiber->impl->previous_fiber.reset();
        fiber->impl->entry_point();
        UNREACHABLE();
    });
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    SetGrowableStackLimit(impl->context, *impl->stack);
#endif
}

Fiber::Fiber() : impl{std::make_unique<FiberImpl>()} {}

Fiber::~Fiber() {
    if (impl && !impl->released) {
        // Make sure the Fiber is not being used
        const bool locked = impl->guard.try_lock();
        ASSERT_MSG(locked, "Destroying a fiber that's still running");
        if (locked) {
            impl->guard.unlock();
        }
    }
}

void Fiber::Abandon() {
    impl.reset();
}

void Fiber::Exit() {
    ASSERT_MSG(impl->is_thread_fiber, "Exiting non main thread fiber");
    if (impl->is_thread_fiber) {
        impl->guard.unlock();
        impl->released = true;
    }
}

void Fiber::YieldTo(std::weak_ptr<Fiber> weak_from, Fiber& to) {
    to.impl->guard.lock();
    to.impl->previous_fiber = weak_from.lock();

    auto transfer = boost::context::detail::jump_fcontext(to.impl->context, &to);
    // "from" might no longer be valid if the thread was killed
    if (auto from = weak_from.lock()) {
        if (from->impl->previous_fiber == nullptr) {
            ASSERT(false && "previous_fiber is nullptr!");
        } else {
            from->impl->previous_fiber->impl->context = transfer.fctx;
            from->impl->previous_fiber->impl->guard.unlock();
            from->impl->previous_fiber.reset();
        }
    }
}

std::shared_ptr<Fiber> Fiber::ThreadToFiber() {
    std::shared_ptr<Fiber> fiber = std::shared_ptr<Fiber>{new Fiber()};
    fiber->impl->guard.lock();
    fiber->impl->is_thread_fiber = true;
    return fiber;
}

} // namespace Common
