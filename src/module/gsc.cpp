#include <std_include.hpp>
#include "gsc.hpp"
#include "gsc_script_names.hpp"
#include "ps3style_assets.hpp"
#include "dedi_native.hpp"
#include "server_addresses.hpp"
#include "command.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"
#include "utils/io.hpp"
#include "utils/string.hpp"

namespace
{
	// ---- iw5mp 1.9.388110 (the dedicated server's: dedi_native.hpp; on both: server_addresses.hpp) ----
	// GScr_LoadScripts after the game type and the map script: a call to an empty function (0x71D130), the place for
	// our scripts (loading them before the map script made its lookup wait forever)
	constexpr DWORD load_custom_scripts_call = 0x523F43;
	constexpr DWORD start_game_type_call = 0x50D4E8; // G_InitGame -> the game type's main() thread 0x527AF0
	constexpr DWORD start_game_type_addr = 0x527AF0;
	constexpr DWORD scr_exec_thread_addr = 0x56E240; // unsigned short (int handle, unsigned int paramcount)
	constexpr DWORD scr_free_thread_addr = 0x569E20; // (unsigned short id)
	constexpr DWORD cl_console_print_addr = 0x487B00;
	constexpr DWORD pmem_alloc_addr = 0x5C15C0; // void* (size, alignment, type, source)
	constexpr unsigned int token_main = 0x672;
	constexpr unsigned int token_init = 0x680;

	struct script_file
	{
		const char* name;
		int compressed_len;
		int len;
		int bytecode_len;
		const char* buffer;
		unsigned char* bytecode;
	};

	static_assert(sizeof(script_file) == 0x18);

	struct gscbin_header
	{
		char magic[4];
		std::uint32_t compressed_len;
		std::uint32_t len;
		std::uint32_t bytecode_len;
	};

	struct script_entry
	{
		std::string file; // compiled .gscbin
		script_file* loaded = nullptr;
		unsigned int generation = 0;
	};

	std::string game_dir;
	std::vector<std::string> mod_dirs; // <game>\mods\<mod>
	std::unordered_map<std::string, script_entry> scripts; // name (lower case, '/') -> entry
	std::unordered_map<unsigned int, std::string> token_names; // 943 -> maps/mp/gametypes/_callbacksetup
	std::vector<std::string> autorun; // scripts/<name> of the top-level files in <game>\scripts
	std::vector<std::pair<int, int>> handles; // init, main of the autorun scripts this game
	unsigned int generation = 0;

	std::map<unsigned int, gsc::builtin> functions, methods;

	// the dedicated server prints with Com_Printf; what comes before its first game (the compiler's messages at start)
	// waits for that game
	bool dedi_game_loaded = false;
	std::vector<std::string> dedi_waiting_prints;

	void print(const std::string& text)
	{
		if (game::is_dedi())
		{
			if (dedi_game_loaded) dedi_native::com_printf(text.data());
			else dedi_waiting_prints.push_back(text);
			return;
		}

		reinterpret_cast<void(*)(int, int, const char*, int, int, int)>(cl_console_print_addr)(0, 0, text.data(), 0, 0, 0);
	}

	void log(const std::string& text)
	{
		utils::io::write_file(game_dir + "\\open-iw5\\gsc.log", text, true);
	}

	std::string normalize(std::string name)
	{
		std::replace(name.begin(), name.end(), '\\', '/');
		return utils::string::to_lower(name);
	}

	// gsc-tool -m comp -g iw5 -s pc <root> in <cache>: writes <cache>\compiled\iw5\<relative path>.gscbin
	std::string run_compiler(const std::string& root, const std::string& cache)
	{
		const auto tool = game_dir + "\\open-iw5\\gsc-tool.exe";
		if (!utils::io::file_exists(tool)) return "gsc-tool.exe is missing (" + tool + ")\n";

		std::error_code ec;
		std::filesystem::remove_all(cache + "\\compiled", ec);
		std::filesystem::create_directories(cache, ec);

		SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
		HANDLE read_pipe = nullptr, write_pipe = nullptr;
		if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return "could not start gsc-tool\n";
		SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOA si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdOutput = write_pipe;
		si.hStdError = write_pipe;
		PROCESS_INFORMATION pi{};
		auto command_line = "\"" + tool + "\" -m comp -g iw5 -s pc \"" + root + "\"";
		const auto started = CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
		                                    cache.data(), &si, &pi);
		CloseHandle(write_pipe);
		if (!started)
		{
			CloseHandle(read_pipe);
			return "could not start gsc-tool\n";
		}

