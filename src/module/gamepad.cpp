#include <std_include.hpp>
#include "gamepad.hpp"
#include "command.hpp"
#include "input.hpp"
#include "scheduler.hpp"
#include "ps3style.hpp"
#include "game/game.hpp"
#include "game/ui_structs.hpp"
#include "utils/hook.hpp"
#include "utils/io.hpp"
#include "utils/string.hpp"

#include <Xinput.h>
#include <mmsystem.h>
#include <rapidjson/document.h>

#pragma comment(lib, "winmm.lib")

using namespace game::native::ui;

namespace
{
	// ---- iw5mp 1.9.388110 ----
	constexpr DWORD sys_que_event_addr = 0x5CC560; // (time, type, value, value2, ptrLength, ptr)
	constexpr DWORD key_string_to_keynum_addr = 0x48BF90; // int (const char*)
	constexpr DWORD key_keynum_to_string_addr = 0x48C080; // const char* (int keynum, int translate)
	constexpr DWORD cl_mouse_move_addr = 0x489C40; // ebx = localClientNum, edi = usercmd
	constexpr DWORD dvar_register_float_addr = 0x5BEA80; // (name, value, min, max, flags, description)
	constexpr DWORD dvar_register_bool_addr = 0x5BE9F0;
	constexpr DWORD cbuf_add_text_addr = 0x545680;
	constexpr DWORD ui_context = 0x58EF640; // menuStack @A3C, openMenuCount @A7C
	constexpr DWORD key_catchers = 0x10625EC; // KEYCATCH_CONSOLE 1, KEYCATCH_UI 0x10
	constexpr DWORD view_pitch = 0x1065C84; // clientActive viewangles
	constexpr DWORD view_yaw = 0x1065C88;
	constexpr DWORD profile_invert_pitch = 0x132B344; // gamer profile (the PS3 options set these)
	constexpr DWORD profile_auto_aim = 0x132B345;
	constexpr DWORD profile_view_sensitivity = 0x132B304;
	constexpr DWORD profile_buttons_config = 0x132B348;
	constexpr DWORD profile_sticks_config = 0x132B368;
	constexpr DWORD bg_weapon_defs = 0x8DFD88; // WeaponDef*[] by weapon & 0xFF, [0] = "none" (as the game reads it, 0x419AB9; bg_weaponCompleteDefs 0x8DF9C0 + 0x3C8)
	constexpr DWORD bind_commands = 0x8AA3B8; // the bindable commands (key bindings are indexes into it)
	constexpr int bind_command_count = 0x5B;

	// console key numbers (as the PS3 build; the PC game leaves them free)
	enum key_num
	{
		K_BUTTON_A = 1,
		K_BUTTON_B = 2,
		K_BUTTON_X = 3,
		K_BUTTON_Y = 4,
		K_BUTTON_LSHLDR = 5,
		K_BUTTON_RSHLDR = 6,
		K_TAB = 9,
		K_ENTER = 13,
		K_BUTTON_START = 14,
		K_BUTTON_BACK = 15,
		K_BUTTON_LSTICK = 16,
		K_BUTTON_RSTICK = 17,
		K_BUTTON_LTRIG = 18,
		K_BUTTON_RTRIG = 19,
		K_DPAD_UP = 20,
		K_DPAD_DOWN = 21,
		K_DPAD_LEFT = 22,
		K_DPAD_RIGHT = 23,
		K_APAD_UP = 28,
		K_APAD_DOWN = 29,
		K_APAD_LEFT = 30,
		K_APAD_RIGHT = 31,
		K_ESCAPE = 27,
		K_MOUSE1 = 200,
		K_UPARROW = 154,
		K_DOWNARROW = 155,
		K_LEFTARROW = 156,
		K_RIGHTARROW = 157,
	};

	struct key_info
	{
		int key;
		const char* name;
		const char* icon; // ps3_button_* (ps3style\images)
	};

	const key_info keys[] = {
		{K_BUTTON_A, "BUTTON_A", "cross"},
		{K_BUTTON_B, "BUTTON_B", "circle"},
		{K_BUTTON_X, "BUTTON_X", "square"},
		{K_BUTTON_Y, "BUTTON_Y", "triangle"},
		{K_BUTTON_LSHLDR, "BUTTON_LSHLDR", "l1"},
		{K_BUTTON_RSHLDR, "BUTTON_RSHLDR", "r1"},
		{K_BUTTON_START, "BUTTON_START", "start"},
		{K_BUTTON_BACK, "BUTTON_BACK", "select"},
		{K_BUTTON_LSTICK, "BUTTON_LSTICK", "l3"},
		{K_BUTTON_RSTICK, "BUTTON_RSTICK", "r3"},
		{K_BUTTON_LTRIG, "BUTTON_LTRIG", "l2"},
		{K_BUTTON_RTRIG, "BUTTON_RTRIG", "r2"},
		{K_DPAD_UP, "DPAD_UP", "dpad_up"},
		{K_DPAD_DOWN, "DPAD_DOWN", "dpad_down"},
		{K_DPAD_LEFT, "DPAD_LEFT", "dpad_left"},
		{K_DPAD_RIGHT, "DPAD_RIGHT", "dpad_right"},
		{K_APAD_UP, "APAD_UP", "l3"},
		{K_APAD_DOWN, "APAD_DOWN", "l3"},
		{K_APAD_LEFT, "APAD_LEFT", "l3"},
		{K_APAD_RIGHT, "APAD_RIGHT", "l3"},
	};

	// XInput button bit -> console key
	const std::pair<WORD, int> xinput_buttons[] = {
		{XINPUT_GAMEPAD_A, K_BUTTON_A}, {XINPUT_GAMEPAD_B, K_BUTTON_B}, {XINPUT_GAMEPAD_X, K_BUTTON_X},
		{XINPUT_GAMEPAD_Y, K_BUTTON_Y}, {XINPUT_GAMEPAD_LEFT_SHOULDER, K_BUTTON_LSHLDR},
		{XINPUT_GAMEPAD_RIGHT_SHOULDER, K_BUTTON_RSHLDR}, {XINPUT_GAMEPAD_START, K_BUTTON_START},
		{XINPUT_GAMEPAD_BACK, K_BUTTON_BACK}, {XINPUT_GAMEPAD_LEFT_THUMB, K_BUTTON_LSTICK},
		{XINPUT_GAMEPAD_RIGHT_THUMB, K_BUTTON_RSTICK}, {XINPUT_GAMEPAD_DPAD_UP, K_DPAD_UP},
		{XINPUT_GAMEPAD_DPAD_DOWN, K_DPAD_DOWN}, {XINPUT_GAMEPAD_DPAD_LEFT, K_DPAD_LEFT},
		{XINPUT_GAMEPAD_DPAD_RIGHT, K_DPAD_RIGHT},
	};

	// what a console key does in a menu that has no handler of its own for it (the PC menu code knows these). Select
	// isn't back on the PS3: only the menus' own Select handlers use it (Game Summary, Original Maps)
	const std::pair<int, int> menu_keys[] = {
		{K_BUTTON_A, K_ENTER}, {K_BUTTON_START, K_ENTER}, {K_BUTTON_B, K_ESCAPE},
		{K_DPAD_UP, K_UPARROW}, {K_APAD_UP, K_UPARROW}, {K_DPAD_DOWN, K_DOWNARROW}, {K_APAD_DOWN, K_DOWNARROW},
		{K_DPAD_LEFT, K_LEFTARROW}, {K_APAD_LEFT, K_LEFTARROW}, {K_DPAD_RIGHT, K_RIGHTARROW}, {K_APAD_RIGHT, K_RIGHTARROW},
	};

	// ---- axes (bindaxis, thumbstick_*.cfg) ----
	enum physical_axis { A_LSTICK_X, A_LSTICK_Y, A_RSTICK_X, A_RSTICK_Y, A_LTRIGGER, A_RTRIGGER, A_COUNT };
	enum virtual_axis { VA_NONE = -1, VA_SIDE, VA_FORWARD, VA_UP, VA_YAW, VA_PITCH, VA_ATTACK, VA_COUNT };
	enum axis_mapping { MAP_LINEAR, MAP_SQUARED };

	const char* physical_names[] = {"A_LSTICK_X", "A_LSTICK_Y", "A_RSTICK_X", "A_RSTICK_Y", "A_LTRIGGER", "A_RTRIGGER"};
	const char* virtual_names[] = {"VA_SIDE", "VA_FORWARD", "VA_UP", "VA_YAW", "VA_PITCH", "VA_ATTACK"};

	struct axis_binding
	{
		virtual_axis axis = VA_NONE;
		axis_mapping mapping = MAP_LINEAR;
	};

	axis_binding axis_bindings[A_COUNT];

	// ---- state ----
	using xinput_get_state_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
	using xinput_set_state_t = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
	xinput_get_state_t xinput_get_state = nullptr;
	xinput_set_state_t xinput_set_state = nullptr;

	struct pad_state
	{
		bool connected = false;
		int port = -1;
		WORD buttons = 0;
		float sticks[4]{}; // LX, LY, RX, RY (deadzoned, -1..1, up = +)
		float triggers[2]{};
		bool keys_down[64]{};
		unsigned int repeat_time[64]{};
	};

	pad_state pad;
	std::atomic<bool> used_last{false}; // controller more recent than mouse / keyboard (the input filter thread reads it)
	unsigned int last_poll_connect = 0;
	std::atomic<DWORD> pad_active_until{0}; // GetTickCount() until which the pad counts as in use (sticks / buttons)
	std::atomic<bool> mouse_filter_on{false}; // the low-level mouse hook runs
	std::atomic<bool> mouse_filter_wanted{false}; // the low-level mouse hook should run (the pad is in use)
	std::atomic<DWORD> filter_thread{0}; // the hooks' thread (PostThreadMessage)
	std::atomic<DWORD> real_mouse_move{0}; // GetTickCount() of the last mouse move that wasn't injected
	std::atomic<bool> mouse_move_injected{false}; // the last mouse move was injected
	std::unordered_map<int, std::string> icons; // key -> inline icon text (ps3style\button_icons.json)

	game::native::dvar_t* gpad_enabled = nullptr;
	game::native::dvar_t* gpad_ignore_mapped_input = nullptr;
	game::native::dvar_t* gpad_analog_move = nullptr; // the stick's moves as they are while hosting (else -127 / 0 / 127)
	// the PS3's gpad dvars and defaults (MW3 PS3 1.24 0x2ECE74; not saved there, so not here either)
	game::native::dvar_t* gpad_stick_deadzone_min = nullptr;
	game::native::dvar_t* gpad_stick_deadzone_max = nullptr;
	game::native::dvar_t* gpad_button_deadzone = nullptr;
	game::native::dvar_t* gpad_stick_pressed = nullptr;
	game::native::dvar_t* gpad_stick_pressed_hysteresis = nullptr;
	game::native::dvar_t* gpad_menu_scroll_delay_first = nullptr;
	game::native::dvar_t* gpad_menu_scroll_delay_rest = nullptr;

	unsigned int sys_milliseconds()
	{
		return timeGetTime();
	}

	game::native::dvar_t* register_float(const char* name, float value, float min, float max, const char* description, const int flags = 1)
	{
		return reinterpret_cast<game::native::dvar_t*(*)(const char*, float, float, float, int, const char*)>(dvar_register_float_addr)(
			name, value, min, max, flags, description);
	}

	game::native::dvar_t* register_bool(const char* name, bool value, const char* description)
	{
		return reinterpret_cast<game::native::dvar_t*(*)(const char*, bool, int, const char*)>(dvar_register_bool_addr)(
			name, value, 1, description);
	}

	float value(const game::native::dvar_t* dvar, const float fallback)
	{
		return dvar ? dvar->current.value : fallback;
	}

	void cbuf(const std::string& text)
	{
		reinterpret_cast<void(*)(int, const char*)>(cbuf_add_text_addr)(0, text.data());
	}

	bool debug_keys = false; // gpad_debug 1: key events -> ps3style.log

	int location_mapped[64]{}; // pad key -> the key its down went to the location selection as (0 = not mapped)

