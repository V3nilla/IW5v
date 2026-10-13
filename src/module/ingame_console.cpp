#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "command.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"
#include "steam/steam.hpp"
#include "utils/hook.hpp"
#include "utils/io.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>

// The in-game (drop-down) console on ` / ~, as IW4x has it (Shift + ` = the full console with its output). MW3 PC
// keeps the console but made it a remote console: it only opens when sv_allowClientConsole and cl_enableRCon are both
// on (or never with monkeytoy), its input starts with "rcon " and Enter sends the line to the server. Here it always
// opens, starts empty and runs the line locally. MW3 PC also compiled out the console's own messages (no echo, no
// dvar values, no "unknown command") and its auto-complete (the list of matching dvars / commands under the input
// line, Tab): those come back here, plus IW4x's "name" and a "quit".
namespace
{
	using game::native::dvar_t;
	using game::native::DvarValue;

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

	// g_consoleField (field_t 0xB3C518): cursor +0, fixed prefix length +4 (Backspace stops there; 0 here), scroll +8,
	// buffer +0x1C
	constexpr DWORD console_field_addr = 0xB3C518;
	constexpr size_t console_field_size = 256;

	char* field_buffer()
	{
		return reinterpret_cast<char*>(console_field_addr + 0x1C);
	}

	std::string field_text()
	{
		return std::string(field_buffer(), strnlen(field_buffer(), console_field_size));
	}

	std::string value_string(const dvar_t* dvar, const DvarValue& v)
	{
		switch (dvar->type)
		{
		case 0: return v.enabled ? "1" : "0";
		case 1: return utils::string::va("%g", v.value);
		case 2: return utils::string::va("%g %g", v.vector[0], v.vector[1]);
		case 3:
		case 9: return utils::string::va("%g %g %g", v.vector[0], v.vector[1], v.vector[2]);
		case 4: return utils::string::va("%g %g %g %g", v.vector[0], v.vector[1], v.vector[2], v.vector[3]);
		case 5: return utils::string::va("%i", v.integer);
		case 6:
			if (v.integer >= 0 && v.integer < dvar->domain.enumeration.stringCount && dvar->domain.enumeration.strings)
			{
				return dvar->domain.enumeration.strings[v.integer];
			}
			return utils::string::va("%i", v.integer);
		case 7: return v.string ? v.string : "";
		case 8: return utils::string::va("%g %g %g %g", static_cast<unsigned char>(v.color[0]) / 255.0f, static_cast<unsigned char>(v.color[1]) / 255.0f,
		                                 static_cast<unsigned char>(v.color[2]) / 255.0f, static_cast<unsigned char>(v.color[3]) / 255.0f);
		default: return "?";
		}
	}

	// Dvar_DomainToString's IW wording (MW3 PC compiled the texts out)
	std::vector<std::string> domain_lines(const dvar_t* dvar)
	{
		const auto range = [](const bool no_low, const bool no_high, const std::string& low, const std::string& high) -> std::string
		{
			if (no_low) return no_high ? "" : " " + high + " or smaller";
			return no_high ? " " + low + " or bigger" : " from " + low + " to " + high;
		};
		const auto float_range = [&](const game::native::float_limit& limit)
		{
			return range(limit.min <= -FLT_MAX, limit.max >= FLT_MAX, utils::string::va("%g", limit.min), utils::string::va("%g", limit.max));
		};

		const auto& domain = dvar->domain;
		switch (dvar->type)
		{
		case 0: return {"Domain is 0 or 1"};
		case 1: return {"Domain is any number" + float_range(domain.value)};
		case 2:
		case 3:
		case 4:
		{
			const auto components = float_range(domain.vector);
			return {utils::string::va("Domain is any %iD vector", static_cast<int>(dvar->type)) + (components.empty() ? std::string() : " with components" + components)};
		}
		case 5: return {"Domain is any integer" + range(domain.integer.min == INT_MIN, domain.integer.max == INT_MAX,
		                                                std::to_string(domain.integer.min), std::to_string(domain.integer.max))};
		case 6:
		{
			std::vector<std::string> lines{"Domain is one of the following:"};
			for (auto i = 0; domain.enumeration.strings && i < domain.enumeration.stringCount; ++i)
			{
				const auto* text = domain.enumeration.strings[i];
				lines.emplace_back(utils::string::va("  %2i: %s", i, text ? text : ""));
			}
			return lines;
		}
		case 7: return {"Domain is any text"};
		case 8: return {"Domain is any 4-component color, in RGBA format"};
		case 9: return {"Domain is any 3-component color, in RGB format"};
		default: return {};
		}
	}

