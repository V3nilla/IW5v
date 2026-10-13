#pragma once
#include "game/game.hpp"

namespace game::scripting::safe_executer
{
	bool call(const native::scr_call_t function, const native::scr_entref_t entref);
	bool set_entity_field(const native::scr_entref_t entref, const int offset);
	bool get_entity_field(const native::scr_entref_t entref, const int offset, native::VariableValue* value);
}
