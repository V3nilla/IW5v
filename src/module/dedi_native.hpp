#pragma once

// iw5mp_server.exe (the dedicated server, 1.9.388110): the game functions open-iw5's MP modules (gsc)
// call on iw5mp.exe, as the dedicated server has them. LTCG gave 19 of them register arguments there (and inlined
// 12), so they are called through the wrappers below, which take the MP (cdecl) arguments. Addresses that mean the
// same thing on both binaries are in server_addresses.hpp.
//
// function                         MP (cdecl)                       dedicated
// Com_Printf                       (CL_ConsolePrint 0x487B00)       0x4D8F20 (fmt, ...) cdecl
// PMem_Alloc                       0x5C15C0 (size, align, type, src) 0x533D10 eax = source, ecx = alignment; size, type
// Scr_ExecThread                   0x56E240 (handle, paramcount)    inlined: core 0x4F0170 eax = handle,
//                                                                    ecx = *levelEntityId; paramcount
// Scr_FreeThread                   0x569E20 (id)                    inlined: RemoveRefToObject 0x4E88C0 eax = id
// Scr_AddEntity                    0x52B060 (gentity_s*)            inlined: Scr_GetEntityId 0x4EA2F0 (entnum, classnum)
//                                                                    then Scr_AddObject 0x4F0F90 esi = id
// DB_FindXAssetEntry               0x4CA450 edi = type; name        0x44E570 edi = name; type
// Dvar_FindVar                     0x5BDC40 edi = name              0x531320 edi = name
// Dvar_RegisterInt                 0x5BEA40 (name, value, min, max, flags, description)
//                                                                    inlined: Dvar_RegisterVariant 0x531F70 eax = name;
//                                                                    type 5, flags, DvarValue[16], min, max
// Dvar_SetFromStringByName         0x5BF740 (name, value)           0x532B30 eax = name; value, source (game.cpp)
// Cbuf_AddText                     0x545680 (localClientNum, text)  0x4CB5D0 eax = text, ecx = localClientNum
// SV_Cmd_TokenizeString            0x545D40 (text)                  0x4CBFB0 ecx = text
// SV_SendClientGameState           0x570FC0 esi = client            0x4F8B90 eax = client
// SV_ClientEnterWorld              0x571100 (client, usercmd)       0x4F8E10 edx = usercmd; client
// SV_ClientThink                   0x571AE0 (client, usercmd)       0x4F9910 eax = client; usercmd
// SV_DropClient                    0x570980 (client, reason, tell)  0x4F8160 eax = client; reason, tellThem
// Scr_NotifyNum                    (Scr_Notify 0x52B190)            0x4EFB40 eax = classnum; entnum, stringValue,
//                                                                    paramcount
// Scr_AddString                    0x56AC00 (string)                0x4F1010 edi = string
// SV_Trace                         0x57D8A0 (results, start, end, bounds, ignore, mask, locational, priorityMap,
//                                  staticModels)                    0x5045F0 eax = bounds; the other 8 in MP's order
// G_LocationalTracePassed          0x50D050 (start, end, passEnt, passEnt2, mask, priorityMap)
//                                                                    inlined: SV_TracePassed 0x5047B0 esi = bounds,
//                                                                    edi = end; start, passEnt, passEnt2, mask,
//                                                                    locational, staticModels (no priorityMap: 0)
// SV_EntityContact                 0x573500 (bounds, gentity_s*)    0x4FA430 eax = gentity_s*, edi = bounds
// ("x; y" = x in registers, y on the stack in this order; the caller pops the stack arguments)
namespace dedi_native
{
	// ---- script loading (gsc): hook sites only the dedicated server has ----
	// GScr_LoadScripts 0x4A7250 -> the map script's loader 0x4A71D0 (MP 0x523DA0): our scripts load right after it (MP
	// has a call to an empty function there, 0x523F43; the dedicated server has none)
	constexpr DWORD load_map_script_call = 0x4A738C;
	constexpr DWORD load_map_script_addr = 0x4A71D0;
	// G_InitGame 0x489AB0 runs the game type's start thread inline (MP calls 0x527AF0): ExecThread(handle [0x1B3AF50])
	// at 0x489F97, then this call frees the thread (eax = its id); our init / main threads run after it. The next
	// instructions load eax, ecx and edx again.
	constexpr DWORD start_game_type_free_call = 0x489FA2;
	// the script loader's asset lookup (0x4F1740, eax = name: DB_FindXAssetHeader(39, name, 1), then
	// DB_IsXAssetDefault(39, name)) - its only lookup of script files: our scripts come from here (MP: ps3style's
	// DB_FindXAssetHeader hook)
	constexpr DWORD script_find_x_asset_header_call = 0x4F174E;
	constexpr DWORD db_find_x_asset_header_addr = 0x44E7A0; // void* (XAssetType, const char*, int allowCreateDefault)
	// The script linker on a call to a function that doesn't exist (a call site's operand: 3 bytes, (target - operand)
	// * 4; "unknown function" 0x7774F0):
	// - into a file not loaded yet, linked when that file is (0x4E5D20, every call site of the function in turn):
	//   push text; call 0x4E7170 = Sys_Error, which waits on a message box and then in its message loop for good (with
	//   +unattended its exit(-1) hangs too). There eax = the call site's entry ([eax] = the operand); esi, edi, ebx and
	//   the stack are the loop's, which goes on with the next call site at 0x4E5D96
	// - into a file already loaded (0x4E5BC0, operand [ebp + 8]): push text; push 1; call Com_Error: the map is
	//   dropped and the server stays without one. 0x4E5C5E is that function's own return
	// MP has the same errors (0x5615F2, 0x561609 -> Sys_Error 0x5CC3B0; 0x561458 Com_Error)
	constexpr DWORD link_unknown_function_text = 0x7774F0;
	constexpr DWORD link_unknown_function = 0x4E5DCA;
	constexpr DWORD link_unknown_function_error = 0x4E5DCF;
	constexpr DWORD link_next_call_site = 0x4E5D96;
	constexpr DWORD link_unknown_far_function = 0x4E5C20;
	constexpr DWORD link_unknown_far_function_error = 0x4E5C25;
	constexpr DWORD link_far_function_return = 0x4E5C5E;