	// ---- the matching dvars / commands (CONSOLE-1, CONSOLE-2) ----

	struct input_match
	{
		const char* name;
		const dvar_t* dvar; // nullptr: a command
	};

	bool name_matches(const char* name, const std::string& token, const bool substring)
	{
		if (!substring) return _strnicmp(name, token.data(), token.size()) == 0;

		const auto length = std::strlen(name);
		for (size_t i = 0; i + token.size() <= length; ++i)
		{
			if (_strnicmp(name + i, token.data(), token.size()) == 0) return true;
		}
		return false;
	}

	struct match_query
	{
		const std::string* token;
		bool substring;
		std::vector<input_match>* out;
	};

	void __cdecl collect_dvar(const dvar_t* dvar, void* user)
	{
		const auto* query = static_cast<const match_query*>(user);
		if (dvar && dvar->name && name_matches(dvar->name, *query->token, query->substring))
		{
			query->out->push_back({dvar->name, dvar});
		}
	}

	// The dvars (Dvar_ForEach 0x5BFFB0 hands them sorted by name, under the dvar lock), then the commands (cmd list
	// 0x1C97958, unsorted) whose names start with the token, as the IW engine lists them; when nothing starts with it,
	// those containing it. A command named like a listed dvar (the "name" command) is listed once, as the dvar
	std::vector<input_match> collect_matches(const std::string& token, const bool dvars_only)
	{
		std::vector<input_match> matches;
		if (token.empty()) return matches;

		for (const auto substring : {false, true})
		{
			match_query query{&token, substring, &matches};
			reinterpret_cast<void(__cdecl*)(void(__cdecl*)(const dvar_t*, void*), void*)>(0x5BFFB0)(collect_dvar, &query);

			if (!dvars_only)
			{
				// matches holds only the dvars until the commands are appended below
				std::vector<input_match> commands;
				for (auto* cmd = *reinterpret_cast<game::native::cmd_function_t**>(0x1C97958); cmd; cmd = cmd->next)
				{
					if (!cmd->name || !name_matches(cmd->name, token, substring)) continue;

					const auto* cmd_name = cmd->name;
					if (std::none_of(matches.begin(), matches.end(), [cmd_name](const input_match& m) { return _stricmp(m.name, cmd_name) == 0; }))
					{
						commands.push_back({cmd_name, nullptr});
					}
				}

				std::sort(commands.begin(), commands.end(), [](const input_match& a, const input_match& b) { return _stricmp(a.name, b.name) < 0; });
				matches.insert(matches.end(), commands.begin(), commands.end());
			}

			if (!matches.empty()) break;
		}

		return matches;
	}

	// The name being typed: the first word (Con_TokenizeInput 0x487DF0 drops a leading / or \ as execute_line does),
	// or the dvar after set / seta / sets / reset / toggle / togglep (Con_IsDvarCommand 0x488140), which only matches dvars
	struct console_input
	{
		std::string text;
		size_t start = 0;
		size_t end = 0;
		bool dvars_only = false;

		std::string token() const
		{
			return this->text.substr(this->start, this->end - this->start);
		}