		std::string output;
		char buffer[4096];
		DWORD read = 0;
		while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) && read) output.append(buffer, read);
		WaitForSingleObject(pi.hProcess, 60000);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		CloseHandle(read_pipe);
		return output;
	}

	// every compiled script of a root as <prefix><relative path>
	void index_compiled(const std::string& cache, const std::string& prefix)
	{
		const std::filesystem::path base = cache + "\\compiled\\iw5";
		std::error_code ec;
		for (auto it = std::filesystem::recursive_directory_iterator(base, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (ec) break;
			if (!it->is_regular_file() || it->path().extension() != ".gscbin") continue;

			auto relative = std::filesystem::relative(it->path(), base, ec).replace_extension().generic_string();
			auto& entry = scripts[normalize(prefix + relative)];
			entry.file = it->path().string();
			entry.loaded = nullptr;
		}
	}

	bool has_sources(const std::string& root)
	{
		std::error_code ec;
		for (auto it = std::filesystem::recursive_directory_iterator(root, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (ec) break;
			if (it->is_regular_file() && it->path().extension() == ".gsc") return true;
		}
		return false;
	}

	void compile_all()
	{
		scripts.clear();
		autorun.clear();
		mod_dirs.clear();
		utils::io::write_file(game_dir + "\\open-iw5\\gsc.log", "");

		const auto compile = [](const std::string& root, const std::string& cache_name, const std::string& prefix)
		{
			if (!has_sources(root)) return;
			const auto output = run_compiler(root, game_dir + "\\open-iw5\\gsc_cache\\" + cache_name);
			log("== " + root + "\n" + output);

			// gsc-tool prints a line per file: "compiled iw5/...", errors as "[ERROR] ..."
			std::istringstream lines(output);
			for (std::string line; std::getline(lines, line);)
			{
				if (line.find("ERROR") != std::string::npos || line.find("error") != std::string::npos)
				{
					print("^1GSC: " + line + "\n");
				}
			}
			index_compiled(game_dir + "\\open-iw5\\gsc_cache\\" + cache_name, prefix);
		};

		// mods first (sorted), the scripts folder last
		std::error_code ec;
		std::vector<std::string> names;
		for (const auto& entry : std::filesystem::directory_iterator(game_dir + "\\mods", ec))
		{
			const auto name = entry.path().filename().string();
			if (entry.is_directory() && !name.empty() && name[0] != '_' && name[0] != '.') names.push_back(name);
		}
		std::sort(names.begin(), names.end());
		for (const auto& name : names)
		{
			mod_dirs.push_back(game_dir + "\\mods\\" + name);
			compile(mod_dirs.back(), "mod_" + name, "");
		}

		const auto own = game_dir + "\\scripts";
		compile(own, "scripts", "scripts/");
		for (const auto& entry : std::filesystem::directory_iterator(own, ec))
		{
			if (entry.is_regular_file() && entry.path().extension() == ".gsc")
			{
				autorun.push_back(normalize("scripts/" + entry.path().stem().string()));
			}
		}

		log(utils::string::va("%u scripts, %u to run at game start\n", static_cast<unsigned int>(scripts.size()),
		                      static_cast<unsigned int>(autorun.size())));
	}

	script_file* load(const std::string& name, script_entry& entry)
	{
		std::string data;
		if (!utils::io::read_file(entry.file, &data) || data.size() < sizeof(gscbin_header)) return nullptr;

		const auto* header = reinterpret_cast<const gscbin_header*>(data.data());
		const auto payload = data.size() - sizeof(gscbin_header);
		if (std::memcmp(header->magic, "GSC\0", 4) != 0 || header->compressed_len > payload ||
		    header->bytecode_len != payload - header->compressed_len)
		{
			print("^1GSC: bad compiled file " + entry.file + "\n");
			return nullptr;
		}

		// the VM runs the bytecode in place and only inside its script memory (g_scriptmem, 2 MB: function handles are
		// offsets into it): PMem_Alloc (size, alignment, type, source 3 = script) from the high end, the end the map's
		// zone uses, so it is given back with the map (scripts are read again for every game)
		auto* bytecode = static_cast<unsigned char*>(game::is_dedi()
			? dedi_native::pmem_alloc(header->bytecode_len + 1, 4, 0, 3)
			: reinterpret_cast<void*(*)(unsigned int, unsigned int, unsigned int, int)>(pmem_alloc_addr)(header->bytecode_len + 1, 4, 0, 3));
		if (!bytecode)
		{
			print("^1GSC: no script memory left for " + name + "\n");
			return nullptr;
		}

		auto* file = static_cast<script_file*>(calloc(1, sizeof(script_file)));
		auto* stored_name = static_cast<char*>(calloc(1, name.size() + 1));
		auto* buffer = static_cast<char*>(calloc(1, header->compressed_len + 1));
		std::memcpy(stored_name, name.data(), name.size());
		std::memcpy(buffer, data.data() + sizeof(gscbin_header), header->compressed_len);
		std::memcpy(bytecode, data.data() + sizeof(gscbin_header) + header->compressed_len, header->bytecode_len);

		file->name = stored_name;
		file->compressed_len = static_cast<int>(header->compressed_len);
		file->len = static_cast<int>(header->len);
		file->bytecode_len = static_cast<int>(header->bytecode_len);
		file->buffer = buffer;
		file->bytecode = bytecode;
		return file;
	}

	// the game's own builtins we replaced (for wrappers that pass other calls on)
	std::map<unsigned int, gsc::builtin> original_functions, original_methods;

	void install_builtins()
	{
		for (const auto& [id, function] : functions)
		{
			auto& entry = game::native::scr_globalFunctions[id];
			if (reinterpret_cast<gsc::builtin>(entry) != function) original_functions[id] = reinterpret_cast<gsc::builtin>(entry);
			entry = reinterpret_cast<game::native::scr_call_t>(function);
		}
		for (const auto& [id, method] : methods)
		{
			auto& entry = game::native::scr_instanceFunctions[id];
			if (reinterpret_cast<gsc::builtin>(entry) != method) original_methods[id] = reinterpret_cast<gsc::builtin>(entry);
			entry = reinterpret_cast<game::native::scr_call_t>(method);
		}
	}

	// ---- session mode (private match / online / system link) ----
	// The lobby menus own these dvars. A script that writes one mid-match (the Xenia mod's R3XT set xblive_privatematch
	// 0) turns a private match into matchmaking: the human joins no team (PartyUsesCodeTeams 0x4A6C40 takes the team
	// from the party) and the end of the match goes to the public lobby (UI_GetMenuToReturnTo 0x5A4FB0 = 8 +
	// xblive_privatematch). Scripts only run on our own server, so their writes are dropped; a value changed some
	// other way during the match is put back when the match ends.
	constexpr DWORD xblive_privatematch_addr = 0x5A9701C; // dvar_t* (registered at 0x5CB29C)
	constexpr DWORD dvar_set_bool_addr = 0x5BF350; // Dvar_SetBool (dvar_t*, bool), as UI_SetActiveMenu 0x58F235
	constexpr DWORD g_shutdown_game_addr = 0x50C100; // G_ShutdownGame (int freeScripts)
	constexpr DWORD g_shutdown_game_calls[] = {0x573C82, 0x573D3A}; // SV_RestartGameProgs (map_restart), SV_ShutdownGameProgs
	constexpr DWORD ui_get_menu_to_return_to_addr = 0x5A4FB0; // int (int localClientNum)
	constexpr DWORD ui_get_menu_to_return_to_calls[] = {0x5A5025, 0x5A5045}; // its only callers (two small wrappers)
	// builtin ids (gsc-tool iw5 pc; game table 0x8ABDA8, methods 0x7FD630)
	constexpr unsigned int setdvar_id = 42; // GScr_SetDvar 0x517AB0
	constexpr unsigned int setdynamicdvar_id = 43;
	constexpr unsigned int setdevdvar_id = 45; // developer alias of 42
	constexpr unsigned int makedvarserverinfo_id = 332;
	constexpr unsigned int setclientdvar_id = 0x82C1; // 0x4FE5B0, sends "q <dvar> <value>" to the client

	int match_private_match = -1; // xblive_privatematch when this match started, -1 = no match of ours running

	game::native::dvar_t* private_match_dvar()
	{
		return *reinterpret_cast<game::native::dvar_t**>(xblive_privatematch_addr);
	}

	void remember_private_match()
	{
		const auto* dvar = private_match_dvar();
		match_private_match = dvar ? (dvar->current.enabled ? 1 : 0) : -1;
	}

	void restore_private_match(const char* where)
	{
		auto* dvar = private_match_dvar();
		if (match_private_match < 0 || !dvar || dvar->current.enabled == (match_private_match != 0)) return;

		log(utils::string::va("xblive_privatematch was changed during the match: set back to %i (%s)\n", match_private_match, where));
		reinterpret_cast<void(*)(game::native::dvar_t*, bool)>(dvar_set_bool_addr)(dvar, match_private_match != 0);
	}

	// a builtin writing a session dvar (param 0 the name, 1 the value): logged and dropped
	bool drop_session_dvar_write(const char* builtin)
	{
		const std::string name = gsc::get_string(0);
		if (_stricmp(name.data(), "xblive_privatematch") != 0 && _stricmp(name.data(), "onlinegame") != 0 &&
		    _stricmp(name.data(), "systemlink") != 0) return false;

		const std::string value = gsc::get_string(1);
		log("ignored script " + std::string(builtin) + " " + name + " \"" + value + "\" (the lobby sets the session mode)\n");
		return true;
	}

	void call_original(const std::map<unsigned int, gsc::builtin>& originals, const unsigned int id, const unsigned int entref)
	{
		const auto itr = originals.find(id);
		if (itr != originals.end() && itr->second) itr->second(entref);
	}

	void gscr_set_dvar(const unsigned int entref)
	{
		if (!drop_session_dvar_write("setdvar")) call_original(original_functions, setdvar_id, entref);
	}

	void gscr_set_dynamic_dvar(const unsigned int entref)
	{
		if (!drop_session_dvar_write("setdynamicdvar")) call_original(original_functions, setdynamicdvar_id, entref);
	}

	void gscr_set_dev_dvar(const unsigned int entref)
	{
		if (!drop_session_dvar_write("setdevdvar")) call_original(original_functions, setdevdvar_id, entref);
	}

	void gscr_make_dvar_server_info(const unsigned int entref)
	{
		if (!drop_session_dvar_write("makedvarserverinfo")) call_original(original_functions, makedvarserverinfo_id, entref);
	}

	void gscr_set_client_dvar(const unsigned int entref)
	{
		if (!drop_session_dvar_write("setclientdvar")) call_original(original_methods, setclientdvar_id, entref);
	}

	// the match is over (also before a map_restart): a changed value goes back, later changes belong to the menus
	void __cdecl shutdown_game_stub(const int free_scripts)
	{
		reinterpret_cast<void(*)(int)>(g_shutdown_game_addr)(free_scripts);
		restore_private_match("game shutdown");
		match_private_match = -1;
	}

	// asked while our match still runs (else the shutdown above already did it)
	int __cdecl get_menu_to_return_to_stub(const int local_client_num)
	{
		restore_private_match("menu to return to");
		return reinterpret_cast<int(*)(int)>(ui_get_menu_to_return_to_addr)(local_client_num);
	}

	void close_all_files();

	// ---- hooks ----
	void __cdecl load_scripts_stub()
	{
		++generation; // a new script session: scripts are read again
		handles.clear();
		close_all_files();

		if (game::is_dedi() && !dedi_game_loaded)
		{
			dedi_game_loaded = true;
			for (const auto& text : dedi_waiting_prints) print(text);
			dedi_waiting_prints.clear();
		}

		reinterpret_cast<void(*)()>(server_addresses::get().load_scripts)();
	}

	void __cdecl load_custom_scripts_stub()
	{
		const auto& addresses = server_addresses::get();
		install_builtins();
		for (const auto& name : autorun)
		{
			if (!reinterpret_cast<int(*)(const char*)>(addresses.scr_load_script)(name.data()))
			{
				print("^1GSC: could not load " + name + "\n");
				continue;
			}
			const auto init = reinterpret_cast<int(*)(const char*, unsigned int)>(addresses.scr_get_function_handle)(name.data(), token_init);
			const auto main = reinterpret_cast<int(*)(const char*, unsigned int)>(addresses.scr_get_function_handle)(name.data(), token_main);
			handles.emplace_back(init, main);
			log(utils::string::va("loaded %s: init %s, main %s\n", name.data(), init ? "yes" : "no", main ? "yes" : "no"));
		}
	}

	// dedicated server: GScr_LoadScripts -> the map script's loader, then ours (dedi_native.hpp)
	void __cdecl load_map_script_stub_dedi()
	{
		reinterpret_cast<void(*)()>(dedi_native::load_map_script_addr)();
		load_custom_scripts_stub();
	}

	void run_thread(const int handle)
	{
		if (!handle) return;
		if (game::is_dedi())
		{
			dedi_native::scr_free_thread(dedi_native::scr_exec_thread(handle, 0));
			return;
		}

		const auto id = reinterpret_cast<unsigned short(*)(int, unsigned int)>(scr_exec_thread_addr)(handle, 0);
		reinterpret_cast<void(*)(unsigned short)>(scr_free_thread_addr)(id);
	}

	void run_autorun_threads()
	{
		log(utils::string::va("game start: %u scripts\n", static_cast<unsigned int>(handles.size())));
		for (const auto& [init, main] : handles) run_thread(init);
		for (const auto& [init, main] : handles) run_thread(main);
	}

	void __cdecl start_game_type_stub()
	{
		remember_private_match(); // before the game type's scripts run (also after a map_restart, which loads none)
		reinterpret_cast<void(*)()>(start_game_type_addr)();
		run_autorun_threads();
	}

	void __cdecl start_game_type_freed_dedi(const unsigned int id)
	{
		dedi_native::scr_free_thread(id);
		run_autorun_threads();
	}

	// dedicated server: G_InitGame frees the game type's start thread (eax = its id) right after running it
	// (dedi_native.hpp); every register is kept for G_InitGame
	__declspec(naked) void start_game_type_free_stub_dedi()
	{
		__asm
		{
			pushad
			push eax
			call start_game_type_freed_dedi
			add esp, 4
			popad
			retn
		}
	}

	// dedicated server: the script loader's lookup of a script file (dedi_native.hpp); MP has ps3style's hook
	void* __cdecl find_script_asset_stub_dedi(const int type, const char* name, const int allow_create_default)
	{
		if (type == 39 && name) // ASSET_TYPE_SCRIPTFILE
		{
			if (auto* script = gsc::find_script(name)) return script;
		}
		return reinterpret_cast<void*(*)(int, const char*, int)>(dedi_native::db_find_x_asset_header_addr)(type, name, allow_create_default);
	}

	// dedicated server: a script that calls a function that doesn't exist stopped the server for good, or dropped its
	// map (the linker's Sys_Error / Com_Error, dedi_native.hpp). Such a call is linked to an empty function instead (it
	// does nothing and gives undefined), and the console and gsc.log say which script has it.
	const unsigned char* empty_function = nullptr; // in script memory, which every game gives back
	unsigned int empty_function_generation = 0;
	unsigned int unknown_function_reports = 0;
	DWORD link_unknown_function_text = 0;
	DWORD link_unknown_function_error = 0;
	DWORD link_next_call_site = 0;
	DWORD link_unknown_far_function_error = 0;
	DWORD link_far_function_return = 0;

	std::string script_containing(const unsigned char* code)
	{
		for (const auto& [name, entry] : scripts)
		{
			const auto* file = entry.loaded;
			if (file && entry.generation == generation && code >= file->bytecode && code < file->bytecode + file->bytecode_len) return name;
		}
		return {};
	}

	int __cdecl link_unknown_function_dedi(unsigned char* operand)
	{
		if (!empty_function || empty_function_generation != generation)
		{
			auto* code = static_cast<unsigned char*>(dedi_native::pmem_alloc(2, 4, 0, 3));
			if (!code) return 0;
			code[0] = 0x76; // OP_checkclearparams
			code[1] = 0x00; // OP_End
			empty_function = code;
			empty_function_generation = generation;
			unknown_function_reports = 0;
		}

		const auto distance = static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(empty_function) - reinterpret_cast<std::uintptr_t>(operand));
		if (distance <= -(1 << 21) || distance >= (1 << 21)) return 0;
		const auto value = static_cast<std::uint32_t>(distance * 4);
		operand[0] = static_cast<unsigned char>(value);
		operand[1] = static_cast<unsigned char>(value >> 8);
		operand[2] = static_cast<unsigned char>(value >> 16);

		const auto script = script_containing(operand);
		const auto text = "GSC: " + (script.empty() ? std::string("a stock script") : script) +
			" calls a function that doesn't exist: the call does nothing\n";
		log(text);
		if (++unknown_function_reports <= 10) print("^1" + text);
		return 1;
	}

	// a file not loaded yet: eax = the call site's entry
	__declspec(naked) void link_unknown_function_stub_dedi()
	{
		__asm
		{
			pushad
			push dword ptr [eax] // the operand
			call link_unknown_function_dedi
			add esp, 4
			test eax, eax
			popad
			jz link_error
			jmp dword ptr [link_next_call_site]

		link_error:
			push dword ptr [link_unknown_function_text]
			jmp dword ptr [link_unknown_function_error]
		}
	}

	// a file already loaded: the operand at [ebp + 8]
	__declspec(naked) void link_unknown_far_function_stub_dedi()
	{
		__asm
		{
			pushad
			push dword ptr [ebp + 8] // the operand
			call link_unknown_function_dedi
			add esp, 4
			test eax, eax
			popad
			jz far_link_error
			jmp dword ptr [link_far_function_return]

		far_link_error:
			push dword ptr [link_unknown_function_text]
			jmp dword ptr [link_unknown_far_function_error]
		}
	}

	// DB_IsXAssetDefault: Scr_LoadScript asks it about every script. For a name the asset pool doesn't have, the game's
	// own function returns 1 without releasing the pool's read lock (0x4CA87D, dedicated server 0x44EB89), and the next
	// lookup that has to wait for the loader waits forever - so our scripts, and names the pool doesn't have, are
	// answered here
	DWORD db_is_x_asset_default_resume = 0; // after the 5 bytes below (server_addresses.hpp)

	__declspec(naked) int db_is_x_asset_default_original(int /*type*/, const char* /*name*/)
	{
		__asm
		{
			push ebx
			mov ebx, [esp + 0Ch]
			jmp dword ptr [db_is_x_asset_default_resume]
		}
	}

	int __cdecl db_is_x_asset_default_stub(const int type, const char* name)
	{
		if (type == 39 && gsc::find_script(name)) return 0;
		const auto* loaded = !name ? nullptr : game::is_dedi() ? dedi_native::find_loaded_asset(type, name) : ps3style_assets::find_loaded(type, name);
		if (!loaded) return 1;
		return db_is_x_asset_default_original(type, name);
	}

	// ---- builtins: files (scriptdata), console ----
	std::map<int, FILE*> files;
	int next_file = 1;

	void close_all_files()
	{
		for (auto& [handle, file] : files) fclose(file);
		files.clear();
	}

	// openfile(<path>, "read" | "write" | "append"): a handle (1+) or -1
	void gscr_open_file(unsigned int)
	{
		if (gsc::num_params() != 2)
		{
			gsc::add_int(-1);
			return;
		}

		const std::string mode = gsc::get_string(1);
		const char* file_mode = mode == "read" ? "rt" : mode == "write" ? "wt" : mode == "append" ? "at" : nullptr;
		if (!file_mode || files.size() >= 32)
		{
			gsc::add_int(-1);
			return;
		}

		const std::string path = gsc::get_string(0);
		if (path.empty() || path.find("..") != std::string::npos || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos)
		{
			gsc::add_int(-1);
			return;
		}

		const auto full = gsc::mod_path(path, file_mode[0] != 'r');
		if (file_mode[0] != 'r')
		{
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(full).parent_path(), ec);
		}

		FILE* file = nullptr;
		if (fopen_s(&file, full.data(), file_mode) != 0 || !file)
		{
			gsc::add_int(-1);
			return;
		}

		const auto handle = next_file++;
		files[handle] = file;
		gsc::add_int(handle);
	}

	void gscr_close_file(unsigned int)
	{
		const auto itr = gsc::num_params() == 1 ? files.find(gsc::get_int(0)) : files.end();
		if (itr == files.end())
		{
			gsc::add_int(0);
			return;
		}
		fclose(itr->second);
		files.erase(itr);
		gsc::add_int(1);
	}

	// fprintln(<handle>, <text>)
	void gscr_fprintln(unsigned int)
	{
		const auto itr = gsc::num_params() == 2 ? files.find(gsc::get_int(0)) : files.end();
		if (itr == files.end())
		{
			gsc::add_int(0);
			return;
		}
		gsc::add_int(fprintf(itr->second, "%s\n", gsc::get_string(1)) >= 0 ? 1 : 0);
	}

	// freadln(<handle>): the next line, undefined at the end
	void gscr_freadln(unsigned int)
	{
		const auto itr = gsc::num_params() == 1 ? files.find(gsc::get_int(0)) : files.end();
		char line[4096];
		if (itr == files.end() || !fgets(line, sizeof(line), itr->second))
		{
			gsc::add_undefined();
			return;
		}

		auto length = std::strlen(line);
		while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
		gsc::add_string(line);
	}

	void gscr_println(unsigned int)
	{
		std::string text;
		for (auto i = 0; i < gsc::num_params(); ++i) text += gsc::get_string(i);
		print(text + "\n");
	}
}

