// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The emulated console's keyboard applet for the Xbox frontend. A game asking for text reaches it
// on an emulator thread; it queues what the game asked for (game_keyboard.h's KeyboardEvents) for
// the play loop, which shows the on-screen keyboard over the game and sends the replies back. The
// game keeps running meanwhile, as on the console, but sees no buttons while the keyboard is up.
// Without it the core's stand-in keyboard answers on its own and the player cannot type.

#include <mutex>
#include <vector>

#include "core/frontend/applets/software_keyboard.h"
#include "eden_uwp/game_keyboard.h"

namespace EdenXbox {

class UwpSoftwareKeyboard final : public Core::Frontend::SoftwareKeyboardApplet {
public:
    ~UwpSoftwareKeyboard() override;

    /// What the game asked for since the last call, oldest first. Play loop thread.
    [[nodiscard]] std::vector<KeyboardEvent> TakeEvents();
    /// Sends a reply to the game. Play loop thread.
    void Reply(const KeyboardReply& reply);

    void Close() const override;
    void InitializeKeyboard(bool is_inline, Core::Frontend::KeyboardInitializeParameters parameters,
                            SubmitNormalCallback submit_normal, SubmitInlineCallback submit_inline) override;
    void ShowNormalKeyboard() const override;
    void ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult result,
                             std::u16string message) const override;
    void ShowInlineKeyboard(Core::Frontend::InlineAppearParameters appear) const override;
    void HideInlineKeyboard() const override;
    void InlineTextChanged(Core::Frontend::InlineTextParameters text) const override;
    void ExitKeyboard() const override;

private:
    void Post(KeyboardEvent event) const;

    mutable std::mutex mutex;
    mutable std::vector<KeyboardEvent> events;
    /// The keyboard the game initialized, shown on its next Show call.
    mutable KeyboardOpen pending;
    mutable SubmitNormalCallback submit_normal;
    mutable SubmitInlineCallback submit_inline;
};

} // namespace EdenXbox