		// the tokenizer drops trailing spaces, so this comes from the field itself
		bool space_after() const
		{
			return this->end < this->text.size() && this->text[this->end] == ' ';
		}
	};

	console_input parse_input()
	{
		console_input input;
		input.text = field_text();

		const auto& text = input.text;
		const auto size = text.size();
		const auto word_end = [&](size_t from)
		{
			while (from < size && text[from] != ' ' && text[from] != ';') ++from;
			return from;
		};

		size_t i = 0;
		while (i < size && text[i] == ' ') ++i;
		while (i < size && (text[i] == '/' || text[i] == '\\')) ++i;
		while (i < size && text[i] == ' ') ++i;
		input.start = i;
		input.end = word_end(i);

		if (input.space_after() && reinterpret_cast<bool(__cdecl*)(const char*)>(0x488140)(input.token().data()))
		{
			auto arg = input.end;
			while (arg < size && text[arg] == ' ') ++arg;
			input.start = arg;
			input.end = word_end(arg);
			input.dvars_only = true;
		}

		return input;
	}

	// The Tab cycle: the matches of the name as it was typed, the one in the field now and the text around it. It ends
	// when the field holds anything else than what it wrote last (typing, history, a new console)
	struct tab_cycle
	{
		std::vector<std::string> names;
		std::vector<const dvar_t*> dvars;
		int index = -1;
		std::string before;
		std::string after;
		std::string line;
	};

	tab_cycle cycle_state;

	bool cycling()
	{
		return cycle_state.index >= 0 && cycle_state.index < static_cast<int>(cycle_state.names.size()) && cycle_state.line == field_text();
	}

	// the field's new text with the cursor after the completed name; Field_AdjustScroll (0x48BC10: scrPlace from
	// 0x4B7600, field) as Console_Key does after its edits
	void write_field(const std::string& text, const size_t cursor)
	{
		auto* const buffer = field_buffer();
		strncpy_s(buffer, console_field_size, text.data(), _TRUNCATE);

		const auto length = static_cast<int>(std::strlen(buffer));
		*reinterpret_cast<int*>(console_field_addr) = std::min(static_cast<int>(cursor), length);
		*reinterpret_cast<int*>(console_field_addr + 8) = 0;
		reinterpret_cast<void(__cdecl*)(void*, void*)>(0x48BC10)(reinterpret_cast<void*(__cdecl*)()>(0x4B7600)(),
		                                                       reinterpret_cast<void*>(console_field_addr));
	}

	// the case-insensitive common start of all names, in the first one's case
	std::string common_prefix(const std::vector<input_match>& matches)
	{
		std::string prefix = matches.front().name;
		for (const auto& m : matches)
		{
			size_t n = 0;
			while (n < prefix.size() && m.name[n] &&
			       std::tolower(static_cast<unsigned char>(prefix[n])) == std::tolower(static_cast<unsigned char>(m.name[n])))
			{
				++n;
			}
			prefix.resize(n);
		}
		return prefix;
	}