	void queue_key(const int key, const bool down)
	{
		const auto que = [](const int k, const bool d)
		{
			reinterpret_cast<void(*)(int, int, int, int, int, void*)>(sys_que_event_addr)(sys_milliseconds(), 1, k, d ? 1 : 0, 0, nullptr);
		};

		// the location selection (airstrike map, KEYCATCH_LOCATION_SELECTION 8) takes only Mouse1 (select) and Escape
		// (cancel) (CL_KeyEvent 0x48D207); cross / circle stand for them, as on the PS3. The game acts on the key while
		// it is down (an up clears the state 0xB3D360 before CL_CreateCmd reads it), so the up is sent on the real
		// release, as the same key, whatever the key catchers are by then
		if (key > 0 && key < 64 && !down && location_mapped[key])
		{
			que(location_mapped[key], false);
			location_mapped[key] = 0;
			return;
		}
		if (down && (*reinterpret_cast<int*>(key_catchers) & 8) && (key == K_BUTTON_A || key == K_BUTTON_B))
		{
			location_mapped[key] = key == K_BUTTON_A ? K_MOUSE1 : K_ESCAPE;
			que(location_mapped[key], true);
			return;
		}

		if (debug_keys)
		{
			utils::io::write_file(ps3style::folder() + "\\ps3style.log", utils::string::va("key %d %s (keycatchers %X, used %d)\n", key,
			                                                                               down ? "down" : "up", *reinterpret_cast<int*>(key_catchers), used_last.load()), true);
		}
		que(key, down);
	}

	// ---- XInput ----
	bool load_xinput()
	{
		for (const auto* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
		{
			if (auto* lib = LoadLibraryA(dll))
			{
				xinput_get_state = reinterpret_cast<xinput_get_state_t>(GetProcAddress(lib, "XInputGetState"));
				xinput_set_state = reinterpret_cast<xinput_set_state_t>(GetProcAddress(lib, "XInputSetState"));
				if (xinput_get_state) return true;
			}
		}
		return false;
	}

	// radial deadzone (PS3 0x2ED0B8): below min nothing, above (1 - max) full, scaled in between
	void apply_deadzone(float& x, float& y)
	{
		const auto min = value(gpad_stick_deadzone_min, 0.15f);
		const auto max = value(gpad_stick_deadzone_max, 0.01f);
		const auto length = std::sqrt(x * x + y * y);
		if (length < min || length <= 0.0f)
		{
			x = y = 0.0f;
			return;
		}

		const auto scaled = length <= 1.0f - max ? (length - min) / std::max(0.01f, 1.0f - (min + max)) : 1.0f;
		x = x / length * scaled;
		y = y / length * scaled;
	}

	void start_input_filter();

	bool poll()
	{
		if (!xinput_get_state) return false;

		// a port that isn't connected is only asked again every second (XInputGetState is slow on empty ports)
		const auto now = sys_milliseconds();
		if (!pad.connected && now - last_poll_connect < 1000) return false;

		XINPUT_STATE state{};
		auto found = false;
		for (DWORD i = 0; i < XUSER_MAX_COUNT && !found; ++i)
		{
			const auto port = pad.connected && pad.port >= 0 ? static_cast<DWORD>(pad.port) : i;
			if (xinput_get_state(port, &state) == ERROR_SUCCESS)
			{
				pad.port = static_cast<int>(port);
				found = true;
			}
			if (pad.connected) break;
		}

		if (!found)
		{
			last_poll_connect = now;
			pad.connected = false;
			return false;
		}

		if (!pad.connected)
		{
			printf("Game pad: XInput controller on port %d\n", pad.port);
			start_input_filter();
		}
		pad.connected = true;
		const auto& g = state.Gamepad;
		pad.buttons = g.wButtons;
		pad.sticks[0] = std::max(-1.0f, g.sThumbLX / 32767.0f);
		pad.sticks[1] = std::max(-1.0f, g.sThumbLY / 32767.0f);
		pad.sticks[2] = std::max(-1.0f, g.sThumbRX / 32767.0f);
		pad.sticks[3] = std::max(-1.0f, g.sThumbRY / 32767.0f);
		apply_deadzone(pad.sticks[0], pad.sticks[1]);
		apply_deadzone(pad.sticks[2], pad.sticks[3]);
		pad.triggers[0] = g.bLeftTrigger / 255.0f;
		pad.triggers[1] = g.bRightTrigger / 255.0f;

		auto active = pad.buttons != 0 || pad.triggers[0] > 0.1f || pad.triggers[1] > 0.1f;
		for (const auto stick : pad.sticks) active = active || stick != 0.0f;
		if (active) pad_active_until = GetTickCount() + 2000;
		return true;
	}

	// ---- the same pad as keyboard / mouse ----
	// Steam's desktop controller layout (and mappers like it) also turn the pad into keyboard and mouse input: left stick
	// = arrow keys (turning in MW3), right stick = mouse, triggers = mouse buttons, face buttons = Enter / Space / Page Up
	// / Page Down. While the pad drives the game that input is dropped. It is injected (SendInput); a real keyboard or
	// mouse is not, so they keep working. Low-level hooks on a thread of their own, so a loading screen never holds up
	// the system's input.
	bool game_in_front()
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		return pid == GetCurrentProcessId();
	}

	// the mouse also while the pad is the last thing used, not only for 2 s after it moved: a mapper's mouse that keeps
	// going (touchpad, gyro) would else take the menus back from the pad. The keyboard only for 2 s: a mapper's keys come
	// with pad presses, and no key clears used_last, so other injected keys (on-screen keyboards, streaming) would else
	// be dropped for good
	bool drop_injected(const bool while_used_last)
	{
		if (gpad_ignore_mapped_input && !gpad_ignore_mapped_input->current.enabled) return false;
		return ((while_used_last && used_last) || static_cast<int>(pad_active_until.load() - GetTickCount()) > 0) && game_in_front();
	}