void* gsc::find_script(const char* name)
{
	if (!name || !*name || scripts.empty()) return nullptr;

	// <game>\open-iw5\gsc_debug.txt there: every script the game asks for -> gsc.log
	static const bool debug = utils::io::file_exists(game_dir + "\\open-iw5\\gsc_debug.txt");
	if (debug) log(std::string("request ") + name + "\n");

	std::string key = name;
	if (std::all_of(key.begin(), key.end(), [](const unsigned char c) { return std::isdigit(c) != 0; }))
	{
		const auto itr = token_names.find(static_cast<unsigned int>(std::strtoul(key.data(), nullptr, 10)));
		if (itr == token_names.end()) return nullptr;
		key = itr->second;
	}

	const auto itr = scripts.find(normalize(key));
	if (itr == scripts.end()) return nullptr;

	auto& entry = itr->second;
	if (!entry.loaded || entry.generation != generation)
	{
		entry.loaded = load(name, entry);
		entry.generation = generation;
	}
	return entry.loaded;
}

void gsc::add_function(const unsigned int id, const builtin function)
{
	functions[id] = function;
}

void gsc::add_method(const unsigned int id, const builtin method)
{
	methods[id] = method;
}

gsc::builtin gsc::original_method(const unsigned int id)
{
	const auto itr = original_methods.find(id);
	return itr != original_methods.end() ? itr->second : nullptr;
}

