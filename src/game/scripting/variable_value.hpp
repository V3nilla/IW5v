#pragma once
#include "game/game.hpp"

namespace game::scripting
{
	class variable_value final
	{
	public:
		explicit variable_value(native::VariableValue value);
		~variable_value();

		explicit operator native::VariableValue() const;

	private:
		native::VariableValue value_;
	};
}