	// ---- test clients ----
	// the same as on MP (cdecl, no register arguments)
	constexpr DWORD sv_cmd_end_tokenized_string = 0x4CBFE0; // MP 0x545D70
	constexpr DWORD sv_direct_connect = 0x4F74C0; // (netadr_t), MP 0x572750
	constexpr DWORD sv_calc_pings = 0x4FE570; // MP 0x576AC0
	constexpr DWORD stat_version = 0x4D0390; // playerdata.def's version, MP 0x548D60
	constexpr DWORD stat_checksum = 0x4D03D0; // and its checksum, MP 0x548D80
	// what SV_DirectConnect compares a connect's "protocol" and "checksum" with: the protocol inline (0x4F7556, MP
	// GetProtocolVersion 0x5BC190 returns the same), the checksum as SV_SpawnServer kept it (0x4FB9C0, from
	// BG_NetDataChecksum 0x40C8C0)
	constexpr int protocol_version = 0x507C;
	constexpr DWORD net_data_checksum = 0x1FD82E8;
	constexpr DWORD sv_send_client_game_state_addr = 0x4F8B90; // eax = client (sv_send_client_game_state below)
	// client_t: 4 bytes less than MP's from +0x41C9C on (statsId MP +0x452BE), 8 from +0x452C0; 1023 = no stats to
	// wait for (else SV_SendClientGameState sends "requeststats" and nothing else)
	constexpr DWORD cl_stats_id = 0x452BA;
	constexpr DWORD scr_const_menuresponse = 0x1B42118; // unsigned short, MP 0x1C8E170
	// a dvar flag: only dvars that have it can be printed or set from the console, set and seta (Cmd_ExecuteString
	// 0x4CC5AD, the print 0x530AD1; sv_hostname is registered with 0x10400)
	constexpr unsigned int dvar_flag_console = 0x10000;
	// level.startTime (level 0x1B21A20, time @+0x4A8): G_InitGame 0x489AB0 sets both to the server time it is
	// given, which goes on across maps, so a new value is a new game: a map, a map_restart or fast_restart
	constexpr DWORD level_start_time = 0x1B21ED4;
	// hook sites
	// SV_UpdateBots 0x4FEB70 -> SV_BotUserMove 0x4FE950 (esi = client) for every client with a state and address type
	// 0 -> SV_ClientThink: eax = client, the usercmd pushed; only edi and ebx (popped) after it
	constexpr DWORD sv_bot_user_move_think_call = 0x4FEB5A;
	// SV_Frame 0x4FEC50 -> SV_CalcPings, before SV_UpdateBots (0x4FEFC5) and G_RunFrame (0x4FEFD7); after it only
	// ebx, esi and ebp, which a cdecl function keeps
	constexpr DWORD sv_calc_pings_call = 0x4FEF62;
	// G_SelectWeapon (MP 0x5313F0): eax = clientNum, the weapon on the stack; push ebp; mov ebp, esp; and esp, -8 is
	// 6 bytes. Called by 0x4733A6, 0x47AAEE, 0x485BF6
	constexpr DWORD g_select_weapon = 0x4B7EE0;
	constexpr DWORD g_select_weapon_resume = 0x4B7EE6;
	// SV_DirectConnect: a connect that passed every check and found no free slot (from sv_privateClients on) falls
	// through to "EXE_SERVERISFULL" here; mov ecx, [esp + 450h] (7 bytes) starts it. 0x4F7C69 takes the slot in esi
	// (edi = 0: its reliable sequence / acknowledge are zeroed, then the whole client_t). The connecting address
	// (netadr_t, by value) is the function's argument at [esp + 450h], its type first
	constexpr DWORD direct_connect_server_full = 0x4F7C1F;
	constexpr DWORD direct_connect_server_full_resume = 0x4F7C26;
	constexpr DWORD direct_connect_slot_found = 0x4F7C69;
	// Dvar_RegisterVariant (0x531F70) registers nothing (returns 0) while this byte is 0 (as the server reads a DSR file)
	constexpr DWORD dvar_registration_open = 0x7B23CE;
	// the server config (sv_config): run once at start (0x4DAEDE -> 0x4D9B00, no arguments) with that byte 0 (mov byte
	// ptr [0x7B23CE], 0: 7 bytes), line by line (0x4CC4F0): only seta / bind / unbindall / gameOpt lines (0x7C74FC)
	// and "<dvar> <value>" for a dvar that has the console flag; set is ignored. So a seta of a dvar the game makes
	// later (the scripts' r3xt_*) did nothing, and one the code registers later without the archive flag is reset then
	constexpr DWORD server_config_exec_call = 0x4DAEDE;
	constexpr DWORD server_config_exec = 0x4D9B00;
	constexpr DWORD server_config_registration_closed = 0x4D9BD2;

