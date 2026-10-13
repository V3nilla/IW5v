#include <std_include.hpp>
#include "ps3style.hpp"
#include "ps3style_menus.hpp"
#include "ps3style_assets.hpp"
#include "gamepad.hpp"
#include "gsc.hpp"
#include "command.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"
#include "utils/io.hpp"
#include "utils/nt.hpp"
#include "utils/string.hpp"
#include "utils/flags.hpp"
#include "utils/memory.hpp"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/document.h>

#include <algorithm>

using namespace game::native::ui;

namespace
{
	// iw5mp 1.9.388110
	constexpr DWORD db_find_x_asset_header_addr = 0x4CA620; // XAssetHeader (XAssetType, const char*, int allowCreateDefault)
	constexpr DWORD dvar_register_bool_addr = 0x5BE9F0; // (name, value, flags, description)
	constexpr DWORD dvar_register_int_addr = 0x5BEA40; // (name, value, min, max, flags, description)
	constexpr DWORD dvar_register_string_addr = 0x5BEC90; // (name, value, flags, description)
	constexpr DWORD dvar_set_from_string_addr = 0x5BF740; // (name, value)
	constexpr DWORD cbuf_add_text_addr = 0x545680; // (localClientNum, text)

	game::native::dvar_t* ui_ps3_style = nullptr;

	// ui_ps3ButtonStyle (archived): the PS3 button icons' size, 0 the English PS3's, 1 the Japanese PS3's (read while
	// drawing, so it changes at once; nullptr until it is registered: 0). It replaces ui_ps3ButtonSize, which configs
	// saved as "1" (the Japanese size, its old default), so that one is no longer read
	game::native::dvar_t* ui_ps3_button_style = nullptr;

	// every menu list / menu name the game asked for (for the dumps)
	std::mutex names_mutex;
	std::set<std::string> menu_list_names;
	std::set<std::string> menu_names;

	bool log_assets = false;

	// decided at start (the menus are loaded before the dvar exists): the ps3style folder is there and the config
	// doesn't turn ui_ps3Style off
	bool active = false;

	// ui_ps3StyleFonts 1: the Japanese PS3 disc's fonts (fonts\*.json) with its button letters (strings_ps3font.json,
	// bindings_ps3font.json); 2: the English PS3 disc's fonts (fonts\*_en.json), the button icons stay inline images
	// (sized as the English disc's glyphs: inline_image_bytes)
	bool ps3_fonts = false;
	bool english_fonts = false;

	// ps3style\strings.json: localized strings that replace the game's (PS3 prompts)
	std::unordered_map<std::string, LocalizeEntry*> strings;

	// the file is UTF-8 JSON, the game's text Latin-1 (code points < 256 become that byte)
	std::string latin1(const char* utf8)
	{
		std::string out;
		for (const auto* p = reinterpret_cast<const unsigned char*>(utf8); *p;)
		{
			if (*p < 0x80) out += static_cast<char>(*p++);
			else if ((*p & 0xE0) == 0xC0 && p[1])
			{
				out += static_cast<char>(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
				p += 2;
			}
			else
			{
				out += '?';
				++p;
				while ((*p & 0xC0) == 0x80) ++p;
			}
		}
		return out;
	}

	// ps3style\bindings.json: command -> text that "[{command}]" shows (the PS3 button of the PS3 default layout)
	std::unordered_map<std::string, std::string> bindings;

	// ui_ps3StyleFonts 1: PS3 button image -> the PS3 font's button letter (bindings.json beside bindings_ps3font.json),
	// for the pad's icons (button_icons.json has the images)
	std::unordered_map<std::string, std::string> icon_letters;

	std::unordered_map<std::string, std::string> read_bindings(const std::string& path)
	{
		std::unordered_map<std::string, std::string> result;
		std::string data;
		if (!utils::io::read_file(path, &data)) return result;

		rapidjson::Document doc;
		doc.Parse(data.data(), data.size());
		if (doc.HasParseError() || !doc.IsObject()) return result;

		for (auto itr = doc.MemberBegin(); itr != doc.MemberEnd(); ++itr)
		{
			if (itr->value.IsString()) result[utils::string::to_lower(itr->name.GetString())] = latin1(itr->value.GetString());
		}
		return result;
	}

	// the material of the first inline image in a text ("^" 1|2 width height nameLength name), "" when there is none
	std::string inline_image_name(const std::string& text)
	{
		for (size_t i = 0; i + 4 < text.size(); ++i)
		{
			if (text[i] == '^' && (text[i + 1] == 1 || text[i + 1] == 2))
			{
				return text.substr(i + 5, static_cast<unsigned char>(text[i + 4]));
			}
		}
		return {};
	}

	void load_bindings(const std::string& path)
	{
		// the PS3 font's button letters only with the PS3 icons: the Xbox icons are inline images in every font
		const auto letters = ps3_fonts && !ps3style_assets::xbox_buttons();
		bindings = read_bindings(path + (letters ? "\\bindings_ps3font.json" : "\\bindings.json"));
		if (!letters) return;

		for (const auto& [command, icon] : read_bindings(path + "\\bindings.json"))
		{
			const auto itr = bindings.find(command);
			const auto image = inline_image_name(icon);
			if (itr != bindings.end() && !image.empty()) icon_letters[image] = itr->second;
		}
	}

	// The PS3 animates its stick buttons (0x254AB0): while Sys_Milliseconds() % 1000 > 800 a text's L3 / R3 (letters
	// 10h / 11h) become the sticks pressed down (BCh / BDh: the cap on the base, the same size), a hard cut on one clock
	// for every stick on screen, chosen when the text is made. It does that to the text of a "[{command}]" (0x2711B4),
	// of the menus' KeyBinding(command, "true") (0x2413F0) and of every text with a "&&1" (the end of
	// UI_ReplaceConversions 0x254B68); the "&&1" key text alone (0x271190, the PC's 0x5A3AC0) and static texts ("Use [L3]
	// to position") keep still. The PC compiled it out: 0x5A75C5 and 0x58182A call the shared empty function 0x71D130,
	// its UI_ReplaceConversions (0x58F810) has nothing at the end.
	bool stick_pressed()
	{
		return GetTickCount() % 1000 > 800;
	}

	// the pressed stick of a stick's inline image ("ps3_button_l3" -> "ps3_button_l3_pressed"); "" for other images and
	// when its image isn't there (an older ps3style folder; with the Xbox icons the Xbox one too, else the PS3 one shows)
	std::string pressed_stick(const std::string& name)
	{
		if (name != "ps3_button_l3" && name != "ps3_button_r3") return {};

		const auto pressed = name + "_pressed";
		if (!ps3style_assets::has_image(pressed.data())) return {};
		if (ps3style_assets::xbox_buttons() && !ps3style_assets::has_image(("xbox_button_" + pressed.substr(11)).data())) return {};
		return pressed;
	}

	// 0x254AB0 for the PC's texts: the sticks' inline images ("^" 1|2 width height nameLength name) get the pressed
	// stick's name, 8 bytes longer (only while the whole text still fits in size, else that one stays as it is); the PS3
	// font's letters (ui_ps3StyleFonts 1 with the PS3 icons) 10h / 11h become BCh / BDh. Nothing inside an image's bytes
	// is swapped (name lengths 10h / 11h: ps3_button_start, _select ...). Reads at most size bytes: 0x58F810 copies
	// without a check, a full buffer may have no NUL
	void animate_sticks(char* text, const size_t size)
	{
		if (!active || !text || !size || !stick_pressed()) return;

		const auto letters = ps3_fonts && !ps3style_assets::xbox_buttons();
		const auto length = strnlen(text, size);
		auto room = length < size ? size - 1 - length : 0; // how much longer the text may get
		auto changed = false;
		std::string out;
		for (size_t i = 0; i < length;)
		{
			if (text[i] == '^' && i + 4 < length && (text[i + 1] == 1 || text[i + 1] == 2) && text[i + 2] && text[i + 3] && text[i + 4])
			{
				const size_t name_length = static_cast<unsigned char>(text[i + 4]);
				const auto end = std::min(length, i + 5 + name_length);
				const auto pressed = end == i + 5 + name_length ? pressed_stick(std::string(text + i + 5, name_length)) : std::string();
				if (!pressed.empty() && pressed.size() <= 127 && room >= pressed.size() - name_length)
				{
					room -= pressed.size() - name_length;
					out.append(text + i, 4); // "^", 1|2, the same width and height
					out += static_cast<char>(pressed.size());
					out += pressed;
					changed = true;
				}
				else
				{
					out.append(text + i, end - i);
				}
				i = end;
				continue;
			}

			auto c = text[i++];
			if (letters && (c == 0x10 || c == 0x11))
			{
				c = static_cast<char>(c == 0x10 ? 0xBC : 0xBD);
				changed = true;
			}
			out += c;
		}

		if (!changed) return;
		std::memcpy(text, out.data(), out.size());
		if (out.size() < size) text[out.size()] = 0;
	}

	// animate: the sticks as the PS3 does for a "[{command}]", not for the key text of a "&&1" (animate_sticks)
	int binding_text(const char* command, char* out, const int size, const bool animate)
	{
		if (!active || !command || !out || size <= 0) return 0;

		// the pad button bound to it in the profile's layout (buttons_default_alt ...), as the PS3 looks it up (0x270E34);
		// else the PS3 default layout's from bindings.json (keyboard-only commands, nothing bound)
		std::string text;
		if (const auto* bound = gamepad::bound_icon(command))
		{
			text = bound;
			if (ps3_fonts)
			{
				const auto letter = icon_letters.find(inline_image_name(text));
				if (letter != icon_letters.end()) text = letter->second;
			}
		}
		else
		{
			const auto itr = bindings.find(utils::string::to_lower(command));
			if (itr == bindings.end()) return 0;
			text = itr->second;
		}

		strncpy_s(out, size, text.data(), _TRUNCATE);
		if (animate) animate_sticks(out, static_cast<size_t>(size));
		return 1;
	}

	int __cdecl command_binding_text(const char* command, char* out, const int size)
	{
		return binding_text(command, out, size, true);
	}

	// 0x5A75A0 writes the text of a "[{command}]" (the keys bound to it): eax = command, edi = output, esi = size
	__declspec(naked) void binding_text_stub()
	{
		__asm
		{
			push eax
			push ecx
			push edx
			push esi
			push edi
			push eax
			call command_binding_text
			add esp, 0Ch
			pop edx
			pop ecx
			test eax, eax
			pop eax
			jnz handled

			sub esp, 200h
			push 5A75A6h
			retn

		handled:
			retn
		}
	}

	// ps3style\rawfiles\<name>: raw files (configs) the game gets as assets, e.g. the PS3 button / stick layouts
	struct RawFile
	{
		const char* name;
		int compressedLen; // 0: buffer is the text itself
		int len;
		const char* buffer;
	};

	std::unordered_map<std::string, RawFile*> rawfiles;

	void load_rawfiles(const std::string& path)
	{
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(path, ec))
		{
			if (!entry.is_regular_file()) continue;
			std::string data;
			if (!utils::io::read_file(entry.path().string(), &data)) continue;

			const auto name = utils::string::to_lower(entry.path().filename().string());
			auto* raw = utils::memory::allocate<RawFile>();
			raw->name = utils::memory::duplicate_string(name);
			raw->len = static_cast<int>(data.size());
			raw->buffer = utils::memory::duplicate_string(data);
			rawfiles[name] = raw;
		}
	}

	// 0x5A3AC0 writes the keys bound to a command as text ("F or MOUSE1", used for "&&1" in hints): eax = command,
	// ecx = localClientNum, stack (buffer, size, single key); returns the number of keys. The PS3 button instead.
	int __cdecl key_assignment_text(const char* command, char* out, const int size)
	{
		return binding_text(command, out, size, false);
	}

	// the key binding items (Item_Bind_Paint 0x5A7FC0, returns to 0x5A7FFC; the MOUSE & KEYBOARD pages) show the keys
	// as they are: the keyboard / mouse keys bound to the command, not the PS3 button
	__declspec(naked) void key_assignment_stub()
	{
		__asm
		{
			cmp dword ptr [esp], 5A7FFCh
			je original

			push ecx
			push eax
			push [esp + 10h]
			push [esp + 10h]
			push eax
			call key_assignment_text
			add esp, 0Ch
			test eax, eax
			jnz handled

			pop eax
			pop ecx

		original:
			sub esp, 100h
			push 5A3AC6h
			retn

		handled:
			add esp, 8
			retn
		}
	}

	// the menus' KeyBinding(command, "true") (0x5817C0: the text at 0x58DAC50, 100h) animates its sticks as the PS3's
	// (0x2413F0 -> 0x254AB0; the PS3 menus' XM25 hint): the call of the empty 0x71D130 at 0x58182A (cdecl, the caller
	// pops; afterwards only edi is used)
	void __cdecl keybinding_animate(char* text, const int size)
	{
		if (size > 0) animate_sticks(text, static_cast<size_t>(size));
	}

	// UI_ReplaceConversions (0x58F810, cdecl: text, args, output, size) puts the "&&1" ... arguments into a text: the
	// original (its first instructions, then 0x58F816)
	__declspec(naked) void ui_replace_conversions_original(const char* /*text*/, const void* /*args*/, char* /*out*/, int /*size*/)
	{
		__asm
		{
			push ecx
			push esi
			mov esi, [esp + 0Ch]
			push 58F816h
			retn
		}
	}

	// the sticks of every text with a "&&" animated, as at the end of the PS3's UI_ReplaceConversions (0x254B68; its
	// copy-only path 0x254BD0 skips it): "Hold [L3] to steady" (0x45D42B -> 0x592BE0), the cursor hints, the XM25 hint
	// (locstring), the spectator lines. None of its callers reads eax
	void __cdecl ui_replace_conversions_stub(const char* text, const void* args, char* out, const int size)
	{
		const auto conversions = text && std::strstr(text, "&&");
		ui_replace_conversions_original(text, args, out, size);
		if (conversions && size > 0) animate_sticks(out, static_cast<size_t>(size));
	}