void gsc::add_entity(void* entity)
{
	// Scr_AddEntity (0x52B060, gentity_s*)
	if (game::native::scr_VmPub->outparamcount) game::native::Scr_ClearOutParams();
	if (game::is_dedi()) dedi_native::scr_add_entity(entity);
	else reinterpret_cast<void(*)(void*)>(0x52B060)(entity);
}

int gsc::num_params()
{
	return static_cast<int>(game::native::scr_VmPub->outparamcount);
}

namespace
{
	const game::native::VariableValue* param(const int index)
	{
		if (index < 0 || index >= gsc::num_params()) return nullptr;
		return &game::native::scr_VmPub->top[-index];
	}
}

const char* gsc::get_string(const int index)
{
	const auto* value = param(index);
	if (!value) return "";
	switch (static_cast<int>(value->type))
	{
	case 2: // string
	case 3: // localized string
		return game::native::SL_ConvertToString(value->u.stringValue);
	case 5: return utils::string::va("%g", value->u.floatValue);
	case 6: return utils::string::va("%i", value->u.intValue);
	default: return "";
	}
}

int gsc::get_int(const int index)
{
	const auto* value = param(index);
	if (!value) return 0;
	if (value->type == game::native::SCRIPT_INTEGER) return value->u.intValue;
	if (value->type == game::native::SCRIPT_FLOAT) return static_cast<int>(value->u.floatValue);
	return 0;
}

