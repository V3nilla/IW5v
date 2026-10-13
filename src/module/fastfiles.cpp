#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "game/structs.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"

static __declspec(naked) void db_load_stub_client(game::native::XZoneInfo*, unsigned int, int)
{
	__asm
	{
		sub esp, 0Ch
		mov eax, [esp + 18h]

		mov ecx, game::native::DB_LoadXAssets
		add ecx, 7h
		push ecx
		retn
	}
}

static __declspec(naked) void db_load_stub_server(game::native::XZoneInfo*, unsigned int, int)
{
	__asm
	{
		sub esp, 10h
		mov eax, [esp + 1Ch]

		mov ecx, game::native::DB_LoadXAssets
		add ecx, 7h
		push ecx
		retn
	}
}

class fastfiles final : public module
{
public:
	void post_load() override
	{
		utils::hook(game::native::DB_LoadXAssets, db_load_stub, HOOK_JUMP).install()->quick();
	}

private:
	static void db_load_stub(game::native::XZoneInfo* zone_info, const unsigned int zone_count, const int sync)
	{
		for (unsigned int i = 0; i < zone_count; ++i)
		{
			if (zone_info[i].name)
			{
				printf("Loading FastFile: %s (0x%X | 0x%X)\n", zone_info[i].name, zone_info[i].allocFlags,
				       zone_info[i].freeFlags);
			}
		}

		if (game::is_dedi()) return db_load_stub_server(zone_info, zone_count, sync);
		else return db_load_stub_client(zone_info, zone_count, sync);
	}
};

REGISTER_MODULE(fastfiles)
