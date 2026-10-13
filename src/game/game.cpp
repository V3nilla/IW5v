#include <std_include.hpp>
#include "game.hpp"

namespace game
{
	namespace native
	{
		Cmd_AddCommand_t Cmd_AddCommand;

		Com_Error_t Com_Error;

		DB_LoadXAssets_t DB_LoadXAssets;

		Dvar_SetFromStringByName_t Dvar_SetFromStringByName;

		G_RunFrame_t G_RunFrame;

		MSG_ReadData_t MSG_ReadData;

		MT_AllocIndex_t MT_AllocIndex;

		RemoveRefToValue_t RemoveRefToValue;

		SL_GetStringOfSize_t SL_GetStringOfSize;

		Sys_ShowConsole_t Sys_ShowConsole;

		VM_Notify_t VM_Notify;

		decltype(longjmp)* _longjmp;

		int* cmd_args;
		int* cmd_argc;
		const char*** cmd_argv;

		short* scrVarGlob;
		char** scrMemTreePub;
		char* scrMemTreeGlob;

		scrVmPub_t* scr_VmPub;

		scr_call_t* scr_instanceFunctions;
		scr_call_t* scr_globalFunctions;

		unsigned int* levelEntityId;

		int* g_script_error_level;
		jmp_buf* g_script_error;

		scr_classStruct_t* g_classMap;

		void AddRefToValue(VariableValue* value)
		{
			if (value->type == SCRIPT_OBJECT)
			{
				++scrVarGlob[4 * value->u.entityId];
			}
			else if ((value->type & ~1) == SCRIPT_STRING)
			{
				static const auto size = is_sp() ? 16 : 12;
				const auto ref_count = reinterpret_cast<unsigned volatile *>(*scrMemTreePub + size * value
				                                                                                     ->u.stringValue);
				InterlockedIncrement(ref_count);
			}
			else if (value->type == SCRIPT_VECTOR)
			{
				if (!*PBYTE(value->u.vectorValue - 1))
				{
					++*PWORD(value->u.vectorValue - 4);
				}
			}
		}

		__declspec(naked) unsigned int conbuf_append_text_dedicated(const char* message)
		{
			static DWORD func = 0x53C790;

			__asm
			{
				mov ecx, [esp + 4] // message
				call func
				retn
			}
		}

		void Conbuf_AppendText(const char* message)
		{
			if (is_dedi())
			{
				conbuf_append_text_dedicated(message);
			}
			else
			{
				reinterpret_cast<void(*)(const char*)>(SELECT_VALUE(0x4C84E0, 0x5CF610, 0))(message);
			}
		}

		__declspec(naked) unsigned int find_variable_dedicated(unsigned int parentId, unsigned int name)
		{
			static DWORD func = 0x4E7ED0;

			__asm
			{
				mov eax, [esp + 8] // name
				mov ecx, [esp + 4] // parentId
				call func
				retn
			}
		}

		unsigned int FindVariable(const unsigned int parentId, const unsigned int name)
		{
			if (is_dedi())
			{
				return find_variable_dedicated(parentId, name);
			}
			else
			{
				return reinterpret_cast<unsigned int(*)(unsigned int, unsigned int)> //
					(SELECT_VALUE(0x4C4E70, 0x5651F0, 0x0))(parentId, name);
			}
		}

		__declspec(naked) VariableValue get_entity_field_value_dedicated(unsigned int classnum, int entnum, int _offset)
		{
			static DWORD func = 0x4F1400;

			__asm
			{
				push [esp + 0Ch] // _offset
				push [esp + 0Ch] // entnum
				mov ecx, [esp + 0Ch] // classnum
				call func
				add esp, 8h
				retn
			}
		}

		VariableValue GetEntityFieldValue(const unsigned int classnum, const int entnum, const int offset)
		{
			if (is_dedi())
			{
				return get_entity_field_value_dedicated(classnum, entnum, offset);
			}
			else
			{
				return reinterpret_cast<VariableValue(*)(unsigned int, int, int)> //
					(SELECT_VALUE(0x530E30, 0x56AF20, 0x0))(classnum, entnum, offset);
			}
		}

		// the dedicated server's Dvar_SetFromStringByNameFromSource (0x532B30): name in eax; value, source (0, as MP's
		// Dvar_SetFromStringByName passes) on the stack
		__declspec(naked) void dvar_set_from_string_by_name_dedicated(const char* /*dvarName*/, const char* /*string*/)
		{
			static DWORD func = 0x532B30;

			__asm
			{
				mov eax, [esp + 4] // dvarName
				push 0 // source
				push [esp + 0Ch] // string
				call func
				add esp, 8h
				retn
			}
		}

