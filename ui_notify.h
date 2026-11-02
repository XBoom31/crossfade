#pragma once

#include <string>
#include "ytdlp.h"

// Toast notification system: small popups stacked in the bottom-right corner
// (above the bottom bar) that surface backend / playback errors and other
// status changes. Replaces the old inline "Backend: ..." text in the Home tab.
//
// Call ui_notify_draw() exactly once per frame, after all other UI. Because
// toasts animate, ui_notify_animating() tells the frame scheduler to keep
// repainting while anything is on screen or animating out.

enum class UiNotifyType
{
    Info,
    Success,
    Warning,
    Error
};

// Show a toast. duration <= 0 means sticky: it stays until the user closes it
// (used for errors). action_label (optional) renders a button on the toast.
void ui_notify(UiNotifyType type, const std::string &title, const std::string &message,
               float duration = 6.0f, const char *action_label = nullptr);

// Turn a classified backend error into the right toast: sign-in problems get
// a sticky error with an "Open settings" action, network outages a normal
// warning, a dead backend a sticky error. Same errors are deduped for 10 s
// so every failed command re-reporting the root cause doesn't spam.
void ui_notify_backend_error(const BackendError &err);

// Buttons on toasts: set these once at startup. The settings action is what
// auth-type errors offer ("Open settings").
void ui_notify_set_open_settings(void (*fn)());

// Draw all active toasts. Call after the rest of the frame's UI.
void ui_notify_draw();

// Feed a horizontal touchpad swipe delta (pixels, rightward positive) to the
// toast under the cursor. Returns true when a toast consumed (or was merely
// hovered over and should suppress the global back/forward gesture): the
// toast tracks the fingers, and when the swipe goes idle past a threshold it
// flings out and closes; otherwise it springs back.
bool ui_notify_swipe_feed(float dx_pixels);

// True while any toast is actively animating (slide-in / fade-out), so the
// caller can keep the frame loop repainting at full rate. A resting toast
// (including sticky ones) reports false.
bool ui_notify_animating();

// True when a toast needs a frame soon for any reason: it is animating, or it
// expired while the loop was idle and its fade-out hasn't been started yet.
// Unlike ui_notify_animating() this does NOT mean "render at 60 FPS" -- it
// only means "run one frame now"; the fade then flips animating() on.
bool ui_notify_needs_frame();

void ui_notify_dismiss_all();