	// ---- game globals ----
	constexpr DWORD level = 0x1B21A20; // level_locals_t (MP 0x1C6D4D8): clients @0, time @+0x4A8 as on MP
	constexpr DWORD cm = 0x1B45680; // clipMap_t (MP 0x1C91780): mapEnts @+0x98 as on MP (entity string 0x4B1EC4)
	constexpr DWORD gclient_size = 0x38A8; // gclient_s (MP 0x38EC): the same fields up to +0x3624, the bots read none after
	// by weapon index (& 0xFF), [0] "none", as the game reads them (0x401400 / 0x403607; MP 0x8DF9C0 / 0x8DFD88)
	constexpr DWORD weapon_complete_defs = 0x7DAB90;
	constexpr DWORD weapon_defs = 0x7DAF58;
	constexpr DWORD scr_mem_tree_pub = 0x1C152A4; // SL_ConvertToString: *it + 12 * id + 4 (0x4A61EC; MP 0x1E32000)
	constexpr DWORD cmd_args = 0x1B455F8; // CmdArgs as MP's (0x1C978D0): argc @+0x44, argv @+0x64
	// notifyOnPlayerCommand's table {int client (-1 any), ushort command, ushort notify}[256] and its count, read by
	// Cmd_CheckNotify 0x4CC2C0 (MP 0x1C9A198 / 0x1C9795C)
	constexpr DWORD cmd_notify = 0x1B4DFD8;
	constexpr DWORD cmd_notify_count = 0x1B4B79C;
	constexpr DWORD script_active = 0x1CD872C; // Cmd_CheckNotify notifies only while it isn't 0 (MP: Scr_IsSystemActive)
	constexpr DWORD configstrings = 0x1FB0486; // sv.configstrings: unsigned short string ids (MP 0x20FA3AE)
	constexpr DWORD cmd_functions = 0x1B4567C; // cmd_function_s* list (next @0, name @4, function @8; 0x4F5E60)
	// client_t: the persistent player data (GScr_GetPlayerData 0x4A6054; MP + 0x41CC1) and its modified bits (MP
	// + 0x44CBD)
	constexpr DWORD cl_persistent_data = 0x41CBD;
	constexpr DWORD cl_persistent_flags = 0x44CB9;
	// player_die (MP 0x503460), cdecl with MP's 10 arguments; the game reaches it through the entity handler table's
	// die slots and three direct calls (MOD_SUICIDE, 100000 damage: suicide, team change, disconnect); push ebp;
	// mov ebp, esp; and esp, -8 is 6 bytes
	constexpr DWORD player_die = 0x47F4D0;
	constexpr DWORD player_die_slots[] = {0x7C8728, 0x7C8750};
	constexpr DWORD player_die_calls[] = {0x46E240, 0x46E268, 0x47B60B};
	constexpr DWORD scr_add_int = 0x4F0E60; // cdecl (int), MP 0x56AA20