	// Tab, as IW4x: one match -> its name and a space; several -> as far as they all agree; already that far -> cycle
	// through them (Shift+Tab backwards), the list staying the one of the name as typed
	void complete_input(const bool backwards)
	{
		if (cycling())
		{
			const auto count = static_cast<int>(cycle_state.names.size());
			cycle_state.index = (cycle_state.index + (backwards ? count - 1 : 1)) % count;
		}
		else
		{
			cycle_state = {};

			const auto input = parse_input();
			const auto token = input.token();
			const auto matches = collect_matches(token, input.dvars_only);
			if (matches.empty()) return;

			const auto before = input.text.substr(0, input.start);
			const auto after = input.text.substr(input.end);

			if (matches.size() == 1)
			{
				const std::string name = matches.front().name;
				write_field(before + name + (after.empty() ? " " : after), before.size() + name.size() + (after.empty() ? size_t{1} : size_t{0}));
				return;
			}

			const auto common = common_prefix(matches);
			if (common.size() > token.size() && _strnicmp(common.data(), token.data(), token.size()) == 0)
			{
				write_field(before + common + after, before.size() + common.size());
				return;
			}

			auto exact = -1;
			for (size_t i = 0; i < matches.size(); ++i)
			{
				cycle_state.names.emplace_back(matches[i].name);
				cycle_state.dvars.push_back(matches[i].dvar);
				if (exact < 0 && _stricmp(matches[i].name, token.data()) == 0) exact = static_cast<int>(i);
			}

			// the name typed in full is the cycle's start: the first Tab moves on from it
			const auto count = static_cast<int>(matches.size());
			if (exact >= 0) cycle_state.index = (exact + (backwards ? count - 1 : 1)) % count;
			else cycle_state.index = backwards ? count - 1 : 0;
			cycle_state.before = before;
			cycle_state.after = after;
		}

		const auto& name = cycle_state.names[cycle_state.index];
		write_field(cycle_state.before + name + cycle_state.after, cycle_state.before.size() + name.size());
		cycle_state.line = field_text();
	}

	// ---- drawing (real screen pixels, as Con_DrawInput 0x4881C0) ----

	// IW4's con_input* color defaults (MW3 PC dropped those dvars; only con_inputHintBoxColor is left, 0xB3C108)
	constexpr float dvar_match_color[4] = {1.0f, 1.0f, 0.8f, 1.0f};
	constexpr float dvar_value_color[4] = {1.0f, 1.0f, 0.8f, 1.0f};
	constexpr float dvar_inactive_color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
	constexpr float cmd_match_color[4] = {0.8f, 0.8f, 1.0f, 1.0f};
	constexpr float hint_box_default[4] = {0.4f, 0.4f, 0.35f, 1.0f};
	constexpr float selected_line_color[4] = {1.0f, 1.0f, 1.0f, 0.15f};

	constexpr auto max_value_chars = 64;

	// cls.consoleFont
	void* console_font()
	{
		return *reinterpret_cast<void**>(0xFB6954);
	}

	// conDrawInputGlob.fontHeight
	float line_height()
	{
		return *reinterpret_cast<float*>(0xB1F094);
	}

	// R_AddCmdDrawText (text, maxChars, font, x, baseline y, xScale, yScale, rotation, color, style)
	void draw_text(const char* text, const float x, const float top, const float* color, const int max_chars = 0x7FFFFFFF)
	{
		reinterpret_cast<void(__cdecl*)(const char*, int, void*, float, float, float, float, float, const float*, int)>(0x67EB40)(
			text, max_chars, console_font(), x, top + line_height(), 1.0f, 1.0f, 0.0f, color, 0);
	}

	// R_TextWidth (0x669B00: text, maxChars, font)
	float text_width(const char* text)
	{
		return static_cast<float>(reinterpret_cast<int(__cdecl*)(const char*, int, void*)>(0x669B00)(text, 0, console_font()));
	}

	// R_AddCmdDrawStretchPic (x, y, w, h, s0, t0, s1, t1, color, material) with cls.whiteMaterial (0xFB694C)
	void draw_rect(const float x, const float y, const float w, const float h, const float* color)
	{
		reinterpret_cast<void(__cdecl*)(float, float, float, float, float, float, float, float, const float*, void*)>(0x67E5B0)(
			x, y, w, h, 0.0f, 0.0f, 0.0f, 0.0f, color, *reinterpret_cast<void**>(0xFB694C));
	}

	// ConDraw_Box (0x487ED0): filled, with a 2 px border at half the color's brightness
	void draw_box(const float x, const float y, const float w, const float h, const float* color)
	{
		const float border[4] = {color[0] * 0.5f, color[1] * 0.5f, color[2] * 0.5f, color[3]};
		draw_rect(x, y, w, h, color);
		draw_rect(x, y, 2.0f, h, border);
		draw_rect(x + w - 2.0f, y, 2.0f, h, border);
		draw_rect(x, y, w, 2.0f, border);
		draw_rect(x, y + h - 2.0f, w, 2.0f, border);
	}

