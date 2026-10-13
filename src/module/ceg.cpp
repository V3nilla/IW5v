#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "utils/hook.hpp"
#include "game/game.hpp"

class ceg final : public module
{
public:
	void post_load() override
	{
		// Only SP has CEG
		// CEG in MP has accidentally been removed due to CVE-2018-10718
		if (!game::is_sp()) return;

		utils::hook::signature signature(0x401000, 0x3E1000);

		signature.add({
			"\x56\xE8\x00\x00\x00\x00\x8B\xF0\xE8\x00\x00\x00\x00\x50\x56\xE8", "xx????xxx????xxx", [](char* address)
			{
				utils::hook::set<DWORD>(address, 0xC301B0);
			}
		});

		// Generic killer caller.
		signature.add({
			"\x55\x8B\xEC\x80\x7D\x08\x00\x75\x55", "xxxxxx?xx", [](char* address)
			{
				utils::hook::set<DWORD>(address, 0xC301B0);
			}
		});

		// CEG initialization.
		signature.add({
			"\x55\x8B\xEC\x83\xEC\x18\x53\x56\x57\xE8\x00\x00\x00\x00", "xxxxxxxxxx????", [](char* address)
			{
				utils::hook::set<BYTE>(address, 0xC3);
			}
		});

		// Some odd trap.
		signature.add({
			"\x55\x8B\xEC\x81\xEC\x00\x00\x00\x00\x53\x56\x57\x8B\x3D", "xxxxx??xxxxxxx", [](char* address)
			{
				utils::hook::set<DWORD>(address, 0xC301B0);
			}
		});

		// Custom shit
		signature.add({
			"\x55\x8B\xEC\x68\x00\x00\x00\x00\x68\x00\x00\x00\x00\x64\xFF\x35\x00\x00\x00\x00\x64\x89\x25\x00\x00\x00\x00\xE8",
			"xxxx????x????xxx????xxx????x", [](char* address)
			{
				utils::hook::set<BYTE>(address, 0xC3);
			}
		});

		// hkcr guid check
		signature.add({
			"\x55\x8B\xEC\xB8\x00\x00\x00\x00\xE8\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x84\xC0\x75\x06",
			"xxxx????x????x????xxxx", [](char* address)
			{
				utils::hook::nop(address + 0xD, 5); // Call
				utils::hook::nop(address + 0x14, 2); // Jump
			}
		});

		// hkcr guid check 2
		signature.add({
			"\x55\x8B\xEC\x81\xEC\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x84\xC0\x75\x06", "xxxxx????x????xxxx", [
			](char* address)
			{
				utils::hook::nop(address + 0x9, 5); // Call
				utils::hook::nop(address + 0x10, 2); // Jump
			}
		});

		signature.process();

		// Function fixup
		utils::hook(0x4CA310, game::native::DB_LoadXAssets, HOOK_JUMP).install()->quick();

		// Some value obfuscation
		utils::hook(0x493B81, 0x493BFC, HOOK_JUMP).install()->quick();

		// CEG uninitialization
		utils::hook::set<BYTE>(0x527110, 0xC3);
	}
};

REGISTER_MODULE(ceg)
