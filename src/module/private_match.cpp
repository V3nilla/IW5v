#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"

// Private Match is off on IW5V for good: nobody can start one, and there is no switch to turn it back on. The PS3 PLAY
// menu shows the PRIVATE MATCH row locked (make_menus.py). Here the ways in are shut for the PC menus, the console,
// binds and other menus too:
//   - xstartprivatematch (Cmd 0x48DA80, registered by the push at 0x492E3D) does nothing
//   - the private match lobby menu (menu_xboxlive_privatelobby) doesn't open (Menus_Open 0x5AAD80)
//   - xblive_privatematch (dvar_t* @0x5A9701C, the session is a private match) is kept at 0; a private match lobby
//     (g_lobbyData 0x132ECA0 with the dvar on) is left the way the lobby's own Leave does it (leavelobbywarning:
//     xstopparty, the lobby menus closed, xblive_privatematch 0), a private match this PC hosts is ended first
// The friends party (xstartprivateparty / g_partyData), invites and Find Game (xblive_privatematch 0) don't need it.
namespace
{
	using game::native::dvar_t;

	constexpr DWORD dvar_set_bool_addr = 0x5BF350; // (dvar_t*, bool): the code's own write, past the protection
	constexpr DWORD cbuf_add_text_addr = 0x545680; // (localClientNum, text)
	constexpr DWORD console_print_addr = 0x487B00; // CL_ConsolePrint (lcn, channel, text, duration, pixelWidth, flags)

	constexpr DWORD xblive_privatematch_addr = 0x5A9701C; // dvar_t*
	constexpr DWORD lobby_data = 0x132ECA0; // g_lobbyData: the Find Game / Private Match lobby, exists @1F18
	constexpr DWORD connection_state = 0x10625F4; // clientUIActives[0]: 0..2 disconnected, 3+ connecting / in a match
	constexpr DWORD ui_context = 0x58EF640; // menuStack[16] @A3C, openMenuCount @A7C; a menuDef_t's name @4

	constexpr DWORD menus_open_addr = 0x5AAD80; // Menus_Open (uiContext*, menuDef_t*, int)
	constexpr DWORD xstartprivatematch_push = 0x492E3D; // push 0x48DA80 before Cmd_AddCommand("xstartprivatematch")

	const char* const blocked_menus[] = {"menu_xboxlive_privatelobby"};

	DWORD next_notice = 0;

	// Dvar_FindVar (0x5BDC40, edi = name)
	__declspec(naked) dvar_t* find_dvar(const char* /*name*/)
	{
		__asm
		{
			push edi
			mov edi, [esp + 8]
			mov eax, 5BDC40h
			call eax
			pop edi
			retn
		}
	}

	void print(const char* text)
	{
		printf("%s", text);
		reinterpret_cast<void(*)(int, int, const char*, int, int, int)>(console_print_addr)(0, 0, text, 0, 0, 0);
	}

	void notice(const char* what)
	{
		// one line per attempt (a menu may try a few things in one go)
		const auto now = GetTickCount();
		if (now < next_notice) return;
		next_notice = now + 1000;

		char text[256];
		sprintf_s(text, "Private Match is disabled (%s refused).\n", what);
		print(text);
	}

	dvar_t* dvar_at(const DWORD address)
	{
		return *reinterpret_cast<dvar_t**>(address);
	}

	void set_bool(dvar_t* dvar, const bool value)
	{
		reinterpret_cast<void(*)(dvar_t*, bool)>(dvar_set_bool_addr)(dvar, value);
	}

	void cbuf(const char* text)
	{
		reinterpret_cast<void(*)(int, const char*)>(cbuf_add_text_addr)(0, text);
	}

	const char* menu_name(const void* menu)
	{
		return menu ? *reinterpret_cast<const char* const*>(reinterpret_cast<const char*>(menu) + 4) : nullptr;
	}

	bool menu_open(const char* name)
	{
		const auto count = *reinterpret_cast<int*>(ui_context + 0xA7C);
		for (auto i = 0; i < count && i < 16; ++i)
		{
			const auto* menu_ptr = *reinterpret_cast<void**>(ui_context + 0xA3C + 4 * i);
			const auto* open = menu_name(menu_ptr);
			if (open && _stricmp(open, name) == 0) return true;
		}
		return false;
	}

	// Menus_Open's menu is refused (true) when it is a private match menu
	bool __cdecl refuse_menu(const void* menu)
	{
		const auto* name = menu_name(menu);
		if (!name) return false;
		for (const auto* blocked : blocked_menus)
		{
			if (_stricmp(name, blocked) == 0)
			{
				notice(name);
				return true;
			}
		}
		return false;
	}

	// Menus_Open 0x5AAD80 begins push ecx; push ebx; push ebp; push esi; xor ebp, ebp (6 bytes): those are run here
	// and the function goes on at 0x5AAD86
	__declspec(naked) void menus_open_stub()
	{
		__asm
		{
			push dword ptr [esp + 8] // menuDef_t*
			call refuse_menu
			add esp, 4
			test al, al
			jnz refused

			push ecx
			push ebx
			push ebp
			push esi
			xor ebp, ebp
			push 5AAD86h
			retn

		refused:
			retn
		}
	}

	void __cdecl xstartprivatematch_stub()
	{
		notice("xstartprivatematch");
	}

	bool dvar_enabled(const dvar_t* dvar)
	{
		return dvar && dvar->current.enabled;
	}

	void guard()
	{
		auto* private_match = dvar_at(xblive_privatematch_addr);
		if (!dvar_enabled(private_match)) return;

		const auto in_lobby = *reinterpret_cast<int*>(lobby_data + 0x1F18) != 0;
		const auto in_menu = menu_open("menu_xboxlive_privatelobby") || menu_open("menu_xboxlive_teams");
		const auto connected = *reinterpret_cast<int*>(connection_state) > 2;
		const auto hosting = dvar_enabled(find_dvar("sv_running"));

		if (in_lobby || in_menu || hosting)
		{
			// a private match of ours or someone's lobby: left as the lobby's Leave does it
			notice("private match lobby");
			std::string text;
			if (hosting || connected) text += "disconnect\n";
			text += "xstopparty\nclosemenu menu_xboxlive_privatelobby\nclosemenu menu_xboxlive_teams\n";
			cbuf(text.data());
			set_bool(private_match, false);
		}
		else if (!connected)
		{
			// the dvar alone (console, a menu script): back to 0, nothing to leave
			set_bool(private_match, false);
		}
		// connected to a server without a lobby (a dedicated server): its game, left alone
	}
}

class private_match final : public module
{
public:
	void post_load() override
	{
		if (!game::is_mp()) return;

		// the developer build (IW5V_DEV, never set in the normal Release build): Private Match stays open
#ifdef IW5V_DEV
		constexpr auto locked = false;
#else
		constexpr auto locked = true;
#endif
		if constexpr (locked)
		{
			utils::hook(menus_open_addr, menus_open_stub, HOOK_JUMP).install()->quick();
			utils::hook::nop(menus_open_addr + 5, 1);
			utils::hook::set<DWORD>(xstartprivatematch_push + 1, reinterpret_cast<DWORD>(&xstartprivatematch_stub));

			scheduler::once([] { scheduler::on_frame(guard); });
		}
	}
};

REGISTER_MODULE(private_match)