	// The IW engine's hint box under the input box: one match (or the exact name with a space after it) -> its value,
	// default, latched value and domain; too many -> their count; otherwise the names with the dvars' values (the Tab
	// cycle's list while it runs, its name highlighted)
	void draw_matches()
	{
		const auto font_height = line_height();
		if (!console_font() || font_height <= 0.0f) return;

		// conDrawInputGlob.leftX (where the typed text starts, after "> ") / y; con.screenMax; con.outputVisible
		const auto left = *reinterpret_cast<float*>(0xB1F090);
		const auto input_y = *reinterpret_cast<float*>(0xB1F08C);
		const auto right = *reinterpret_cast<float*>(0xB37880);
		const auto bottom = *reinterpret_cast<float*>(0xB37884);
		const auto full_console = *reinterpret_cast<unsigned char*>(0xB37868) != 0;

		// the input box ends 6 px under its line; this one starts 4 px lower, its first line 6 px in
		const auto top = input_y + font_height + 16.0f;
		const auto max_lines = full_console ? std::max(3, static_cast<int>((bottom - top) / font_height) - 1) : 24;

		const auto* hint_dvar = *reinterpret_cast<dvar_t**>(0xB3C108);
		const auto* hint_color = hint_dvar ? hint_dvar->current.vector : hint_box_default;

		const auto box = [&](const int lines)
		{
			draw_box(left - 6.0f, top - 6.0f, right - (left - 6.0f), static_cast<float>(lines) * font_height + 12.0f, hint_color);
		};
		const auto line_top = [&](const int n)
		{
			return top + static_cast<float>(n) * font_height;
		};

		std::vector<input_match> matches;
		auto selected = -1;
		const auto is_cycling = cycling();
		if (is_cycling)
		{
			for (size_t i = 0; i < cycle_state.names.size(); ++i) matches.push_back({cycle_state.names[i].data(), cycle_state.dvars[i]});
			selected = cycle_state.index;
		}
		else
		{
			const auto input = parse_input();
			const auto token = input.token();
			matches = collect_matches(token, input.dvars_only);
			if (matches.empty()) return;

			const auto exact = std::find_if(matches.begin(), matches.end(), [&](const input_match& m) { return _stricmp(m.name, token.data()) == 0; });
			if (matches.size() == 1 || (exact != matches.end() && input.space_after()))
			{
				const auto m = matches.size() == 1 ? matches.front() : *exact;
				if (!m.dvar)
				{
					box(1);
					draw_text(m.name, left, line_top(0), cmd_match_color);
					return;
				}

				const auto value = value_string(m.dvar, m.dvar->current);
				const auto reset = value_string(m.dvar, m.dvar->reset);
				const auto latched = value_string(m.dvar, m.dvar->latched);
				const auto has_latched = latched != value;
				const auto domain = domain_lines(m.dvar);

				const auto fixed_lines = has_latched ? 3 : 2;
				const auto lines = std::max(fixed_lines, std::min(max_lines, fixed_lines + static_cast<int>(domain.size())));
				const auto column = left + std::max({text_width(m.name), text_width("  default"), text_width("  latched")}) + text_width("  ");

				box(lines);
				draw_text(m.name, left, line_top(0), dvar_match_color);
				draw_text(value.data(), column, line_top(0), dvar_value_color, max_value_chars);
				draw_text("  default", left, line_top(1), dvar_inactive_color);
				draw_text(reset.data(), column, line_top(1), dvar_inactive_color, max_value_chars);
				if (has_latched)
				{
					draw_text("  latched", left, line_top(2), dvar_inactive_color);
					draw_text(latched.data(), column, line_top(2), dvar_inactive_color, max_value_chars);
				}
				for (auto n = fixed_lines; n < lines; ++n)
				{
					draw_text(domain[n - fixed_lines].data(), left, line_top(n), dvar_inactive_color);
				}
				return;
			}
		}

		const auto count = static_cast<int>(matches.size());
		if (count > max_lines && !is_cycling)
		{
			box(1);
			draw_text(full_console ? utils::string::va("%i matches (too many to show here)", count)
			                       : utils::string::va("%i matches (too many to show here, press shift+tilde to open full console)", count),
			          left, line_top(0), dvar_match_color);
			return;
		}

		// a cycle longer than the box: the part around its name
		const auto shown = std::min(count, max_lines);
		const auto first = count > max_lines ? std::clamp(selected - max_lines / 2, 0, count - max_lines) : 0;

		auto name_width = 0.0f;
		for (auto n = first; n < first + shown; ++n) name_width = std::max(name_width, text_width(matches[n].name));
		const auto column = left + name_width + text_width("  ");

		box(shown);
		for (auto n = 0; n < shown; ++n)
		{
			const auto& m = matches[first + n];
			if (first + n == selected) draw_rect(left - 4.0f, line_top(n), right - left, font_height, selected_line_color);

			draw_text(m.name, left, line_top(n), m.dvar ? dvar_match_color : cmd_match_color);
			if (m.dvar) draw_text(value_string(m.dvar, m.dvar->current).data(), column, line_top(n), dvar_value_color, max_value_chars);
		}
	}