	void load_strings(const std::string& path)
	{
		std::string data;
		if (!utils::io::read_file(path, &data)) return;

		rapidjson::Document doc;
		doc.Parse(data.data(), data.size());
		if (doc.HasParseError() || !doc.IsObject()) return;

		for (auto itr = doc.MemberBegin(); itr != doc.MemberEnd(); ++itr)
		{
			if (!itr->value.IsString()) continue;
			auto* entry = utils::memory::allocate<LocalizeEntry>();
			entry->name = utils::memory::duplicate_string(itr->name.GetString());
			entry->value = utils::memory::duplicate_string(latin1(itr->value.GetString()));
			strings[itr->name.GetString()] = entry;
		}
	}

	// a dvar's value in players2\config_mp.cfg ("" when it isn't there)
	std::string config_value(const std::string& dvar)
	{
		std::string config;
		const auto path = std::filesystem::path(ps3style::folder()).parent_path() / "players2" / "config_mp.cfg";
		if (!utils::io::read_file(path.string(), &config)) return {};

		const std::regex pattern("seta\\s+" + dvar + "\\s+\"?([^\"\\s]*)", std::regex::icase);
		std::smatch match;
		return std::regex_search(config, match, pattern) ? match[1].str() : std::string();
	}

	// ps3style\ps3style.log
	void log(const char* fmt, ...)
	{
		char buffer[2048];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buffer, sizeof(buffer), fmt, ap);
		va_end(ap);
		utils::io::write_file(ps3style::folder() + "\\ps3style.log", buffer, true);
	}

	std::string hex_dump(const void* data, const size_t size)
	{
		std::string out;
		const auto* b = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < size; i += 4)
		{
			if (i % 16 == 0) out += utils::string::va("\n  %03X:", i);
			out += utils::string::va(" %08X", *reinterpret_cast<const unsigned int*>(b + i));
		}
		return out + "\n";
	}

	// PS3 button icons are inline images ("^" 1|2 width height nameLength name), which the PC draws (size byte - 16) *
	// font height / 32 font px (0x694D15), so they grew with the font. The images are the PS3 glyphs' texels + a
	// transparent border. On the English PS3 (the reference videos) they are font glyphs of one size in every font: 32
	// px, face buttons 42 px in the 42-px fonts (a 40-texel disc where the 32-px glyph's is 30), start / select 32x20, L3
	// / R3 58x33, so they get the size bytes that draw them at their texel size in font px (face buttons x 4/3 in the big
	// fonts; ui_ps3ButtonStyle 0, the default). On the Japanese PS3 every button glyph is 32 texels in its 24-px fonts,
	// so in every font 1 texel = 1/24 of the line height: size byte - 16 = texels * 4/3 (ui_ps3ButtonStyle 1). The Xbox
	// icons (ui_xboxButtons) always take the English rule: the Xbox 360's 32-px button glyphs are drawn in its 33-px
	// fonts. Which font a text is drawn in matters as much (default_font). Drawing (0x694CF0) and measuring (R_TextWidth)
	// both go through here.
	int inline_image_pixels(const int size_byte, const int pixel_height)
	{
		return ((size_byte - 16) * pixel_height + 16) / 32;
	}

	int inline_image_byte(const int pixels, const int pixel_height)
	{
		// the closest the engine's formula gets; 17-127 (it reads the byte with movsx)
		const auto error = [&](const int size_byte)
		{
			const auto d = inline_image_pixels(size_byte, pixel_height) - pixels;
			return d < 0 ? -d : d;
		};

		auto best = 17;
		for (auto b = 18; b <= 127; ++b)
		{
			if (error(b) < error(best)) best = b;
		}
		return best;
	}

	// the size bytes the text code uses for the inline image at seq (its 1|2 byte) in that font
	void inline_image_bytes(const char* seq, const Font_s* font, int* width, int* height)
	{
		*width = static_cast<signed char>(seq[1]);
		*height = static_cast<signed char>(seq[2]);

		constexpr char prefix[] = "ps3_button_";
		constexpr size_t prefix_length = sizeof(prefix) - 1;
		const size_t length = static_cast<unsigned char>(seq[3]);
		if (!font || font->pixelHeight <= 0 || length <= prefix_length || length > 127 || std::strncmp(seq + 4, prefix, prefix_length) != 0)
		{
			return;
		}

		const std::string name(seq + 4, length);
		auto w = 0, h = 0;
		if (!ps3style_assets::image_size(name.data(), &w, &h)) return;

		const auto* style = ui_ps3_button_style;
		if (!ps3style_assets::xbox_buttons() && style && style->current.integer == 1)
		{
			// the Japanese PS3: 1 texel = 1/24 of the font height in every font (35 -> 63, 23 -> 47, 61 -> 97, 36 -> 64)
			*width = std::min(127, 16 + (w * 4 + 1) / 3);
			*height = std::min(127, 16 + (h * 4 + 1) / 3);
			return;
		}

		// the English PS3: 1 texel = 1 font px; its face buttons in the 42-px fonts are the 42-px glyphs, whose disc is 4/3
		// of the 32-px one's (40 texels, 30): 35 -> 47 font px
		const auto face = name == "ps3_button_cross" || name == "ps3_button_circle" || name == "ps3_button_square" || name == "ps3_button_triangle";
		if (face && font->pixelHeight >= 40)
		{
			w = (w * 4 + 1) / 3;
			h = (h * 4 + 1) / 3;
		}

		*width = inline_image_byte(w, font->pixelHeight);
		*height = inline_image_byte(h, font->pixelHeight);
	}

	// 0x694CF0 draws an inline image (ecx = its 1|2 byte, eax = font): a PS3 button gets its size bytes in a copy (the
	// text can be read-only; the caller steps over the name with its own pointer, 0x695976). Only the render thread draws
	// text (0x6954A0 is only called from the backend's text command 0x6966E0).
	char inline_image_copy[4 + 128];

	const char* __cdecl inline_image_sequence(const char* seq, const Font_s* font)
	{
		auto w = 0, h = 0;
		inline_image_bytes(seq, font, &w, &h);
		if (w == static_cast<signed char>(seq[1]) && h == static_cast<signed char>(seq[2])) return seq;

		inline_image_copy[0] = seq[0];
		inline_image_copy[1] = static_cast<char>(w);
		inline_image_copy[2] = static_cast<char>(h);
		inline_image_copy[3] = seq[3];
		std::memcpy(inline_image_copy + 4, seq + 4, static_cast<unsigned char>(seq[3])); // <= 127 (inline_image_bytes)
		return inline_image_copy;
	}

	// set while a button icon's drop shadow is drawn (inline_image_drop_shadow): inline_image_color makes it black
	bool inline_image_shadow = false;

	// the original 0x694CF0 (ecx = the 1|2 byte, eax = font, 7 stack arguments; the width it took in st(0))
	__declspec(naked) void inline_image_original()
	{
		__asm
		{
			sub esp, 114h
			push 694CF6h
			retn
		}
	}

	// 0x694CF0's stack arguments
	struct inline_image_args
	{
		float x;
		float y;
		float rotation[2];
		float x_scale;
		float y_scale;
		unsigned int color;
	};

	// cdecl: draws an inline image through the original
	__declspec(naked) float inline_image_draw(const char* /*seq*/, const Font_s* /*font*/, const inline_image_args* /*args*/)
	{
		__asm
		{
			push esi
			mov esi, [esp + 10h]
			push dword ptr [esi + 18h]
			push dword ptr [esi + 14h]
			push dword ptr [esi + 10h]
			push dword ptr [esi + 0Ch]
			push dword ptr [esi + 8]
			push dword ptr [esi + 4]
			push dword ptr [esi]
			mov ecx, [esp + 24h] // seq
			mov eax, [esp + 28h] // font
			call inline_image_original
			add esp, 1Ch
			pop esi
			retn
		}
	}

	// the PS3 draws its button glyphs over the text's drop shadow (0x3B49F4: renderFlags & 4), so a shadowed prompt's
	// icons look solid at the text's alpha; the PC's text (0x6954A0) gives only its font glyphs one (0x695A7E-0x695AB6),
	// not the inline images (0x695956). The same shadow under a PS3 button icon, under the same conditions: renderFlags
	// & 4 ([ebp + 30h], ebp = 0x6954A0's frame), the main pass (pass type [esp + 94h + 4 * [esp + 68h]] == 0 there) and
	// [esp + 2Ah] == 0 there (20h further from 0x694CF0's esp: its 7 arguments + return address); 1 px down / right (2
	// with renderFlags & 8), black with the text's alpha. entry = esp at 0x694CF0 (its return address)
	void __cdecl inline_image_drop_shadow(const char* seq, const Font_s* font, const unsigned char* frame, const unsigned char* entry)
	{
		constexpr char prefix[] = "ps3_button_";
		constexpr size_t prefix_length = sizeof(prefix) - 1;
		const size_t length = static_cast<unsigned char>(seq[3]);
		if (length <= prefix_length || std::strncmp(seq + 4, prefix, prefix_length) != 0) return;

		const auto render_flags = frame[0x30];
		const auto pass = *reinterpret_cast<const int*>(entry + 0x88);
		if (!(render_flags & 4) || pass < 0 || pass > 3 || *reinterpret_cast<const int*>(entry + 0xB4 + 4 * pass) != 0
			|| entry[0x4A] != 0)
		{
			return;
		}

		auto args = *reinterpret_cast<const inline_image_args*>(entry + 4);
		const auto offset = render_flags & 8 ? 2.0f : 1.0f;
		args.x += offset;
		args.y += offset;

		inline_image_shadow = true;
		inline_image_draw(seq, font, &args);
		inline_image_shadow = false;
	}

	__declspec(naked) void inline_image_stub()
	{
		__asm
		{
			pushad
			push eax
			push ecx
			call inline_image_sequence
			add esp, 8
			mov [esp + 18h], eax // ecx after popad

			lea ecx, [esp + 20h] // the return address, then the arguments
			push ecx
			push ebp
			push dword ptr [esp + 24h] // font (eax of the pushad)
			push eax
			call inline_image_drop_shadow
			add esp, 10h
			popad

			sub esp, 114h
			push 694CF6h
			retn
		}
	}

	// the button icons keep their colours (white, the text's alpha) as the PS3's button glyphs do (0x3B3AE8 forces
	// them white), not the text's: "Hold^3 &&1 ^7to steady" made them yellow; their drop shadow is black with the text's
	// alpha. 0x694DC1 loads the quad's colour (eax = the image's material)
	unsigned int __cdecl inline_image_color(const Material* image, const unsigned int color)
	{
		const auto* name = image ? image->info.name : nullptr;
		if (!name || _strnicmp(name, "ps3_button_", 11) != 0) return color;
		return inline_image_shadow ? color & 0xFF000000 : color | 0x00FFFFFF;
	}

	__declspec(naked) void inline_image_color_stub()
	{
		__asm
		{
			mov ecx, [esp + 130h]
			push eax
			push ecx
			push eax
			call inline_image_color
			add esp, 8
			mov ecx, eax
			pop eax
			push 694DC8h
			retn
		}
	}

	// ui_ps3StyleFonts 1 draws the PS3 font's button letters (1-6, 0Eh-17h, 1Ch-1Fh, BCh, BDh), white with the text's
	// alpha as on the PS3 (0x3B3AE8). 0x6959EC loads the glyph colour (esi = glyph); the flags of the cmp at 0x6959DB
	// are still read at 0x695A1E
	__declspec(naked) void glyph_color_stub()
	{
		__asm
		{
			pushfd
			push ecx
			mov eax, [esp + 54h] // [esp + 4Ch] at the hook
			movzx ecx, word ptr [esi]
			cmp ecx, 1
			jb glyph_keep
			cmp ecx, 6
			jbe glyph_white
			cmp ecx, 0Eh
			jb glyph_keep
			cmp ecx, 17h
			jbe glyph_white
			cmp ecx, 1Ch
			jb glyph_keep
			cmp ecx, 1Fh
			jbe glyph_white
			cmp ecx, 0BCh
			je glyph_white
			cmp ecx, 0BDh
			jne glyph_keep

		glyph_white:
			or eax, 0FFFFFFh

		glyph_keep:
			pop ecx
			popfd
			mov [esp + 10h], ecx
			push 6959F4h
			retn
		}
	}

	// R_TextWidth (0x669A50: text, maxChars, font, case) knows color codes but not inline images ("^" 1|2 width height
	// nameLength name, which the text drawing handles): their names were measured as letters, so the PS3 button icons
	// pushed right / centered text off. Text with images is measured in parts here; the image width is the one the
	// drawing uses (inline_image_bytes). The case (textStyle 9 = upper case, getTextWidthModCase 0x583760) is passed on:
	// without it upper-case labels were measured in mixed case and ran under the focus button.
	__declspec(naked) int r_text_width_original(const char* /*text*/, int /*max_chars*/, Font_s* /*font*/, int /*mod_case*/)
	{
		__asm
		{
			push ecx
			push ebx
			push edi
			xor ebx, ebx
			push 669A55h
			retn
		}
	}

	int __cdecl r_text_width_stub(const char* text, int max_chars, Font_s* font, const int mod_case)
	{
		if (!text || !font || !std::strstr(text, "^\x01") && !std::strstr(text, "^\x02"))
		{
			return r_text_width_original(text, max_chars, font, mod_case);
		}

		if (max_chars <= 0) max_chars = 0x7FFFFFFF;

		auto width = 0;
		auto best = 0;
		std::string part;
		const auto flush = [&]
		{
			if (!part.empty()) width += r_text_width_original(part.data(), 0x7FFFFFFF, font, mod_case);
			part.clear();
		};

		auto count = 0;
		for (const auto* p = text; *p && count < max_chars;)
		{
			if (p[0] == '^' && (p[1] == 1 || p[1] == 2) && p[2] && p[3] && p[4])
			{
				flush();
				auto w = 0, h = 0;
				inline_image_bytes(p + 1, font, &w, &h);
				width += inline_image_pixels(w, font->pixelHeight);
				p += 5 + static_cast<unsigned char>(p[4]);
				++count;
				continue;
			}

			if (*p == '\n' || *p == '\r')
			{
				flush();
				best = std::max(best, width);
				width = 0;
				++p;
				continue;
			}

			part += *p++;
			++count;
		}

		flush();
		return std::max(best, width);
	}

	// The default font (fontEnum 0 / 1: the cursor hints, "Hold [L3] to steady", the party list, the spectator lines, the
	// news ticker ...) as the PS3 picks it. UI_GetFontHandle (0x58BD90, the PS3's 0x252848) takes the small, normal,
	// big or extra big font by scale x a factor against ui_smallFont / ui_bigFont / ui_extraBigFont. The PC's factor is
	// the real pixels per virtual unit (0x40FF10: 0x4B6720() x scaleVirtualToReal[1], 2.25 at 1080p) against 0.25 / 0.4
	// / 0.55 in the config, so at 1080p nearly all of that text came in the 42-px extraBigFont; the PS3's is its HD 1.5
	// (0x2528F8, the 720p pixels) against 0.563 / 0.688 / 1.0 (registered at 0x25DFE4), so a hint at 0.375 is in
	// smallFont at any resolution, and its button glyphs are sized in that font (inline_image_bytes). The Xbox 360
	// references measure the same (the A of "Press A to" and the spectator lines' Y 42 px at 1080p: its 28-texel disc x
	// 1.5 in smallFont, 35 px in extraBigFont), so this is the rule with the Xbox icons too. The ui_*Font dvars can't say
	// it (at most 1.0, 1080p would need 1.032 and 1.5). The hook is UI_GetFontHandle's default branch (0x58BDE9: fld
	// [esp + 0Ch]), reached with the entry esp, so this returns to its caller; scr is unused (0x4471E9 passes nullptr)
	Font_s* __cdecl default_font(const void* /*scr*/, const int /*font_enum*/, const float scale)
	{
		const auto real = scale * 1.5f;
		if (real <= 0.563f) return *reinterpret_cast<Font_s**>(0x58F2574); // smallFont
		if (real >= 1.0f) return *reinterpret_cast<Font_s**>(0x58F2584); // extraBigFont
		if (real >= 0.688f) return *reinterpret_cast<Font_s**>(0x58F2570); // bigFont
		return *reinterpret_cast<Font_s**>(0x58F2580); // normalFont
	}

	// hud elements (0x4528E0) ask for their font with a size already in real pixels (0.25 x fontScale x ... x
	// scaleVirtualToReal[1], kept at +21Ch for drawing), so default_font would get 1.5 times the PS3's at 1080p; the
	// PS3 (0x74D44-0x74D74) multiplies by its own scaleVirtualToReal[1], 1.5 at 720p, before the same chooser. Only the
	// font asked for changes (the call at 0x4529AC), not the size it is drawn at: "Respawn" (default 1.6) stays a 42-px
	// font, default 1.0 texts get smallFont as on the PS3
	Font_s* __cdecl hud_elem_font(const float* scr, const int font_enum, const float scale)
	{
		const auto real = scr && scr[1] > 0.0f ? scale * 1.5f / scr[1] : scale;
		return reinterpret_cast<Font_s*(__cdecl*)(const float*, int, float)>(0x58BD90)(scr, font_enum, real); // UI_GetFontHandle
	}

	// development: the arguments of the last "statement -> text" evaluation (0x588990) and the thread that ran it (the
	// main thread, which evaluates the menus), named in crash reports
	volatile DWORD last_text_eval[2]{};
	volatile DWORD last_text_eval_thread = 0;

	__declspec(naked) void text_eval_stub()
	{
		__asm
		{
			mov eax, [esp + 4]
			mov last_text_eval, eax
			mov eax, [esp + 8]
			mov last_text_eval + 4, eax
			mov eax, fs:[24h]
			mov last_text_eval_thread, eax
			mov eax, 13BCh
			push 588995h
			retn
		}
	}

	// open-iw5's own code for the crash log's stack: the exe's ".main" section (std_include.cpp merges .text into it;
	// 0xA560000-0xA728305 on 2026-10-10, it moves with every build). The loader puts the game's PE headers over the
	// exe's, so the section is found from a function in it: the executable pages around it (.clr before it, .rsrc
	// after it are read-only)
	DWORD own_code_start = 0;
	DWORD own_code_end = 0;

	void find_own_code()
	{
		const auto executable = [](const DWORD address, MEMORY_BASIC_INFORMATION& mbi)
		{
			constexpr auto exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			return VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi) &&
				mbi.State == MEM_COMMIT && (mbi.Protect & exec) != 0;
		};

		MEMORY_BASIC_INFORMATION mbi{};
		if (!executable(reinterpret_cast<DWORD>(&find_own_code), mbi)) return;

		auto start = reinterpret_cast<DWORD>(mbi.BaseAddress);
		auto end = start + static_cast<DWORD>(mbi.RegionSize);
		const auto* image = mbi.AllocationBase;
		while (start > 0x10000 && executable(start - 1, mbi) && mbi.AllocationBase == image)
		{
			start = reinterpret_cast<DWORD>(mbi.BaseAddress);
		}
		while (executable(end, mbi) && mbi.AllocationBase == image)
		{
			end = reinterpret_cast<DWORD>(mbi.BaseAddress) + static_cast<DWORD>(mbi.RegionSize);
		}

		// not the game's code (0x401000-0x7E0000), should the two ever touch
		own_code_start = std::max(start, 0x7E0000ul);
		own_code_end = end;
	}

	// development: how many of the wanted bytes from address on are committed and readable (VirtualQuery): nothing is
	// read. IsBadReadPtr finds out by reading, and its access violations went through log_exception too
	size_t readable_size(const void* address, const size_t wanted)
	{
		constexpr auto readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
			PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		const auto start = reinterpret_cast<size_t>(address);
		auto end = start;
		MEMORY_BASIC_INFORMATION mbi{};
		while (end - start < wanted && VirtualQuery(reinterpret_cast<void*>(end), &mbi, sizeof(mbi)) == sizeof(mbi) &&
		       mbi.State == MEM_COMMIT && (mbi.Protect & readable) != 0 && (mbi.Protect & PAGE_GUARD) == 0)
		{
			const auto next = reinterpret_cast<size_t>(mbi.BaseAddress) + mbi.RegionSize;
			if (next <= end) break; // the top of the address space
			end = next;
		}
		return std::min(end - start, wanted);
	}

	// development: an access violation is logged (address, registers, return addresses on the stack) before the game
	// handles it / dies
	LONG WINAPI log_exception(EXCEPTION_POINTERS* info)
	{
		const auto code = info->ExceptionRecord->ExceptionCode;
		if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		// one at a time: a fault in here (or one on another thread meanwhile) isn't logged as one more exception (no
		// thread_local: open-iw5's TLS is the game's)
		static volatile LONG busy = 0;
		if (InterlockedExchange(&busy, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;
		const auto _ = gsl::finally([] { InterlockedExchange(&busy, 0); });

		// the same fault again is counted, not logged: the game catches the one in swapchain->Present (0x69699F) and the
		// next frame's faults the same way, which used up the 8 entries
		const auto* c = info->ContextRecord;
		const auto address = info->ExceptionRecord->ExceptionInformation[1];
		// (only once one is logged: a fault at EIP 0 / address 0, the render thread's present one, matched the starting zeros)
		static bool logged = false;
		static DWORD last_eip = 0;
		static ULONG_PTR last_address = 0;
		static int repeats = 0;
		if (logged && c->Eip == last_eip && address == last_address)
		{
			++repeats;
			return EXCEPTION_CONTINUE_SEARCH;
		}

		static int count = 0;
		if (++count > 8) return EXCEPTION_CONTINUE_SEARCH;

		std::string text = repeats ? utils::string::va("  (the one before %d more times)\n", repeats) : "";
		logged = true;
		last_eip = c->Eip;
		last_address = address;
		repeats = 0;

		text += utils::string::va("exception %08X at %08X (address %08X) eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X\n  stack:",
		                          code, c->Eip, address, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);

		// esp up to the thread's stack base (the handler runs on the faulting thread): the render thread's esp was within
		// 8 KB of it, and reading on past it faulted (three deep, each one logged)
		const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
		auto size = readable_size(reinterpret_cast<const void*>(c->Esp), 2048 * sizeof(DWORD));
		if (c->Esp >= reinterpret_cast<DWORD>(tib->StackLimit) && c->Esp < reinterpret_cast<DWORD>(tib->StackBase))
		{
			size = std::min<size_t>(size, reinterpret_cast<DWORD>(tib->StackBase) - c->Esp);
		}
		const auto* stack = reinterpret_cast<const DWORD*>(c->Esp);
		auto found = 0;
		for (size_t i = 0; i < size / sizeof(DWORD) && found < 24; ++i)
		{
			const auto v = stack[i];
			if ((v >= 0x401000 && v < 0x7E0000) || (v >= own_code_start && v < own_code_end))
			{
				text += utils::string::va(" %X", v);
				++found;
			}
		}
		// the main thread's last text eval (its statement: the first argument is always 0) says nothing about another
		// thread's fault (the render thread's ...)
		if (last_text_eval_thread == GetCurrentThreadId())
		{
			text += utils::string::va("\n  last text eval: %08X -> %s", last_text_eval[1],
			                          ps3style_menus::describe(reinterpret_cast<void*>(last_text_eval[1])).data());
		}
		else
		{
			text += utils::string::va("\n  thread %u, not the main thread (%u)", GetCurrentThreadId(), last_text_eval_thread);
		}
		utils::io::write_file(ps3style::folder() + "\\ps3style.log", text + "\n", true);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	void cbuf_add_text(const std::string& text)
	{
		reinterpret_cast<void(*)(int, const char*)>(cbuf_add_text_addr)(0, text.data());
	}

	// development: the console commands in ps3style\console_in.txt are run (and the file removed), so the game can be
	// driven from outside while testing
	void run_console_file()
	{
		static auto next = 0ull;
		const auto now = GetTickCount64();
		if (now < next) return;
		next = now + 50;

		// renamed first: a file still being written can't be taken (and isn't read twice)
		const auto path = ps3style::folder() + "\\console_in.txt";
		const auto taken = ps3style::folder() + "\\console_in.run";
		DeleteFileA(taken.data());
		if (!MoveFileA(path.data(), taken.data())) return;
		std::string text;
		if (!utils::io::read_file(taken, &text)) return;
		DeleteFileA(taken.data());
		if (!text.empty() && text.back() != '\n') text += '\n';
		cbuf_add_text(text);
	}

	void set_dvar(const char* name, const std::string& value)
	{
		reinterpret_cast<void(*)(const char*, const char*)>(dvar_set_from_string_addr)(name, value.data());
	}

	// as the menu script "open" (Menus_FindByName 0x59CFE0, Menus_Open 0x5AAD80 on the UI context 0x58EF640)
	bool open_menu(const char* name)
	{
		const auto ui = 0x58EF640u;
		auto* menu = reinterpret_cast<menuDef_t*(*)(DWORD, const char*)>(0x59CFE0)(ui, name);
		if (!menu) return false;
		reinterpret_cast<void(*)(DWORD, menuDef_t*, int)>(0x5AAD80)(ui, menu, 0);
		return true;
	}

	// cg_drawBreathHint (dvar_t* @8FC8DC): the PS3 controls menus set it 0 while they are open and put it back on
	// close. The PS3 registers it with flags 0, the PC archived (0x45523D push 1, patched to 0), so a 0 that stayed
	// was saved and the "Hold [L3] to steady" hint was gone for good. Outside the menus (keycatchers 0x10625EC without
	// KEYCATCH_UI 0x10) it is on, as on the PS3; that also undoes a "seta cg_drawBreathHint 0" in config_mp.cfg.
	void breath_hint_guard()
	{
		auto* dvar = *reinterpret_cast<game::native::dvar_t**>(0x8FC8DC);
		if (!dvar || dvar->current.enabled || (*reinterpret_cast<int*>(0x10625EC) & 0x10)) return;

		set_dvar("cg_drawBreathHint", "1");
		dvar->flags &= ~1u; // the "seta" made it archived again
	}

	// The MP scripts run their console code when ps3Game or xenonGame is "true" (level.console, set when a map loads);
	// the PC exe has neither dvar, so the host ran the PC's killstreaks: each streak on its own action slot (+actionslot
	// 4 the crate slot, 5-7 the streaks: no pad button has those) and none selected. The PS3 layouts (d-pad right
	// +actionslot 4 = use the selected streak, up / down +actionslot 1 / 2 = choose it) and the PS3 killstreaks_hd (the
	// highlight and d-pad arrows: playerdata killstreaksState.selectedIndex, -1 on PC) are the console's. xenonGame, not
	// ps3Game: level.ps3 also shortens long names (processlobbydata) and changes some maps' sun shadows. The scripts'
	// other console branches come with it (the 15 s "Match begins in" countdown, no quick messages, a carried sentry put
	// away with +actionslot 4 ...); only matches this PC hosts. Registered before the first map (scheduler::once: the
	// tweakables see level.console when they are registered); ps3style_consoleScripts 0 sets it "false" from the next map
	game::native::dvar_t* console_scripts = nullptr;
	game::native::dvar_t* xenon_game = nullptr;

	void console_scripts_guard()
	{
		if (!console_scripts || !xenon_game || !xenon_game->current.string) return;

		// not while hosting: round-based modes restart the map (and read level.console again) within one match
		const auto* sv_running = static_cast<const game::native::dvar_t*>(ps3style_assets::find_dvar("sv_running"));
		if (sv_running && sv_running->current.enabled) return;

		const auto* value = console_scripts->current.enabled ? "true" : "false";
		if (std::strcmp(xenon_game->current.string, value) != 0) set_dvar("xenonGame", value);
	}

	// clan tag: the PS3 popup_callsign's CLAN TAG runs "editclanname" (the PS3's on-screen keyboard, 0x277950 there),
	// the console "clantag <tag>". Profile of controller 0 (0x132B300, keys 0x7F6A78): clanPrefix @2A6 [5] (the PS3's
	// field), useEliteClanTag @2AB, eliteClanTagText @2CC [5]; the PC shows / sends eliteClanTagText while
	// useEliteClanTag is set (userinfo ec_usingTag / ec_TagText, 0x5575F0).
	constexpr DWORD profile_clan_prefix = 0x132B5A6;
	constexpr DWORD profile_use_elite_clan_tag = 0x132B5AB;
	constexpr DWORD profile_elite_clan_tag = 0x132B5CC;

	// ui_clanTag (archived): the tag set here, put back into the profile when something clears it (the elite clan
	// refresh, 0x49D3F0)
	game::native::dvar_t* clan_tag_dvar = nullptr;

	// 4 letters, as the PS3 keyboard allows; nothing that breaks the userinfo string
	std::string clan_tag_text(const std::string& text)
	{
		std::string tag;
		for (const auto c : text)
		{
			if (tag.size() >= 4) break;
			if (c >= 0x20 && c < 0x7F && c != '\\' && c != '"' && c != ';' && c != '%') tag += c;
		}
		return tag;
	}

	std::string profile_clan_tag()
	{
		const auto* field = reinterpret_cast<const char*>(profile_elite_clan_tag);
		return std::string(field, strnlen(field, 4));
	}

	void write_profile_clan_tag(const std::string& tag)
	{
		char field[5]{};
		std::memcpy(field, tag.data(), std::min<size_t>(tag.size(), 4));
		std::memcpy(reinterpret_cast<void*>(profile_elite_clan_tag), field, sizeof(field));
		std::memcpy(reinterpret_cast<void*>(profile_clan_prefix), field, sizeof(field));
		*reinterpret_cast<unsigned char*>(profile_use_elite_clan_tag) = static_cast<unsigned char>(tag.empty() ? 0 : 1);
	}

	void set_clan_tag(const std::string& text)
	{
		const auto tag = clan_tag_text(text);
		write_profile_clan_tag(tag);
		set_dvar("ui_clanTag", tag);
		cbuf_add_text("updateGamerProfile\n"); // saves the profile (0x48F800); the userinfo has it from the next connect
		if (active) log("clan tag \"%s\"\n", tag.data()); // (log makes the ps3style folder, which turns the style on)
	}

	void clan_tag_guard()
	{
		if (!clan_tag_dvar || !clan_tag_dvar->current.string || !*clan_tag_dvar->current.string) return;

		const auto tag = clan_tag_text(clan_tag_dvar->current.string);
		if (!tag.empty() && (profile_clan_tag() != tag || !*reinterpret_cast<unsigned char*>(profile_use_elite_clan_tag)))
		{
			write_profile_clan_tag(tag);
		}
	}

	// editclanname: the PC's keyboard popup instead (popup_demo_keyboard in ui_mp/code.txt, the theater's: title / text
	// dvars, OK runs "cl_demo_keyboardcomplete 0", cancel "... 1"); the answer comes to keyboard_complete_stub. The
	// title tells whose popup it was (the theater sets its own when it opens it, 0x6AEE85).
	bool clan_tag_editing = false;
	std::string clan_tag_title;

	void edit_clan_tag()
	{
		// SEH_LocalizeTextMessage (0x58F760): the text of a localized string (the key when there is none)
		clan_tag_title = reinterpret_cast<const char*(*)(const char*)>(0x58F760)("MENU_CLAN_TAG_CAPS");
		set_dvar("cl_demo_ui_keyboardtitle", clan_tag_title);
		set_dvar("cl_demo_ui_keyboard_edittext", profile_clan_tag());
		clan_tag_editing = open_menu("popup_demo_keyboard");
		if (!clan_tag_editing) log("editclanname: no popup_demo_keyboard\n");
	}

	// cl_demo_keyboardcomplete 0x6A3920 (argv 1: 0 = OK, 1 = cancel)
	__declspec(naked) void keyboard_complete_original()
	{
		__asm
		{
			push ecx
			mov eax, dword ptr ds:[1C978D0h]
			push 6A3926h
			retn
		}
	}

	void keyboard_complete_stub()
	{
		const auto* title = static_cast<game::native::dvar_t*>(ps3style_assets::find_dvar("cl_demo_ui_keyboardtitle"));
		if (!clan_tag_editing || !title || !title->current.string || clan_tag_title != title->current.string)
		{
			keyboard_complete_original();
			return;
		}

		clan_tag_editing = false;
		const auto index = *game::native::cmd_args;
		if (game::native::cmd_argc[index] != 2 || std::atoi(game::native::cmd_argv[index][1]) != 0) return;

		const auto* edit = static_cast<game::native::dvar_t*>(ps3style_assets::find_dvar("cl_demo_ui_keyboard_edittext"));
		set_clan_tag(edit && edit->current.string ? edit->current.string : "");
	}

	// Clan tags in the match, as on the PS3. In a match (CA_ACTIVE, 0x48EE50) CL_GetClientName 0x497F40 copies only the
	// snapshot clientState's name (0x498153, I_strncpyz at 0x4981A9): the killfeed, overhead and crosshair names, the
	// player lists. The scoreboard's name column (CG_DrawClientScore case 0, 0x4661AE) draws the clientinfo's name. The
	// PS3 puts the clan tag first in both (0xE3288, 0x92368). The PC's clientState has only the Elite tag
	// (useEliteClanTag +7Ch, eliteClanTagText +A0h; clientinfo +538h / +558h), from the userinfo's ec_usingTag /
	// ec_TagText (the player's own from the profile, 0x5575F0). Plain "[TAG]Name": the
	// row's, team's or killfeed's colour for all of it, as the PS3's (59.webp). No CRT formatting here: the scoreboard
	// calls it with two x87 values live
	void tagged_name(char* out, const size_t size, const char* name, const char* tag)
	{
		if (!out || !size) return;

		size_t at = 0;
		const auto put = [&](const char c)
		{
			if (at + 1 < size) out[at++] = c;
		};

		const auto length = tag ? strnlen(tag, 4) : 0;
		if (length)
		{
			put('[');
			for (size_t i = 0; i < length; ++i) put(tag[i]);
			put(']');
		}
		for (const auto* c = name; c && *c; ++c) put(*c);
		out[at] = 0;
	}

	// the E8 at 0x4981A9: I_strncpyz(out, clientState + 44h (name), size)
	void __cdecl client_name_copy(char* out, const char* cs_name, const int size)
	{
		const auto* cs = cs_name - 0x44;
		const auto* tag = *reinterpret_cast<const int*>(cs + 0x7C) ? cs + 0xA0 : nullptr;
		tagged_name(out, size > 0 ? static_cast<size_t>(size) : 0, cs_name, tag);
	}

	// the scoreboard row's text, the clientinfo's (cgs.clientinfo 0x9FE678 + n * 564h: name +0Ch, the Elite tag's
	// flag +538h and text +558h)
	const char* __cdecl scoreboard_name(const char* ci)
	{
		static char text[48];
		tagged_name(text, sizeof(text), ci + 0xC, *reinterpret_cast<const unsigned char*>(ci + 0x538) ? ci + 0x558 : nullptr);
		return text;
	}

	// 0x4661AE: mov edi, [esp + 28h] (clientinfo); mov eax, [ebp + 20h] (status icon); add edi, 0Ch -> edi = the text
	// 0x465980 draws (shrunk to fit the column). ecx / edx are loaded again before they are read
	__declspec(naked) void scoreboard_name_stub()
	{
		__asm
		{
			push dword ptr [esp + 28h]
			call scoreboard_name
			add esp, 4
			mov edi, eax
			mov eax, [ebp + 20h]
			push 4661B8h
			retn
		}
	}

	// the host quality check (0x5CB960, cdecl, a party dvar's value): post_load
	bool __cdecl good_host_stub(int)
	{
		return true;
	}

	// The console scoreboard's white box around the selected row. PS3 CG_DrawClientScore 0x91F28, tail 0x931AC-0x93304:
	// UI_DrawRect(margin + 3 + 2, y, cg_scoreboardWidth - 6 - 4, itemHeight, horz 1, vert 0, size 2, {1,1,1,color.a}).
	// The selection starts on the local player's row (ListBySide 0x93590). PC CG_DrawClientScore 0x465D60
	// (cdecl: lc, color, y, score, width; returns y + itemHeight) has no selection. Its only call is in ListBySide at 0x4666A2.
	constexpr DWORD draw_client_score_addr = 0x465D60;
	constexpr DWORD draw_client_score_call = 0x4666A2;

	float __cdecl draw_client_score_stub(const int lc, const float* color, const float y, const int* score, const float width)
	{
		const auto bottom = reinterpret_cast<float(__cdecl*)(int, const float*, float, const int*, float)>(draw_client_score_addr)(
			lc, color, y, score, width);
		// bottom > y: the row was really drawn (0x465D7D returns y when clientinfo is invalid)
		// score->client == cg.clientNum: the same test as cg_scoreboardMyColor at 0x465EA4
		// [0x58F2550]: sharedUiInfo whiteMaterial
		if (bottom > y && score && color && *score == *reinterpret_cast<int*>(0x9042E0) && *reinterpret_cast<void**>(0x58F2550))
		{
			const auto* place = static_cast<const float*>(reinterpret_cast<void*(__cdecl*)(int)>(0x4B7610)(lc));
			const auto sb_w = (*reinterpret_cast<game::native::dvar_t**>(0xB0DAF4))->current.value;                         // cg_scoreboardWidth
			const auto item_h = static_cast<float>((*reinterpret_cast<game::native::dvar_t**>(0xB0DAB8))->current.integer);  // cg_scoreboardItemHeight
			const auto margin = std::max(0.0f, (place[12] - place[10] - sb_w) * 0.5f);                                       // as 0x465250
			const float white[4]{1.0f, 1.0f, 1.0f, color[3]};
			reinterpret_cast<void(__cdecl*)(const void*, float, float, float, float, int, int, float, const float*)>(0x58B5E0) // UI_DrawRect
				(place, margin + 5.0f, y, sb_w - 10.0f, item_h, 1, 0, 2.0f, white);
		}
		return bottom;
	}

	// The spectator's button lines (hud_fullscreen ownerdraw 61: CG_DrawSpectatorMessage 0x45CF10, cdecl, its only call
	// in CG_OwnerDraw at 0x45FE0A). The PC draws only Next / Previous player / Stop following, one text height apart. The
	// console (PS3 0x81BD0) draws Menu (togglemenu), then while following Next / Previous (flag 0x2000) and Free (0x4000),
	// or Follow when free (0x2000), then Scores, stepping UI_TextHeight + rect.h. The follow lines' strings carry the
	// console's own buttons (circle Next, cross Previous, triangle Follow / Free, which the pad does while spectating:
	// gamepad.cpp); their commands are the PC's (0x45CF1F-0x45CF4F), for a "&&1". The PS3's first / third person line is
	// left out: the PC has nothing for it.
	void __cdecl spectator_message(const int lcn, const rectDef_s* rect, Font_s*, const float scale, const float* color,
	                               const int style, const int align)
	{
		// cg_descriptiveText; nothing while a menu has the keys (KEYCATCH_UI)
		const auto* descriptive = *reinterpret_cast<game::native::dvar_t**>(0xB0C734);
		if (!rect || !descriptive || !descriptive->current.enabled) return;
		if (reinterpret_cast<int(__cdecl*)(int, int)>(0x48C700)(lcn, 0x10)) return;

		// the playerState's flags (0x900FA0 + 0x10): 0x800 following, 0x2000 / 0x4000 what the PC's lines are drawn for
		const auto flags = *reinterpret_cast<const int*>(0x900FB0);
		if (!(flags & 0x6000)) return;

		const auto* scr = reinterpret_cast<const void*(__cdecl*)(int)>(0x4B7610)(lcn);
		auto* font = reinterpret_cast<Font_s*(__cdecl*)(const void*, int, float)>(0x58BD90)(scr, 0, scale); // UI_GetFontHandle
		if (!font) return;

		// the console's key where the PS3 data (strings.json) has it, else the PC's (nullptr: no line)
		const auto key = [](const char* console, const char* pc) { return strings.count(console) ? console : pc; };

		// the previous-player command: the first of these with a pad button (0x45CEB0 takes the first bound one)
		const char* ads = "+speed_throw";
		for (const auto* command : {"+toggleads_throw", "+speed_throw", "+speed", "toggleads"})
		{
			if (gamepad::bound_icon(command))
			{
				ads = command;
				break;
			}
		}

		std::pair<const char*, const char*> lines[5]{}; // command, string
		auto count = 0;
		lines[count++] = {"togglemenu", key("PLATFORM_MP_OPEN_MENU", nullptr)};
		if (flags & 0x800)
		{
			if (flags & 0x2000)
			{
				lines[count++] = {"+attack", "PLATFORM_FOLLOWNEXTPLAYER"};
				lines[count++] = {ads, key("PLATFORM_FOLLOWPREVPLAYER", "PLATFORM_FOLLOWPREVIOUSPLAYER")};
			}
			if (flags & 0x4000) lines[count++] = {"+melee", "PLATFORM_FOLLOWSTOP"};
		}
		else if (flags & 0x2000)
		{
			lines[count++] = {"+attack", key("PLATFORM_FOLLOWSTART", nullptr)};
		}
		lines[count++] = {"+scores", key("PLATFORM_SCOREBOARD", nullptr)};

		// UI_TextHeight + rect.h (18 + 5 = 23 units at 0.375, about the PS3's 51 px at 1080p)
		const auto step = static_cast<float>(reinterpret_cast<int(__cdecl*)(Font_s*, float)>(0x58B950)(font, scale)) + rect->h;
		static auto logged = false;
		if (!logged)
		{
			logged = true;
			log("spectator lines: rect h %.2f, step %.2f\n", rect->h, step);
		}

		const auto localize = reinterpret_cast<const char*(__cdecl*)(const char*)>(0x58F760); // SEH_LocalizeTextMessage
		auto y = rect->y;
		for (auto i = 0; i < count; ++i)
		{
			if (!lines[i].second) continue;

			// "&&1": the keys bound to the command (0x5A3BD0 -> 0x5A3AC0, key_assignment_stub), else KEY_UNBOUND
			char keys[256]{};
			if (!reinterpret_cast<int(__cdecl*)(int, const char*, char*, int)>(0x5A3BD0)(lcn, lines[i].first, keys, sizeof(keys)))
			{
				const auto* unbound = localize("KEY_UNBOUND");
				strncpy_s(keys, unbound ? unbound : "", _TRUNCATE);
			}

			const auto* text = reinterpret_cast<const char*(__cdecl*)(const char*, const char*)>(0x592BE0)(localize(lines[i].second), keys);
			rectDef_s text_rect{};
			reinterpret_cast<void(__cdecl*)(const void*, const char*, const rectDef_s*, Font_s*, float, float, float, const float*,
			                                int, int, rectDef_s*, int)>(0x5A3040) // UI_DrawWrappedText
				(scr, text, rect, font, rect->x, y, scale, color, style, align, &text_rect, 0);
			y += step;
		}
	}

	// Overhead names: CG_DrawOverheadNames (0x4471B0) sizes the name (cg_overheadNamesSize x the distance scale, the call
	// at 0x4473EE) and the rank (cg_overheadRankSize, the call at 0x44755E) with R_NormalizedTextScale (0x669A30: scale
	// x 48 / pixelHeight) in screen pixels; the PS3 (0x647B4-0x6482C) sizes them for a 1280x720 screen, so at 1080p they
	// were 2/3 of the console's. The rank icon (cg_overheadIconSize x the name's size), the centring and the rank's place
	// follow these two; only the 2-px gap between icon and name stays as it is
	float __cdecl overhead_text_scale(const Font_s* font, const float scale)
	{
		if (!font || font->pixelHeight <= 0) return scale;

		// ScrPlace_GetActivePlacement (0x4B7610, its argument unused): realViewportSize.y at +24h, the screen height
		const auto* place = reinterpret_cast<const float*(__cdecl*)(int)>(0x4B7610)(0);
		const auto k = place && place[9] > 0.0f ? place[9] / 720.0f : 1.0f;
		return scale * 48.0f / static_cast<float>(font->pixelHeight) * k;
	}

	// The killcam timer (kc_timer: "hudbig" 1.0, setTenthsTimer) at the console's size: the PS3's hud-element fonts'
	// base scale (0x74BE4) is 7/12 for hudbig / bigfixed where the PC's (0x4528E0, its switch on the element's font) is
	// 0.5, so its digits were 14% small (31 px at 1080p on the PS3). Only the tenths timers (types 8-10, which 0x452C40
	// sends to 0x4520E0's "%i:%02i:%02i.%i"): the +XP popups (hudbig 0.65) measure the PC's size on the English PS3. The
	// hudbig case at 0x45293E (fld [0x89C4E0]; mov edi, 9; jmp 0x452960), ebx = the hudelem_s (its type at +0); eax isn't
	// read before it is set again but is kept anyway
	float hud_big_tenths_timer_scale = 7.0f / 12.0f;

	__declspec(naked) void hud_big_scale_stub()
	{
		__asm
		{
			push eax
			mov eax, [ebx]
			sub eax, 8
			cmp eax, 2
			pop eax
			ja hud_big_default

			fld hud_big_tenths_timer_scale
			jmp hud_big_done

		hud_big_default:
			fld dword ptr ds:[89C4E0h]

		hud_big_done:
			mov edi, 9
			push 452960h
			retn
		}
	}

	// The news ticker (menu item type 20: the bar at the bottom of the PSN menu, the lobbies, the barracks, the
	// leaderboards). The PS3 takes its text from its news feed before the item's (0x275704: feed 1 is
	// news/mp_newsfeed1.news, "[MOTD]" first) and starts the animation (state 1 at 0x2757F0 / 0x275B1C / 0x26AE28); the
	// PC has no news feed, so the PS3 menus' tickers (no text of their own) stayed empty, and nothing on the PC ever
	// leaves state 0 (which draws nothing). Feed 1 is <game>\ps3style\ticker.txt here (one message per line, "[MOTD]" =
	// the motd dvar), shown as the PS3 does: a line slides up into the bar (state 1), stays 5 s (2), scrolls left when it
	// is wider than the bar (3, then 3 s: 4), slides up out of it (5), then the next line. The state is the PC paint's
	// (0x5A4610, its only call at 0x5AB519: eax = item, the dc pushed): one per local client at 0x5989828
	struct news_ticker_state
	{
		int last_time;
		int hold_until;
		float x;
		float y;
		int state;
	};

	std::vector<std::string> ticker_lines;
	size_t ticker_line = 0;
	// a line with "[MOTD]" replaced (the item's text while it is shown): a fixed buffer, as every ticker item keeps the
	// pointer until its next paint
	char ticker_text[0x400]{};

	void load_ticker(const std::string& path)
	{
		std::string data;
		if (!utils::io::read_file(path, &data)) return;
		if (data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);

		std::istringstream lines(data);
		std::string line;
		while (std::getline(lines, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.find_first_not_of(" \t") == std::string::npos) continue;

			// the PS3's feed buffer is 200h bytes
			auto text = latin1(line.data());
			if (text.size() > 0x1FF) text.resize(0x1FF);
			ticker_lines.emplace_back(std::move(text));
		}
	}

	// the PC's ticker paint (0x5A4610: eax = item, the dc on the stack)
	__declspec(naked) void news_ticker_original(itemDef_s* /*item*/, const int* /*dc*/)
	{
		__asm
		{
			mov eax, [esp + 4]
			push dword ptr [esp + 8]
			mov edx, 5A4610h
			call edx
			add esp, 4
			retn
		}
	}

	// as the PS3 (re)starts a line: lastTime now, x 0, y below the bar, sliding in
	void news_ticker_start(news_ticker_state* st, const itemDef_s* item)
	{
		st->last_time = reinterpret_cast<int(*)()>(0x5CE740)(); // Sys_Milliseconds
		st->x = 0.0f;
		st->y = item->window.rect.h;
		st->state = 1;
	}

	void __cdecl news_ticker(const int* dc, itemDef_s* item)
	{
		// one local client (the next variable is at 0x598983C)
		if (!dc || *dc != 0 || !item)
		{
			news_ticker_original(item, dc);
			return;
		}

		auto* st = reinterpret_cast<news_ticker_state*>(0x5989828);
		const auto feed = item->type == ITEM_TYPE_NEWS_TICKER && item->typeData.ticker && item->typeData.ticker->feedId == 1
			&& !ticker_lines.empty();
		if (feed)
		{
			const auto& line = ticker_lines[ticker_line % ticker_lines.size()];
			if (line.find("[MOTD]") == std::string::npos)
			{
				item->text = line.data();
			}
			else
			{
				const auto* motd = static_cast<const game::native::dvar_t*>(ps3style_assets::find_dvar("motd"));
				const std::string value = motd && motd->current.string ? motd->current.string : "";
				auto text = line;
				for (auto at = text.find("[MOTD]"); at != std::string::npos; at = text.find("[MOTD]", at + value.size()))
				{
					text.replace(at, 6, value);
				}
				strncpy_s(ticker_text, text.data(), _TRUNCATE);
				item->text = ticker_text;
			}
		}

		// the paint draws the text up to its first line break ('@': localized), nothing when that is empty
		const auto* text = item->text;
		if (text && *text == '@') text = reinterpret_cast<const char*(__cdecl*)(const char*)>(0x58F760)(text + 1); // SEH_LocalizeTextMessage
		const auto has_text = text && *text && *text != '\n' && *text != '\r';

		// never started (state 0), or a menu was opened (0x59D0DE: lastTime, x 0, y = rect.h, the state as it was): the
		// line slides in again, as the PS3's menu open (0x26AE28) does. y is rect.h exactly only after such a reset (a
		// copy of it); states 2-5 are at 0 or above the bar
		if (has_text && (st->state == 0 || (st->state >= 2 && st->y == item->window.rect.h))) news_ticker_start(st, item);

		const auto state = st->state;
		news_ticker_original(item, dc);

		// the line slid out in this paint (state 5 ends with the reset 0x5A45C0 at y = rect.h, the state left at 5): the
		// next line, sliding in
		if (has_text && state == 5 && st->state == 5 && st->y == item->window.rect.h)
		{
			if (feed) ticker_line = (ticker_line + 1) % ticker_lines.size();
			news_ticker_start(st, item);
		}
	}

	// Item_Paint's call of the ticker paint (0x5AB519: push ebx (dc); mov eax, esi (item); call 0x5A4610; its caller
	// pops the dc and doesn't read eax)
	__declspec(naked) void news_ticker_stub()
	{
		__asm
		{
			push eax
			push dword ptr [esp + 8]
			call news_ticker
			add esp, 8
			retn
		}
	}

	// the PS3 menus are built once, but the zones they point into are not: ui_mp / localized_ui_mp are freed when a
	// level starts and loaded again after it, a level's zones go with it. When the game loads its menus again (UI_Init
	// 0x592630 asks for ui_mp/code.txt, a level for ui_mp/ingame.txt, 0x58C780) the menus' zone assets are looked up
	// again, and once more when the zone loads have finished (Sys_IsDatabaseReady 0x55FA20: the event DB_LoadXAssets
	// resets), as localized_ui_mp may still be loading then. Kept pointers drew a checkered box for the lobby's NEW
	// badges and a stretched picture for the MOTD ticker bar.
	std::atomic<bool> menu_assets_stale{false};

	bool database_ready()
	{
		return reinterpret_cast<int(*)()>(0x55FA20)() != 0;
	}

	void refresh_menu_assets()
	{
		static auto was_ready = true;
		const auto ready = database_ready();
		if (ready && (!was_ready || menu_assets_stale))
		{
			ps3style_menus::refresh();
			menu_assets_stale = false;
		}
		was_ready = ready;
	}

	__declspec(naked) void* db_find_x_asset_header_original(int, const char*, int)
	{
		__asm
		{
			// the 8 bytes the hook replaced, then the rest of DB_FindXAssetHeader
			push ecx
			push ebx
			mov ebx, dword ptr ds:[7E01F8h]
			push 4CA628h
			retn
		}
	}

	game::native::dvar_t* register_bool(const char* name, const bool value, const int flags, const char* description)
	{
		return reinterpret_cast<game::native::dvar_t*(*)(const char*, bool, int, const char*)>(dvar_register_bool_addr)(
			name, value, flags, description);
	}

	game::native::dvar_t* register_int(const char* name, const int value, const int min, const int max, const int flags,
	                                   const char* description)
	{
		return reinterpret_cast<game::native::dvar_t*(*)(const char*, int, int, int, int, const char*)>(dvar_register_int_addr)(
			name, value, min, max, flags, description);
	}

	game::native::dvar_t* register_string(const char* name, const char* value, const int flags, const char* description)
	{
		return reinterpret_cast<game::native::dvar_t*(*)(const char*, const char*, int, const char*)>(dvar_register_string_addr)(
			name, value, flags, description);
	}

	// ---- PC menus as JSON, in the layout of the MW3 PS3 dump's reader (tools\dump_ps3_menus.py) ----
	using json_writer = rapidjson::Writer<rapidjson::StringBuffer>;

	void write_string(json_writer& w, const char* s)
	{
		if (s) w.String(s);
		else w.Null();
	}

	void write_floats(json_writer& w, const float* v, const int count)
	{
		w.StartArray();
		for (auto i = 0; i < count; ++i) w.Double(v[i]);
		w.EndArray();
	}

	void write_rect(json_writer& w, const rectDef_s& r)
	{
		w.StartArray();
		w.Double(r.x);
		w.Double(r.y);
		w.Double(r.w);
		w.Double(r.h);
		w.Int(r.horzAlign);
		w.Int(r.vertAlign);
		w.EndArray();
	}

	void write_statement(json_writer& w, const Statement_s* st, const int depth = 0)
	{
		if (!st || depth > 16)
		{
			w.Null();
			return;
		}

		w.StartObject();
		w.Key("entries");
		w.StartArray();
		for (auto i = 0; i < st->numEntries; ++i)
		{
			const auto& e = st->entries[i];
			w.StartArray();
			if (e.type == 0)
			{
				w.String("op");
				w.Int(e.data.op);
			}
			else
			{
				switch (e.data.operand.dataType)
				{
				case VAL_INT:
					w.String("int");
					w.Int(e.data.operand.internals.intVal);
					break;
				case VAL_FLOAT:
					w.String("float");
					w.Double(e.data.operand.internals.floatVal);
					break;
				case VAL_STRING:
					w.String("str");
					write_string(w, e.data.operand.internals.stringVal.string);
					break;
				case VAL_FUNCTION:
					w.String("func");
					write_statement(w, e.data.operand.internals.function, depth + 1);
					break;
				default:
					w.String("?");
					w.Int(e.data.operand.dataType);
					w.Int(e.data.operand.internals.intVal);
					break;
				}
			}
			w.EndArray();
		}
		w.EndArray();
		w.Key("sd");
		w.Uint(reinterpret_cast<unsigned int>(st->supportingData));
		w.EndObject();
	}

	void write_handler_set(json_writer& w, const MenuEventHandlerSet* set, const int depth = 0)
	{
		w.StartArray();
		if (set && depth < 12)
		{
			for (auto i = 0; i < set->eventHandlerCount; ++i)
			{
				const auto* h = set->eventHandlers[i];
				if (!h) continue;

				w.StartArray();
				switch (h->eventType)
				{
				case EVENT_UNCONDITIONAL:
					w.String("script");
					write_string(w, h->eventData.unconditionalScript);
					break;
				case EVENT_IF:
					w.String("if");
					write_statement(w, h->eventData.conditionalScript->eventExpression);
					write_handler_set(w, h->eventData.conditionalScript->eventHandlerSet, depth + 1);
					break;
				case EVENT_ELSE:
					w.String("else");
					write_handler_set(w, h->eventData.elseScript, depth + 1);
					break;
				default:
					w.String(utils::string::va("setvar%d", h->eventType));
					write_string(w, h->eventData.setLocalVarData->localVarName);
					write_statement(w, h->eventData.setLocalVarData->expression);
					break;
				}
				w.EndArray();
			}
		}
		w.EndArray();
	}

	void write_key_handlers(json_writer& w, const ItemKeyHandler* k)
	{
		w.StartArray();
		for (auto n = 0; k && n < 64; k = k->next, ++n)
		{
			w.StartArray();
			w.Int(k->key);
			write_handler_set(w, k->action);
			w.EndArray();
		}
		w.EndArray();
	}

	const char* material_name(const Material* m)
	{
		return m ? m->info.name : nullptr;
	}

	void write_window(json_writer& w, const windowDef_t& win)
	{
		w.Key("name");
		write_string(w, win.name);
		w.Key("rect");
		write_rect(w, win.rect);
		w.Key("rectClient");
		write_rect(w, win.rectClient);
		w.Key("group");
		write_string(w, win.group);
		w.Key("style");
		w.Int(win.style);
		w.Key("border");
		w.Int(win.border);
		w.Key("ownerDraw");
		w.Int(win.ownerDraw);
		w.Key("ownerDrawFlags");
		w.Int(win.ownerDrawFlags);
		w.Key("borderSize");
		w.Double(win.borderSize);
		w.Key("staticFlags");
		w.Uint(static_cast<unsigned int>(win.staticFlags));
		w.Key("dynamicFlags");
		w.Uint(static_cast<unsigned int>(win.dynamicFlags[0]));
		w.Key("foreColor");
		write_floats(w, win.foreColor, 4);
		w.Key("backColor");
		write_floats(w, win.backColor, 4);
		w.Key("borderColor");
		write_floats(w, win.borderColor, 4);
		w.Key("outlineColor");
		write_floats(w, win.outlineColor, 4);
		w.Key("disableColor");
		write_floats(w, win.disableColor, 4);
		w.Key("background");
		write_string(w, material_name(win.background));
	}

	void write_type_data(json_writer& w, const itemDef_s* item)
	{
		const auto& t = item->typeData;
		if (!t.data)
		{
			w.Null();
			return;
		}

		w.StartObject();
		switch (item->type)
		{
		case ITEM_TYPE_LISTBOX:
			w.Key("kind");
			w.String("listBox");
			w.Key("drawPadding");
			w.Int(t.listBox->drawPadding);
			w.Key("elementWidth");
			w.Double(t.listBox->elementWidth);
			w.Key("elementHeight");
			w.Double(t.listBox->elementHeight);
			w.Key("elementStyle");
			w.Int(t.listBox->elementStyle);
			w.Key("columns");
			w.StartArray();
			for (auto i = 0; i < t.listBox->numColumns && i < 16; ++i)
			{
				const auto& c = t.listBox->columnInfo[i];
				w.StartArray();
				for (const auto v : {c.xpos, c.ypos, c.width, c.height, c.maxChars, c.alignment}) w.Int(v);
				w.EndArray();
			}
			w.EndArray();
			w.Key("onDoubleClick");
			write_handler_set(w, t.listBox->onDoubleClick);
			w.Key("notselectable");
			w.Int(t.listBox->notselectable);
			w.Key("noScrollBars");
			w.Int(t.listBox->noScrollBars);
			w.Key("usePaging");
			w.Int(t.listBox->usePaging);
			w.Key("selectBorder");
			write_floats(w, t.listBox->selectBorder, 4);
			w.Key("selectIcon");
			write_string(w, material_name(t.listBox->selectIcon));
			w.Key("elementHeightExp");
			write_statement(w, t.listBox->elementHeightExp);
			break;
		case ITEM_TYPE_MULTI:
			w.Key("kind");
			w.String("multi");
			w.Key("strDef");
			w.Int(t.multi->strDef);
			w.Key("names");
			w.StartArray();
			for (auto i = 0; i < t.multi->count && i < 32; ++i) write_string(w, t.multi->dvarList[i]);
			w.EndArray();
			w.Key("values");
			w.StartArray();
			for (auto i = 0; i < t.multi->count && i < 32; ++i)
			{
				if (t.multi->strDef) write_string(w, t.multi->dvarStr[i]);
				else w.Double(t.multi->dvarValue[i]);
			}
			w.EndArray();
			break;
		case ITEM_TYPE_DVARENUM:
			w.Key("kind");
			w.String("enumDvar");
			w.Key("dvar");
			write_string(w, t.enumDvarName);
			break;
		case ITEM_TYPE_NEWS_TICKER:
			w.Key("kind");
			w.String("newsTicker");
			w.Key("feedId");
			w.Int(t.ticker->feedId);
			w.Key("speed");
			w.Int(t.ticker->speed);
			w.Key("spacing");
			w.Int(t.ticker->spacing);
			break;
		case ITEM_TYPE_TEXT_SCROLL:
			w.Key("kind");
			w.String("textScroll");
			break;
		default:
		{
			// raw words: the edit field layout is checked from these
			w.Key("kind");
			w.String("editField");
			w.Key("raw");
			w.StartArray();
			const auto* words = reinterpret_cast<const unsigned int*>(t.editField);
			for (auto i = 0; i < 9; ++i) w.Uint(words[i]);
			w.EndArray();
			break;
		}
		}
		w.EndObject();
	}

	bool has_type_data(const int type)
	{
		switch (type)
		{
		case ITEM_TYPE_LISTBOX:
		case ITEM_TYPE_MULTI:
		case ITEM_TYPE_DVARENUM:
		case ITEM_TYPE_NEWS_TICKER:
		case ITEM_TYPE_TEXT_SCROLL:
		case ITEM_TYPE_EDITFIELD:
		case ITEM_TYPE_NUMERICFIELD:
		case ITEM_TYPE_SLIDER:
		case ITEM_TYPE_YESNO:
		case ITEM_TYPE_BIND:
		case ITEM_TYPE_VALIDFILEFIELD:
		case ITEM_TYPE_DECIMALFIELD:
		case ITEM_TYPE_UPREDITFIELD:
		case ITEM_TYPE_EMAILFIELD:
		case ITEM_TYPE_PASSWORDFIELD:
			return true;
		default:
			return false;
		}
	}

	void write_item(json_writer& w, const itemDef_s* item)
	{
		if (!item)
		{
			w.Null();
			return;
		}

		w.StartObject();
		write_window(w, item->window);
		w.Key("textRect");
		write_rect(w, item->textRect[0]);
		w.Key("type");
		w.Int(item->type);
		w.Key("dataType");
		w.Int(item->dataType);
		w.Key("alignment");
		w.Int(item->alignment);
		w.Key("fontEnum");
		w.Int(item->fontEnum);
		w.Key("textAlignMode");
		w.Int(item->textAlignMode);
		w.Key("textalignx");
		w.Double(item->textalignx);
		w.Key("textaligny");
		w.Double(item->textaligny);
		w.Key("textscale");
		w.Double(item->textscale);
		w.Key("textStyle");
		w.Int(item->textStyle);
		w.Key("gameMsgWindowIndex");
		w.Int(item->gameMsgWindowIndex);
		w.Key("gameMsgWindowMode");
		w.Int(item->gameMsgWindowMode);
		w.Key("text");
		write_string(w, item->text);
		w.Key("itemFlags");
		w.Int(item->itemFlags);
		w.Key("parentOk");
		w.Bool(item->parent != nullptr);

		const std::pair<const char*, MenuEventHandlerSet*> sets[] = {
			{"mouseEnterText", item->mouseEnterText}, {"mouseExitText", item->mouseExitText},
			{"mouseEnter", item->mouseEnter}, {"mouseExit", item->mouseExit}, {"action", item->action},
			{"accept", item->accept}, {"onFocus", item->onFocus}, {"hasFocus", item->hasFocus},
			{"leaveFocus", item->leaveFocus},
		};
		for (const auto& [key, set] : sets)
		{
			w.Key(key);
			write_handler_set(w, set);
		}

		w.Key("dvar");
		write_string(w, item->dvar);
		w.Key("dvarTest");
		write_string(w, item->dvarTest);
		w.Key("onKey");
		write_key_handlers(w, item->onKey);
		w.Key("enableDvar");
		write_string(w, item->enableDvar);
		w.Key("localVar");
		write_string(w, item->localVar);
		w.Key("dvarFlags");
		w.Int(item->dvarFlags);
		w.Key("focusSound");
		write_string(w, item->focusSound ? *static_cast<const char**>(item->focusSound) : nullptr);
		w.Key("special");
		w.Double(item->special);
		w.Key("typeData");
		if (has_type_data(item->type)) write_type_data(w, item);
		else w.Null();
		w.Key("floatExpressions");
		w.StartArray();
		for (auto i = 0; i < item->floatExpressionCount; ++i)
		{
			w.StartArray();
			w.Int(item->floatExpressions[i].target);
			write_statement(w, item->floatExpressions[i].expression);
			w.EndArray();
		}
		w.EndArray();

		const std::pair<const char*, Statement_s*> exps[] = {
			{"visibleExp", item->visibleExp}, {"disabledExp", item->disabledExp}, {"textExp", item->textExp},
			{"materialExp", item->materialExp}, {"textAlignYExp", item->textAlignYExp},
		};
		for (const auto& [key, exp] : exps)
		{
			w.Key(key);
			write_statement(w, exp);
		}

		w.Key("glowColor");
		write_floats(w, item->glowColor, 4);
		w.EndObject();
	}

	void write_menu(json_writer& w, const menuDef_t* menu)
	{
		w.StartObject();
		write_window(w, menu->window);
		w.Key("itemCount");
		w.Int(menu->itemCount);

		if (const auto* d = menu->data)
		{
			w.Key("fullScreen");
			w.Int(d->fullScreen);
			w.Key("fadeCycle");
			w.Int(d->fadeCycle);
			w.Key("fadeClamp");
			w.Double(d->fadeClamp);
			w.Key("fadeAmount");
			w.Double(d->fadeAmount);
			w.Key("fadeInAmount");
			w.Double(d->fadeInAmount);
			w.Key("blurRadius");
			w.Double(d->blurRadius);

			const std::pair<const char*, MenuEventHandlerSet*> sets[] = {
				{"onOpen", d->onOpen}, {"onCloseRequest", d->onCloseRequest}, {"onClose", d->onClose},
				{"onESC", d->onESC}, {"onFocusDueToClose", d->onFocusDueToClose},
			};
			for (const auto& [key, set] : sets)
			{
				w.Key(key);
				write_handler_set(w, set);
			}

			w.Key("onKey");
			write_key_handlers(w, d->onKey);
			w.Key("visibleExp");
			write_statement(w, d->visibleExp);
			w.Key("allowedBinding");
			write_string(w, d->allowedBinding);
			w.Key("soundName");
			write_string(w, d->soundName);
			w.Key("focusColor");
			write_floats(w, d->focusColor, 4);

			const std::pair<const char*, Statement_s*> exps[] = {
				{"rectXExp", d->rectXExp}, {"rectYExp", d->rectYExp}, {"rectWExp", d->rectWExp},
				{"rectHExp", d->rectHExp}, {"openSoundExp", d->openSoundExp}, {"closeSoundExp", d->closeSoundExp},
				{"soundLoopExp", d->soundLoopExp},
			};
			for (const auto& [key, exp] : exps)
			{
				w.Key(key);
				write_statement(w, exp);
			}

			w.Key("priority");
			w.Int(d->priority);

			w.Key("staticDvars");
			w.StartArray();
			if (const auto* esd = d->expressionData)
			{
				for (auto i = 0; i < esd->numStaticDvars; ++i)
				{
					write_string(w, esd->staticDvars[i] ? esd->staticDvars[i]->dvarName : nullptr);
				}
			}
			w.EndArray();

			w.Key("uiStrings");
			w.StartArray();
			if (const auto* esd = d->expressionData)
			{
				for (auto i = 0; i < esd->totalStrings; ++i) write_string(w, esd->strings[i]);
			}
			w.EndArray();

			w.Key("uiFunctions");
			w.Int(d->expressionData ? d->expressionData->totalFunctions : 0);
		}

		w.Key("items");
		w.StartArray();
		for (auto i = 0; i < menu->itemCount; ++i) write_item(w, menu->items[i]);
		w.EndArray();
		w.EndObject();
	}

	void write_menu_text(const menuDef_t* menu, std::string* out)
	{
		rapidjson::StringBuffer buffer;
		json_writer w(buffer);
		write_menu(w, menu);
		*out = buffer.GetString();
	}

	// a bad pointer (wrong layout guess) skips the menu instead of ending the game
	bool try_write_menu_text(const menuDef_t* menu, std::string* out)
	{
		__try
		{
			write_menu_text(menu, out);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// ps3style_dumpPCMenus: every menu list / menu the game has asked for so far -> ps3style\pc_menus.json
	void dump_pc_menus()
	{
		std::set<std::string> lists, menus;
		{
			std::lock_guard _(names_mutex);
			lists = menu_list_names;
			menus = menu_names;
		}

		rapidjson::StringBuffer buffer;
		json_writer w(buffer);
		w.StartObject();

		std::set<std::string> written;
		log("dump: %u lists, %u menus\n", lists.size(), menus.size());
		for (const auto& name : lists)
		{
			const auto* list = static_cast<MenuList*>(ps3style::find_asset(ASSET_TYPE_MENULIST, name.data(), 0));
			log("list %s %p%s", name.data(), list, list ? hex_dump(list, 12).data() : "\n");
			if (list && list->menuCount > 0)
			{
				const auto* menu = list->menus[0];
				log("menu[0] %p%s", menu, hex_dump(menu, sizeof(menuDef_t)).data());
				if (menu && menu->data) log("data%s", hex_dump(menu->data, 0xD4).data());
				if (menu && menu->itemCount > 0) log("item[0]%s", hex_dump(menu->items[0], sizeof(itemDef_s)).data());
			}
		}

		w.Key("lists");
		w.StartObject();
		for (const auto& name : lists)
		{
			const auto* list = static_cast<MenuList*>(ps3style::find_asset(ASSET_TYPE_MENULIST, name.data(), 0));
			if (!list) continue;

			w.Key(name.data());
			w.StartArray();
			for (auto i = 0; i < list->menuCount; ++i)
			{
				const auto* menu = list->menus[i];
				write_string(w, menu ? menu->window.name : nullptr);
				if (menu && menu->window.name) menus.emplace(menu->window.name);
			}
			w.EndArray();
		}
		w.EndObject();

		w.Key("menus");
		w.StartObject();
		for (const auto& name : menus)
		{
			if (written.count(name)) continue;
			log("menu %s\n", name.data());
			const auto* menu = static_cast<menuDef_t*>(ps3style::find_asset(ASSET_TYPE_MENU, name.data(), 0));
			if (!menu) continue;

			std::string text;
			if (!try_write_menu_text(menu, &text))
			{
				log("  FAULT while reading menu %s\n", name.data());
				continue;
			}

			written.emplace(name);
			w.Key(name.data());
			w.RawValue(text.data(), text.size(), rapidjson::kObjectType);
		}
		w.EndObject();
		w.EndObject();

		const auto path = ps3style::folder() + "\\pc_menus.json";
		utils::io::write_file(path, buffer.GetString());
		printf("PS3 style: %u menu lists, %u menus -> %s\n", lists.size(), written.size(), path.data());
	}

	// CL_Vid_Restart_f (0x490E00, no arguments), after the PS3 assets of the old renderer are dropped
	void vid_restart_stub()
	{
		log("vid_restart: PS3 images, materials and fonts made again\n");
		ps3style_assets::reset();
		reinterpret_cast<void(*)()>(0x490E00)();
	}
}

std::string ps3style::folder()
{
	static const auto path = []
	{
		char exe[MAX_PATH]{};
		GetModuleFileNameA(nullptr, exe, sizeof(exe));
		return std::filesystem::path(exe).parent_path().string() + "\\ps3style";
	}();
	return path;
}

bool ps3style::enabled()
{
	// decided at start (post_load): the ps3style folder is there, ui_ps3Style isn't 0 in the config, and no -nops3style
	return active;
}

void* ps3style::find_asset(const int type, const char* name, const int allow_create_default)
{
	return db_find_x_asset_header_original(type, name, allow_create_default);
}

void* ps3style::db_find_x_asset_header_stub(const int type, const char* name, const int allow_create_default)
{
	if (name && (type == ASSET_TYPE_MENULIST || type == ASSET_TYPE_MENU))
	{
		std::lock_guard _(names_mutex);
		(type == ASSET_TYPE_MENULIST ? menu_list_names : menu_names).emplace(name);
	}

	if (log_assets && name)
	{
		log("asset %d %s\n", type, name);
	}

	// scripts compiled from <game>\scripts / <game>\mods (gsc module)
	if (name && type == 39) // ASSET_TYPE_SCRIPTFILE
	{
		if (auto* script = gsc::find_script(name)) return script;
	}

	if (name && active && type == ASSET_TYPE_MATERIAL && ps3style_assets::has_image(name))
	{
		if (auto* material = ps3style_assets::material(name)) return material;
	}

	if (name && active && type == ASSET_TYPE_RAWFILE && !rawfiles.empty())
	{
		const auto itr = rawfiles.find(utils::string::to_lower(name));
		if (itr != rawfiles.end()) return itr->second;
	}

	if (name && active && type == ASSET_TYPE_LOCALIZE_ENTRY)
	{
		const auto itr = strings.find(name);
		if (itr != strings.end()) return itr->second;
	}

	if (name && active && (ps3_fonts || english_fonts) && type == ASSET_TYPE_FONT)
	{
		if (auto* font = ps3style_assets::font(name)) return font;
	}

	if (name && active && ps3style_menus::loaded())
	{
		if (type == ASSET_TYPE_MENU)
		{
			if (auto* menu = ps3style_menus::find_menu(name)) return menu;
		}
		else if (type == ASSET_TYPE_MENULIST)
		{
			auto* pc_list = static_cast<MenuList*>(db_find_x_asset_header_original(type, name, allow_create_default));
			if (!_stricmp(name, "ui_mp/code.txt") || !_stricmp(name, "ui_mp/ingame.txt"))
			{
				// the game loads its menus again: the zones changed (refresh_menu_assets)
				ps3style_menus::refresh();
				menu_assets_stale = true;
			}
			if (auto* list = ps3style_menus::find_list(name, pc_list)) return list;
			return pc_list;
		}
	}

	return db_find_x_asset_header_original(type, name, allow_create_default);
}

void ps3style::post_load()
{
	if (!game::is_mp()) return;

	utils::hook(db_find_x_asset_header_addr, db_find_x_asset_header_stub, HOOK_JUMP).install()->quick();

	if (utils::io::directory_exists(folder()))
	{
		find_own_code();
		AddVectoredExceptionHandler(1, log_exception);
		utils::hook(0x588990, text_eval_stub, HOOK_JUMP).install()->quick();
	}

	active = utils::io::directory_exists(folder()) && config_value("ui_ps3Style") != "0" && !utils::flags::has_flag("nops3style");
	const auto fonts = config_value("ui_ps3StyleFonts");
	ps3_fonts = active && fonts == "1";
	english_fonts = active && fonts == "2";
	if (active)
	{
		// the Xbox 360 look (ui_xboxButtons 1 or -xboxbuttons): its button icons, its wording (strings_xbox.json over
		// strings.json, tools\make_strings.py) and its controller pictures in the controls menus (menus_xbox)
		const auto xbox = config_value("ui_xboxButtons") == "1" || utils::flags::has_flag("xboxbuttons");
		ps3style_assets::init(folder(), xbox, english_fonts ? "_en" : "");
		load_strings(folder() + (ps3_fonts ? "\\strings_ps3font.json" : "\\strings.json"));
		if (xbox) load_strings(folder() + "\\strings_xbox.json");
		load_bindings(folder());
		load_rawfiles(folder() + "\\rawfiles");
		load_ticker(folder() + "\\ticker.txt");
		utils::hook(0x5A75A0, binding_text_stub, HOOK_JUMP).install()->quick();

		// vid_restart (CL_Vid_Restart_f 0x490E00: the command, and its calls from 0x491030 / 0x491110 after a video
		// setting changed) makes a new D3D device and loads the zones again: the PS3 images, materials and fonts made for
		// the old ones must go first, else the menus draw with dead textures / techniques ("Tried to use '(null)' when
		// it isn't valid")
		utils::hook::set<DWORD>(0x492D98, reinterpret_cast<DWORD>(&vid_restart_stub));
		utils::hook(0x4910C3, vid_restart_stub, HOOK_CALL).install()->quick();
		utils::hook(0x4911DF, vid_restart_stub, HOOK_CALL).install()->quick();
		utils::hook(0x669A50, r_text_width_stub, HOOK_JUMP).install()->quick();
		utils::hook(0x5A3AC0, key_assignment_stub, HOOK_JUMP).install()->quick();

		// the L3 / R3 icons pressed for a fifth of every second, as on the PS3 (animate_sticks): the KeyBinding
		// expression's call of the empty 0x71D130 (not that function: other empty calls share it, gsc.cpp hooks one) and
		// UI_ReplaceConversions (0x58F810: push ecx; push esi; mov esi, [esp + 0Ch])
		utils::hook(0x58182A, keybinding_animate, HOOK_CALL).install()->quick();
		utils::hook(0x58F810, ui_replace_conversions_stub, HOOK_JUMP).install()->quick();
		ps3style_menus::set_xbox(xbox);
		const auto ok = ps3style_menus::load(folder() + "\\menus.json");
		log("PS3 style on%s%s, menus %s, news ticker %u lines\n", xbox ? " (Xbox buttons)" : "",
		    ps3_fonts ? " (Japanese PS3 fonts)" : english_fonts ? " (English PS3 fonts)" : "", ok ? "loaded" : "not loaded",
		    ticker_lines.size());

		// PS3 button icons: the PS3's size in every font (ui_ps3ButtonStyle: the English or the Japanese PS3's; the Xbox
		// icons the English rule), over the text's drop shadow (0x694CF0: sub esp, 114h) and their own colours
		// (0x694DC1: mov ecx, [esp + 130h]); with the PS3 fonts the button letters' colours (0x6959EC: mov eax,
		// [esp + 4Ch]; mov [esp + 10h], ecx)
		utils::hook(0x694CF0, inline_image_stub, HOOK_JUMP).install()->quick();
		utils::hook(0x694DC1, inline_image_color_stub, HOOK_JUMP).install()->quick();
		utils::hook::nop(0x694DC6, 2);
		if (ps3_fonts)
		{
			utils::hook(0x6959EC, glyph_color_stub, HOOK_JUMP).install()->quick();
			utils::hook::nop(0x6959F1, 3);
		}

		// cg_drawBreathHint registered without DVAR_ARCHIVE, as on the PS3 (breath_hint_guard)
		utils::hook::set<BYTE>(0x45523E, 0);

		// editclanname's answer from the keyboard popup (keyboard_complete_stub)
		utils::hook(0x6A3920, keyboard_complete_stub, HOOK_JUMP).install()->quick();

		// borderless fullscreen by default (the PS3 menus have no video options): the default of r_displayMode becomes
		// "windowed (no border)", the game's own borderless mode (R_SetWndParms 0x66E8B0: the desktop resolution at
		// 0,0); a saved r_displayMode still wins, and "r_displayMode fullscreen; vid_restart" gives exclusive
		// fullscreen. R_RegisterDvars 0x667973: mov ecx, [0x5FC6104] (default index) -> mov ecx, 1
		utils::hook::set<BYTE>(0x667973, 0xB9);
		utils::hook::set<DWORD>(0x667974, 1);
		utils::hook::nop(0x667978, 1);

		// Find Game (the playlists from <game>\ps3style, party_minplayers 1): the lobby host must pass the host quality
		// check 0x5CB960 (the bandwidth test result 0x5A96F70, upload speed, NAT not strict), which needs DemonWare's
		// bandwidth test that the emulator doesn't have, so the lobby sat at "Waiting for better host" (party flag 0x100,
		// 0x4ABBF7) forever. Offline every player is a good host
		utils::hook(0x5CB960, good_host_stub, HOOK_JUMP).install()->quick();

		// the white box around the local player's row on the scoreboard, as the console's selected row
		// (draw_client_score_stub: the E8 at 0x4666A2)
		utils::hook(draw_client_score_call, draw_client_score_stub, HOOK_CALL).install()->quick();

		// clan tags in the match as on the PS3, "[TAG]Name" (client_name_copy: the E8 at 0x4981A9; scoreboard_name_stub:
		// the scoreboard's name column 0x4661AE)
		utils::hook(0x4981A9, client_name_copy, HOOK_CALL).install()->quick();
		utils::hook(0x4661AE, scoreboard_name_stub, HOOK_JUMP).install()->quick();

		// the console's spectator lines: Menu / Next, Previous, Free or Follow / Scores (spectator_message replaces
		// 0x45CF10: sub esp, 174h)
		utils::hook(0x45CF10, spectator_message, HOOK_JUMP).install()->quick();

		// the default font picked as on the PS3 (default_font: UI_GetFontHandle's default branch 0x58BDE9), hud elements
		// asking with the PS3's size (hud_elem_font: the E8 at 0x4529AC)
		utils::hook(0x58BDE9, default_font, HOOK_JUMP).install()->quick();
		utils::hook(0x4529AC, hud_elem_font, HOOK_CALL).install()->quick();

		// overhead names, rank numbers and icons at the console's size (overhead_text_scale: the E8s at 0x4473EE and
		// 0x44755E)
		utils::hook(0x4473EE, overhead_text_scale, HOOK_CALL).install()->quick();
		utils::hook(0x44755E, overhead_text_scale, HOOK_CALL).install()->quick();

		// the killcam timer at the console's size (hud_big_scale_stub: the hudbig case at 0x45293E)
		utils::hook(0x45293E, hud_big_scale_stub, HOOK_JUMP).install()->quick();

		// the news tickers show ps3style\ticker.txt (news_ticker_stub: the E8 at 0x5AB519)
		utils::hook(0x5AB519, news_ticker_stub, HOOK_CALL).install()->quick();
	}

	scheduler::once([]
	{
		// DVAR_ARCHIVE
		ui_ps3_style = register_bool("ui_ps3Style", true, 1, "MW3 PS3 look (menus, fonts, button prompts, images) from <game>\\ps3style. Requires a restart.");
		register_int("ui_ps3StyleFonts", 0, 0, 2, 1, "PS3 look: fonts. 0 = the PC's with PS3 button icons, 1 = the Japanese PS3 disc's for all text (its button letters), 2 = the English PS3 disc's with PS3 button icons. Requires a restart.");
		register_bool("ui_xboxButtons", false, 1, "PS3 look: Xbox 360 button icons, wording and controller pictures instead of the PS3 ones (also -xboxbuttons). Requires a restart.");
		ui_ps3_button_style = register_int("ui_ps3ButtonStyle", 0, 0, 1, 1, "PS3 look: size of the PS3 button icons: 0 = the English PS3's, 1 = the Japanese PS3's (the Xbox icons keep the English size).");
		console_scripts = register_bool("ps3style_consoleScripts", true, 1, "PS3 look: matches you host run the console's MP scripts (d-pad killstreaks: up / down choose, right uses; console prematch countdown). From the next map.");
		// on by default since 2026-10-11 (the user's choice): Find Game fills with lobby players right away

		command::add("ps3style_dumpPCMenus", [](const std::vector<std::string>&)
		{
			utils::io::create_directory(folder());
			dump_pc_menus();
		});

		// ps3style_shot <name>: the next frame -> ps3style\shots\<name>.bmp (also with the window in the background)
		command::add("ps3style_shot", [](const std::vector<std::string>& params)
		{
			const auto name = params.size() > 1 ? params[1] : std::string("shot");
			utils::io::create_directory(folder() + "\\shots");
			if (!ps3style_assets::capture_frame(folder() + "\\shots\\" + name + ".bmp")) log("ps3style_shot: no device\n");
		});

		scheduler::on_frame(run_console_file);

		// clan tag (clan_tag_guard): "clantag <tag>" sets it (4 letters), "clantag \"\"" clears it
		clan_tag_dvar = register_string("ui_clanTag", "", 1, "Clan tag (4 letters) shown with your name. Set with clantag <tag> or CLAN TAG in the PS3 menus.");
		command::add("clantag", [](const std::vector<std::string>& params)
		{
			if (params.size() < 2)
			{
				printf("\"clantag\" is: \"%s\" (clantag <tag> sets it)\n", profile_clan_tag().data());
				return;
			}
			set_clan_tag(params[1]);
		});
		scheduler::on_frame(clan_tag_guard);

		if (active)
		{
			command::add("editclanname", [](const std::vector<std::string>&)
			{
				edit_clan_tag();
			});
			scheduler::on_frame(breath_hint_guard);
			scheduler::on_frame(refresh_menu_assets);

			// the MP scripts' console code (console_scripts_guard; the PC exe has no xenonGame)
			xenon_game = register_string("xenonGame", console_scripts && console_scripts->current.enabled ? "true" : "false", 0,
			                             "PS3 look: \"true\" makes the MP scripts run their console code (set by ps3style_consoleScripts).");
			scheduler::on_frame(console_scripts_guard);
		}

		// the UI context's menu stack (uiContext 0x58EF640: Menus[640] @38, menuCount @A38, menuStack[16] @A3C,
		// openMenuCount @A7C) -> ps3style.log
		command::add("ps3style_openMenus", [](const std::vector<std::string>&)
		{
			const auto ui = 0x58EF640u;
			const auto count = *reinterpret_cast<int*>(ui + 0xA38);
			const auto open = *reinterpret_cast<int*>(ui + 0xA7C);
			log("menus loaded %d, open %d:", count, open);
			for (auto i = 0; i < open && i < 16; ++i)
			{
				const auto* menu = *reinterpret_cast<menuDef_t**>(ui + 0xA3C + 4 * i);
				log(" %s", menu && menu->window.name ? menu->window.name : "?");
			}
			log("\n");
		});

		// ps3style_focus: the top menu's cursor item and the items that have the focus flag -> ps3style.log
		command::add("ps3style_focus", [](const std::vector<std::string>& params)
		{
			if (params.size() > 1)
			{
				// ps3style_focus <item>: that item's rects of the top menu
				const auto ui = 0x58EF640u;
				const auto open = *reinterpret_cast<int*>(ui + 0xA7C);
				const auto* menu = open > 0 ? *reinterpret_cast<menuDef_t**>(ui + 0xA3C + 4 * (open - 1)) : nullptr;
				const auto index = std::atoi(params[1].data());
				if (!menu || index < 0 || index >= menu->itemCount || !menu->items[index]) return;
				const auto* item = menu->items[index];
				log("item %d: rect %.1f %.1f %.1f %.1f (%d %d) textRect %.1f %.1f %.1f %.1f align %d alignx %.1f scale %.3f font %d\n", index,
				    item->window.rect.x, item->window.rect.y, item->window.rect.w, item->window.rect.h, item->window.rect.horzAlign,
				    item->window.rect.vertAlign, item->textRect[0].x, item->textRect[0].y, item->textRect[0].w, item->textRect[0].h,
				    item->textAlignMode, item->textalignx, item->textscale, item->fontEnum);
				return;
			}

			const auto ui = 0x58EF640u;
			const auto open = *reinterpret_cast<int*>(ui + 0xA7C);
			if (open <= 0) return;
			const auto* menu = *reinterpret_cast<menuDef_t**>(ui + 0xA3C + 4 * (open - 1));
			if (!menu) return;
			std::string text = utils::string::va("focus in %s: cursorItem %d, focused:", menu->window.name,
			                                     menu->data ? menu->data->cursorItem[0] : -2);
			for (auto i = 0; i < menu->itemCount; ++i)
			{
				const auto* item = menu->items[i];
				if (item && (item->window.dynamicFlags[0] & 2))
				{
					text += utils::string::va(" %d(%s, flags %X)", i, item->window.name ? item->window.name : "-", item->window.dynamicFlags[0]);
				}
			}
			log("%s\n", text.data());
		});

		// ps3style_dvar <name> ...: dvar values -> ps3style.log
		command::add("ps3style_dvar", [](const std::vector<std::string>& params)
		{
			for (size_t i = 1; i < params.size(); ++i)
			{
				const auto* dvar = static_cast<game::native::dvar_t*>(ps3style_assets::find_dvar(params[i].data()));
				if (!dvar) log("dvar %s: none\n", params[i].data());
				else log("dvar %s: type %d flags %X value int %d float %.3f string %s\n", params[i].data(), dvar->type, dvar->flags,
				         dvar->current.integer, dvar->current.value,
				         dvar->type == 7 || dvar->type == 8 ? dvar->current.string : "-");
			}
		});

		// ps3style_open <menu>: opens a menu (as the menu script "open"; the PC game has no openmenu command)
		command::add("ps3style_open", [](const std::vector<std::string>& params)
		{
			if (params.size() < 2) return;
			if (!open_menu(params[1].data())) log("open %s: no such menu\n", params[1].data());
		});

		// ps3style_closeAll: closes every open menu but the bottom one
		command::add("ps3style_closeAll", [](const std::vector<std::string>&)
		{
			cbuf_add_text("closemenu all\n");
		});

		// ps3style_strings <type> <asset> [bytes] [pointer depth 0|1]: the string pointers in an asset's header (and in the
		// structs it points to) -> ps3style.log, to find fields by their values
		command::add("ps3style_strings", [](const std::vector<std::string>& params)
		{
			if (params.size() < 3) return;
			const auto* base = static_cast<const unsigned char*>(find_asset(std::atoi(params[1].data()), params[2].data(), 0));
			const auto size = params.size() > 3 ? std::strtoul(params[3].data(), nullptr, 0) : 0x100;
			const auto deep = params.size() > 4 && params[4] == "1";
			if (!base) return;

			const auto str_at = [](const void* p) -> const char*
			{
				if (!p || readable_size(p, 4) < 4) return nullptr;
				const auto* s = static_cast<const char*>(p);
				for (auto i = 0; i < 64; ++i)
				{
					if (readable_size(s + i, 1) < 1) return nullptr;
					if (!s[i]) return i >= 3 ? s : nullptr;
					if (s[i] < 32 || s[i] > 126) return nullptr;
				}
				return nullptr;
			};

			std::string text = utils::string::va("strings in %s @%p:\n", params[2].data(), base);
			for (size_t off = 0; off + 4 <= size; off += 4)
			{
				const auto* p = *reinterpret_cast<void* const*>(base + off);
				if (const auto* s = str_at(p))
				{
					text += utils::string::va("  +%03X %s\n", off, s);
				}
				else if (deep && p && readable_size(p, 0x1000) == 0x1000)
				{
					for (size_t o2 = 0; o2 < 0x1000; o2 += 4)
					{
						if (const auto* s2 = str_at(*reinterpret_cast<void* const*>(static_cast<const unsigned char*>(p) + o2)))
						{
							text += utils::string::va("  +%03X -> +%03X %s\n", off, o2, s2);
						}
					}
				}
			}
			log("%s", text.data());
		});

		// ps3style_snap <hex address> <hex size> <file>: raw memory -> ps3style\<file> (unreadable pages as zeros)
		command::add("ps3style_snap", [](const std::vector<std::string>& params)
		{
			if (params.size() < 4) return;
			const auto address = std::strtoul(params[1].data(), nullptr, 16);
			const auto size = std::strtoul(params[2].data(), nullptr, 16);
			std::string data(size, '\0');
			for (size_t off = 0; off < size; off += 0x1000)
			{
				const auto n = std::min<size_t>(0x1000, size - off);
				if (readable_size(reinterpret_cast<void*>(address + off), n) == n) std::memcpy(&data[off], reinterpret_cast<void*>(address + off), n);
			}
			utils::io::write_file(folder() + "\\" + params[3], data);
		});

		// ps3style_peek <hex address> [bytes]: raw memory -> ps3style.log
		command::add("ps3style_peek", [](const std::vector<std::string>& params)
		{
			if (params.size() < 2) return;
			const auto address = std::strtoul(params[1].data(), nullptr, 16);
			const auto size = params.size() > 2 ? std::strtoul(params[2].data(), nullptr, 0) : 0x40;
			if (!address || readable_size(reinterpret_cast<void*>(address), size) < size) return;
			log("peek %08X%s", address, hex_dump(reinterpret_cast<void*>(address), size).data());
		});

		// ps3style_peekAsset <type> <name>: raw bytes of an asset (font: + its material, material: + texture table /
		// first image) -> ps3style.log
		command::add("ps3style_peekAsset", [](const std::vector<std::string>& params)
		{
			if (params.size() < 3) return;
			const auto type = std::atoi(params[1].data());
			auto* header = find_asset(type, params[2].data(), 0);
			log("asset %d %s -> %p\n", type, params[2].data(), header);
			if (!header) return;
			log("header%s", hex_dump(header, 0x20).data());

			auto* mat = type == ASSET_TYPE_MATERIAL ? static_cast<Material*>(header) : nullptr;
			if (type == ASSET_TYPE_FONT)
			{
				log("font%s", hex_dump(header, sizeof(Font_s)).data());
				mat = static_cast<Font_s*>(header)->material;
				log("glyph[0..1]%s", hex_dump(static_cast<Font_s*>(header)->glyphs, 2 * sizeof(Glyph)).data());
			}
			if (!mat) return;

			log("material %s%s", mat->info.name, hex_dump(mat, sizeof(Material)).data());
			if (mat->textureTable)
			{
				log("textures (%d)%s", mat->textureCount, hex_dump(mat->textureTable, 12 * std::max<int>(1, mat->textureCount)).data());
				log("image[0]%s", hex_dump(mat->textureTable[0].image, 0x40).data());
				const auto* image_name = *reinterpret_cast<const char**>(reinterpret_cast<char*>(mat->textureTable[0].image) + 0x18);
				if (readable_size(image_name, 4) == 4) log("  name@18: %.60s\n", image_name);
			}
		});

		// ps3style_dumpMenu <name>: the menu the game gets for that name (the PS3 one when there is one), as the game
		// sees it -> ps3style\menu_<name>.json
		command::add("ps3style_dumpMenu", [](const std::vector<std::string>& params)
		{
			if (params.size() < 2) return;
			const auto* menu = static_cast<menuDef_t*>(reinterpret_cast<void*(*)(int, const char*, int)>(db_find_x_asset_header_addr)(
				ASSET_TYPE_MENU, params[1].data(), 0));
			std::string text;
			if (!menu || !try_write_menu_text(menu, &text)) text = "null";
			utils::io::write_file(folder() + "\\menu_" + params[1] + ".json", text);
			log("menu %s %p: dynamicFlags %08X items %d\n", params[1].data(), menu, menu ? menu->window.dynamicFlags[0] : 0,
			    menu ? menu->itemCount : 0);
		});

		// ps3style_dumpPCStrings: the PC game's text for every key in ps3style\string_keys.txt -> pc_strings.json
		command::add("ps3style_dumpPCStrings", [](const std::vector<std::string>&)
		{
			std::string keys;
			if (!utils::io::read_file(folder() + "\\string_keys.txt", &keys)) return;

			rapidjson::StringBuffer buffer;
			json_writer w(buffer);
			w.StartObject();
			std::istringstream lines(keys);
			std::string key;
			auto count = 0;
			while (std::getline(lines, key))
			{
				if (!key.empty() && key.back() == '\r') key.pop_back();
				if (key.empty()) continue;
				const auto* entry = static_cast<LocalizeEntry*>(ps3style_assets::find_loaded(ASSET_TYPE_LOCALIZE_ENTRY, key.data()));
				if (!entry || !entry->value) continue;
				w.Key(key.data());
				w.String(entry->value);
				++count;
			}
			w.EndObject();
			utils::io::write_file(folder() + "\\pc_strings.json", buffer.GetString());
			log("PC strings: %d -> pc_strings.json\n", count);
		});

		// builds every PS3 menu and writes what they use that the PC game doesn't have -> ps3style\missing_assets.txt
		// (tools\make_assets.py makes the images of the missing materials)
		command::add("ps3style_menuReport", [](const std::vector<std::string>&)
		{
			ps3style_menus::build_all();
			std::string text;
			for (const auto& name : ps3style_menus::missing()) text += name + "\n";
			utils::io::write_file(folder() + "\\missing_assets.txt", text);
			log("menu report: %u missing assets -> missing_assets.txt\n", ps3style_menus::missing().size());
		});

		command::add("ps3style_logAssets", [](const std::vector<std::string>& params)
		{
			log_assets = params.size() > 1 ? params[1] != "0" : !log_assets;
		});
	});
}

REGISTER_MODULE(ps3style)
