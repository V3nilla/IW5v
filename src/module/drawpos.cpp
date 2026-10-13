#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "utils/hook.hpp"
#include "utils/io.hpp"
#include "utils/string.hpp"
#include "game/game.hpp"
#include "command.hpp"
#include "scheduler.hpp"


// Where am I: iw5v_drawPos 1 draws the map, X Y Z, view yaw / pitch in the top right corner in a match; the "pos" command prints that line, copies "map x y z yaw" to the
// clipboard and adds it to <game>\open-iw5\positions.txt. MP client only.
namespace
{
	constexpr DWORD cg_draw_2d_call = 0x685189; // CG_DrawActive (0x684EE0): call CG_Draw2D(localClient)
	constexpr DWORD cg_draw_2d = 0x448770;
	constexpr DWORD dvar_register_bool_addr = 0x5BE9F0; // (name, value, flags, description)
	constexpr DWORD connection_state = 0x10625F4; // clientUIActives[0]: 11 = active in a match (CL_FirstSnapshot)
	// cg.predictedPlayerState (CG_PredictPlayerState; the local player, or the player followed when spectating):
	// origin @1C, viewangles @158 (pitch, yaw, roll; tu6::playerState_s, Xbox layout = PC)
	constexpr DWORD predicted_ps = 0x900FA0;
	constexpr DWORD cg_refdef = 0x96C1B0; // refdef_t: x, y, width, height (real screen pixels)
	constexpr DWORD configstring_offsets = 0xFF7434; // CL_GetConfigString 0x4849D0: 0xFFB0DC + offsets[index], 0 serverinfo
	constexpr DWORD configstring_data = 0xFFB0DC;
	constexpr DWORD r_add_cmd_draw_stretch_pic = 0x67E5B0; // (x, y, w, h, s0, t0, s1, t1, color, material)
	constexpr DWORD r_add_cmd_draw_text = 0x67EB40; // (text, maxChars, font, x, baseline y, xScale, yScale, angle, color, style)
	constexpr DWORD r_text_width = 0x669B00; // (text, maxChars, font)
	constexpr DWORD cls_white_material = 0xFB694C;
	constexpr DWORD cls_console_font = 0xFB6954;
	constexpr DWORD cl_console_print = 0x487B00; // (localClientNum, channel, text, duration, pixelWidth, flags)

	game::native::dvar_t* iw5v_draw_pos = nullptr;

	struct position
	{
		std::string map;
		float origin[3];
		float yaw;
		float pitch;
	};

	bool in_match()
	{
		return *reinterpret_cast<const int*>(connection_state) == 11;
	}

	std::string map_name()
	{
		const auto offset = *reinterpret_cast<const int*>(configstring_offsets);
		if (offset < 0) return "?";
		const auto* info = reinterpret_cast<const char*>(configstring_data + offset);
		const std::string text(info, strnlen(info, 2048));
		const std::string key = "\\mapname\\";
		const auto at = text.find(key);
		if (at == std::string::npos) return "?";
		const auto start = at + key.size();
		return text.substr(start, text.find('\\', start) - start);
	}

	float angle_180(float angle)
	{
		angle = std::fmod(angle, 360.0f);
		if (angle > 180.0f) angle -= 360.0f;
		if (angle < -180.0f) angle += 360.0f;
		return angle;
	}

	position read_position()
	{
		position pos{};
		pos.map = map_name();
		const auto* origin = reinterpret_cast<const float*>(predicted_ps + 0x1C);
		const auto* angles = reinterpret_cast<const float*>(predicted_ps + 0x158);
		for (auto i = 0; i < 3; ++i) pos.origin[i] = origin[i];
		pos.pitch = angle_180(angles[0]);
		pos.yaw = angle_180(angles[1]);
		return pos;
	}

	std::string describe(const position& pos)
	{
		std::string line = utils::string::va("%s  %.1f %.1f %.1f  yaw %.1f  pitch %.1f", pos.map.data(), pos.origin[0],
		                                     pos.origin[1], pos.origin[2], pos.yaw, pos.pitch);
		return line;
	}

