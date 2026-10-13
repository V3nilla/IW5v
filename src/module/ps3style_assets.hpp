#pragma once
#include "game/ui_structs.hpp"

// PS3 images and fonts for the PC game, from <game>\ps3style (made by ps3style\tools\make_assets.py):
//   images\<material>.ps3z   "PS3Z", u32 width, u32 height, zlib(BGRA rows) - becomes a 2D material of that name
//   fonts\<font>.json        {"pixelHeight", "image", "glyphs": [[letter, x0, y0, dx, w, h, s0, t0, s1, t1], ...]}
//   fonts\<font>_en.json     the same for the English PS3 disc's fonts (ui_ps3StyleFonts 2; atlases images\*_en.ps3z)
namespace ps3style_assets
{
	// xbox: the button icons from images\xbox_button_*.ps3z (dvar ui_xboxButtons), the PS3 ones where there is none;
	// fonts_suffix: the fonts font() reads ("": fonts\<font>.json, the Japanese PS3 disc's; "_en": fonts\<font>_en.json,
	// the English one's)
	void init(const std::string& folder, bool xbox, const std::string& fonts_suffix = "");

	// the renderer is restarted (vid_restart: a new D3D device, the zones loaded again): every image, material and font
	// made so far is dropped and made again on its next use
	void reset();

	// the Xbox 360 button icons are on (decided at start: config ui_xboxButtons 1 or -xboxbuttons)
	bool xbox_buttons();

	// an asset the game has loaded, without waiting for the loader (DB_FindXAssetHeader waits while zones load)
	void* find_loaded(int type, const char* name);

	// Dvar_FindVar (0x5BDC40, edi = name): the dvar_t of that name or nullptr
	void* find_dvar(const char* name);

	// a ps3style image of that material name exists
	bool has_image(const char* name);

	// the material made from ps3style\images\<name>.ps3z (made on first use; sampled linear and clamped, as the PC's UI
	// images), nullptr when there is none
	game::native::ui::Material* material(const char* name);

	// the texel size of ps3style\images\<name>.ps3z (read from its header, no texture needed); false when there is none
	bool image_size(const char* name, int* width, int* height);

	// the PS3 version of a stock font ("fonts/normalfont" ...): fonts\<font><suffix>.json with the suffix init() was
	// given, nullptr when there is none
	game::native::ui::Font_s* font(const char* name);

	// development: the next presented frame (the game's back buffer) is written to path as a BMP, also while the window
	// is in the background
	bool capture_frame(const std::string& path);
}