		void* MT_Alloc(const int numBytes, const int type)
		{
			return scrMemTreeGlob + 12 * size_t(MT_AllocIndex(numBytes, type));
		}

		const float* Scr_AllocVector(const float* v)
		{
			const auto mem = static_cast<DWORD*>(MT_Alloc(16, 2));
			*mem = 0;

			const auto array = reinterpret_cast<float*>(mem + 1);
			array[0] = v[0];
			array[1] = v[1];
			array[2] = v[2];

			return array;
		}

		void Scr_ClearOutParams()
		{
			// the out params are the top values (top, top - 1, ...): released and popped, as the game's own does
			while (scr_VmPub->outparamcount)
			{
				RemoveRefToValue(scr_VmPub->top->type, scr_VmPub->top->u);
				--scr_VmPub->top;
				--scr_VmPub->outparamcount;
			}
		}

		scr_entref_t Scr_GetEntityIdRef(const unsigned int id)
		{
			static auto class_array = reinterpret_cast<DWORD*>(SELECT_VALUE(0x19AFC84, 0x1E72184, 0x1D3C804));
			static auto ent_array = reinterpret_cast<WORD*>(SELECT_VALUE(0x19AFC82, 0x1E72182, 0x1D3C802));

			scr_entref_t result{};
			result.raw.classnum = static_cast<unsigned short>(class_array[2 * id]) >> 8;
			result.raw.entnum = ent_array[4 * id];

			return result;
		}

		scr_call_t Scr_GetFunc(const unsigned int index)
		{
			if (index > 0x1C7)
			{
				return scr_instanceFunctions[index];
			}
			else
			{
				return scr_globalFunctions[index];
			}
		}

		__declspec(naked) void scr_notify_id_multiplayer(unsigned int id, unsigned int stringValue,
		                                                 unsigned int paramcount)
		{
			static DWORD func = 0x56B5E0;

			__asm
			{
				mov eax, [esp + 0Ch] // paramcount
				push [esp + 8] // stringValue
				push [esp + 8] // id
				call func
				add esp, 8h
				retn
			}
		}

		__declspec(naked) void scr_notify_id_singleplayer(unsigned int id, unsigned int stringValue,
		                                                  unsigned int paramcount)
		{
			static DWORD func = 0x610980;

			__asm
			{
				mov eax, [esp + 0Ch] // paramcount
				push [esp + 8] // stringValue
				push [esp + 8] // id
				call func
				add esp, 8h
				retn
			}
		}

		void Scr_NotifyId(unsigned int id, unsigned int stringValue, unsigned int paramcount)
		{
			if (is_mp())
			{
				return scr_notify_id_multiplayer(id, stringValue, paramcount);
			}
			else if (is_sp())
			{
				return scr_notify_id_singleplayer(id, stringValue, paramcount);
			}
			else
			{
				return reinterpret_cast<void(*)(unsigned int, unsigned int, unsigned int)>(0x4EFAA0)(
					id, stringValue, paramcount);
			}
		}

		__declspec(naked) int scr_set_object_field_dedicated(unsigned int classnum, int entnum, int _offset)
		{
			static DWORD func = 0x4B15C0;

			__asm
			{
				mov ecx, [esp + 0Ch] // _offset
				mov eax, [esp + 8] // entnum
				push [esp + 4] // classnum
				call func
				add esp, 4h
				retn
			}
		}

		int Scr_SetObjectField(const unsigned int classnum, const int entnum, const int offset)
		{
			if (is_dedi())
			{
				return scr_set_object_field_dedicated(classnum, entnum, offset);
			}
			else
			{
				return reinterpret_cast<int(*)(unsigned int, int, int)> //
					(SELECT_VALUE(0x42CAD0, 0x52BCC0, 0x0))(classnum, entnum, offset);
			}
		}

		const char* SL_ConvertToString(const unsigned int stringValue)
		{
			if (!stringValue) return nullptr;

			static const auto size = is_sp() ? 16 : 12;
			return *scrMemTreePub + size * stringValue + 4;
		}

		unsigned int SL_GetString(const char* str, const unsigned int user)
		{
			return SL_GetStringOfSize(str, user, strlen(str) + 1, 7);
		}
	}

	launcher::mode mode = launcher::mode::none;

