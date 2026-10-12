// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pulse phase-2 payload: a multithreaded homebrew that keeps checking its own invariants, so the
// frontend can capture it at any moment, destroy the process, restore it into a fresh System and
// see whether it carries on correctly (boot.cfg "pulse_cycles=").
//
// It exercises what the phase-2 gate names: threads on several cores, a mutex and condition
// variables (address-arbiter waits), a kernel event with timeouts, long sleeps, synchronous IPC
// with sm: and exclusive (LDAXR/STLXR) atomics. Only sm: is opened: __appInit is replaced, so no
// other service session exists for the checkpoint to reject.
//
// Output, through svcOutputDebugString:
//   PULSE_HB <beat> atomic=.. mutex=.. items=.. events=.. timeouts=.. ipc=.. late_ms=..
//   PULSE_FAIL <reason>     an invariant broke (never expected)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define HEAP_SIZE 0x2000000 // 32 MiB: the checkpoint stores every heap page
#define QUEUE_SIZE 16

static volatile u64 g_atomic;      // LDAXR/STLXR increments from every thread
static u64 g_mutex_count;          // under g_lock
static u64 g_mutex_expected;       // under g_lock, a second copy updated separately
static Mutex g_lock;
static Mutex g_queue_lock;
static CondVar g_not_empty, g_not_full, g_never;
static u64 g_queue[QUEUE_SIZE];
static u32 g_queue_head, g_queue_count;
static u64 g_produced, g_consumed; // under g_queue_lock
static Event g_event;
static volatile u64 g_events_fired, g_events_seen, g_timeouts, g_ipc, g_late_ms;
static volatile u64 g_thread_atomic[4];
static volatile bool g_failed;

static void Report(const char* format, ...) {
    char line[192];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    svcOutputDebugString(line, length > 0 && length < (int)sizeof(line) ? (u64)length : strlen(line));
}

static void Fail(const char* reason, u64 a, u64 b) {
    if (!g_failed) Report("PULSE_FAIL %s %lu %lu", reason, a, b);
    g_failed = true;
}

static u64 NowNs(void) {
    return armTicksToNs(armGetSystemTick());
}

static void Bump(int thread) {
    // Part first, total second: a reader that loads the total before the parts never sees the
    // parts behind it.
    __atomic_fetch_add(&g_thread_atomic[thread], 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_atomic, 1, __ATOMIC_SEQ_CST);
}

// A timed wait that must end no earlier than its timeout and not absurdly late (the restored
// timeline is virtual: a capture must not turn a 1.5 s wait into a minute).
static void CheckElapsed(u64 start, u64 timeout_ns) {
    const u64 elapsed = NowNs() - start;
    if (elapsed + 2000000 < timeout_ns) Fail("early_timeout_ms", elapsed / 1000000, timeout_ns / 1000000);
    if (elapsed > timeout_ns) g_late_ms = (elapsed - timeout_ns) / 1000000;
}

static void MutexWorker(void* arg) {
    (void)arg;
    for (u64 i = 0;; ++i) {
        mutexLock(&g_lock);
        ++g_mutex_count;
        ++g_mutex_expected;
        if (g_mutex_count != g_mutex_expected) Fail("mutex", g_mutex_count, g_mutex_expected);
        mutexUnlock(&g_lock);
        Bump(0);
        if ((i & 63) == 0) svcSleepThread(100000);
    }
}

static void Producer(void* arg) {
    (void)arg;
    for (;;) {
        mutexLock(&g_queue_lock);
        while (g_queue_count == QUEUE_SIZE) condvarWait(&g_not_full, &g_queue_lock);
        g_queue[(g_queue_head + g_queue_count) % QUEUE_SIZE] = g_produced++;
        ++g_queue_count;
        condvarWakeOne(&g_not_empty);
        mutexUnlock(&g_queue_lock);
        Bump(1);
        svcSleepThread(1000000);
    }
}

static void Consumer(void* arg) {
    (void)arg;
    for (;;) {
        mutexLock(&g_queue_lock);
        while (g_queue_count == 0) condvarWaitTimeout(&g_not_empty, &g_queue_lock, 500000000);
        const u64 item = g_queue[g_queue_head];
        g_queue_head = (g_queue_head + 1) % QUEUE_SIZE;
        --g_queue_count;
        if (item != g_consumed) Fail("queue_order", item, g_consumed);
        ++g_consumed;
        condvarWakeOne(&g_not_full);
        mutexUnlock(&g_queue_lock);
        Bump(2);
    }
}

