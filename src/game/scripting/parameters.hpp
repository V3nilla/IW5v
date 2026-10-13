#pragma once
#include "game/game.hpp"

namespace game::scripting
{
	class context;

	class parameters final
	{
	public:
		explicit parameters(context* context);

		void push(const chaiscript::Boxed_Value& value) const;
		chaiscript::Boxed_Value load(native::VariableValue value) const;

		chaiscript::Boxed_Value get_return_value() const;
	private:
		context* context_;
	};
}
