#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "utils/hook.hpp"
#include "game/game.hpp"

class patches final : public module
{
public:
	void post_load() override
	{
		if (!game::is_dedi()) this->patch_clients();

		if (game::is_sp()) this->patch_sp();
		else if (game::is_mp()) this->patch_mp();
		else if (game::is_dedi()) this->patch_dedi();

		utils::hook(game::native::_longjmp, long_jump_stub, HOOK_JUMP).install()->quick();
	}

private:
	void patch_clients() const
	{
		// Remove improper quit check
		utils::hook::nop(SELECT_VALUE(0x53444A, 0x5CCDC0, 0), 9);

		// Ignore sdm files
		utils::hook::nop(SELECT_VALUE(0x4438BA, 0x6371EA, 0), 2);
	}

	void patch_sp() const
	{
		// SP doesn't initialize WSA
		WSADATA wsa_data;
		WSAStartup(MAKEWORD(2, 2), &wsa_data);

		// Disable remote storage
		utils::hook::set<BYTE>(0x663B5A, 0xEB);
		utils::hook::set<BYTE>(0x663C54, 0xEB);
	}

	void patch_mp() const
	{
	}

	void patch_dedi() const
	{
	}

	static __declspec(noreturn) void long_jump_stub(jmp_buf buf, const int value) noexcept(false)
	{
#ifdef DEBUG
		{
			printf("Unwinding the stack...\n");
		}
#endif

		longjmp(buf, value);
	}
};

REGISTER_MODULE(patches)