// Long waits: the checkpoint will often find this thread blocked in one of them.
static void LongWaiter(void* arg) {
    (void)arg;
    for (u64 round = 0;; ++round) {
        u64 start = NowNs();
        const Result rc = eventWait(&g_event, 3000000000ULL);
        if (R_SUCCEEDED(rc)) {
            ++g_events_seen;
            if (g_events_seen > g_events_fired) Fail("event_count", g_events_seen, g_events_fired);
        } else if (R_VALUE(rc) == KERNELRESULT(TimedOut)) {
            ++g_timeouts;
            CheckElapsed(start, 3000000000ULL);
        } else {
            Fail("event_wait", rc, 0);
        }
        if (round % 4 == 1) {
            start = NowNs();
            svcSleepThread(2000000000LL);
            CheckElapsed(start, 2000000000ULL);
        } else if (round % 4 == 3) {
            mutexLock(&g_queue_lock);
            start = NowNs();
            const Result cv = condvarWaitTimeout(&g_never, &g_queue_lock, 1500000000ULL);
            mutexUnlock(&g_queue_lock);
            if (R_VALUE(cv) != KERNELRESULT(TimedOut)) Fail("never_cv", cv, 0);
            CheckElapsed(start, 1500000000ULL);
            ++g_timeouts;
        }
        Bump(3);
    }
}

// A synchronous IPC round trip with sm: (RegisterClient, idempotent), as libnx would send it.
static Result PingSm(void) {
    if (hosversionAtLeast(12, 0, 0)) {
        return tipcDispatch(smGetServiceSessionTipc(), 0, .in_send_pid = true);
    }
    return serviceDispatch(smGetServiceSession(), 0, .in_send_pid = true);
}

void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    void* address = NULL;
    if (R_FAILED(svcSetHeapSize(&address, HEAP_SIZE))) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    fake_heap_start = (char*)address;
    fake_heap_end = (char*)address + HEAP_SIZE;
}

void __appInit(void) {
    const Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
}

void __appExit(void) {
    smExit();
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    mutexInit(&g_lock);
    mutexInit(&g_queue_lock);
    condvarInit(&g_not_empty);
    condvarInit(&g_not_full);
    condvarInit(&g_never);
    if (R_FAILED(eventCreate(&g_event, true))) Fail("event_create", 0, 0);

    static Thread threads[4];
    static const ThreadFunc entries[4] = {MutexWorker, Producer, Consumer, LongWaiter};
    for (int i = 0; i < 4; ++i) {
        // 64 KiB stacks from the heap, mapped into the stack region (svcMapMemory).
        Result rc = threadCreate(&threads[i], entries[i], NULL, NULL, 0x10000, 0x2c, -2);
        if (R_SUCCEEDED(rc)) rc = threadStart(&threads[i]);
        if (R_FAILED(rc)) Fail("thread_start", i, rc);
    }

    for (u64 beat = 1;; ++beat) {
        svcSleepThread(250000000);
        Bump(0);
        if (beat % 4 == 0) {
            ++g_events_fired;
            eventFire(&g_event);
        }
        const Result rc = PingSm();
        if (R_FAILED(rc)) Fail("sm_ipc", rc, 0);
        ++g_ipc;
        mutexLock(&g_lock);
        const u64 mutex_count = g_mutex_count;
        if (g_mutex_count != g_mutex_expected) Fail("mutex", g_mutex_count, g_mutex_expected);
        mutexUnlock(&g_lock);
        mutexLock(&g_queue_lock);
        const u64 items = g_consumed;
        if (g_produced - g_consumed != g_queue_count) Fail("queue_count", g_produced, g_consumed);
        mutexUnlock(&g_queue_lock);
        const u64 atomic = __atomic_load_n(&g_atomic, __ATOMIC_SEQ_CST);
        u64 parts = 0;
        for (int i = 0; i < 4; ++i) parts += __atomic_load_n(&g_thread_atomic[i], __ATOMIC_SEQ_CST);
        // Total first (see Bump): the parts can only be ahead of it.
        if (parts < atomic) Fail("atomic", atomic, parts);
        Report("PULSE_HB %lu atomic=%lu mutex=%lu items=%lu events=%lu timeouts=%lu ipc=%lu late_ms=%lu%s",
               beat, atomic, mutex_count, items, g_events_seen, g_timeouts, g_ipc, g_late_ms,
               g_failed ? " FAILED" : "");
    }
    return 0;
}
