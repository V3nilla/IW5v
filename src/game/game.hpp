#pragma once

#include "structs.hpp"
#include "launcher/launcher.hpp"

#define SELECT_VALUE(sp, mp, dedi) (game::is_sp() ? (sp) : (game::is_mp() ? (mp) : (dedi)))

#define SERVER_CD_KEY "Open-IW5-CD-Key"

namespace game
{
	namespace native
	{
		typedef void (*Cmd_AddCommand_t)(const char* cmdName, void (*function)(), cmd_function_t* allocedCmd);
		extern Cmd_AddCommand_t Cmd_AddCommand;

		typedef void (*Com_Error_t)(int code, const char* fmt, ...);
		extern Com_Error_t Com_Error;

		typedef void (*DB_LoadXAssets_t)(XZoneInfo* zoneInfo, unsigned int zoneCount, int sync);
		extern DB_LoadXAssets_t DB_LoadXAssets;

		typedef void (*Dvar_SetFromStringByName_t)(const char* dvarName, const char* string);
		extern Dvar_SetFromStringByName_t Dvar_SetFromStringByName;

		typedef int (*G_RunFrame_t)(int, int);
		extern G_RunFrame_t G_RunFrame;

		typedef void (*MSG_ReadData_t)(msg_t* msg, void* data, int len);
		extern MSG_ReadData_t MSG_ReadData;

		typedef void* (*MT_AllocIndex_t)(int numBytes, int type);
		extern MT_AllocIndex_t MT_AllocIndex;

		typedef void (*RemoveRefToValue_t)(scriptType_e type, VariableUnion u);
		extern RemoveRefToValue_t RemoveRefToValue;

		typedef unsigned int (*SL_GetStringOfSize_t)(const char* str, unsigned int user, unsigned int len, int type);
		extern SL_GetStringOfSize_t SL_GetStringOfSize;

		typedef void (*Sys_ShowConsole_t)();
		extern Sys_ShowConsole_t Sys_ShowConsole;

		typedef void (*VM_Notify_t)(unsigned int notifyListOwnerId, unsigned int stringValue, VariableValue* top);
		extern VM_Notify_t VM_Notify;

		extern decltype(longjmp)* _longjmp;

		extern int* cmd_args;
		extern int* cmd_argc;
		extern const char*** cmd_argv;

		extern short* scrVarGlob;
		extern char** scrMemTreePub;
		extern char* scrMemTreeGlob;

		extern scrVmPub_t* scr_VmPub;

		extern scr_call_t* scr_instanceFunctions;
		extern scr_call_t* scr_globalFunctions;

		extern unsigned int* levelEntityId;

		extern int* g_script_error_level;
		extern jmp_buf* g_script_error;

		extern scr_classStruct_t* g_classMap;

		void AddRefToValue(VariableValue* value);

		void Conbuf_AppendText(const char* message);

		unsigned int FindVariable(unsigned int parentId, unsigned int name);

		VariableValue GetEntityFieldValue(unsigned int classnum, int entnum, int offset);

		void* MT_Alloc(int numBytes, int type);

		const float* Scr_AllocVector(const float* v);
		void Scr_ClearOutParams();
		scr_entref_t Scr_GetEntityIdRef(unsigned int id);
		scr_call_t Scr_GetFunc(unsigned int index);
		void Scr_NotifyId(unsigned int id, unsigned int stringValue, unsigned int paramcount);
		int Scr_SetObjectField(unsigned int classnum, int entnum, int offset);

		const char* SL_ConvertToString(unsigned int stringValue);
		unsigned int SL_GetString(const char* str, unsigned int user);
	}

	bool is_mp();
	bool is_sp();
	bool is_dedi();

	void initialize(launcher::mode mode);
}
