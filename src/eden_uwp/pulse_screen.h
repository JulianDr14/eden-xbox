// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The screen shown while Pulse saves a session (back to the library) or resumes one: a ring that
// fills with the progress, a comet orbiting it and a heartbeat glow, so the player sees the app is
// working through the seconds it takes. Drawn like the in-game menu (Direct2D on a CPU bitmap) on
// its own thread at about 30 fps, and shown by the renderer over the whole screen.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace Core {
class System;
}

namespace EdenXbox {

class PulseScreen {
public:
    enum class Kind { Save, Resume };

    /// Shows the screen at once (the first frame is presented before this returns). The system's
    /// GPU thread must be running, and stay so until the screen is destroyed.
    PulseScreen(Core::System& system, Kind kind, std::wstring game);
    /// Stops drawing. A resume's screen is removed (the game's frame shows again); a save's last
    /// frame stays for the shutdown that follows.
    ~PulseScreen();
    PulseScreen(const PulseScreen&) = delete;
    PulseScreen& operator=(const PulseScreen&) = delete;

    /// A step of PulseSessions::Save ("idle", "process", "gpu", "write", "verify", "sync",
    /// "replace"). Nothing is presented from "idle" to "gpu": the capture finds the GPU thread
    /// with only the guest's work. Returns once a present in flight has finished.
    void SaveStep(std::string_view step);
    /// Resume: the session is restored, the disk shader cache builds next.
    void ShadersStarting();
    /// Resume: a LoadDiskResources callback. Any thread.
    void Shaders(std::size_t done, std::size_t total);
    /// Resume: the guest runs; the screen closes by itself once the game presents.
    void GameStarting();
    /// Save: the closing frame (saved, or not) for a moment, then drawing stops.
    void Finish(bool saved);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace EdenXbox
