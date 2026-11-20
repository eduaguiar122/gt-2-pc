#pragma once
// What the phone's android_main tells the flat window backend (game_window_phone.cpp) beyond android_host.h.
//
// A phone, unlike the Quest, draws into the activity's ANativeWindow. Android hands the window over with
// APP_CMD_INIT_WINDOW and takes it back with APP_CMD_TERM_WINDOW whenever the activity leaves the screen; after the
// TERM_WINDOW callback returns, the window can no longer be presented to. The looper thread therefore waits in
// ReleaseWindow until the game's thread has dropped its swapchain and surface (bounded, so that a game thread busy
// loading cannot make the activity unresponsive; the renderer tolerates a lost surface for that case).
#include "android_host.h"

namespace gt2game::android::phone {

// APP_CMD_INIT_WINDOW: the window the game draws into from now on (ANativeWindow*).
void SetWindow(void* nativeWindow);
// APP_CMD_TERM_WINDOW: blocks until the game released the surface of the current window (at most ~3 s), then forgets
// the window.
void ReleaseWindow();
// APP_CMD_GAINED_FOCUS / APP_CMD_LOST_FOCUS (notification shade, system dialogs): input is ignored without focus.
void SetFocused(bool focused);

} // namespace gt2game::android::phone
