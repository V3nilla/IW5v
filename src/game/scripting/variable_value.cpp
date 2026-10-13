#include "std_include.hpp"
#include "variable_value.hpp"

namespace game::scripting
{
	variable_value::variable_value(native::VariableValue value) : value_(value)
	{
		native::AddRefToValue(&value);
	}

	variable_value::~variable_value()
	{
		native::RemoveRefToValue(this->value_.type, this->value_.u);
	}

	variable_value::operator native::VariableValue() const
	{
		return this->value_;
	}
}
