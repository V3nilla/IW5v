#pragma once
#include "loader/module_loader.hpp"

// GSC scripts for MW3 PC, compiled with gsc-tool (xensik; <game>\open-iw5\gsc-tool.exe) when the game starts:
//   <game>\scripts\*.gsc        own scripts (as IW4x's scripts folder): each top-level file's init() and main() run
//                               when a game starts; files in subfolders are libraries (scripts\<sub>\<file>::func)
//   <game>\mods\<mod>\...       script trees (CoD Xe style): a file at a stock script's path replaces it
//                               (mods\<mod>\maps\mp\gametypes\_callbacksetup.gsc), others are new scripts they call;
//                               mods\<mod>\scriptdata\ is where openfile() reads / writes. Folders starting with _ are off.
// Compiler messages: the in-game console (the dedicated server's console from its first map) and
// <game>\open-iw5\gsc.log. Runs on MP and on the dedicated server (no session mode there: dedi_native.hpp).
class gsc final : public module
{
public:
	void post_load() override;

	// the ScriptFile (asset) for a script name ("maps/mp/gametypes/_callbacksetup" or its number "943"), nullptr
	// when no script of ours has that name
	static void* find_script(const char* name);

	// a builtin function / method of ours: the game's builtin table entry id (functions < 0x8000, methods 0x8000+)
	using builtin = void(*)(unsigned int entref);
	static void add_function(unsigned int id, builtin function);
	static void add_method(unsigned int id, builtin method);
	// the game's builtin a method of ours replaced (nullptr before the first game)
	static builtin original_method(unsigned int id);

	// the script VM, for builtins
	static int num_params();
	static const char* get_string(int index);
	static int get_int(int index);
	static float get_float(int index);
	static void get_vector(int index, float* out);
	static bool is_defined(int index);
	static void add_int(int value);
	static void add_float(float value);
	static void add_string(const char* value);
	static void add_undefined();
	static void add_entity(void* entity); // gentity_s*

	// <game>\mods\<mod>\<path> of the first mod that has it, else of the first mod (for writing), else <game>\<path>
	static std::string mod_path(const std::string& path, bool for_writing);
};
