#pragma once
#include "loader/module_loader.hpp"
#include "game/ui_structs.hpp"

// MW3 on PS3 look for the PC game, from the data in <game>\ps3style (made by the ps3style tools from the
// MW3 PS3 dump): PS3 menus, fonts, button prompts and images. Dvar ui_ps3Style (0 = the normal PC look).
class ps3style final : public module
{
public:
	void post_load() override;

	// <game>\ps3style
	static std::string folder();

	// the PS3 style is on (decided at start: the ps3style folder is there, ui_ps3Style isn't 0, no -nops3style)
	static bool enabled();

	// the game's own asset lookup (DB_FindXAssetHeader), without the PS3 overrides
	static void* find_asset(int type, const char* name, int allow_create_default = 1);

private:
	static void* db_find_x_asset_header_stub(int type, const char* name, int allow_create_default);
};
