// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator
// Project// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <stdexcept>

#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/pulse_service.h"

namespace Service::AM {

namespace {

std::array<bool*, 23> PulseFlags(Applet& applet) {
    return {&applet.home_button_double_click_enabled,
            &applet.home_button_short_pressed_blocked,
            &applet.home_button_long_pressed_blocked,
            &applet.vr_mode_curtain_required,
            &applet.sleep_required_by_high_temperature,
            &applet.sleep_required_by_low_battery,
            &applet.handling_capture_button_short_pressed_message_enabled_for_applet,
            &applet.handling_capture_button_long_pressed_message_enabled_for_applet,
            &applet.game_play_recording_supported,
            &applet.media_playback_state,
            &applet.jit_service_launched,
            &applet.application_crash_report_enabled,
            &applet.sleep_lock_enabled,
            &applet.vr_mode_enabled,
            &applet.vr_mode_enabled_3d,
            &applet.lcd_backlight_off_enabled,
            &applet.request_exit_to_library_applet_at_execute_next_program_enabled,
            &applet.exit_locked,
            &applet.handles_request_to_display,
            &applet.auto_sleep_disabled,
            &applet.album_image_taken_notification_enabled,
            &applet.record_volume_muted,
            &applet.window_visible};
}

void SaveBuffers(Pulse::StateWriter& out, const std::vector<std::vector<u8>>& buffers) {
    out.U32(static_cast<u32>(buffers.size()));
    for (const auto& buffer : buffers) {
        out.U32(static_cast<u32>(buffer.size()));
        out.Bytes(buffer);
    }
}

std::vector<std::vector<u8>> LoadBuffers(Pulse::StateReader& in) {
    std::vector<std::vector<u8>> buffers(in.Count(4));
    for (auto& buffer : buffers) {
        buffer.resize(in.Count(1));
        in.Bytes(buffer);
    }
    return buffers;
}

} // namespace

void Applet::PulseSave(Core::System& system, Pulse::StateWriter& out) {
    std::unique_lock lk{lock, std::try_to_lock};
    if (!lk.owns_lock()) throw Pulse::ServiceRejected(true, "am: the application applet is busy");
    if (!child_applets.empty() || reserved_applet || is_winding) {
        throw Pulse::ServiceRejected(true, "am: a library applet is open");
    }
    out.U32(terminate_result.raw);
    out.S32(display_logical_width);
    out.S32(display_logical_height);
    out.Raw(display_magnification);
    out.S32(cpu_boost_request_priority);
    out.U32(application_core_usage_mode);
    out.Raw(game_play_recording_state);
    out.Raw(boost_mode);
    out.S32(fatal_section_count);
    out.Raw(album_image_orientation);
    out.Raw(screenshot_permission);
    out.Raw(idle_time_detection_extension);
    out.U64(suspended_ticks);
    out.Raw(screen_shot_identity);
    for (const bool* flag : PulseFlags(*this)) out.Bool(*flag);
    SaveBuffers(out, user_channel_launch_parameter);
    SaveBuffers(out, preselected_user_launch_parameter);
    SaveBuffers(out, friend_invitation_storage_channel);

    // The context stack, bottom first.
    std::vector<std::vector<u8>> contexts;
    for (auto stack = context_stack; !stack.empty(); stack.pop()) {
        contexts.insert(contexts.begin(), stack.top()->GetData());
    }
    SaveBuffers(out, contexts);

    // The modes are the host's: a change since the capture is notified on restore.
    out.Bool(Settings::IsDockedMode());
    out.U32(static_cast<u32>(system.GetAPMController().GetCurrentPerformanceMode()));
    lifecycle_manager.PulseSave(out);
    display_layer_manager.PulseSave(out);
}

void Applet::PulseLoad(Core::System& system, Kernel::KProcess* owner, Pulse::StateReader& in) {
    std::scoped_lock lk{lock};
    terminate_result.raw = in.U32();
    display_logical_width = in.S32();
    display_logical_height = in.S32();
    display_magnification = in.Raw<Common::Rectangle<f32>>();
    cpu_boost_request_priority = in.S32();
    application_core_usage_mode = in.U32();
    game_play_recording_state = in.Raw<GamePlayRecordingState>();
    boost_mode = in.Raw<APM::CpuBoostMode>();
    fatal_section_count = in.S32();
    album_image_orientation = in.Raw<Capture::AlbumImageOrientation>();
    screenshot_permission = in.Raw<ScreenshotPermission>();
    idle_time_detection_extension = in.Raw<IdleTimeDetectionExtension>();
    suspended_ticks = in.U64();
    screen_shot_identity = in.Raw<AppletIdentityInfo>();
    for (bool* flag : PulseFlags(*this)) *flag = in.Bool();
    user_channel_launch_parameter = LoadBuffers(in);
    preselected_user_launch_parameter = LoadBuffers(in);
    friend_invitation_storage_channel = LoadBuffers(in);

    context_stack = {};
    for (auto& data : LoadBuffers(in)) {
        context_stack.push(std::make_shared<IStorage>(system, std::move(data)));
    }

    const bool docked = in.Bool();
    const u32 performance = in.U32();
    lifecycle_manager.PulseLoad(in);
    display_layer_manager.PulseLoad(system, owner, applet_id, library_applet_mode, in);
    if (docked != Settings::IsDockedMode() ||
        performance != static_cast<u32>(system.GetAPMController().GetCurrentPerformanceMode())) {
        lifecycle_manager.OnOperationAndPerformanceModeChanged(system.Kernel());
    }
}

Applet::Applet(Core::System& system, std::unique_ptr<Process> process_, bool is_application)
    : context(system, "Applet"), lifecycle_manager(system, context, is_application),
      process(std::move(process_)), hid_registration(system, *process), overlay_event(context),
      gpu_error_detected_event(context), friend_invitation_storage_channel_event(context),
      notification_storage_channel_event(context), health_warning_disappeared_system_event(context),
      unknown_event(context), acquired_sleep_lock_event(context), pop_from_general_channel_event(context),
      library_applet_launchable_event(context), accumulated_suspended_tick_changed_event(context),
      sleep_lock_event(context), state_changed_event(context) {

    aruid.pid = process->GetProcessId();
    program_id = process->GetProgramId();
}

Applet::~Applet() = default;

void Applet::UpdateSuspensionStateLocked(bool force_message) {
    // Remove any forced resumption.
    lifecycle_manager.RemoveForceResumeIfPossible();

	const bool update_requested_focus_state = lifecycle_manager.UpdateRequestedFocusState();
    const bool curr_activity_runnable = lifecycle_manager.IsRunnable();
    const bool prev_activity_runnable = is_activity_runnable;
    const bool was_changed = curr_activity_runnable != prev_activity_runnable;

    if (was_changed) {
        if (curr_activity_runnable) {
            process->Suspend(false);
        } else {
            process->Suspend(true);
			lifecycle_manager.RequestResumeNotification();
        }

        is_activity_runnable = curr_activity_runnable;
    }

    if (lifecycle_manager.GetForcedSuspend()) {
        // TODO: why is this allowed?
        return;
    }

    // Signal if the focus state was changed or the process state was changed.
    if (update_requested_focus_state || was_changed || force_message) {
        lifecycle_manager.SignalSystemEventIfNeeded(context.kernel);
    }
}

void Applet::SetInteractibleLocked(bool pad_interactible, bool touch_interactible) {
    if (is_pad_interactible == pad_interactible && is_touch_interactible == touch_interactible) {
        return;
    }

    is_pad_interactible = pad_interactible;
    is_touch_interactible = touch_interactible;

    const bool exit_requested = lifecycle_manager.GetExitRequested();
    const bool pad_enabled = pad_interactible && !exit_requested;
    const bool touch_enabled = touch_interactible && !exit_requested;

    LOG_DEBUG(Service_AM, "applet={} pad={} touch={} exit_requested={}",
              static_cast<u32>(applet_id), pad_enabled, touch_enabled, exit_requested);

    hid_registration.EnableAppletToGetInput(pad_enabled, touch_enabled);
}

void Applet::OnProcessTerminatedLocked() {
    is_completed = true;
    state_changed_event.Signal(context.kernel);
}

} // namespace Service::AM
