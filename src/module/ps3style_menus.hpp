#pragma once
#include "game/ui_structs.hpp"

// PS3 menus for the PC game: ps3style\menus.json (made by ps3style\tools\make_menus.py from the MW3 PS3 menus) turned
// into the game's menu structures, built when the game first asks for a menu or menu list.
namespace ps3style_menus
{
	// ui_xboxButtons: the menus of "menus_xbox" in menus.json (the Xbox 360 controller pictures) instead of the PS3 ones
	// of that name, while images\xenon_controller_*.ps3z are there; kept by reload()
	void set_xbox(bool xbox);

	// reads ps3style\menus.json; false when there is none
	bool load(const std::string& path);
	bool loaded();

	// the PS3 version of a menu, nullptr when there is none
	game::native::ui::menuDef_t* find_menu(const char* name);

	// a PS3 menu list: its PS3 menus + the menus of the PC list of that name that the PS3 one doesn't have (the
	// PC game's code still opens some of them); pc_list may be nullptr
	game::native::ui::MenuList* find_list(const char* name, const game::native::ui::MenuList* pc_list);

	// builds every menu of the file (for ps3style_menuReport)
	void build_all();

	// assets the PS3 menus use that the PC game doesn't have ("material <name>" / "sound <name>")
	std::vector<std::string> missing();

	// "menu / item n / field" of a statement the builder made (crash reports)
	std::string describe(const void* statement);

	// looks up again every zone asset the built menus point to (materials, focus sounds) and forgets the built menu
	// lists (they hold menus of the PC lists): for when zones were freed / loaded (the UI zones when a level starts or
	// ends, the map's zones per level). The menus themselves stay (the game's menu lists point to them).
	void refresh();

	// throws away the built menus and reads the file again (the game's menu lists keep pointing to the old ones until
	// they are loaded again)
	void reload(const std::string& path);
}