	// ---- the player name (CONSOLE-4) ----

	// The name comes from Steam's GetPersonaName (open-iw5: steam::set_persona_name). The game copies it into its
	// Steam client (*0x5CF0500 + 0x156, 0x80 bytes) and from there into the Live name 0x5A966C4, which the userinfo
	// (Dvar_InfoString), party and menus read. The name dvar (0x1062774) is otherwise unused: it follows the name here
	std::string name_dvar_value; // the dvar's value as last applied / written: the watcher reacts to anything else
	bool name_dvar_checked = false;

	dvar_t* name_dvar()
	{
		return *reinterpret_cast<dvar_t**>(0x1062774);
	}

	void apply_name(const char* requested, const char* source)
	{
		// ClientCleanName (0x5C3A50: in, out, size): no color codes / [{ }], no leading spaces, "UnnamedPlayer" if
		// empty; at most 15 characters (ClientUserinfoChanged 0x4FADB0 keeps 16 bytes)
		const auto client_clean_name = reinterpret_cast<void(__cdecl*)(const char*, char*, int)>(0x5C3A50);
		// no ':' either (as iw4v's UserInfoSanitize; the game's cleaner takes the ^ colour codes)
		std::string text = requested ? requested : "";
		text.erase(std::remove(text.begin(), text.end(), ':'), text.end());

		char clean[32]{};
		client_clean_name(text.data(), clean, 32);
		clean[15] = 0;

		// every change goes to <game>\open-iw5\name.log with what asked for it
		const auto* old_name = steam::SteamFriends()->GetPersonaName();
		if (std::strcmp(old_name, clean) != 0)
		{
			const auto* dvar = name_dvar();
			utils::io::write_file("open-iw5\\name.log", utils::string::va("name \"%s\" -> \"%s\" (%s; dvar \"%s\", live \"%s\")\n", old_name, clean,
			                      source, dvar && dvar->type == 7 && dvar->current.string ? dvar->current.string : "",
			                      reinterpret_cast<const char*>(0x5A966C4)), true);
		}

		steam::set_persona_name(clean);

		// Live_GetLocalName (0x637300) reads the Steam client's copy; without that object (before SteamAPI_Init) it
		// gives "" and Live_Init makes the Live name from the persona later anyway
		if (auto* const steam_client = *reinterpret_cast<char**>(0x5CF0500))
		{
			strncpy_s(steam_client + 0x156, 0x80, clean, _TRUNCATE);
			client_clean_name(reinterpret_cast<const char*(__cdecl*)()>(0x637300)(), reinterpret_cast<char*>(0x5A966C4), 0x20);
		}

		// the dvar last (Dvar_SetStringByName 0x5BF660): its change sends the userinfo, now with the new Live name, and
		// saves the config (archived)
		const auto* dvar = name_dvar();
		if (dvar && dvar->type == 7)
		{
			if (!dvar->current.string || std::strcmp(dvar->current.string, clean) != 0)
			{
				reinterpret_cast<void(__cdecl*)(const char*, const char*)>(0x5BF660)("name", clean);
			}
			name_dvar_value = dvar->current.string ? dvar->current.string : "";
			name_dvar_checked = true;
		}
	}
}