	LRESULT CALLBACK keyboard_filter(const int code, const WPARAM w_param, const LPARAM l_param)
	{
		if (code == HC_ACTION && (reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param)->flags & LLKHF_INJECTED) && drop_injected(false)) return 1;
		return CallNextHookEx(nullptr, code, w_param, l_param);
	}

	LRESULT CALLBACK mouse_filter(const int code, const WPARAM w_param, const LPARAM l_param)
	{
		if (code == HC_ACTION)
		{
			// remembered for cl_mouse_event_stub: only a real mouse gives the menus back to the mouse
			const auto injected = (reinterpret_cast<const MSLLHOOKSTRUCT*>(l_param)->flags & LLMHF_INJECTED) != 0;
			if (w_param == WM_MOUSEMOVE)
			{
				mouse_move_injected = injected;
				if (!injected) real_mouse_move = GetTickCount();
			}
			if (injected && drop_injected(true)) return 1;
		}
		return CallNextHookEx(nullptr, code, w_param, l_param);
	}

	// once a pad is found. Every mouse / key event on the PC waits for these hooks (an 8 kHz mouse: 8000 calls a
	// second), and Windows silently drops a hook that answers too slowly: the thread runs time critical, and the mouse
	// hook is only there while the pad is in use (update_mouse_filter), so a mouse player with a pad plugged in pays
	// nothing for it. The keyboard hook stays (keys come only when pressed / released).
	constexpr UINT filter_update_message = WM_APP + 1;

	void start_input_filter()
	{
		static auto started = false;
		if (started) return;
		started = true;

		std::thread([]
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			MSG msg;
			PeekMessageA(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for PostThreadMessage
			filter_thread = GetCurrentThreadId();

			auto* instance = GetModuleHandleA(nullptr);
			auto* keyboard = SetWindowsHookExA(WH_KEYBOARD_LL, keyboard_filter, instance, 0);
			HHOOK mouse = nullptr;
			const auto update = [&]
			{
				const auto want = mouse_filter_wanted.load();
				if (want && !mouse) mouse = SetWindowsHookExA(WH_MOUSE_LL, mouse_filter, instance, 0);
				else if (!want && mouse)
				{
					UnhookWindowsHookEx(mouse);
					mouse = nullptr;
				}
				mouse_filter_on = mouse != nullptr;
			};

			update();
			while (GetMessageA(&msg, nullptr, 0, 0) > 0)
			{
				if (msg.message == filter_update_message) update();
			}
			mouse_filter_on = false;
			filter_thread = 0;
			if (keyboard) UnhookWindowsHookEx(keyboard);
			if (mouse) UnhookWindowsHookEx(mouse);
		}).detach();
	}

	// every frame: the mouse hook while the pad is in use (it drops the mouse a pad mapper injects and tells a real
	// mouse apart for cl_mouse_event_stub), gone a second after that ends
	void update_mouse_filter(const bool enabled)
	{
		static DWORD unused_since = 0;
		const auto now = GetTickCount();
		const auto pad_in_use = enabled && pad.connected && (used_last || static_cast<int>(pad_active_until.load() - now) > 0) && game_in_front();

		auto want = mouse_filter_wanted.load();
		if (pad_in_use)
		{
			want = true;
			unused_since = 0;
		}
		else if (want)
		{
			if (!unused_since) unused_since = now;
			if (now - unused_since > 1000) want = false;
		}

		if (want == mouse_filter_wanted.load()) return;
		mouse_filter_wanted = want;
		if (const auto thread = filter_thread.load()) PostThreadMessageA(thread, filter_update_message, 0, 0);
	}

	// ---- menus ----
	bool ui_has_focus()
	{
		return (*reinterpret_cast<int*>(key_catchers) & 0x10) != 0;
	}

	// the menu the keys go to: UI_KeyEvent 0x592820 -> Menu_GetFocused 0x5A49A0 (the top visible menu with the focus),
	// not always the top of the menu stack
	menuDef_t* top_menu()
	{
		const auto count = *reinterpret_cast<int*>(ui_context + 0xA7C);
		if (count <= 0 || count > 16) return nullptr;
		return reinterpret_cast<menuDef_t*(*)(DWORD)>(0x5A49A0)(ui_context);
	}

	bool has_key_handler(const ItemKeyHandler* handler, const int key)
	{
		for (auto n = 0; handler && n < 64; handler = handler->next, ++n)
		{
			if (handler->key == key) return true;
		}
		return false;
	}

	// the menu handles this console key itself (the PS3 menus do for some buttons), as the engine decides it: a key
	// the focused item doesn't use goes from Menu_HandleKey 0x5AC9C0 to 0x5A21C0, which runs the menu's onKey, then
	// the onKey of each visible item that has the focus or is a decoration (staticFlags 0x100000: the footer / blade
	// buttons, Friends, Game Summary, Challenges), unless disabled
	bool menu_handles(const int key)
	{
		auto* menu = top_menu();
		if (!menu) return false;
		if (menu->data && has_key_handler(menu->data->onKey, key)) return true;
		if (!menu->items) return false;

		for (auto i = 0; i < menu->itemCount; ++i)
		{
			auto* item = menu->items[i];
			if (!item || !has_key_handler(item->onKey, key)) continue;

			const auto flags = item->window.dynamicFlags[0];
			if (!(flags & 0x4) || !((flags & 0x2) || (item->window.staticFlags & 0x100000))) continue;
			if (!reinterpret_cast<int(*)(int, itemDef_s*)>(0x5AE110)(0, item)) continue; // Item_IsVisible (visibleExp, dvar show)
			if (item->disabledExp && reinterpret_cast<bool(*)(int, Statement_s*)>(0x588A70)(0, item->disabledExp)) continue;
			if ((item->dvarFlags & 3) && !reinterpret_cast<int(*)(itemDef_s*, int)>(0x5ADC50)(item, 1)) continue; // enableDvar
			return true;
		}
		return false;
	}

	int key_sent[64]{}; // pad key -> the key its down went to the game as (0 = dropped)

	// a console key goes to the game as it is when a bind or the menu uses it, else as the PC key a menu understands
	int game_key(const int key)
	{
		if (ui_has_focus())
		{
			// a key binding item waits for a key (g_waitingForKey; the MOUSE & KEYBOARD pages): the pad only cancels,
			// a pad button would else be bound as the PC key it stands for (cross = ENTER)
			if (*reinterpret_cast<int*>(0x5989878)) return key == K_BUTTON_B || key == K_BUTTON_START ? K_ESCAPE : 0;

			if (menu_handles(key)) return key;

			for (const auto& [pad_key, pc_key] : menu_keys)
			{
				if (pad_key == key) return pc_key;
			}
			return 0;
		}

		// in game: start opens the menu like escape does
		return key == K_BUTTON_START ? K_ESCAPE : key;
	}

	// ---- spectating: the console's fixed buttons ----
	// The consoles change the followed player with fixed buttons, whatever the layout: circle the next player, cross the
	// previous one, triangle follows (free view) or stops following. The server's spectator think (0x4F8F60) acts on
	// the buttons the PC's spectator lines (0x45CF10) name: attack (next, or follow), ADS (previous), melee (stop). While
	// spectating those pad keys run these binds themselves, as CL_KeyEvent runs a key's bind (0x48AF00), and the game
	// doesn't get the key; the other buttons (R1 / L1 ...) keep their binds. cg.predictedPlayerState otherFlags (set by
	// the server at 0x4F9A8E..0x4F9B62): 0x800 following, 0x2000 may follow / change player, 0x4000 may stop following
	constexpr DWORD ps_other_flags = 0x900FB0;
	constexpr DWORD cl_run_binding_addr = 0x48AF00; // (localClientNum, binding index, key); the up runs index + 1

	int spectator_binding[64]{}; // pad key -> the binding its down ran while spectating (0 = none)

	int binding_index(const char* command)
	{
		for (auto i = 1; i < bind_command_count; ++i)
		{
			if (!_stricmp(*reinterpret_cast<const char* const*>(bind_commands + 4 * i), command)) return i;
		}
		return 0;
	}

	// the binding a pad key runs while spectating, or 0 (not with a menu, the console, the airstrike map or the chat up:
	// keycatchers 0x10, 1, 8, 0x20)
	int spectator_binding_of(const int key)
	{
		if (*reinterpret_cast<int*>(key_catchers) & 0x39) return 0;
		const auto flags = *reinterpret_cast<int*>(ps_other_flags);
		if (!(flags & 0x6000)) return 0;

		const char* command = nullptr;
		if (flags & 0x800)
		{
			if (key == K_BUTTON_B && (flags & 0x2000)) command = "+attack";
			else if (key == K_BUTTON_A && (flags & 0x2000)) command = "+speed_throw"; // ADS held (a toggle takes two presses)
			else if (key == K_BUTTON_Y && (flags & 0x4000)) command = "+melee";
		}
		else if (key == K_BUTTON_Y && (flags & 0x2000)) command = "+attack";
		return command ? binding_index(command) : 0;
	}

	void run_binding(const int binding, const int key)
	{
		if (debug_keys)
		{
			utils::io::write_file(ps3style::folder() + "\\ps3style.log", utils::string::va("key %d -> spectator %s\n", key,
			                                                                               *reinterpret_cast<const char* const*>(bind_commands + 4 * binding)), true);
		}
		reinterpret_cast<void(*)(int, int, int)>(cl_run_binding_addr)(0, binding, key);
	}

	void release_spectator_key(const int key)
	{
		if (spectator_binding[key]) run_binding(spectator_binding[key] + 1, key);
		spectator_binding[key] = 0;
	}

	// a spectator bind still held when the spectating ends (spawned), the player changes state or a menu opens is let go
	// (no shooting after the spawn)
	void update_spectator_keys()
	{
		for (auto key = 1; key < 64; ++key)
		{
			if (spectator_binding[key] && spectator_binding_of(key) != spectator_binding[key]) release_spectator_key(key);
		}
	}

	// the up goes out as the key the down went as, whatever is open by then (a menu the down opened, e.g. Game
	// Summary, gets no stray Escape up)
	void key_event(const int key, const bool down)
	{
		if (key <= 0 || key >= 64) return;
		pad.keys_down[key] = down;

		const auto sent = key_sent[key];
		if (!down)
		{
			key_sent[key] = 0;
			if (sent) queue_key(sent, false);
			release_spectator_key(key);
			return;
		}

		// spectating: cross / circle / triangle run the console's spectator binds instead
		const auto spectator = spectator_binding_of(key);
		if (spectator_binding[key] && spectator_binding[key] != spectator) release_spectator_key(key);
		if (spectator)
		{
			key_sent[key] = 0;
			if (sent) queue_key(sent, false);
			if (!spectator_binding[key]) run_binding(spectator, key);
			spectator_binding[key] = spectator;
			return;
		}

		// the console open takes the keys (CL_KeyEvent gives it everything): cross would be its Enter and run the line
		const auto target = (*reinterpret_cast<int*>(key_catchers) & 1) ? 0 : game_key(key);
		if (sent && sent != target) queue_key(sent, false); // a repeat that now goes elsewhere
		key_sent[key] = target;
		if (target) queue_key(target, true);
	}

	// menu scrolling: d-pad / left stick held repeats (first delay, then faster)
	void repeat_keys(const unsigned int now)
	{
		if (!ui_has_focus()) return;

		for (const auto key : {K_DPAD_UP, K_DPAD_DOWN, K_DPAD_LEFT, K_DPAD_RIGHT, K_APAD_UP, K_APAD_DOWN, K_APAD_LEFT, K_APAD_RIGHT})
		{
			if (!pad.keys_down[key] || now < pad.repeat_time[key]) continue;
			pad.repeat_time[key] = now + static_cast<unsigned int>(value(gpad_menu_scroll_delay_rest, 0.21f) * 1000.0f);
			key_event(key, true);
		}
	}

	void update_buttons()
	{
		static WORD last_buttons = 0;
		static bool last_triggers[2]{};
		static bool last_apad[4]{};
		const auto now = sys_milliseconds();
		const auto first_delay = static_cast<unsigned int>(value(gpad_menu_scroll_delay_first, 0.42f) * 1000.0f);
		update_spectator_keys();

		for (const auto& [bit, key] : xinput_buttons)
		{
			const auto down = (pad.buttons & bit) != 0;
			if (down != ((last_buttons & bit) != 0))
			{
				key_event(key, down);
				pad.repeat_time[key] = now + first_delay;
				used_last = true;
			}
		}
		last_buttons = pad.buttons;

		// analog buttons (PS3: L2 / R2) are down past gpad_button_deadzone
		const auto threshold = value(gpad_button_deadzone, 0.4f);
		for (auto t = 0; t < 2; ++t)
		{
			const auto down = pad.triggers[t] > threshold;
			if (down != last_triggers[t])
			{
				key_event(t ? K_BUTTON_RTRIG : K_BUTTON_LTRIG, down);
				used_last = true;
			}
			last_triggers[t] = down;
		}

		// the left stick pushed as a d-pad (menus; PS3 0x2ED5B4): down past gpad_stick_pressed + hysteresis, up again
		// below gpad_stick_pressed - hysteresis
		const auto pressed = value(gpad_stick_pressed, 0.4f);
		const auto hysteresis = value(gpad_stick_pressed_hysteresis, 0.1f);
		const float apad_values[4] = {pad.sticks[1], -pad.sticks[1], -pad.sticks[0], pad.sticks[0]};
		bool apad[4];
		for (auto i = 0; i < 4; ++i) apad[i] = apad_values[i] > (last_apad[i] ? pressed - hysteresis : pressed + hysteresis);
		const int apad_keys[4] = {K_APAD_UP, K_APAD_DOWN, K_APAD_LEFT, K_APAD_RIGHT};
		for (auto i = 0; i < 4; ++i)
		{
			if (apad[i] != last_apad[i])
			{
				key_event(apad_keys[i], apad[i]);
				pad.repeat_time[apad_keys[i]] = now + first_delay;
				used_last = true;
			}
			last_apad[i] = apad[i];
		}

		for (const auto s : pad.sticks)
		{
			if (std::fabs(s) > 0.3f) used_last = true;
		}

		repeat_keys(now);
	}

	// the PS3 layouts' commands the PC's bind table (0x8AA3B8, 5Bh commands) doesn't have: Key_Bind_f (0x48C320) drops
	// those binds, so Lefty / N0M4D kept the previous layout's buttons there (Flipped -> N0M4D: no aim button). The PC's
	// closest: fire / aim for the akimbo ones (the PC's aim fires the left gun with akimbo), melee + zoom and sprint +
	// hold breath for the sticks (melee and sprint on the PS3's stick, hold breath and zoom on the other one)
	const std::pair<const char*, const char*> pc_commands[] = {
		{"+attack_akimbo_accessible", "+attack"},
		{"+ads_akimbo_accessible", "+speed_throw"},
		{"+melee_breath", "+melee_zoom"},
		{"+sprint_zoom", "+breath_sprint"},
	};

	// whole commands only (not "+melee_breath" inside a longer name)
	std::string pc_layout(std::string text)
	{
		const auto word = [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
		for (const auto& [ps3, pc] : pc_commands)
		{
			const std::string from = ps3;
			for (auto pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos))
			{
				const auto end = pos + from.size();
				if ((pos > 0 && word(text[pos - 1])) || (end < text.size() && word(text[end])))
				{
					pos = end;
					continue;
				}
				text.replace(pos, from.size(), pc);
				pos += std::strlen(pc);
			}
		}
		return text;
	}

	// the profile's button / stick layout (set by the PS3 options) -> exec its config
	void apply_profile_layouts()
	{
		static std::string buttons, sticks;
		std::string b = reinterpret_cast<const char*>(profile_buttons_config);
		std::string s = reinterpret_cast<const char*>(profile_sticks_config);
		if (b.empty() || b.size() > 31) b = "buttons_default";
		if (s.empty() || s.size() > 31) s = "thumbstick_default";

		// the PC game's exec only reads files on disk: the config text goes to the console (with the PC's commands)
		const auto run = [](const std::string& name)
		{
			std::string text;
			if (utils::io::read_file(ps3style::folder() + "\\rawfiles\\" + name + ".cfg", &text)) cbuf(pc_layout(text) + "\n");
		};

		if (b != buttons)
		{
			buttons = b;
			run(b);
		}
		if (s != sticks)
		{
			sticks = s;
			cbuf("unbindallaxis\n");
			run(s);
		}
	}

	// while the controller is used the menus get no mouse events and the cursor is hidden and kept off screen every
	// frame (else the item under it takes the focus; a menu that opens parks it at 639,479 again, 0x58FA1C); a
	// deliberate mouse move gives it back
	void update_cursor()
	{
		static bool hidden = false;
		auto* cursor = reinterpret_cast<float*>(ui_context + 0x10);
		auto* visible = reinterpret_cast<int*>(ui_context + 0x1C);

		if (used_last)
		{
			cursor[0] = cursor[1] = -1000.0f;
			*visible = 0;
			hidden = true;
		}
		else if (hidden)
		{
			cursor[0] = 320.0f;
			cursor[1] = 240.0f;
			*visible = 1;
			hidden = false;
		}
	}

	// IN_MouseMove 0x5C90A0 -> CL_MouseEvent (0x48BB00, x, y, dx, dy in OS pixels; every frame): while the controller is
	// used only a deliberate move of a real mouse (16 px within 250 ms, not injected) gives the menus back to it, not a
	// nudge of the desk or a pad mapper's touchpad / gyro mouse. With raw input (m_rawinput) the game gets the mouse's
	// own movement since the last frame instead of the cursor's (which stops at the screen edge and has Windows'
	// pointer speed / acceleration in it); the cursor still places the menus' pointer.
	int __cdecl cl_mouse_event_stub(const int x, const int y, const int dx, const int dy)
	{
		auto raw_dx = 0, raw_dy = 0;
		const auto raw = input::take_raw_mouse(raw_dx, raw_dy, dx || dy);

		if (used_last)
		{
			static DWORD start = 0;
			static int moved = 0;
			if (!dx && !dy) return 0;

			const auto now = GetTickCount();
			if (now - start > 250)
			{
				start = now;
				moved = 0;
			}
			moved += std::abs(dx) + std::abs(dy);

			// raw input tells a real mouse by its device (injected moves have none); else the input filter does, and
			// without it (no pad found yet) nothing tells injected moves apart; with mapped input kept
			// (gpad_ignoreMappedInput 0: streaming clients, a mapper's mouse) an injected mouse counts as well
			const auto keep_injected = gpad_ignore_mapped_input && !gpad_ignore_mapped_input->current.enabled;
			const auto real = keep_injected || (raw ? now - input::raw_real_move_time() <= 250
			                                        : !mouse_filter_on || now - real_mouse_move.load() <= 250);
			if (moved < 16 || !real) return 0;

			if (debug_keys)
			{
				utils::io::write_file(ps3style::folder() + "\\ps3style.log",
				                      utils::string::va("mouse takes over: dx %d dy %d (%d px in %u ms), last real move %u ms ago, last move injected %d, filter %d\n",
				                                        dx, dy, moved, now - start, now - real_mouse_move.load(), mouse_move_injected.load(),
				                                        mouse_filter_on.load()), true);
			}
			moved = 0;
			used_last = false;
		}
		return reinterpret_cast<int(*)(int, int, int, int)>(0x48BB00)(x, y, raw ? raw_dx : dx, raw ? raw_dy : dy);
	}

	// a menu that opens without a focused item gets its first one (the PS3 menus' "focusfirst" doesn't set the
	// focus on the PC), as a down + up
	void update_focus()
	{
		static menuDef_t* last_top = nullptr;
		auto* menu = ui_has_focus() ? top_menu() : nullptr;
		if (menu == last_top) return;
		last_top = menu;
		if (!menu || !used_last) return;

		for (auto i = 0; i < menu->itemCount; ++i)
		{
			if (menu->items[i] && (menu->items[i]->window.dynamicFlags[0] & 0x2)) return;
		}

		queue_key(K_DOWNARROW, true);
		queue_key(K_DOWNARROW, false);
		queue_key(K_UPARROW, true);
		queue_key(K_UPARROW, false);
	}

	// ---- rumble ----
	// The PC game has no rumble code at all. The PS3 rumbles (ps3style\rumble: <name> = "RUMBLE\lowRumbleFile\x\
	// highRumbleFile\y\duration\0.5\...", <graph>.rmb = "RUMBLEGRAPHFILE" + point count + "t value" lines) are
	// played here: the weapon's fire rumble (WeaponDef +6BC) when the clip ammo of the local player drops.
	struct rumble_graph
	{
		std::vector<std::pair<float, float>> points;

		float at(const float t) const
		{
			if (points.empty()) return 0.0f;
			if (t <= points.front().first) return points.front().second;
			for (size_t i = 1; i < points.size(); ++i)
			{
				if (t <= points[i].first)
				{
					const auto& [t0, v0] = points[i - 1];
					const auto& [t1, v1] = points[i];
					return v0 + (v1 - v0) * (t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f);
				}
			}
			return points.back().second;
		}
	};

	struct rumble_info
	{
		float duration = 0.0f;
		bool loop = false;
		rumble_graph low, high;
	};

	struct active_rumble
	{
		const rumble_info* info;
		unsigned int start;
	};

	std::unordered_map<std::string, std::unique_ptr<rumble_info>> rumbles;
	std::vector<active_rumble> playing;
	game::native::dvar_t* gpad_rumble = nullptr;

	rumble_graph load_graph(const std::string& name)
	{
		rumble_graph graph;
		std::string text;
		if (!utils::io::read_file(ps3style::folder() + "\\rumble\\" + name, &text)) return graph;

		std::istringstream lines(text);
		std::string line;
		std::getline(lines, line); // RUMBLEGRAPHFILE
		auto count = 0;
		while (std::getline(lines, line) && !count)
		{
			count = std::atoi(line.data());
		}
		while (count-- > 0 && std::getline(lines, line))
		{
			float t = 0.0f, v = 0.0f;
			if (sscanf_s(line.data(), "%f %f", &t, &v) == 2) graph.points.emplace_back(t, v);
		}
		return graph;
	}

	const rumble_info* get_rumble(const std::string& name)
	{
		const auto itr = rumbles.find(name);
		if (itr != rumbles.end()) return itr->second.get();

		std::unique_ptr<rumble_info> info;
		std::string text;
		if (utils::io::read_file(ps3style::folder() + "\\rumble\\" + name, &text))
		{
			info = std::make_unique<rumble_info>();
			std::vector<std::string> parts;
			std::istringstream fields(text);
			std::string part;
			while (std::getline(fields, part, '\\')) parts.push_back(part);
			for (size_t i = 1; i + 1 < parts.size(); i += 2)
			{
				const auto& key = parts[i];
				const auto& val = parts[i + 1];
				if (key == "duration") info->duration = static_cast<float>(std::atof(val.data()));
				else if (key == "loop") info->loop = std::atoi(val.data()) != 0;
				else if (key == "lowRumbleFile") info->low = load_graph(val);
				else if (key == "highRumbleFile") info->high = load_graph(val);
			}
			if (info->duration <= 0.0f) info->duration = 0.5f;
		}

		auto* result = info.get();
		rumbles[name] = std::move(info);
		return result;
	}

	void play_rumble(const std::string& name)
	{
		if (const auto* info = get_rumble(name)) playing.push_back({info, sys_milliseconds()});
	}

	void update_rumble()
	{
		static bool was_on = false;
		const auto enabled = pad.connected && pad.port >= 0 && xinput_set_state && (!gpad_rumble || gpad_rumble->current.enabled);
		const auto now = sys_milliseconds();

		auto low = 0.0f, high = 0.0f;
		for (auto itr = playing.begin(); itr != playing.end();)
		{
			const auto t = (now - itr->start) / 1000.0f / itr->info->duration;
			if (t >= 1.0f && !itr->info->loop)
			{
				itr = playing.erase(itr);
				continue;
			}
			const auto f = itr->info->loop ? std::fmod(t, 1.0f) : t;
			low = std::max(low, itr->info->low.at(f));
			high = std::max(high, itr->info->high.at(f));
			++itr;
		}

		if (!enabled)
		{
			playing.clear();
			if (was_on && xinput_set_state && pad.port >= 0)
			{
				XINPUT_VIBRATION off{};
				xinput_set_state(pad.port, &off);
			}
			was_on = false;
			return;
		}

		if (!was_on && low <= 0.0f && high <= 0.0f) return;
		XINPUT_VIBRATION v{};
		v.wLeftMotorSpeed = static_cast<WORD>(std::clamp(low, 0.0f, 1.0f) * 65535.0f);
		v.wRightMotorSpeed = static_cast<WORD>(std::clamp(high, 0.0f, 1.0f) * 65535.0f);
		xinput_set_state(pad.port, &v);
		was_on = low > 0.0f || high > 0.0f;
	}

	// the local player's clip ammo (cg.predictedPlayerState 0x900FA0 + 0x40C ammoInClip: 12 byte entries {Weapon,
	// ammo[2]}): a drop = a shot of that weapon -> its fire rumble (WeaponDef +6BC fireRumble)
	std::string last_fire_rumble; // for gpad_aimdebug
	void update_fire_rumble()
	{
		constexpr DWORD ammo_in_clip = 0x9013AC;
		constexpr int entries = 15; // playerState_s.weapCommon.ammoInClip[15]
		static unsigned int last_weapon[entries]{};
		static int last_ammo[entries]{};
		static bool valid = false;

		if (ui_has_focus() || !pad.connected)
		{
			valid = false;
			return;
		}

		for (auto i = 0; i < entries; ++i)
		{
			const auto* e = reinterpret_cast<const int*>(ammo_in_clip + 12 * i);
			const auto weapon = static_cast<unsigned int>(e[0]);
			const auto ammo = e[1] + e[2];
			if (valid && weapon && weapon == last_weapon[i] && ammo < last_ammo[i] && last_ammo[i] - ammo <= 8)
			{
				const auto index = weapon & 0xFF;
				const auto* def = *reinterpret_cast<const char* const*>(bg_weapon_defs + 4 * index);
				const auto* name = def && !IsBadReadPtr(def + 0x6BC, 4) ? *reinterpret_cast<const char* const*>(def + 0x6BC) : nullptr;
				const auto* rumble = name && *name && !IsBadReadPtr(name, 1) ? name : "defaultweapon_fire";
				play_rumble(rumble);
				last_fire_rumble = utils::string::va("%X %s", weapon, rumble);
			}
			last_weapon[i] = weapon;
			last_ammo[i] = ammo;
		}
		valid = true;
	}

	// gpad_sim: a pretend controller for testing (sticks held for a while)
	unsigned int sim_until = 0;
	float sim_sticks[4]{};

	// gpad_sim tap: the up goes two frames after the down (these callbacks run before Com_Frame, the command inside
	// it), so the game handles the down in a frame of its own, as a real press
	int sim_tap_key = 0;
	int sim_tap_frames = 0;

	void frame()
	{
		if (sim_tap_key && --sim_tap_frames <= 0)
		{
			key_event(sim_tap_key, false);
			sim_tap_key = 0;
		}

		const auto enabled = !gpad_enabled || gpad_enabled->current.enabled;
		update_mouse_filter(enabled);
		if (!enabled) return;

		// XInput doesn't reset Windows' idle timers: playing with the pad only, the display would turn off
		if (static_cast<int>(pad_active_until.load() - GetTickCount()) > 0) input::keep_display_awake();

		apply_profile_layouts();
		update_cursor();
		update_focus();

		if (sim_until)
		{
			if (sys_milliseconds() < sim_until)
			{
				pad.connected = true;
				pad_active_until = GetTickCount() + 2000;
				start_input_filter();
				std::memcpy(pad.sticks, sim_sticks, sizeof(sim_sticks));
				update_buttons();
				return;
			}
			sim_until = 0;
			std::memset(pad.sticks, 0, sizeof(pad.sticks));
			update_buttons();
			pad.connected = false;
		}

		const auto was_connected = pad.connected;
		if (!poll())
		{
			if (was_connected)
			{
				// release everything that was down
				for (auto key = 1; key < 64; ++key)
				{
					if (pad.keys_down[key]) key_event(key, false);
				}
				update_rumble();
				pad = {};
				used_last = false; // the mouse / keyboard again (cursor back, mapped input no longer dropped)
			}
			return;
		}

		update_buttons();
		update_fire_rumble();
		update_rumble();
	}

	// ---- movement / aiming (CL_MouseMove) ----
	constexpr DWORD sv_running_dvar = 0x1CEF588;
	constexpr DWORD cl_max_pitch_speed = 0x1065BE4; // turn caps (deg/s, 0 = none; CL_MouseMove 0x489DBC / 0x489D0D)
	constexpr DWORD cl_max_yaw_speed = 0x1065BE8;
	constexpr DWORD cls_frametime = 0xBA4EE0; // ms (AimInput deltaTimeScaled = this * 0.001, CL_MouseMove 0x489E98)

	char clamp_char(const int v)
	{
		return static_cast<char>(std::clamp(v, -128, 127));
	}

	// the network carries the forward / right moves as -127 / 0 / 127 only: MSG_WriteDeltaUsercmdKey sends a 4 bit mask
	// of them (> 10, < -10; 0x5598CB) and MSG_ReadDeltaUsercmdKey 0x559E10 makes full moves of it again (0x559F6D)
	int move_mask(const char forward, const char right)
	{
		return (forward > 10 ? 1 : forward < -10 ? 2 : 0) | (right > 10 ? 4 : right < -10 ? 8 : 0);
	}

	char net_move(const char move)
	{
		return static_cast<char>(move > 10 ? 127 : move < -10 ? -127 : 0);
	}

	// the stick's moves reach the server as they are: we host (the server takes them back from our cmds,
	// restore_local_moves) and gpad_analogMove is on. Else the client moves as the server will (net_move): a prediction
	// from the analog move misses, and the view snaps back several times a second
	bool analog_moves()
	{
		const auto* sv_running = *reinterpret_cast<game::native::dvar_t**>(sv_running_dvar);
		return sv_running && sv_running->current.enabled && (!gpad_analog_move || gpad_analog_move->current.enabled);
	}

	float axis_value(const virtual_axis axis)
	{
		auto result = 0.0f;
		for (auto p = 0; p < A_COUNT; ++p)
		{
			if (axis_bindings[p].axis != axis) continue;

			auto v = p < 4 ? pad.sticks[p] : pad.triggers[p - 4];
			if (axis_bindings[p].mapping == MAP_SQUARED)
			{
				// PS3 CL_GamepadAxisValue 0xCCA90: scaled by the length of the whole stick (the other axis of it from
				// 0x7282E8 = {1, 0, 3, 2, -1, -1}), not squared per axis, so the direction stays and diagonals are full
				// speed; triggers have no partner (v * |v|)
				const auto other = p < 4 ? pad.sticks[p ^ 1] : 0.0f;
				v *= std::sqrt(v * v + other * other);
			}
			result += v;
		}
		return std::clamp(result, -1.0f, 1.0f);
	}

	// ---- aiming as the PS3 (MW3 PS3 1.24 EBOOT: CL_GamepadMove 0xCFE80 -> AimAssist_UpdateGamePadInput 0x11E08 =
	// tweakables, ADS lerp, AimAssist_ApplyTurnRates 0x11070, auto melee, AimAssist_ApplyLockOn 0x11A68). The PC game
	// still tracks targets like the console (aim assist globals below: player state, screen targets, ADS lerp,
	// tweakables, auto melee - its mouse path runs those) but lost the turn rate / slowdown / lock-on code. The PS3
	// has their tunables as constants (no dvars), so they are constants here too.
	constexpr DWORD aa_glob = 0x8D5980; // AimAssistGlobals (one local client, 0xE60 bytes)
	constexpr DWORD aa_update_tweakables_addr = 0x40FF40; // eax = localClientNum
	constexpr DWORD aa_update_ads_lerp_addr = 0x410070; // edi = AimInput*

	enum aa_offset : DWORD
	{
		AA_PS_VELOCITY = 0x0, // AimAssistPlayerState
		AA_PS_EFLAGS = 0xC,
		AA_PS_LINK_FLAGS = 0x10,
		AA_PS_WEAP_FLAGS = 0x18,
		AA_PS_WEAPON_STATE = 0x1C,
		AA_PS_WEAPON = 0x24, // Weapon (index in the low byte, attachments above)
		AA_PS_HAS_AMMO = 0x28,
		AA_INITIALIZED = 0xB0,
		AA_SLOWDOWN_REGION = 0xB8, // tweakables: width, height (clip space)
		AA_LOCKON_REGION = 0xC8,
		AA_VIEW_AXIS = 0xF4, // [3][3]
		AA_FOV_TURN_RATE_SCALE = 0x118,
		AA_ADS_LERP = 0x120,
		AA_PITCH_DELTA = 0x124,
		AA_YAW_DELTA = 0x128,
		AA_SCREEN_TARGETS = 0x134, // AimScreenTarget[64]
		AA_SCREEN_TARGET_COUNT = 0xE34,
		AA_AUTO_MELEE_STATE = 0xE38,
		AA_LOCKON_TARGET_ENT = 0xE50,
	};

	template <typename T>
	T& aa(const DWORD offset)
	{
		return *reinterpret_cast<T*>(aa_glob + offset);
	}

	struct aim_input
	{
		float delta_time;
		float delta_time_scaled;
		float pitch;
		float pitch_axis;
		float pitch_max;
		float yaw;
		float yaw_axis;
		float yaw_max;
		float forward_axis;
		float right_axis;
		int buttons;
		int local_client_num;
	};

	struct aim_screen_target
	{
		int ent;
		float clip_mins[2];
		float clip_maxs[2];
		float aim_pos[3];
		float velocity[3];
		float dist_sqr;
		float crosshair_dist_sqr;
	};

	static_assert(sizeof(aim_input) == 0x30 && sizeof(aim_screen_target) == 0x34);

	// PS3 constants (AimAssist_ApplyTurnRates / AimAssist_ApplyLockOn)
	constexpr float aim_turnrate_pitch = 90.0f, aim_turnrate_pitch_ads = 55.0f;
	constexpr float aim_turnrate_yaw = 260.0f, aim_turnrate_yaw_ads = 90.0f;
	constexpr float aim_accel_turnrate_lerp = 1200.0f;
	constexpr float aim_slowdown_scale = 0.4f, aim_slowdown_scale_ads = 0.5f; // pitch and yaw alike
	constexpr float aim_lockon_strength = 0.6f; // pitch and yaw alike
	constexpr float aim_lockon_deflection = 0.05f;
	constexpr int aim_target_invalid = 0x7FF;

	// the stick response: rawfile aim_assist/view_input_3.graph of the PS3 (the graph it uses; scale 1)
	constexpr float view_input_graph[][2] = {
		{0.0000f, 0.0000f}, {0.1242f, 0.0423f}, {0.2238f, 0.0885f}, {0.3210f, 0.1553f},
		{0.4118f, 0.2419f}, {0.5116f, 0.3508f}, {1.0000f, 1.0000f},
	};

	float graph_value(const float x)
	{
		for (size_t i = 1; i < std::size(view_input_graph); ++i)
		{
			if (x <= view_input_graph[i][0])
			{
				const auto* a = view_input_graph[i - 1];
				const auto* b = view_input_graph[i];
				return a[1] + (x - a[0]) / (b[0] - a[0]) * (b[1] - a[1]);
			}
		}
		return -1.0f;
	}

	float lerp(const float a, const float b, const float t)
	{
		return a + (b - a) * t;
	}

	// moves toward the target turn rate at most `rate` per second (the engine's LinearTrack)
	float linear_track(const float target, const float current, const float rate, const float dt)
	{
		const auto step = rate * dt;
		if (target > current) return std::min(target, current + step);
		return std::max(target, current - step);
	}

	__declspec(naked) void aa_update_tweakables(int /*local_client_num*/)
	{
		__asm
		{
			pushad
			mov eax, [esp + 24h]
			mov ecx, aa_update_tweakables_addr
			call ecx
			popad
			retn
		}
	}

	__declspec(naked) void aa_update_ads_lerp(const aim_input* /*input*/)
	{
		__asm
		{
			pushad
			mov edi, [esp + 24h]
			mov ecx, aa_update_ads_lerp_addr
			call ecx
			popad
			retn
		}
	}

	// cg: the local player is in a state without aiming (killcam / spectating; PC 0x6A39A0 = PS3 0x3CB0C0)
	bool aim_blocked()
	{
		return *reinterpret_cast<int*>(0xB7F988) == 2;
	}

	// bg_weaponDefs[weapon index] (WeaponDef*)
	const char* weapon_def(const unsigned int weapon)
	{
		const auto index = weapon & 0xFF;
		return index ? *reinterpret_cast<const char* const*>(bg_weapon_defs + 4 * index) : nullptr;
	}

	// BG_GetWeaponAimAssistRange (PS3 0x43E68): the weapon's hip / ADS aim assist range
	bool weapon_aim_assist_range(const unsigned int weapon, float& range, float& range_ads)
	{
		const auto* def = weapon_def(weapon);
		if (!def) return false;
		range = *reinterpret_cast<const float*>(def + 0x3D4);
		range_ads = *reinterpret_cast<const float*>(def + 0x3D8);
		return true;
	}

	bool bounds_intersect_center_box(const aim_screen_target& t, const float half_width, const float half_height)
	{
		return half_width >= t.clip_mins[0] && t.clip_maxs[0] >= -half_width && half_height >= t.clip_mins[1] && t.clip_maxs[1] >= -half_height;
	}

	// AimAssist_GetBestTarget (PS3 0x10F84): the first screen target in range whose box touches the center region
	const aim_screen_target* best_target(const float range, const float width, const float height)
	{
		const auto count = std::clamp(aa<int>(AA_SCREEN_TARGET_COUNT), 0, 64);
		const auto* targets = &aa<aim_screen_target>(AA_SCREEN_TARGETS);
		for (auto i = 0; i < count; ++i)
		{
			if (targets[i].dist_sqr <= range * range && bounds_intersect_center_box(targets[i], width, height)) return &targets[i];
		}
		return nullptr;
	}

	// throwing an offhand (grenade ...; PS3: weapon class 9, the throwing knife, keeps aim assist)
	bool using_offhand()
	{
		const auto weapon = aa<unsigned int>(AA_PS_WEAPON);
		if (!(aa<int>(AA_PS_WEAP_FLAGS) & 2) || !(weapon & 0xFF)) return false;
		const auto* def = weapon_def(weapon);
		return !def || *reinterpret_cast<const int*>(def + 0x30) != 9;
	}

	float assist_range()
	{
		float range = 0.0f, range_ads = 0.0f;
		if (!weapon_aim_assist_range(aa<unsigned int>(AA_PS_WEAPON), range, range_ads)) return 0.0f;
		const auto ads = aa<float>(AA_ADS_LERP);
		return range_ads * ads + range * (1.0f - ads);
	}

	// AimAssist_ApplyTurnRates (PS3 0x11070): stick curve, view axis scaling, slowdown over a target, turn rates
	void apply_turn_rates(const aim_input& input, float& out_pitch, float& out_yaw, const float sensitivity)
	{
		float pitch_axis = 0.0f, yaw_axis = 0.0f, pitch_scale = 1.0f, yaw_scale = 1.0f;
		if (aa<int>(AA_AUTO_MELEE_STATE) != 2)
		{
			// response curve on the stick deflection
			const auto deflection = std::clamp(std::sqrt(input.pitch_axis * input.pitch_axis + input.yaw_axis * input.yaw_axis), 0.0f, 1.0f);
			const auto curve = graph_value(deflection);
			pitch_axis = input.pitch_axis * curve;
			yaw_axis = input.yaw_axis * curve;

			// the smaller axis fades as the other one dominates (straight turns stay straight)
			const auto abs_pitch = std::fabs(pitch_axis), abs_yaw = std::fabs(yaw_axis);
			if (abs_yaw - abs_pitch >= 0.0f) pitch_axis *= 1.0f - (abs_yaw - abs_pitch);
			else yaw_axis *= 1.0f - (abs_pitch - abs_yaw);

			// slowdown: over a target in range the turn rates drop (0.4 hip .. 0.5 ADS)
			auto active = !aim_blocked() && !(aa<int>(AA_PS_LINK_FLAGS) & 4);
			const auto state = aa<int>(AA_PS_WEAPON_STATE);
			if (state == 0x1A || state == 0x1B) active = false; // stunned
			if (!(aa<unsigned int>(AA_PS_WEAPON) & 0xFF)) active = false;
			if (active) active = (aa<int>(AA_PS_EFLAGS) & 0x203800) != 0 || aa<bool>(AA_PS_HAS_AMMO); // turrets have no ammo count

			if (active)
			{
				const auto* region = &aa<float>(AA_SLOWDOWN_REGION);
				const auto ads = aa<float>(AA_ADS_LERP);
				const auto scale = best_target(assist_range(), region[0], region[1]) ? ads * 0.1f + aim_slowdown_scale : 1.0f;
				static_assert(aim_slowdown_scale_ads - aim_slowdown_scale > 0.09f && aim_slowdown_scale_ads - aim_slowdown_scale < 0.11f);
				yaw_scale = scale;
				pitch_scale = using_offhand() ? 1.0f : scale;
			}
		}

		const auto ads = aa<float>(AA_ADS_LERP);
		const auto fov_scale = aa<float>(AA_FOV_TURN_RATE_SCALE);
		auto pitch_rate = pitch_scale * fov_scale * sensitivity * lerp(aim_turnrate_pitch, aim_turnrate_pitch_ads, ads);
		auto yaw_rate = yaw_scale * fov_scale * sensitivity * lerp(aim_turnrate_yaw, aim_turnrate_yaw_ads, ads);
		if (input.pitch_max > 0.0f && pitch_rate > input.pitch_max) pitch_rate = input.pitch_max;
		if (input.yaw_max > 0.0f && yaw_rate > input.yaw_max) yaw_rate = input.yaw_max;

		const auto pitch_sign = pitch_axis >= 0.0f ? 1.0f : -1.0f;
		const auto yaw_sign = yaw_axis >= 0.0f ? 1.0f : -1.0f;
		const auto accel = sensitivity * aim_accel_turnrate_lerp;

		// accelerates up to the new turn rate, slows down at once
		auto& pitch_delta = aa<float>(AA_PITCH_DELTA);
		auto& yaw_delta = aa<float>(AA_YAW_DELTA);
		const auto target_pitch = pitch_rate * std::fabs(pitch_axis);
		const auto target_yaw = yaw_rate * std::fabs(yaw_axis);
		pitch_delta = pitch_delta >= target_pitch ? target_pitch : linear_track(target_pitch, pitch_delta, accel, input.delta_time);
		yaw_delta = yaw_delta >= target_yaw ? target_yaw : linear_track(target_yaw, yaw_delta, accel, input.delta_time);

		out_pitch += pitch_delta * input.delta_time * pitch_sign;
		out_yaw += yaw_delta * input.delta_time * yaw_sign;
	}

	// the lock-on target's velocity, held and smoothed. The PC's AimTarget 0x411860 takes it from the snap / nextSnap
	// positions and sets it to 0 (0x4119B7) whenever both have the same serverTime, and bots' positions come in bursts:
	// used as it is, the lock-on turn rate flips between 0 and the real one from frame to frame and the view shakes
	// while the sticks move. A zero within 250 ms of a real value is "no new snapshot" (the held value stays); the
	// rest is smoothed over 100 ms. A new target (or one locked again later) starts from its own value.
	const float* lock_on_velocity(const aim_screen_target& target, const float delta_time)
	{
		static int ent = aim_target_invalid;
		static float held[3]{};
		static unsigned int last_real = 0; // last non-zero sample
		static unsigned int last_used = 0;

		const auto now = sys_milliseconds();
		const auto* v = target.velocity;
		const auto zero = v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f;

		if (target.ent != ent || now - last_used > 250)
		{
			ent = target.ent;
			std::memcpy(held, v, sizeof(held));
			last_real = zero ? 0 : now;
		}
		else if (!zero || !last_real || now - last_real >= 250)
		{
			const auto t = std::min(1.0f, delta_time / 0.1f);
			for (auto i = 0; i < 3; ++i) held[i] += (v[i] - held[i]) * t;
			if (!zero) last_real = now;
		}

		last_used = now;
		return held;
	}

	// AimAssist_ApplyLockOn (PS3 0x11A68): while the sticks move, the view follows a target's sideways / vertical
	// motion relative to the player
	void apply_lock_on(const aim_input& input, float& out_pitch, float& out_yaw)
	{
		auto& lock_on_ent = aa<int>(AA_LOCKON_TARGET_ENT);
		const auto prev_ent = lock_on_ent;
		lock_on_ent = aim_target_invalid;

		if (aim_blocked()) return;
		if (using_offhand()) return;
		if (aa<int>(AA_PS_LINK_FLAGS) & 4) return;
		if (aa<int>(AA_AUTO_MELEE_STATE) == 2) return;
		if (std::fabs(input.pitch_axis) < aim_lockon_deflection && std::fabs(input.yaw_axis) < aim_lockon_deflection &&
		    std::fabs(input.right_axis) < aim_lockon_deflection) return;
		if (!(aa<unsigned int>(AA_PS_WEAPON) & 0xFF)) return;

		const auto range = assist_range();
		const auto* region = &aa<float>(AA_LOCKON_REGION);
		const aim_screen_target* target = nullptr;

		// the previous target stays while it is still in range and in the region
		if (prev_ent != aim_target_invalid)
		{
			const auto count = std::clamp(aa<int>(AA_SCREEN_TARGET_COUNT), 0, 64);
			const auto* targets = &aa<aim_screen_target>(AA_SCREEN_TARGETS);
			for (auto i = 0; i < count; ++i)
			{
				if (targets[i].ent != prev_ent) continue;
				if (targets[i].dist_sqr < range * range && bounds_intersect_center_box(targets[i], region[0], region[1])) target = &targets[i];
				break;
			}
		}
		if (!target) target = best_target(range, region[0], region[1]);
		if (!target || target->dist_sqr <= 0.0f) return;

		lock_on_ent = target->ent;
		const auto arc_length = std::sqrt(target->dist_sqr) * 3.14159265f;
		const auto* axis = &aa<float>(AA_VIEW_AXIS); // forward, left, up
		const auto* velocity = &aa<float>(AA_PS_VELOCITY);
		const auto* target_velocity = lock_on_velocity(*target, input.delta_time);
		const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

		const auto yaw_rate = (dot(target_velocity, axis + 3) - dot(velocity, axis + 3)) / arc_length * 180.0f * aim_lockon_strength;
		const auto pitch_rate = (dot(target_velocity, axis + 6) - dot(velocity, axis + 6)) / arc_length * 180.0f * aim_lockon_strength;
		out_yaw += yaw_rate * input.delta_time_scaled; // the scaled frame time (PS3 0x11DA4)
		out_pitch -= pitch_rate * input.delta_time_scaled;
	}

	bool assist_debug = false; // gpad_aimdebug 1: aim assist state -> ps3style.log once a second
	bool assist_fake_target = false; // gpad_aimdebug 2: + a pretend target

	void __cdecl gamepad_move(int* cmd_raw, const float delta_time)
	{
		if (!pad.connected || (gpad_enabled && !gpad_enabled->current.enabled) || ui_has_focus()) return;

		// as the game's own CL_MouseMove: nothing without a frame time, no view change in some states
		if (!*reinterpret_cast<int*>(0x106255C)) return;
		if ((*reinterpret_cast<int*>(0x10627A4) & 0x800) && !aim_blocked()) return;

		auto* cmd = reinterpret_cast<char*>(cmd_raw);

		// movement (PS3 CL_GamepadMove): forward / side, diagonals scaled up so the square corner is full speed
		const auto forward = axis_value(VA_FORWARD);
		const auto side = axis_value(VA_SIDE);
		auto scale = 127.0f;
		if (std::fabs(side) > 0.0f || std::fabs(forward) > 0.0f)
		{
			const auto ratio = std::fabs(side) <= std::fabs(forward) ? side / forward : forward / side;
			scale *= std::sqrt(ratio * ratio + 1.0f);
		}
		cmd[0x1C] = clamp_char(cmd[0x1C] + static_cast<int>(forward * scale));
		cmd[0x1D] = clamp_char(cmd[0x1D] + static_cast<int>(side * scale));
		if (!analog_moves()) // the moves as the server will get them, so the prediction matches
		{
			cmd[0x1C] = net_move(cmd[0x1C]);
			cmd[0x1D] = net_move(cmd[0x1D]);
		}

		auto sensitivity = *reinterpret_cast<float*>(profile_view_sensitivity);
		if (!(sensitivity > 0.05f && sensitivity < 20.0f)) sensitivity = 1.0f;

		// the PS3's AimInput: pitch axis negative = stick up (unless inverted), yaw axis negative = stick right; the turn
		// caps (PS3 0xD01C8 / 0xD01DC) as the mouse gets them
		aim_input input{};
		input.delta_time = std::clamp(delta_time, 0.0f, 0.1f);
		input.delta_time_scaled = std::clamp(static_cast<float>(*reinterpret_cast<int*>(cls_frametime)) * 0.001f, 0.0f, 0.1f);
		input.pitch = *reinterpret_cast<float*>(view_pitch);
		input.pitch_max = *reinterpret_cast<float*>(cl_max_pitch_speed);
		input.yaw = *reinterpret_cast<float*>(view_yaw);
		input.yaw_max = *reinterpret_cast<float*>(cl_max_yaw_speed);
		input.pitch_axis = axis_value(VA_PITCH) * (*reinterpret_cast<unsigned char*>(profile_invert_pitch) ? 1.0f : -1.0f);
		input.yaw_axis = -axis_value(VA_YAW);
		input.forward_axis = forward;
		input.right_axis = side;
		input.buttons = cmd_raw[1];

		auto out_pitch = input.pitch, out_yaw = input.yaw;
		if (aa<bool>(AA_INITIALIZED))
		{
			// gpad_aimdebug 2: a pretend target in the crosshair, 500 units away, moving left at 100 units/s (testing)
			if (assist_fake_target)
			{
				auto& target = aa<aim_screen_target>(AA_SCREEN_TARGETS);
				target = {};
				target.ent = 5;
				target.clip_mins[0] = target.clip_mins[1] = -0.02f;
				target.clip_maxs[0] = target.clip_maxs[1] = 0.02f;
				const auto* axis = &aa<float>(AA_VIEW_AXIS);
				for (auto i = 0; i < 3; ++i) target.velocity[i] = axis[3 + i] * 100.0f;
				target.dist_sqr = 500.0f * 500.0f;
				aa<int>(AA_SCREEN_TARGET_COUNT) = 1;
			}

			aa_update_tweakables(0);
			aa_update_ads_lerp(&input);
			apply_turn_rates(input, out_pitch, out_yaw, sensitivity);
			// (auto melee: the game's mouse path right after this runs it)
			apply_lock_on(input, out_pitch, out_yaw);

			if (assist_debug)
			{
				static unsigned int next = 0, calls = 0;
				static float time_sum = 0.0f;
				++calls;
				time_sum += input.delta_time;
				if (sys_milliseconds() >= next)
				{
					next = sys_milliseconds() + 1000;
					float range = 0.0f, range_ads = 0.0f;
					weapon_aim_assist_range(aa<unsigned int>(AA_PS_WEAPON), range, range_ads);
					const auto* region = &aa<float>(AA_SLOWDOWN_REGION);
					utils::io::write_file(ps3style::folder() + "\\ps3style.log",
					                      utils::string::va("aim: weapon %X def %p range %.0f/%.0f ads %.2f targets %d lockon %d region %.3f %.3f fovscale %.2f "
					                                        "ammo %d state %d calls %u time %.3f last fire rumble %s\n", aa<unsigned int>(AA_PS_WEAPON),
					                                        weapon_def(aa<unsigned int>(AA_PS_WEAPON)), range, range_ads,
					                                        aa<float>(AA_ADS_LERP), aa<int>(AA_SCREEN_TARGET_COUNT), aa<int>(AA_LOCKON_TARGET_ENT),
					                                        region[0], region[1], aa<float>(AA_FOV_TURN_RATE_SCALE), aa<bool>(AA_PS_HAS_AMMO),
					                                        aa<int>(AA_PS_WEAPON_STATE), calls, time_sum, last_fire_rumble.data()), true);
					calls = 0;
					time_sum = 0.0f;
				}
			}
		}

		*reinterpret_cast<float*>(view_pitch) = out_pitch;
		*reinterpret_cast<float*>(view_yaw) = out_yaw;
	}

	// ---- location selection and remote control (CL_CreateCmd skips CL_MouseMove for both) ----
	constexpr DWORD location_select_call = 0x48A833; // CL_CreateCmd -> CL_HandleLocationSelectionInput 0x48A1E0 (eax, esi = cmd)
	constexpr DWORD location_select_addr = 0x48A1E0;
	constexpr DWORD remote_control_call = 0x48A80A; // CL_CreateCmd -> CL_RemoteControlMove 0x48A5D0 (eax, ebx = cmd)
	constexpr DWORD remote_control_addr = 0x48A5D0;
	constexpr DWORD location_selection_info = 0x9011A8; // cg.predictedPlayerState.locationSelectionInfo (bit 7: direction too)
	constexpr DWORD selected_location = 0x9781BC; // x, y (0..1 of the map), direction (yaw), the direction point x, y
	constexpr DWORD map_cursor_speed_dvar = 0x8FC938; // cg_mapLocationSelectionCursorSpeed
	constexpr DWORD cg_frametime = 0x96C188; // ms
	constexpr DWORD compass_map_world_size = 0x97814C; // float[2]
	constexpr DWORD dir_sel_convergence_dvar = 0xB3C2A8; // cl_dirSelConvergenceTime (default 0.1)

	// the airstrike map: the move stick moves the cursor (map widths per second, as the mouse code scales it), the look
	// stick points the direction (PS3: "Set location with L3", "Set direction with R3"); the game clamps both after this
	void __cdecl gamepad_location_select()
	{
		if (!pad.connected || (gpad_enabled && !gpad_enabled->current.enabled)) return;
		const auto info = *reinterpret_cast<int*>(location_selection_info);
		if (!info) return;

		auto* location = reinterpret_cast<float*>(selected_location);
		const auto speed = value(*reinterpret_cast<game::native::dvar_t**>(map_cursor_speed_dvar), 0.6f);
		const auto frame_time = static_cast<float>(*reinterpret_cast<int*>(cg_frametime)) * 0.001f;
		const auto* map_size = reinterpret_cast<const float*>(compass_map_world_size);
		const auto aspect = map_size[1] > 0.0f ? map_size[0] / map_size[1] : 1.0f;

		auto right = axis_value(VA_SIDE), up = axis_value(VA_FORWARD);
		const auto length = std::sqrt(right * right + up * up);
		if (length > 1.0f)
		{
			right /= length;
			up /= length;
		}
		location[0] += right * speed * frame_time;
		location[1] -= up * aspect * speed * frame_time;

		if ((info >> 7) & 1)
		{
			// the mouse code: yaw of (-dy, -dx) on the map = atan2(-right, up) for a stick. As the PS3: a stick vector of
			// length <= 0.2 is ignored, then the arrow turns frametime / cl_dirSelConvergenceTime of the way toward it
			const auto dir_right = axis_value(VA_YAW), dir_up = axis_value(VA_PITCH);
			if (std::sqrt(dir_right * dir_right + dir_up * dir_up) > 0.2f)
			{
				const auto target = std::atan2(-dir_right, dir_up) * 180.0f / 3.14159265f;
				const auto convergence = value(*reinterpret_cast<game::native::dvar_t**>(dir_sel_convergence_dvar), 0.1f);
				const auto rate = std::min(1.0f, frame_time / std::max(0.01f, convergence));
				auto delta = (target - location[2]) / 360.0f;
				delta = (delta - std::floor(delta + 0.5f)) * 360.0f; // AngleDelta
				auto angle = location[2] + rate * delta;
				angle -= 360.0f * std::floor(angle / 360.0f); // AngleNormalize360
				location[2] = angle;
			}
		}
	}

	__declspec(naked) void cl_location_select_stub()
	{
		__asm
		{
			pushad
			call gamepad_location_select
			popad
			push location_select_addr
			retn
		}
	}

	// remote controlled killstreaks: the command's remote rates (-1..1 as -127..127 at +0x26 pitch, +0x27 yaw, +0x28
	// forward, +0x29 right), on top of what the mouse / keys gave
	void __cdecl gamepad_remote_move(char* cmd)
	{
		if (!pad.connected || (gpad_enabled && !gpad_enabled->current.enabled) || ui_has_focus()) return;
		const auto invert = *reinterpret_cast<unsigned char*>(profile_invert_pitch) != 0;
		const auto add = [cmd](const int offset, const float rate)
		{
			cmd[offset] = clamp_char(cmd[offset] + static_cast<int>(std::clamp(rate, -1.0f, 1.0f) * 127.0f));
		};
		add(0x26, axis_value(VA_PITCH) * (invert ? 1.0f : -1.0f));
		add(0x27, -axis_value(VA_YAW));
		add(0x28, axis_value(VA_FORWARD));
		add(0x29, axis_value(VA_SIDE));
	}

	__declspec(naked) void cl_remote_control_move_stub()
	{
		__asm
		{
			mov ecx, remote_control_addr
			call ecx
			pushad
			push ebx
			call gamepad_remote_move
			add esp, 4
			popad
			retn
		}
	}

	__declspec(naked) void cl_mouse_move_stub()
	{
		__asm
		{
			pushad
			push dword ptr [esp + 24h] // CL_MouseMove's frame time (s)
			push edi
			call gamepad_move
			add esp, 8
			popad

			// the replaced instructions (the flags of the cmp are used right after)
			sub esp, 54h
			cmp dword ptr ds:[106255Ch], 0
			push 489C4Ah
			retn
		}
	}

	// ---- the host's own moves (SV_UserMove) ----
	// The consoles send the moves as they are; the PC's network makes -127 / 0 / 127 of them (move_mask), so a half
	// pushed stick walked at full speed on the server and slowly in the prediction. When we host, the server takes the
	// moves of our own cmds back from cl.cmds (the same process): only for the loopback client, only for the cmd with the
	// same serverTime, buttons, angles and weapons (all sent exactly), and only when the decoded moves are that cmd's
	// moves as the network carries them (the same mask). A cmd the client sent as "unchanged" was copied from the
	// previous one, already restored; the restored cmd is the next one's "from" (its mask is the same).
	constexpr DWORD sv_read_usercmd_call = 0x571DAE; // SV_UserMove -> MSG_ReadDeltaUsercmdKey (msg, key, from, to); ebx = client_t*
	constexpr DWORD msg_read_delta_usercmd_key_addr = 0x559E10;
	constexpr DWORD svs_clients = 0x4B5CF90; // client_t[], 0x78698 each
	constexpr DWORD client_size = 0x78698;
	constexpr int max_clients = 18;
	constexpr DWORD cl_remote_address_type = 0x28; // netchan.remoteAddress.type: 2 NA_LOOPBACK (0x571521), bots 0
	constexpr DWORD cl_cmds = 0x1065C90; // usercmd_s[128] (0x2C each), by cmd number & 0x7F
	constexpr DWORD cl_cmd_number = 0x1067290;

	unsigned int moves_restored = 0, moves_not_found = 0, moves_other_mask = 0; // gpad_status

	void __cdecl restore_local_moves(const DWORD client, char* to)
	{
		if (client < svs_clients || client >= svs_clients + max_clients * client_size || (client - svs_clients) % client_size) return;
		if (*reinterpret_cast<const int*>(client + cl_remote_address_type) != 2 || !analog_moves()) return;

		const auto number = *reinterpret_cast<const int*>(cl_cmd_number);
		for (auto n = number; n > number - 128; --n)
		{
			const auto* own = reinterpret_cast<const char*>(cl_cmds + 0x2C * (n & 0x7F));
			if (std::memcmp(own, to, 0x1C)) continue; // serverTime, buttons, angles, weapons

			if (move_mask(own[0x1C], own[0x1D]) != move_mask(to[0x1C], to[0x1D]))
			{
				++moves_other_mask;
				return;
			}
			to[0x1C] = own[0x1C];
			to[0x1D] = own[0x1D];
			++moves_restored;
			return;
		}
		++moves_not_found;
	}

	__declspec(naked) void sv_read_usercmd_stub()
	{
		__asm
		{
			// the call as it was: msg, key, from, to
			push dword ptr [esp + 10h]
			push dword ptr [esp + 10h]
			push dword ptr [esp + 10h]
			push dword ptr [esp + 10h]
			mov eax, msg_read_delta_usercmd_key_addr
			call eax
			add esp, 10h

			// its result (al) is tested right after
			push eax
			pushad
			push dword ptr [esp + 34h] // to
			push ebx // client_t*
			call restore_local_moves
			add esp, 8
			popad
			pop eax
			retn
		}
	}

	// ---- key names ----
	__declspec(naked) int key_string_to_keynum_original(const char* /*name*/)
	{
		__asm
		{
			push ebx
			mov ebx, [esp + 8]
			push 48BF95h
			retn
		}
	}

	__declspec(naked) const char* key_keynum_to_string_original(int /*key*/, int /*translate*/)
	{
		__asm
		{
			push esi
			mov esi, [esp + 8]
			push 48C085h
			retn
		}
	}

	// ---- the PC's key binding items without the pad's buttons ----
	// Key_GetKeysForCommand 0x48C220 (ecx = localClientNum, eax = command, ebx = int[2] out; returns how many) finds the
	// first two keys bound to a command. The key binding items (type 14: the MOUSE & KEYBOARD pages) show those
	// (Item_Bind_Paint 0x5A7FC0: its call of 0x5A3AC0 at 0x5A7FF7 -> 0x48C6D0 -> 0x48C620) and, binding a key, unbind both
	// first when there are two (Item_Bind_HandleKey 0x5A3EA4 -> 0x48C270, which calls it at 0x48C27D): with the pad's
	// buttons counted, "STAND/JUMP" showed cross and binding it a key unbound the pad's jump. The button layouts own the
	// pad's buttons, so for those two only keyboard / mouse keys count. Everything else (the key text of the PC look's
	// hints: "[{+usereload}]" 0x5A75A0, "&&1" 0x5A3BD0, both -> 0x5A3AC0) still gets every key, the pad's first.
	constexpr DWORD key_get_keys_for_command_addr = 0x48C220;
	constexpr DWORD key_get_keys_bind_handle_key = 0x48C282; // return address of the call in 0x48C270 (Item_Bind_HandleKey's)
	constexpr DWORD item_bind_paint_keys = 0x5A7FF1; // mov eax, [esi + 114h] (6 bytes), then call 0x5A3AC0
	constexpr DWORD item_bind_paint_keys_done = 0x5A7FFC; // lea ecx, [esp + 0Ch]; add esp, 0Ch (7 bytes)
	constexpr DWORD key_bindings = 0xB3C768; // playerKeys[localClientNum] (0xD2C each): keys[256] {binding, ...} 12 bytes

	bool bind_item_painting = false; // Item_Bind_Paint's key text is being made (main thread)

	int __cdecl keys_for_command(const int local_client, const char* command, int* found, const DWORD return_address)
	{
		static bool pad_key[256]{};
		static auto pad_keys_known = false;
		if (!pad_keys_known)
		{
			for (const auto& k : keys) pad_key[k.key] = true;
			pad_keys_known = true;
		}

		found[0] = found[1] = -1;
		const auto index = command ? binding_index(command) : 0;
		if (!index || local_client < 0 || local_client > 3) return 0;

		const auto keyboard_only = bind_item_painting || return_address == key_get_keys_bind_handle_key;
		auto count = 0;
		for (auto key = 0; key < 256 && count < 2; ++key)
		{
			if (keyboard_only && pad_key[key]) continue;
			if (*reinterpret_cast<const int*>(key_bindings + 0xD2C * local_client + 12 * key) == index) found[count++] = key;
		}
		return count;
	}

	__declspec(naked) void key_get_keys_for_command_stub()
	{
		__asm
		{
			push [esp]
			push ebx
			push eax
			push ecx
			call keys_for_command
			add esp, 10h
			retn
		}
	}

	__declspec(naked) void item_bind_paint_keys_stub()
	{
		__asm
		{
			mov bind_item_painting, 1
			mov eax, [esi + 114h]
			push 5A7FF7h
			retn
		}
	}

	__declspec(naked) void item_bind_paint_keys_done_stub()
	{
		__asm
		{
			mov bind_item_painting, 0
			lea ecx, [esp + 0Ch]
			add esp, 0Ch
			push 5A8003h
			retn
		}
	}

	int __cdecl key_string_to_keynum_stub(const char* name)
	{
		const auto key = gamepad::key_number(name);
		return key >= 0 ? key : key_string_to_keynum_original(name);
	}

	const char* __cdecl key_keynum_to_string_stub(const int key, const int translate)
	{
		if (const auto* name = gamepad::key_name(key))
		{
			if (translate)
			{
				if (const auto* icon = gamepad::key_icon(key)) return icon;
			}
			return name;
		}
		return key_keynum_to_string_original(key, translate);
	}

	// ---- bindaxis ----
	void bind_axis(const std::vector<std::string>& params)
	{
		if (params.size() < 3) return;

		const auto find = [](const char* const* names, const int count, const std::string& name)
		{
			for (auto i = 0; i < count; ++i)
			{
				if (!_stricmp(names[i], name.data())) return i;
			}
			return -1;
		};

		const auto physical = find(physical_names, A_COUNT, params[1]);
		const auto axis = find(virtual_names, VA_COUNT, params[2]);
		if (physical < 0 || axis < 0) return;

		axis_bindings[physical].axis = static_cast<virtual_axis>(axis);
		axis_bindings[physical].mapping = params.size() > 3 && !_stricmp(params[3].data(), "MAP_SQUARED") ? MAP_SQUARED : MAP_LINEAR;
	}

	// ---- testing without a controller ----
	void log(const std::string& text)
	{
		utils::io::write_file(ps3style::folder() + "\\ps3style.log", text, true);
	}

	// key bindings are indexes into the game's bindable command list (bind_commands, 91 entries)
	const char* binding_of(const int key)
	{
		const auto index = *reinterpret_cast<int*>(0xB3C768 + 12 * key);
		if (index <= 0 || index >= bind_command_count) return nullptr;
		return *reinterpret_cast<const char**>(bind_commands + 4 * index);
	}

	// the icon of the first pad key bound to a command, as the PS3's key search 0xD21C4 for pads: keys 1-6, 14-23,
	// 28-31 in that order (APAD_* have no icon of their own here)
	const char* pad_key_icon(const char* command)
	{
		for (auto key = 1; key < 32; ++key)
		{
			if ((key > 6 && key < 14) || (key > 23 && key < 28)) continue;
			const auto* bound = binding_of(key);
			if (!bound || _stricmp(bound, command)) continue;
			if (const auto* icon = gamepad::key_icon(key)) return icon;
		}
		return nullptr;
	}

	// what the PS3 (0x270E34, table 0x554B1C) shows for a command nothing is bound to itself
	struct binding_alias
	{
		const char* command;
		const char* alternatives[2];
	};

	const binding_alias binding_aliases[] = {
		{"+melee", {"+melee_breath", "+melee_zoom"}},
		{"+sprint", {"+breath_sprint", "+sprint_zoom"}},
		{"+holdbreath", {"+melee_breath", "+breath_sprint"}},
		{"+changezoom", {"+melee_zoom", "+sprint_zoom"}},
		{"+attack", {"+attack_akimbo_accessible", nullptr}},
		{"+speed_throw", {"+ads_akimbo_accessible", nullptr}},
	};

	void status()
	{
		std::string text = utils::string::va("gamepad: xinput %s, connected %d (port %d), in use %d, ui focus %d, profile buttons \"%s\" sticks \"%s\" sens %.2f invert %d\n  binds:",
		                                     xinput_get_state ? "yes" : "no", pad.connected, pad.port, used_last.load(), ui_has_focus(),
		                                     reinterpret_cast<const char*>(profile_buttons_config), reinterpret_cast<const char*>(profile_sticks_config),
		                                     *reinterpret_cast<float*>(profile_view_sensitivity), *reinterpret_cast<unsigned char*>(profile_invert_pitch));
		for (const auto& k : keys)
		{
			if (const auto* b = binding_of(k.key)) text += utils::string::va(" %s=%s", k.name, b);
		}
		text += "\n  axes:";
		for (auto p = 0; p < A_COUNT; ++p)
		{
			if (axis_bindings[p].axis != VA_NONE)
			{
				text += utils::string::va(" %s=%s%s", physical_names[p], virtual_names[axis_bindings[p].axis],
				                          axis_bindings[p].mapping == MAP_SQUARED ? "(sq)" : "");
			}
		}
		text += utils::string::va("\n  analog moves %d: host cmds restored %u, not found %u, other mask %u", analog_moves(), moves_restored,
		                          moves_not_found, moves_other_mask);
		log(text + "\n");
	}

	void load_icons()
	{
		std::string data;
		if (!utils::io::read_file(ps3style::folder() + "\\button_icons.json", &data)) return;

		rapidjson::Document doc;
		doc.Parse(data.data(), data.size());
		if (doc.HasParseError() || !doc.IsObject()) return;

		for (auto itr = doc.MemberBegin(); itr != doc.MemberEnd(); ++itr)
		{
			if (!itr->value.IsString()) continue;
			const auto key = gamepad::key_number(itr->name.GetString());
			if (key > 0) icons[key] = itr->value.GetString();
		}
	}
}