float gsc::get_float(const int index)
{
	const auto* value = param(index);
	if (!value) return 0.0f;
	if (value->type == game::native::SCRIPT_FLOAT) return value->u.floatValue;
	if (value->type == game::native::SCRIPT_INTEGER) return static_cast<float>(value->u.intValue);
	return 0.0f;
}

void gsc::get_vector(const int index, float* out)
{
	const auto* value = param(index);
	out[0] = out[1] = out[2] = 0.0f;
	if (value && value->type == game::native::SCRIPT_VECTOR && value->u.vectorValue)
	{
		std::memcpy(out, value->u.vectorValue, sizeof(float) * 3);
	}
}

bool gsc::is_defined(const int index)
{
	const auto* value = param(index);
	return value && value->type != game::native::SCRIPT_NONE;
}

namespace
{
	game::native::VariableValue* push_value()
	{
		auto* vm = game::native::scr_VmPub;
		if (vm->outparamcount) game::native::Scr_ClearOutParams();
		auto* value = ++vm->top;
		++vm->inparamcount;
		value->type = game::native::SCRIPT_NONE;
		value->u.intValue = 0;
		return value;
	}
}

void gsc::add_int(const int value)
{
	auto* v = push_value();
	v->type = game::native::SCRIPT_INTEGER;
	v->u.intValue = value;
}

