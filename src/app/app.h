#pragma once

// Runs the SDL/GL/ImGui application: initialization, the main event/render
// loop, input dispatch, playback housekeeping and shutdown.
int app_run(int argc = 0, char **argv = nullptr);