bool gamepad::in_use()
{
	return pad.connected && used_last;
}

int gamepad::key_number(const char* name)
{
	if (!name) return -1;
	for (const auto& k : keys)
	{
		if (!_stricmp(k.name, name)) return k.key;
	}
	return -1;
}

const char* gamepad::key_name(const int key)
{
	for (const auto& k : keys)
	{
		if (k.key == key) return k.name;
	}
	return nullptr;
}

const char* gamepad::key_icon(const int key)
{
	const auto itr = icons.find(key);
	return itr == icons.end() ? nullptr : itr->second.data();
}

const char* gamepad::bound_icon(const char* command)
{
	if (!command || !*command || !game::is_mp()) return nullptr;

	// as the PS3 (0x270E34): use and reload prompts show the use / reload button
	if (!_stricmp(command, "+activate") || !_stricmp(command, "+reload")) command = "+usereload";
	if (const auto* icon = pad_key_icon(command)) return icon;

	for (const auto& alias : binding_aliases)
	{
		if (_stricmp(alias.command, command)) continue;
		for (const auto* alternative : alias.alternatives)
		{
			if (!alternative) break;
			if (const auto* icon = pad_key_icon(alternative)) return icon;
		}
		break;
	}
	return nullptr;
}

void gamepad::post_load()
{
	if (!game::is_mp()) return;

	// default layout until a config says otherwise (thumbstick_default.cfg)
	axis_bindings[A_LSTICK_X] = {VA_SIDE, MAP_SQUARED};
	axis_bindings[A_LSTICK_Y] = {VA_FORWARD, MAP_SQUARED};
	axis_bindings[A_RSTICK_X] = {VA_YAW, MAP_LINEAR};
	axis_bindings[A_RSTICK_Y] = {VA_PITCH, MAP_LINEAR};

	utils::hook(key_string_to_keynum_addr, key_string_to_keynum_stub, HOOK_JUMP).install()->quick();
	utils::hook(key_keynum_to_string_addr, key_keynum_to_string_stub, HOOK_JUMP).install()->quick();
	utils::hook(cl_mouse_move_addr, cl_mouse_move_stub, HOOK_JUMP).install()->quick();
	utils::hook(location_select_call, cl_location_select_stub, HOOK_CALL).install()->quick();
	utils::hook(remote_control_call, cl_remote_control_move_stub, HOOK_CALL).install()->quick();
	utils::hook(0x5C913B, cl_mouse_event_stub, HOOK_CALL).install()->quick();
	utils::hook(sv_read_usercmd_call, sv_read_usercmd_stub, HOOK_CALL).install()->quick();
	utils::hook(key_get_keys_for_command_addr, key_get_keys_for_command_stub, HOOK_JUMP).install()->quick();
	utils::hook(item_bind_paint_keys, item_bind_paint_keys_stub, HOOK_JUMP).install()->quick();
	utils::hook(item_bind_paint_keys_done, item_bind_paint_keys_done_stub, HOOK_JUMP).install()->quick();

	load_xinput();
	load_icons();

	scheduler::once([]
	{
		command::add("bindaxis", bind_axis);
		command::add("gpad_status", [](const std::vector<std::string>&) { status(); });
		command::add("gpad_debug", [](const std::vector<std::string>& params) { debug_keys = params.size() < 2 || params[1] != "0"; });

		// gpad_sim key <BUTTON_A ...> <1|0>  /  gpad_sim stick <lx> <ly> <rx> <ry> <seconds>
		command::add("gpad_sim", [](const std::vector<std::string>& params)
		{
			if (params.size() >= 3 && params[1] == "tap")
			{
				// press, released two frames later (no menu scroll repeat)
				const auto key = gamepad::key_number(params[2].data());
				if (key > 0)
				{
					if (sim_tap_key) key_event(sim_tap_key, false);
					used_last = true;
					pad.repeat_time[key] = sys_milliseconds() + static_cast<unsigned int>(value(gpad_menu_scroll_delay_first, 0.42f) * 1000.0f);
					key_event(key, true);
					sim_tap_key = key;
					sim_tap_frames = 2;
				}
			}
			else if (params.size() >= 4 && params[1] == "key")
			{
				const auto key = gamepad::key_number(params[2].data());
				if (key > 0)
				{
					used_last = true;
					pad.connected = true;
					key_event(key, params[3] != "0");
					pad.repeat_time[key] = sys_milliseconds() + static_cast<unsigned int>(value(gpad_menu_scroll_delay_first, 0.42f) * 1000.0f);
				}
			}
			else if (params.size() >= 7 && params[1] == "stick")
			{
				for (auto i = 0; i < 4; ++i) sim_sticks[i] = static_cast<float>(std::atof(params[2 + i].data()));
				sim_until = sys_milliseconds() + static_cast<unsigned int>(std::atof(params[6].data()) * 1000.0);
				used_last = true;
			}
		});

		command::add("unbindallaxis", [](const std::vector<std::string>&)
		{
			for (auto& b : axis_bindings) b = {};
		});

		gpad_enabled = register_bool("gpad_enabled", true, "Game pad support (XInput controllers)");
		gpad_ignore_mapped_input = register_bool("gpad_ignoreMappedInput", true,
		                                         "While the pad is used, drop the keyboard / mouse input programs make from it (Steam's desktop layout)");
		gpad_rumble = register_bool("gpad_rumble", true, "Game pad vibration (the PS3 Options' Vibration)");
		gpad_analog_move = register_bool("gpad_analogMove", true,
		                                 "Game pad: walk at the stick's speed while hosting (as the consoles); 0 = full speed or none, as the network sends it");

		// the PS3 Options' Vibration (the PC profile has no rumble field; the menus read the dvar instead)
		command::add("profile_toggleRumble", [](const std::vector<std::string>&)
		{
			cbuf(gpad_rumble && gpad_rumble->current.enabled ? "set gpad_rumble 0\n" : "set gpad_rumble 1\n");
		});

		// gpad_rumble_test <name>: plays a PS3 rumble (ps3style\rumble)
		command::add("gpad_rumble_test", [](const std::vector<std::string>& params)
		{
			play_rumble(params.size() > 1 ? params[1] : "damage_heavy");
		});
		// gpad_aimdebug 1: aim assist state -> ps3style.log once a second
		command::add("gpad_aimdebug", [](const std::vector<std::string>& params)
		{
			assist_debug = params.size() < 2 || params[1] != "0";
			assist_fake_target = params.size() >= 2 && params[1] == "2";
		});

		// the PS3's values (not saved: the PS3 has no setting for them)
		gpad_stick_deadzone_min = register_float("gpad_stick_deadzone_min", 0.15f, 0.0f, 1.0f, "Game pad minimum stick deadzone", 0);
		gpad_stick_deadzone_max = register_float("gpad_stick_deadzone_max", 0.01f, 0.0f, 1.0f, "Game pad maximum stick deadzone", 0);
		gpad_button_deadzone = register_float("gpad_button_deadzone", 0.4f, 0.0f, 1.0f, "Game pad button deadzone threshhold", 0);
		gpad_stick_pressed = register_float("gpad_stick_pressed", 0.4f, 0.0f, 1.0f, "Game pad stick pressed threshhold", 0);
		gpad_stick_pressed_hysteresis = register_float("gpad_stick_pressed_hysteresis", 0.1f, 0.0f, 1.0f,
		                                               "Game pad stick pressed no-change-zone around gpad_stick_pressed to prevent bouncing", 0);
		gpad_menu_scroll_delay_first = register_float("gpad_menu_scroll_delay_first", 0.42f, 0.0f, 2.0f,
		                                              "Menu scroll key-repeat delay, for the first repeat (s)", 0);
		gpad_menu_scroll_delay_rest = register_float("gpad_menu_scroll_delay_rest", 0.21f, 0.0f, 2.0f,
		                                             "Menu scroll key-repeat delay, for repeats after the first (s)", 0);

		// the select button shows the scores like on the console
		cbuf("bind BUTTON_BACK +scores\n");

		// first start: the PS3's defaults in the profile (the PC profile has its own values). The marker holds how far
		// this went, so later additions apply once too.
		const auto marker = ps3style::folder() + "\\gamepad_profile.done";
		if (utils::io::directory_exists(ps3style::folder()))
		{
			std::string done;
			utils::io::read_file(marker, &done);
			auto version = done.empty() ? 0 : (std::isdigit(static_cast<unsigned char>(done[0])) ? std::atoi(done.data()) : 1);
			if (version < 1)
			{
				// look sensitivity level 3 = 1.4
				cbuf("profile_setViewSensitivity 1.4\n");
			}
			if (version < 2)
			{
				// HUD margins (Options: horizontal / vertical margin) 0.85, read from the PS3 game
				*reinterpret_cast<float*>(0x132B318) = 0.85f;
				*reinterpret_cast<float*>(0x132B31C) = 0.85f;
				cbuf("set safeArea_adjusted_horizontal 0.85; set safeArea_adjusted_vertical 0.85\n");
			}
			if (version < 2)
			{
				cbuf("updategamerprofile\n");
			}
			if (version < 3)
			{
				// earlier builds saved their own gpad values in the config: back to the PS3's
				cbuf("set gpad_stick_deadzone_min 0.15; set gpad_stick_deadzone_max 0.01; set gpad_button_deadzone 0.4; "
				     "set gpad_menu_scroll_delay_first 0.42; set gpad_menu_scroll_delay_rest 0.21\n");
				utils::io::write_file(marker, "3 (PS3 profile defaults: look sensitivity 1.4, HUD margins 0.85; PS3 gpad values)\n");
			}
		}
		scheduler::on_frame(frame);
	});
}

REGISTER_MODULE(gamepad)