class ingame_console final : public module
{
public:
	void post_load() override
	{
		if (!game::is_mp()) return;

		// CL_KeyEvent 0x48D120: ` / ~ with the console closed checks sv_allowClientConsole (0x20F9384) and
		// cl_enableRCon (0x1062590) -> straight on to the toggle
		utils::hook(0x48D45C, 0x48D47Bu, HOOK_JUMP).install()->quick();

		// monkeytoy (console restricted, 0xB1F100) doesn't block it either
		utils::hook(0x48D1C6, 0x48D207u, HOOK_JUMP).install()->quick();

		// the input field's initial text "rcon " (0x7F39B4) -> "" (its terminating zero): Con_ToggleConsole and the
		// two console init spots
		for (const auto address : {0x486141u, 0x486725u, 0x486764u})
		{
			utils::hook::set<DWORD>(address, 0x7F39B9);
		}

		// Enter (0x48CFD9: the rcon send 0x554F30) -> the local command buffer
		utils::hook(0x48CFD9, execute_line, HOOK_CALL).install()->quick();

		// Con_DrawInput 0x4881C0 (its match list compiled out): the matches under the input box, right before its
		// Field_Draw call (0x488110) on both paths, while the input is still tokenized
		utils::hook(0x488293, draw_input_stub, HOOK_CALL).install()->quick();
		utils::hook(0x4882E1, draw_input_stub, HOOK_CALL).install()->quick();

		// CL_KeyEvent -> Console_Key (0x48CDB0, which returns on Tab): Tab completes
		utils::hook(0x48D42F, console_key_stub, HOOK_CALL).install()->quick();

		// the name dvar (Dvar_RegisterString 0x492C90 in CL_InitOnceForAllClients) userinfo 0x200 -> also archived
		// (0x201): "seta name" goes to config_mp.cfg and Dvar_Reregister keeps it when the dvar is registered after the
		// config ran
		utils::hook::set<DWORD>(0x492C82, 0x201);

		// the name dvar set any way (config, set name, ui_SetName) -> the name. At the first look still the registration
		// default ("Unknown Soldier", nothing in the config): the dvar takes the Steam name ("GlaDos") instead
		scheduler::on_frame([]
		{
			// the Live name changed by something else than apply_name (logged, to find what)
			static std::string live_name;
			const auto* live = reinterpret_cast<const char*>(0x5A966C4);
			if (live_name != live)
			{
				if (!live_name.empty() && std::strcmp(live, steam::SteamFriends()->GetPersonaName()) != 0)
				{
					utils::io::write_file("open-iw5\\name.log", utils::string::va("live name \"%s\" -> \"%s\" (not by the name code)\n", live_name.data(), live), true);
				}
				live_name = live;
			}

			const auto* dvar = name_dvar();
			if (!dvar || dvar->type != 7 || !dvar->current.string) return;
			if (name_dvar_checked && name_dvar_value == dvar->current.string) return;

			const auto untouched = !name_dvar_checked && dvar->reset.string && std::strcmp(dvar->current.string, dvar->reset.string) == 0;
			apply_name(untouched ? steam::SteamFriends()->GetPersonaName() : dvar->current.string, untouched ? "first look" : "name dvar");
		});

		scheduler::once([]
		{
			command::add("echo", [](const std::vector<std::string>& params)
			{
				std::string text;
				for (size_t i = 1; i < params.size(); ++i) text += (i > 1 ? " " : "") + params[i];
				print(text + "\n");
			});

			// IW4x's "name <new name>"; alone it shows the name in use (the Live name, Live_GetLocalClientName 0x5C9980)
			command::add("name", [](const std::vector<std::string>& params)
			{
				if (params.size() < 2)
				{
					print(utils::string::va("\"name\" is: \"%s^7\"\n", reinterpret_cast<const char*>(0x5A966C4)));
					return;
				}

				std::string text;
				for (size_t i = 1; i < params.size(); ++i) text += (i > 1 ? " " : "") + params[i];
				apply_name(text.data(), "name command");
			});

			// MW3 PC has no quit command, only the menus' uiScript quit (Com_Quit_f 0x5556B0)
			if (!find_command("quit"))
			{
				command::add("quit", [](const std::vector<std::string>&)
				{
					reinterpret_cast<void(__cdecl*)()>(0x5556B0)();
				});
			}
		});
	}