	// ---- functions with register arguments (the table above) ----
	void com_printf(const char* text);
	void* pmem_alloc(unsigned int size, unsigned int alignment, unsigned int type, int source);
	unsigned short scr_exec_thread(int handle, unsigned int paramcount);
	void scr_free_thread(unsigned int id);
	void scr_add_entity(void* entity); // gentity_s*
	void scr_add_string(const char* string);
	void scr_notify_num(int entnum, unsigned int classnum, unsigned int string_value, unsigned int paramcount);
	void* db_find_x_asset_entry(int type, const char* name);
	void* find_loaded_asset(int type, const char* name); // the XAssetHeader of a loaded asset, nullptr if none
	void* dvar_find_var(const char* name); // dvar_t*
	void* dvar_register_int(const char* name, int value, int min, int max, unsigned int flags); // dvar_t*
	void cbuf_add_text(int local_client_num, const char* text);
	void sv_cmd_tokenize_string(const char* text);
	void sv_send_client_game_state(void* client); // client_t*
	void sv_send_client_game_state_esi(); // esi = client_t*, as MP's SV_SendClientGameState takes it
	void sv_client_enter_world(void* client, void* usercmd);
	void sv_client_think(void* client, void* usercmd);
	void sv_drop_client(void* client, const char* reason, int tell_them);
	void dvar_set_from_string(const char* name, const char* value);
	void sv_trace(void* results, const float* start, const float* end, const float* bounds, const int* ignore, int content_mask,
	              int locational, unsigned char* priority_map, int static_models);
	int g_locational_trace_passed(const float* start, const float* end, int pass_entity, int pass_entity2, int content_mask,
	                              unsigned char* priority_map);
	int sv_entity_contact(const float* bounds, const void* entity); // gentity_s*
}
