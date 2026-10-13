#include <std_include.hpp>
#include "dedi_native.hpp"

// The wrappers are naked: each loads the registers the dedicated server's function wants from its own (cdecl)
// arguments, esp-relative (a naked function has no frame of its own), and keeps the registers a cdecl caller expects
// to survive (ebx, esi, edi, ebp) when it has to load one of them.

void dedi_native::com_printf(const char* text)
{
	reinterpret_cast<void(*)(const char*, ...)>(0x4D8F20)("%s", text);
}

__declspec(naked) void* dedi_native::pmem_alloc(unsigned int /*size*/, unsigned int /*alignment*/, unsigned int /*type*/,
                                                int /*source*/)
{
	__asm
	{
		mov eax, [esp + 10h] // source
		mov ecx, [esp + 8] // alignment
		push [esp + 0Ch] // type
		push [esp + 8] // size
		mov edx, 533D10h
		call edx
		add esp, 8
		retn
	}
}

__declspec(naked) unsigned short dedi_native::scr_exec_thread(int /*handle*/, unsigned int /*paramcount*/)
{
	__asm
	{
		mov eax, [esp + 4] // handle
		push [esp + 8] // paramcount
		mov ecx, 1CD873Ch // levelEntityId: the thread runs on level
		mov ecx, [ecx]
		mov edx, 4F0170h
		call edx
		add esp, 4
		retn
	}
}

__declspec(naked) void dedi_native::scr_free_thread(unsigned int /*id*/)
{
	__asm
	{
		movzx eax, word ptr [esp + 4] // id
		mov ecx, 4E88C0h
		call ecx
		retn
	}
}

namespace
{
	__declspec(naked) void scr_add_entity_num(int /*entnum*/)
	{
		__asm
		{
			push esi
			push 0 // classnum: entity
			push [esp + 0Ch] // entnum
			mov eax, 4EA2F0h // Scr_GetEntityId
			call eax
			add esp, 8
			mov esi, eax
			mov eax, 4F0F90h // Scr_AddObject (clears the out params first, as Scr_AddEntity)
			call eax
			pop esi
			retn
		}
	}
}

void dedi_native::scr_add_entity(void* entity)
{
	scr_add_entity_num(*static_cast<int*>(entity)); // s.number
}

__declspec(naked) void dedi_native::scr_add_string(const char* /*string*/)
{
	__asm
	{
		push edi
		mov edi, [esp + 8] // string
		mov eax, 4F1010h
		call eax
		pop edi
		retn
	}
}

__declspec(naked) void dedi_native::scr_notify_num(int /*entnum*/, unsigned int /*classnum*/, unsigned int /*string_value*/,
                                                   unsigned int /*paramcount*/)
{
	__asm
	{
		mov eax, [esp + 8] // classnum
		push [esp + 10h] // paramcount
		push [esp + 10h] // string_value
		push [esp + 0Ch] // entnum
		mov ecx, 4EFB40h
		call ecx
		add esp, 0Ch
		retn
	}
}

__declspec(naked) void* dedi_native::db_find_x_asset_entry(int /*type*/, const char* /*name*/)
{
	__asm
	{
		push edi
		mov edi, [esp + 0Ch] // name
		push [esp + 8] // type
		mov eax, 44E570h
		call eax
		add esp, 4
		pop edi
		retn
	}
}

void* dedi_native::find_loaded_asset(const int type, const char* name)
{
	// the asset hash's reader / writer lock: readers @170E96C, writers @170E970 (as DB_FindXAssetHeader 0x44E7A0)
	auto* readers = reinterpret_cast<volatile long*>(0x170E96C);
	auto* writers = reinterpret_cast<volatile long*>(0x170E970);
	InterlockedIncrement(readers);
	while (*writers) Sleep(0);
	const auto* entry = static_cast<void**>(db_find_x_asset_entry(type, name));
	auto* header = entry ? entry[1] : nullptr;
	InterlockedDecrement(readers);
	return header;
}

__declspec(naked) void* dedi_native::dvar_find_var(const char* /*name*/)
{
	__asm
	{
		push edi
		mov edi, [esp + 8] // name
		mov eax, 531320h
		call eax
		pop edi
		retn
	}
}

__declspec(naked) void* dedi_native::dvar_register_int(const char* /*name*/, int /*value*/, int /*min*/, int /*max*/,
                                                       unsigned int /*flags*/)
{
	__asm
	{
		push [esp + 10h] // max
		push [esp + 10h] // min
		push 0 // DvarValue: the value, then 12 bytes the int type doesn't use
		push 0
		push 0
		push [esp + 1Ch] // value
		push [esp + 2Ch] // flags
		push 5 // DVAR_TYPE_INT
		mov eax, [esp + 24h] // name
		mov ecx, 531F70h
		call ecx
		add esp, 20h
		retn
	}
}

