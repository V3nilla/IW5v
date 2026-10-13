#include "std_include.hpp"
#include "safe_executer.hpp"

#pragma warning(push)
#pragma warning(disable: 4611)

namespace game::scripting::safe_executer
{
	static_assert(sizeof(jmp_buf) == 64);

	bool call(const native::scr_call_t function, const native::scr_entref_t entref)
	{
		*native::g_script_error_level += 1;
		if (setjmp(native::g_script_error[*native::g_script_error_level]))
		{
			*native::g_script_error_level -= 1;
			return false;
		}

		function(entref.val);

		*native::g_script_error_level -= 1;
		return true;
	}

	bool set_entity_field(const native::scr_entref_t entref, const int offset)
	{
		*native::g_script_error_level += 1;
		if (setjmp(native::g_script_error[*native::g_script_error_level]))
		{
			*native::g_script_error_level -= 1;
			return false;
		}

		native::Scr_SetObjectField(entref.raw.classnum, entref.raw.entnum, offset);

		*native::g_script_error_level -= 1;
		return true;
	}

	bool get_entity_field(const native::scr_entref_t entref, const int offset, native::VariableValue* value)
	{
		*native::g_script_error_level += 1;
		if (setjmp(native::g_script_error[*native::g_script_error_level]))
		{
			value->type = native::SCRIPT_NONE;
			value->u.intValue = 0;
			*native::g_script_error_level -= 1;
			return false;
		}

		*value = native::GetEntityFieldValue(entref.raw.classnum, entref.raw.entnum, offset);

		*native::g_script_error_level -= 1;
		return true;
	}
}

#pragma warning(pop)