	void draw_line(const std::string& text)
	{
		auto* font = *reinterpret_cast<void**>(cls_console_font);
		auto* white = *reinterpret_cast<void**>(cls_white_material);
		if (!font || !white) return;

		const auto* refdef = reinterpret_cast<const int*>(cg_refdef);
		const auto screen_w = refdef[2] > 0 ? static_cast<float>(refdef[2]) : 1280.0f;
		const auto screen_h = refdef[3] > 0 ? static_cast<float>(refdef[3]) : 720.0f;
		const auto scale = std::clamp(screen_h / 900.0f, 0.6f, 1.5f);
		const auto width = static_cast<float>(reinterpret_cast<int(__cdecl*)(const char*, int, void*)>(r_text_width)(
			text.data(), 0, font)) * scale;
		const auto height = 16.0f * scale;
		const auto pad = 4.0f * scale;
		const auto x = screen_w - width - pad - 8.0f * scale;
		const auto y = 8.0f * scale;

		const float back[4] = {0.0f, 0.0f, 0.0f, 0.55f};
		reinterpret_cast<void(__cdecl*)(float, float, float, float, float, float, float, float, const float*, void*)>(
			r_add_cmd_draw_stretch_pic)(x - pad, y, width + 2.0f * pad, height + 2.0f * pad, 0.0f, 0.0f, 1.0f, 1.0f, back,
			                            white);
		const float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		reinterpret_cast<void(__cdecl*)(const char*, int, void*, float, float, float, float, float, const float*, int)>(
			r_add_cmd_draw_text)(text.data(), 0x7FFFFFFF, font, x, y + pad + height - 3.0f * scale, scale, scale, 0.0f,
			                     color, 0);
	}

	// CG_Draw2D, then the position line (main thread, after the HUD)
	void __cdecl cg_draw_2d_stub(const int local_client)
	{
		reinterpret_cast<void(__cdecl*)(int)>(cg_draw_2d)(local_client);
		if (local_client == 0 && iw5v_draw_pos && iw5v_draw_pos->current.enabled && in_match())
		{
			draw_line(describe(read_position()));
		}
	}

	void print(const std::string& text)
	{
		printf("%s", text.data());
		reinterpret_cast<void(*)(int, int, const char*, int, int, int)>(cl_console_print)(0, 0, text.data(), 0, 0, 0);
	}

	void copy_to_clipboard(const std::string& text)
	{
		if (!OpenClipboard(nullptr)) return;
		EmptyClipboard();
		if (const auto memory = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1))
		{
			if (auto* data = static_cast<char*>(GlobalLock(memory)))
			{
				std::memcpy(data, text.data(), text.size() + 1);
				GlobalUnlock(memory);
				if (!SetClipboardData(CF_TEXT, memory)) GlobalFree(memory);
			}
			else
			{
				GlobalFree(memory);
			}
		}
		CloseClipboard();
	}

	void pos_command()
	{
		if (!in_match())
		{
			print("not in a match\n");
			return;
		}
		const auto pos = read_position();
		const auto line = describe(pos);
		print(line + "\n");
		copy_to_clipboard(utils::string::va("%s %.1f %.1f %.1f %.1f", pos.map.data(), pos.origin[0], pos.origin[1],
		                                    pos.origin[2], pos.yaw));

		SYSTEMTIME now{};
		GetLocalTime(&now);
		char exe[MAX_PATH]{};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		utils::io::write_file(std::filesystem::path(exe).parent_path().string() + "\\open-iw5\\positions.txt",
		                      utils::string::va("%04d-%02d-%02d %02d:%02d:%02d  %s\n", now.wYear, now.wMonth, now.wDay,
		                                        now.wHour, now.wMinute, now.wSecond, line.data()), true);
	}
}

class drawpos final : public module
{
public:
	void post_load() override
	{
		if (!game::is_mp()) return;

		// the only hook on this call
		utils::hook(cg_draw_2d_call, &cg_draw_2d_stub, HOOK_CALL).install()->quick();

		scheduler::once([]
		{
			// 0x1 = archive (seta): a config's "seta iw5v_drawPos 1" keeps its value
			iw5v_draw_pos = reinterpret_cast<game::native::dvar_t*(*)(const char*, bool, int, const char*)>(
				dvar_register_bool_addr)("iw5v_drawPos", false, 1,
				                         "Draw your map, X Y Z and view angles in the top right corner in a match");
			command::add("pos", [](const std::vector<std::string>&)
			{
				pos_command();
			});
		});
	}
};

REGISTER_MODULE(drawpos)
