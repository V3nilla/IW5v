#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "utils/hook.hpp"
#include "game/game.hpp"

class fov final : public module
{
public:
	void post_load() override
	{
		if (game::is_dedi()) return;

		// Set dvar flag
		utils::hook::set<BYTE>(SELECT_VALUE(0x4302C5, 0x455155, 0), 0x1 | (game::is_mp() ? 0x40 : 0));

		if (game::is_mp())
		{
			// Set dvar limit
			static const auto cg_fov_limit = 90.0f;
			utils::hook::set(0x455148, &cg_fov_limit);

			// Prevent value change via internal scripts
			utils::hook(0x4698BA, &set_server_command_dvar_stub, HOOK_CALL).install()->quick();
		}
	}

private:
	static void set_server_command_dvar_stub(const char* dvar, const char* value)
	{
		if (strcmp(dvar, "cg_fov") != 0 || strcmp(value, "65") != 0)
		{
			game::native::Dvar_SetFromStringByName(dvar, value);
		}
	}
};

REGISTER_MODULE(fov)
