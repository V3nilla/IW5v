#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"

// Fixes for remote crash / abuse exploits of the stock game, as IW4x has them, for the MP exe (client and listen host)
// and the dedicated server. All are one-time static patches written before the game starts.
namespace
{
	using game::native::dvar_t;

	// dvar_t::flags (IW5 values, from Dvar_SetVariant 0x5BD920 and the Dvar_Register* calls) a command may not write:
	// 0x2 latched (sv_maxclients), 0x4 cheat, 0x100 external (made by "set" / Dvar_SetStringByName 0x5BF660), 0x800 write
	// protected (net_queryPort, fs_homepath), 0x2000 read only (shortversion)
	constexpr unsigned int protected_dvar_flags = 0x2 | 0x4 | 0x100 | 0x800 | 0x2000;

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

	// the command list (0x1C97958, the one Cmd_AddCommand 0x545DF0 links into; open-iw5's commands are on it too)
	bool is_command(const char* name)
	{
		for (auto* cmd = *reinterpret_cast<game::native::cmd_function_t**>(0x1C97958); cmd; cmd = cmd->next)
		{
			if (cmd->name && _stricmp(cmd->name, name) == 0) return true;
		}

		return false;
	}
}

class security final : public module
{
public:
	void post_load() override
	{
		if (game::is_sp()) return;

		if (game::is_mp()) patch_mp();
		else if (game::is_dedi()) patch_dedi();
	}

private:
	static void patch_mp()
	{
		// SV_PacketEvent (0x576830): a reliableAcknowledge (client_t +0x20E74) 0x80 or more behind reliableSequence
		// (+0x20E70) ends the packet (acknowledge = sequence), but the compare is signed (jl): an acknowledge ahead of
		// the sequence passes and SV_UpdateServerCommandsToClient (0x57B960) then sends from it up to the sequence, up
		// to 2^31 times (the server freezes). jl -> jb: a negative difference takes the same way out, as ioquake3 has it
		utils::hook::set<BYTE>(0x576965, 0x72);

		// pa_memberjoin (party handler 0x4AEA20, on the host): the Steam ticket length (MSG_ReadShort 0x558FB0, signed)
		// is read into a 0x200 stack buffer with no bound, then passed with the buffer to the Steam auth (0x4AEEE4)
		utils::hook(0x4AECB7, read_ticket_length_stub, HOOK_CALL).install()->quick();

		// selectStringTableEntryInDvar (CL_SelectStringTableEntryInDvar_f 0x58ED00, registered by the push 0x58ED00
		// at 0x493038 in CL_InitOnceForAllClients) sets its dvar as the game's own code (source 0), past every dvar
		// protection
		utils::hook::set(0x493039, &select_string_table_entry_in_dvar_stub);
	}

	static void patch_dedi()
	{
		// SV_PacketEvent (0x4FE40D): the same signed reliableAcknowledge compare as MP 0x576965
		utils::hook::set<BYTE>(0x4FE51D, 0x72);

		// SV_DirectConnect (0x4F74C0): "invited" 1 in the connect userinfo starts the free slot search at 0, past the
		// sv_privateClients slots kept for sv_privatePassword. A dedicated server has no party to invite to: ignored
		// (jne -> jmp). Not on MP, where invited friends join private / party matches that way
		utils::hook::set<BYTE>(0x4F7BF4, 0xEB);
	}

	// 0 (nothing to read: jbe 0x4AECC8 skips the MSG_ReadData) for a negative or too long ticket, so the kept length
	// matches the buffer
	static int read_ticket_length_stub(game::native::msg_t* msg)
	{
		const auto length = reinterpret_cast<int(*)(game::native::msg_t*)>(0x558FB0)(msg);
		return (length < 0 || length > 0x200) ? 0 : length;
	}

	// selectStringTableEntryInDvar <table> <column> <dvar>: not into a command's name (as IW4x) or a protected dvar
	static void select_string_table_entry_in_dvar_stub()
	{
		const auto index = *game::native::cmd_args;
		if (game::native::cmd_argc[index] >= 4)
		{
			const auto* name = game::native::cmd_argv[index][3];
			if (!name) return;

			if (is_command(name))
			{
				printf("selectStringTableEntryInDvar: \"%s\" is a command\n", name);
				return;
			}

			const auto* dvar = find_dvar(name);
			if (dvar && (dvar->flags & protected_dvar_flags))
			{
				printf("selectStringTableEntryInDvar: \"%s\" is protected\n", name);
				return;
			}
		}

		reinterpret_cast<void(*)()>(0x58ED00)();
	}
};

REGISTER_MODULE(security)