	launcher::mode get_mode()
	{
		if (mode == launcher::mode::none)
		{
			throw std::runtime_error("Launcher mode not valid. Something must be wrong.");
		}

		return mode;
	}

	bool is_mp()
	{
		return get_mode() == launcher::mode::multiplayer;
	}

	bool is_sp()
	{
		return get_mode() == launcher::mode::singleplayer;
	}

	bool is_dedi()
	{
		return get_mode() == launcher::mode::server;
	}

	void initialize(const launcher::mode _mode)
	{
		mode = _mode;

		native::Cmd_AddCommand = native::Cmd_AddCommand_t(SELECT_VALUE(0x558820, 0x545DF0, 0));

		native::Com_Error = native::Com_Error_t(SELECT_VALUE(0x425540, 0x555450, 0x4D93F0));

		native::DB_LoadXAssets = native::DB_LoadXAssets_t(SELECT_VALUE(0x48A8E0, 0x4CD020, 0x44F770));

		native::Dvar_SetFromStringByName = is_dedi()
			? native::dvar_set_from_string_by_name_dedicated
			: native::Dvar_SetFromStringByName_t(SELECT_VALUE(0x4DD090, 0x5BF740, 0));

		native::G_RunFrame = native::G_RunFrame_t(SELECT_VALUE(0x52EAA0, 0x50CB70, 0x48AD60));

		native::MSG_ReadData = native::MSG_ReadData_t(SELECT_VALUE(0, 0x5592A0, 0));

		native::MT_AllocIndex = native::MT_AllocIndex_t(SELECT_VALUE(0x4B9610, 0x562080, 0x4E6C30));

		native::RemoveRefToValue = native::RemoveRefToValue_t(SELECT_VALUE(0x477EA0, 0x565730, 0x4E8A40));

		native::SL_GetStringOfSize = native::SL_GetStringOfSize_t(SELECT_VALUE(0x4E13F0, 0x564650, 0x4E7490));

		native::Sys_ShowConsole = native::Sys_ShowConsole_t(SELECT_VALUE(0x470AF0, 0x5CF590, 0));

		native::VM_Notify = native::VM_Notify_t(SELECT_VALUE(0x610200, 0x569720, 0x4EF450));

		native::_longjmp = reinterpret_cast<decltype(longjmp)*>(SELECT_VALUE(0x73AC20, 0x7363BC, 0x655558));

		native::cmd_args = reinterpret_cast<int*>(SELECT_VALUE(0x1750750, 0x1C978D0, 0x1B455F8));
		native::cmd_argc = reinterpret_cast<int*>(SELECT_VALUE(0x1750794, 0x1C97914, 0x1B4563C));
		native::cmd_argv = reinterpret_cast<const char***>(SELECT_VALUE(0x17507B4, 0x1C97934, 0x1B4565C));

		native::scrVarGlob = reinterpret_cast<short*>(SELECT_VALUE(0x19AFC80, 0x1E72180, 0x1D3C800));
		native::scrMemTreePub = reinterpret_cast<char**>(SELECT_VALUE(0x196FB00, 0x1E32000, 0x1C152A4));
		native::scrMemTreeGlob = reinterpret_cast<char*>(SELECT_VALUE(0x186DA00, 0x1D6FF00, 0x1C16600));

		native::scr_VmPub = reinterpret_cast<native::scrVmPub_t*>(SELECT_VALUE(0x1BF2580, 0x20B4A80, 0x1F5B080));

		native::scr_instanceFunctions = reinterpret_cast<native::scr_call_t*>(SELECT_VALUE(0x184CDB0, 0x1D4F258,
		                                                                                   0x1BF59C8));
		native::scr_globalFunctions = reinterpret_cast<native::scr_call_t*>(SELECT_VALUE(0x186C68C, 0x1D6EB34,
		                                                                                 0x1C152A4
		));

		native::g_script_error_level = reinterpret_cast<int*>(SELECT_VALUE(0x1BEFCFC, 0x20B21FC, 0x1F5B058));
		native::g_script_error = reinterpret_cast<jmp_buf*>(SELECT_VALUE(0x1BF1D18, 0x20B4218, 0x1F5A818));

		native::g_classMap = reinterpret_cast<native::scr_classStruct_t*>(SELECT_VALUE(0x92D140, 0x8B4300, 0x7C0408));

		native::levelEntityId = reinterpret_cast<unsigned int*>(SELECT_VALUE(0x1BCBCA4, 0x208E1A4, 0x1CD873C));
	}
}
