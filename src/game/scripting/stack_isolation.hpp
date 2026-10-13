#pragma once
#include "game/game.hpp"

namespace game::scripting
{
	class stack_isolation final
	{
	public:
		stack_isolation();
		~stack_isolation();

	private:
		native::VariableValue stack_[512]{};

		native::VariableValue* max_stack_;
		native::VariableValue* top_;
		unsigned int in_param_count_;
		unsigned int out_param_count_;
	};
}
