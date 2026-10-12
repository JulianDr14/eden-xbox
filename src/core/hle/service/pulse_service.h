// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pulse serializers of HLE service interfaces. A checkpoint stores, per interface the guest can
// reach, a stable kind, the port through which a fresh System reaches its service, and the state
// the interface declares. Restoring never replays IPC: the service's root handler rebuilds each
// interface from that state (SessionRequestHandler::PulseRestore).

#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"

namespace Core {
class System;
}
namespace Kernel {
class KEvent;
class KProcess;
} // namespace Kernel

namespace Service {
class SessionRequestHandler;
using SessionRequestHandlerPtr = std::shared_ptr<SessionRequestHandler>;
} // namespace Service

namespace Service::Pulse {

inline constexpr size_t MaxHandlerState = 1U << 20;
inline constexpr size_t MaxServiceString = 1024; // a file system path (0x301) fits

// The interface cannot be serialized in its current state (an operation in flight, a feature
// without a serializer). Retryable when the cause is transient.
class ServiceRejected final : public std::runtime_error {
public:
    ServiceRejected(bool retryable_, const std::string& reason)
        : std::runtime_error{reason}, retryable{retryable_} {}
    const bool retryable;
};

// Little-endian with explicit widths: no native layouts on disk.
class StateWriter {
public:
    void U8(u8 value) {
        Put(value, 1);
    }
    void U32(u32 value) {
        Put(value, 4);
    }
    void U64(u64 value) {
        Put(value, 8);
    }
    void S32(s32 value) {
        Put(static_cast<u32>(value), 4);
    }
    void S64(s64 value) {
        Put(static_cast<u64>(value), 8);
    }
    void Bool(bool value) {
        Put(value ? 1 : 0, 1);
    }
    void Bytes(std::span<const u8> data) {
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    void String(std::string_view text) {
        if (text.size() > MaxServiceString) throw std::invalid_argument("Pulse: service string");
        U32(static_cast<u32>(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
    template <typename T>
    void Raw(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* data = reinterpret_cast<const u8*>(&value);
        bytes.insert(bytes.end(), data, data + sizeof(T));
    }

    std::vector<u8> bytes;

private:
    void Put(u64 value, size_t width) {
        for (size_t i = 0; i < width; ++i) bytes.push_back(static_cast<u8>(value >> (i * 8)));
    }
};

class StateReader {
public:
    explicit StateReader(std::span<const u8> bytes_) : bytes{bytes_} {}

    u8 U8() {
        return static_cast<u8>(Get(1));
    }
    u32 U32() {
        return static_cast<u32>(Get(4));
    }
    u64 U64() {
        return Get(8);
    }
    s32 S32() {
        return static_cast<s32>(static_cast<u32>(Get(4)));
    }
    s64 S64() {
        return static_cast<s64>(Get(8));
    }
    bool Bool() {
        const auto value = Get(1);
        if (value > 1) throw std::invalid_argument("Pulse: service state boolean");
        return value != 0;
    }
    void Bytes(std::span<u8> out) {
        const auto data = Take(out.size());
        std::copy(data.begin(), data.end(), out.begin());
    }
    std::string String() {
        const auto size = U32();
        if (size > MaxServiceString) throw std::invalid_argument("Pulse: service string");
        const auto data = Take(size);
        return {data.begin(), data.end()};
    }
    template <typename T>
    T Raw() {
        static_assert(std::is_trivially_copyable_v<T>);
        T value;
        Bytes({reinterpret_cast<u8*>(&value), sizeof(T)});
        return value;
    }
    // A count of following records, each at least min_record bytes: bounded by what is left.
    u32 Count(size_t min_record) {
        const auto count = U32();
        if (min_record && count > (bytes.size() - offset) / min_record) {
            throw std::invalid_argument("Pulse: service state count");
        }
        return count;
    }
    void End() const {
        if (offset != bytes.size()) throw std::invalid_argument("Pulse: trailing service state");
    }

private:
    u64 Get(size_t width) {
        const auto data = Take(width);
        u64 value{};
        for (size_t i = 0; i < width; ++i) value |= u64{data[i]} << (i * 8);
        return value;
    }
    std::span<const u8> Take(size_t size) {
        if (size > bytes.size() - offset) throw std::invalid_argument("Pulse: truncated service state");
        const auto data = bytes.subspan(offset, size);
        offset += size;
        return data;
    }

    std::span<const u8> bytes;
    size_t offset{};
};

struct ExportedEvent {
    u32 slot{};
    const Kernel::KEvent* event{};
};

// Filled by SessionRequestHandler::PulseCapture, with the system paused.
class HandlerCapture {
public:
    explicit HandlerCapture(std::function<u64(const SessionRequestHandlerPtr&)> identify_)
        : identify{std::move(identify_)} {}

    // Another interface this one depends on: it is captured too and restored first.
    u64 Reference(const SessionRequestHandlerPtr& handler) {
        return handler ? identify(handler) : 0;
    }
    // A service-owned event the guest may hold, under a slot stable for this kind.
    void ExportEvent(u32 slot, const Kernel::KEvent* event) {
        if (event) events.push_back({slot, event});
    }

    StateWriter state;
    std::vector<ExportedEvent> events;

private:
    std::function<u64(const SessionRequestHandlerPtr&)> identify;
};

// Passed to SessionRequestHandler::PulseRestore in a fresh, stopped System.
class HandlerRestore {
public:
    HandlerRestore(Core::System& system_, Kernel::KProcess* process_, std::span<const u8> state_,
                   std::function<SessionRequestHandlerPtr(u64)> resolve_)
        : system{system_}, process{process_}, state{state_}, resolve{std::move(resolve_)} {}

    // An interface referenced at capture (restored before this one).
    SessionRequestHandlerPtr Resolve(u64 id) const {
        if (!id) return nullptr;
        auto handler = resolve(id);
        if (!handler) throw std::invalid_argument("Pulse: unresolved service reference");
        return handler;
    }

    Core::System& system;
    Kernel::KProcess* process;
    StateReader state;

private:
    std::function<SessionRequestHandlerPtr(u64)> resolve;
};

// ---------------------------------------------------------------- wire records

// ObjectKind::ServiceHandler payload.
struct HandlerRecord {
    std::string port; // a named port of the interface's service
    std::string kind; // serializer name, stable across builds
    bool root{};      // the handler a session to the port starts with
    std::vector<u8> state;
};

inline std::vector<u8> EncodeHandlerRecord(const HandlerRecord& record) {
    if (record.port.empty() || record.kind.empty() || record.state.size() > MaxHandlerState) {
        throw std::invalid_argument("Pulse: malformed service handler record");
    }
    StateWriter out;
    out.U32(1);
    out.Bool(record.root);
    out.String(record.port);
    out.String(record.kind);
    out.U32(static_cast<u32>(record.state.size()));
    out.Bytes(record.state);
    return std::move(out.bytes);
}

inline HandlerRecord DecodeHandlerRecord(std::span<const u8> bytes) {
    StateReader in{bytes};
    if (in.U32() != 1) throw std::invalid_argument("Pulse: service handler record version");
    HandlerRecord record;
    record.root = in.Bool();
    record.port = in.String();
    record.kind = in.String();
    const auto size = in.Count(1);
    if (size > MaxHandlerState) throw std::invalid_argument("Pulse: service handler state size");
    record.state.resize(size);
    in.Bytes(record.state);
    in.End();
    if (record.port.empty() || record.kind.empty()) {
        throw std::invalid_argument("Pulse: malformed service handler record");
    }
    return record;
}

// ObjectKind::ClientSession payload: the port it connects to (empty for an interface session a
// request created) and its session graph (pulse_session_graph.h). A clone (CloneCurrentObject)
// names the earlier session whose service session it shares and carries neither.
struct SessionRecord {
    std::string port;
    std::vector<u8> graph;
    u64 clone_of{};
};

inline std::vector<u8> EncodeSessionRecord(const SessionRecord& record) {
    StateWriter out;
    out.U32(2);
    out.String(record.port);
    out.U32(static_cast<u32>(record.graph.size()));
    out.Bytes(record.graph);
    out.U64(record.clone_of);
    return std::move(out.bytes);
}

inline SessionRecord DecodeSessionRecord(std::span<const u8> bytes) {
    StateReader in{bytes};
    if (in.U32() != 2) throw std::invalid_argument("Pulse: session record version");
    SessionRecord record;
    record.port = in.String();
    record.graph.resize(in.Count(1));
    in.Bytes(record.graph);
    record.clone_of = in.U64();
    in.End();
    if (record.clone_of && (!record.port.empty() || !record.graph.empty())) {
        throw std::invalid_argument("Pulse: malformed session clone record");
    }
    return record;
}

} // namespace Service::Pulse
