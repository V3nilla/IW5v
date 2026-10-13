#pragma once
#include "game/game.hpp"

// Addresses that mean the same thing, with the same calling convention, on iw5mp.exe (MP: the listen host runs its
// server in it) and iw5mp_server.exe (the dedicated server, 1.9.388110), for the modules that run on both. Functions
// that take register arguments on the dedicated server, and hook sites only one of them has: dedi_native.hpp.
// Every entry was read in both disassemblies (port map: side note dedicated-bots-gsc-port.md).
namespace server_addresses
{
	struct table
	{
		// ---- script loading (gsc) ----
		DWORD load_scripts_call; // G_InitGame -> GScr_LoadScripts
		DWORD load_scripts; // GScr_LoadScripts ()
		DWORD scr_load_script; // int (const char* filename)
		DWORD scr_get_function_handle; // int (const char* filename, unsigned int name token)
		DWORD db_is_x_asset_default; // int (XAssetType, const char*), starts push ebx; mov ebx, [esp + 0Ch]
		DWORD db_is_x_asset_default_resume; // after those 5 bytes

		// ---- server ----
		DWORD svs_clients; // client_t[]
		DWORD svs_num_clients; // sv_maxclients
		DWORD svs_time;
		DWORD client_size; // client_t: 8 bytes less on the dedicated server, all after +0x41C9C
		DWORD g_entities; // gentity_s[], 0x274 each on both
	};

	inline constexpr table mp{
		0x50D2ED, 0x523E00, 0x561CC0, 0x5618A0, 0x4CA800, 0x4CA805,
		0x4B5CF90, 0x4B5CF8C, 0x4B5CF80, 0x78698, 0x1A66E28,
	};

	inline constexpr table dedi{
		0x489CC4, 0x4A7250, 0x4E6930, 0x4E61F0, 0x44EB00, 0x44EB05,
		0x4A12E90, 0x4A12E8C, 0x4A12E80, 0x78690, 0x191B900,
	};

	inline const table& get()
	{
		return game::is_dedi() ? dedi : mp;
	}
}