void gsc::add_float(const float value)
{
	auto* v = push_value();
	v->type = game::native::SCRIPT_FLOAT;
	v->u.floatValue = value;
}

void gsc::add_string(const char* value)
{
	auto* v = push_value();
	v->type = game::native::SCRIPT_STRING;
	v->u.stringValue = game::native::SL_GetString(value ? value : "", 0);
}

void gsc::add_undefined()
{
	push_value();
}

std::string gsc::mod_path(const std::string& path, const bool for_writing)
{
	for (const auto& dir : mod_dirs)
	{
		const auto full = dir + "\\" + path;
		if (utils::io::file_exists(full)) return full;
	}
	if (!mod_dirs.empty() && for_writing) return mod_dirs.front() + "\\" + path;
	return game_dir + "\\" + path;
}

void gsc::post_load()
{
	if (!game::is_mp() && !game::is_dedi()) return;

	char exe[MAX_PATH]{};
	GetModuleFileNameA(nullptr, exe, sizeof(exe));
	game_dir = std::filesystem::path(exe).parent_path().string();

	for (const auto& [id, name] : gsc_script_names::list) token_names[id] = name;

	compile_all();

	const auto& addresses = server_addresses::get();
	db_is_x_asset_default_resume = addresses.db_is_x_asset_default_resume;
	utils::hook(addresses.load_scripts_call, load_scripts_stub, HOOK_CALL).install()->quick();
	utils::hook(addresses.db_is_x_asset_default, db_is_x_asset_default_stub, HOOK_JUMP).install()->quick();

	if (game::is_dedi())
	{
		// no lobby there, so no session mode to keep
		utils::hook(dedi_native::load_map_script_call, load_map_script_stub_dedi, HOOK_CALL).install()->quick();
		utils::hook(dedi_native::start_game_type_free_call, start_game_type_free_stub_dedi, HOOK_CALL).install()->quick();
		utils::hook(dedi_native::script_find_x_asset_header_call, find_script_asset_stub_dedi, HOOK_CALL).install()->quick();

		link_unknown_function_text = dedi_native::link_unknown_function_text;
		link_unknown_function_error = dedi_native::link_unknown_function_error;
		link_next_call_site = dedi_native::link_next_call_site;
		link_unknown_far_function_error = dedi_native::link_unknown_far_function_error;
		link_far_function_return = dedi_native::link_far_function_return;
		utils::hook(dedi_native::link_unknown_function, link_unknown_function_stub_dedi, HOOK_JUMP).install()->quick();
		utils::hook(dedi_native::link_unknown_far_function, link_unknown_far_function_stub_dedi, HOOK_JUMP).install()->quick();
	}
	else
	{
		utils::hook(load_custom_scripts_call, load_custom_scripts_stub, HOOK_CALL).install()->quick();
		utils::hook(start_game_type_call, start_game_type_stub, HOOK_CALL).install()->quick();
		for (const auto call : g_shutdown_game_calls) utils::hook(call, shutdown_game_stub, HOOK_CALL).install()->quick();
		for (const auto call : ui_get_menu_to_return_to_calls) utils::hook(call, get_menu_to_return_to_stub, HOOK_CALL).install()->quick();
	}

	add_function(16, gscr_println);
	add_function(413, gscr_open_file);
	add_function(414, gscr_close_file);
	add_function(415, gscr_fprintln);
	add_function(417, gscr_freadln);

	add_function(setdvar_id, gscr_set_dvar);
	add_function(setdynamicdvar_id, gscr_set_dynamic_dvar);
	add_function(setdevdvar_id, gscr_set_dev_dvar);
	add_function(makedvarserverinfo_id, gscr_make_dvar_server_info);
	add_method(setclientdvar_id, gscr_set_client_dvar);
}

REGISTER_MODULE(gsc)
