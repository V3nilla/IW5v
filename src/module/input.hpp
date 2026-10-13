#pragma once
#include "loader/module_loader.hpp"

// Mouse / keyboard input for high polling rate devices (8 kHz mice and pads) on the MP client:
// - the window procedure no longer calls SetThreadExecutionState for every message (one call costs ~76 us; at 8000
//   messages a second that was most of the main thread): the display is kept awake at most every few seconds
// - the frame wait's 1 ms nap before every frame is skipped (com_lowLatency)
// - at most com_maxMessagesPerFrame window messages are handled per frame, so a flood can't starve the frame
// - raw mouse input (m_rawinput): read in batches on a thread of its own, summed and taken once per frame
class input final : public module
{
public:
	void post_load() override;
	void pre_destroy() override;

	// the mouse's own movement (raw input counts) since the last call; false when raw input isn't in use or the cursor
	// moved without raw input seeing the mouse (then the cursor's move counts, as in the stock game)
	static bool take_raw_mouse(int& dx, int& dy, bool cursor_moved);

	// raw input is reading the mouse
	static bool raw_active();

	// GetTickCount() of the last move of a real mouse (not one a program injected) that raw input saw, 0 = none
	static unsigned long raw_real_move_time();

	// resets the display and system idle timers, at most every few seconds (XInput pads don't reset them)
	static void keep_display_awake();
};
