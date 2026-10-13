#pragma once
#include "loader/module_loader.hpp"

// Controller support for MW3 PC (the PC game has none): XInput pads (Xbox, DualShock / DualSense through DS4Windows or
// Steam Input) work like on the console. Buttons are the console's keys (BUTTON_A = cross ... DPAD_*, APAD_* for the
// left stick in menus) with the console's key numbers, so the PS3 menus' key handlers and the PS3 button layouts
// (buttons_*.cfg, "bind BUTTON_A +gostand") work; the sticks move / aim through "bindaxis" (thumbstick_*.cfg) with
// the console's turn rates. The PS3 options (profile: sensitivity, invert, layouts) drive it.
class gamepad final : public module
{
public:
	void post_load() override;

	// a controller has been used more recently than the mouse / keyboard
	static bool in_use();

	// key number of a console key name ("BUTTON_A") or -1, and the name of a console key number (nullptr for others)
	static int key_number(const char* name);
	static const char* key_name(int key);

	// the console button icon (ps3style inline material) of a console key, or nullptr
	static const char* key_icon(int key);

	// the icon of the pad button bound to a command ("+frag") in the current button layout, with the PS3's
	// alternatives (+reload -> +usereload, +melee -> +melee_zoom ...), or nullptr when no pad button has it
	static const char* bound_icon(const char* command);
};