	// CL_ConsolePrint (localClientNum, channel, text, duration, pixelWidth, flags)
	static void print(const std::string& text)
	{
		reinterpret_cast<void(*)(int, int, const char*, int, int, int)>(0x487B00)(0, 0, text.data(), 0, 0, 0);
	}

private:
	static void* find_command(const char* name)
	{
		return reinterpret_cast<void*(*)(const char*)>(0x545DA0)(name);
	}

	static void __cdecl draw_input_stub(const int local_client_num)
	{
		draw_matches();
		reinterpret_cast<void(__cdecl*)(int)>(0x488110)(local_client_num);
	}

	// Shift+Tab goes backwards: playerKeys[lcn].keys[K_SHIFT 0xA0].down (0xB3C760 + lcn * 0xD2C + key * 12), as
	// Console_Key reads it. Only with the console open (keyCatchers 0x10625EC bit 1): Console_Key also gets the keys
	// while disconnected
	static void __cdecl console_key_stub(const int local_client_num, const int key)
	{
		if (key == 9 && (*reinterpret_cast<int*>(0x10625EC) & 1))
		{
			complete_input(*reinterpret_cast<int*>(static_cast<DWORD>(0xB3CEE0 + local_client_num * 0xD2C)) != 0);
			return;
		}

		reinterpret_cast<void(__cdecl*)(int, int)>(0x48CDB0)(local_client_num, key);
	}

	static void __cdecl execute_line(const int local_client_num, const char* text)
	{
		if (!text || !*text) return;

		std::string line = text;
		// a leading slash / backslash as in IW4x's console ("/map mp_dome")
		while (!line.empty() && (line[0] == '/' || line[0] == '\\')) line.erase(0, 1);

		const auto name_end = line.find_first_of(" \t;");
		const auto name = line.substr(0, name_end);
		const auto has_args = name_end != std::string::npos && line.find_first_not_of(" \t", name_end) != std::string::npos &&
		                      line[name_end] != ';';

		if (!name.empty() && !find_command(name.data()))
		{
			const auto* dvar = find_dvar(name.data());
			if (!dvar)
			{
				print(utils::string::va("Unknown command \"%s\"\n", name.data()));
				return;
			}
			if (!has_args)
			{
				print(utils::string::va("\"%s\" is: \"%s^7\"\n", dvar->name, value_string(dvar, dvar->current).data()));
				return;
			}
		}

		line += "\n";
		reinterpret_cast<void(*)(int, const char*)>(0x545680)(local_client_num, line.data());
	}
};

REGISTER_MODULE(ingame_console)