__declspec(naked) void dedi_native::cbuf_add_text(int /*local_client_num*/, const char* /*text*/)
{
	__asm
	{
		mov eax, [esp + 8] // text
		mov ecx, [esp + 4] // local_client_num
		mov edx, 4CB5D0h
		call edx
		retn
	}
}

__declspec(naked) void dedi_native::sv_cmd_tokenize_string(const char* /*text*/)
{
	__asm
	{
		mov ecx, [esp + 4] // text
		mov eax, 4CBFB0h
		call eax
		retn
	}
}

__declspec(naked) void dedi_native::sv_send_client_game_state(void* /*client*/)
{
	__asm
	{
		mov eax, [esp + 4] // client
		mov ecx, 4F8B90h
		call ecx
		retn
	}
}

// for a wrapper that loads esi for MP's function; the dedicated server's keeps ebx, esi, edi and ebp
__declspec(naked) void dedi_native::sv_send_client_game_state_esi()
{
	__asm
	{
		mov eax, esi // client
		mov ecx, 4F8B90h
		jmp ecx
	}
}

__declspec(naked) void dedi_native::sv_client_enter_world(void* /*client*/, void* /*usercmd*/)
{
	__asm
	{
		mov edx, [esp + 8] // usercmd
		push [esp + 4] // client
		mov eax, 4F8E10h
		call eax
		add esp, 4
		retn
	}
}

__declspec(naked) void dedi_native::sv_client_think(void* /*client*/, void* /*usercmd*/)
{
	__asm
	{
		mov eax, [esp + 4] // client
		push [esp + 8] // usercmd
		mov ecx, 4F9910h
		call ecx
		add esp, 4
		retn
	}
}

__declspec(naked) void dedi_native::sv_drop_client(void* /*client*/, const char* /*reason*/, int /*tell_them*/)
{
	__asm
	{
		mov eax, [esp + 4] // client
		push [esp + 0Ch] // tell_them
		push [esp + 0Ch] // reason
		mov ecx, 4F8160h
		call ecx
		add esp, 8
		retn
	}
}

__declspec(naked) void dedi_native::dvar_set_from_string(const char* /*name*/, const char* /*value*/)
{
	__asm
	{
		mov eax, [esp + 4] // name
		push 0 // source: as MP's Dvar_SetFromStringByName
		push [esp + 0Ch] // value
		mov ecx, 532B30h
		call ecx
		add esp, 8
		retn
	}
}

__declspec(naked) void dedi_native::sv_trace(void* /*results*/, const float* /*start*/, const float* /*end*/,
                                             const float* /*bounds*/, const int* /*ignore*/, int /*content_mask*/,
                                             int /*locational*/, unsigned char* /*priority_map*/, int /*static_models*/)
{
	__asm
	{
		mov eax, [esp + 10h] // bounds
		push [esp + 24h] // static_models
		push [esp + 24h] // priority_map
		push [esp + 24h] // locational
		push [esp + 24h] // content_mask
		push [esp + 24h] // ignore
		push [esp + 20h] // end
		push [esp + 20h] // start
		push [esp + 20h] // results
		mov ecx, 5045F0h
		call ecx
		add esp, 20h
		retn
	}
}

// as MP's G_LocationalTracePassed: a point trace (the bounds 0x766D60 are zero), locational, static models too
__declspec(naked) int dedi_native::g_locational_trace_passed(const float* /*start*/, const float* /*end*/, int /*pass_entity*/,
                                                             int /*pass_entity2*/, int /*content_mask*/,
                                                             unsigned char* /*priority_map*/)
{
	__asm
	{
		push esi
		push edi
		mov edi, [esp + 10h] // end
		mov esi, 766D60h // bounds
		push 1 // staticModels
		push 1 // locational
		push [esp + 24h] // content_mask
		push [esp + 24h] // pass_entity2
		push [esp + 24h] // pass_entity
		push [esp + 20h] // start
		mov eax, 5047B0h
		call eax
		add esp, 18h
		pop edi
		pop esi
		retn
	}
}

__declspec(naked) int dedi_native::sv_entity_contact(const float* /*bounds*/, const void* /*entity*/)
{
	__asm
	{
		push edi
		mov edi, [esp + 8] // bounds
		mov eax, [esp + 0Ch] // entity
		mov ecx, 4FA430h
		call ecx
		pop edi
		retn
	}
}
