// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_software_keyboard.h"

#include <utility>

#include "common/logging.h"

namespace EdenXbox {
namespace {
using Service::AM::Frontend::SwkbdReplyType;
using Service::AM::Frontend::SwkbdResult;

// UTF-16 both ways: wchar_t is 16 bits on Windows.
std::wstring Wide(std::u16string_view text) {
    return {text.begin(), text.end()};
}
std::u16string Utf16(std::wstring_view text) {
    return {text.begin(), text.end()};
}
} // namespace

UwpSoftwareKeyboard::~UwpSoftwareKeyboard() = default;

std::vector<KeyboardEvent> UwpSoftwareKeyboard::TakeEvents() {
    std::scoped_lock lock{mutex};
    return std::exchange(events, {});
}

void UwpSoftwareKeyboard::Reply(const KeyboardReply& reply) {
    // The callbacks run without the lock: they reach the game's applet, which may call back in.
    SubmitNormalCallback normal;
    SubmitInlineCallback inline_reply;
    {
        std::scoped_lock lock{mutex};
        normal = submit_normal;
        inline_reply = submit_inline;
    }
    const auto text = Utf16(reply.text);
    const auto cursor = static_cast<s32>(text.size());
    using Kind = KeyboardReply::Kind;
    if (reply.is_inline) {
        if (!inline_reply) return;
        switch (reply.kind) {
        case Kind::Changed: inline_reply(SwkbdReplyType::ChangedString, text, cursor); break;
        case Kind::Submit: inline_reply(SwkbdReplyType::DecidedEnter, text, cursor); break;
        case Kind::Cancel: inline_reply(SwkbdReplyType::DecidedCancel, {}, 0); break;
        }
        return;
    }
    if (!normal || reply.kind == Kind::Changed) return;
    LOG_INFO(Frontend, "Keyboard: {} ({} characters)", reply.kind == Kind::Submit ? "submitted" : "cancelled",
             text.size());
    if (reply.kind == Kind::Submit) normal(SwkbdResult::Ok, text, reply.confirmed);
    else normal(SwkbdResult::Cancel, {}, true);
}

void UwpSoftwareKeyboard::Post(KeyboardEvent event) const {
    std::scoped_lock lock{mutex};
    events.push_back(std::move(event));
}

void UwpSoftwareKeyboard::Close() const {
    {
        // The game's applet goes away: nothing may reach it any more.
        std::scoped_lock lock{mutex};
        submit_normal = {};
        submit_inline = {};
    }
    Post(KeyboardClose{});
}

void UwpSoftwareKeyboard::InitializeKeyboard(bool is_inline,
                                             Core::Frontend::KeyboardInitializeParameters parameters,
                                             SubmitNormalCallback normal, SubmitInlineCallback inline_reply) {
    // The header is the title; a game without one titles the keyboard with its guide text.
    const bool guide_as_title = parameters.header_text.empty();
    KeyboardOpen open{
        .title = Wide(guide_as_title ? parameters.guide_text : parameters.header_text),
        .hint = Wide(!parameters.sub_text.empty() || guide_as_title ? parameters.sub_text : parameters.guide_text),
        .initial = Wide(parameters.initial_text),
        .max_length = parameters.max_text_length,
        .min_length = parameters.min_text_length,
        .masked = parameters.password_mode == Service::AM::Frontend::SwkbdPasswordMode::Enabled,
        .can_cancel = !parameters.disable_cancel_button,
        .is_inline = is_inline,
    };
    std::scoped_lock lock{mutex};
    pending = std::move(open);
    if (is_inline) submit_inline = std::move(inline_reply);
    else submit_normal = std::move(normal);
}

void UwpSoftwareKeyboard::ShowNormalKeyboard() const {
    KeyboardOpen open;
    {
        std::scoped_lock lock{mutex};
        open = pending;
    }
    LOG_INFO(Frontend, "Keyboard: the game asks for text (up to {} characters)", open.max_length);
    Post(std::move(open));
}

void UwpSoftwareKeyboard::ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult result,
                                              std::u16string message) const {
    LOG_INFO(Frontend, "Keyboard: the game checked the text ({})", static_cast<u32>(result));
    Post(KeyboardTextCheck{Wide(message),
                           result == Service::AM::Frontend::SwkbdTextCheckResult::Confirm});
}

void UwpSoftwareKeyboard::ShowInlineKeyboard(Core::Frontend::InlineAppearParameters appear) const {
    KeyboardOpen open;
    {
        std::scoped_lock lock{mutex};
        if (appear.max_text_length != 0) pending.max_length = appear.max_text_length;
        pending.min_length = appear.min_text_length;
        pending.can_cancel = !appear.disable_cancel_button;
        pending.is_inline = true;
        open = pending;
    }
    LOG_INFO(Frontend, "Keyboard: the game shows its inline keyboard (up to {} characters)", open.max_length);
    Post(std::move(open));
}

void UwpSoftwareKeyboard::HideInlineKeyboard() const {
    Post(KeyboardClose{});
}

void UwpSoftwareKeyboard::InlineTextChanged(Core::Frontend::InlineTextParameters text) const {
    SubmitInlineCallback inline_reply;
    {
        std::scoped_lock lock{mutex};
        pending.initial = Wide(text.input_text);
        inline_reply = submit_inline;
    }
    Post(KeyboardInlineText{Wide(text.input_text)});
    // Acknowledged at once, as the core's stand-in keyboard does: games wait for it.
    if (inline_reply)
        inline_reply(SwkbdReplyType::ChangedString, text.input_text, text.cursor_position);
}

void UwpSoftwareKeyboard::ExitKeyboard() const {
    Post(KeyboardClose{});
}

} // namespace EdenXbox
