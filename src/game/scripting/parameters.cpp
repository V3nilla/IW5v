#include "std_include.hpp"
#include "context.hpp"

namespace game::scripting
{
	parameters::parameters(context* context) : context_(context)
	{
	}

	chaiscript::Boxed_Value parameters::load(const native::VariableValue value) const
	{
		if (value.type == native::SCRIPT_STRING)
		{
			const std::string string = native::SL_ConvertToString(value.u.stringValue);
			return chaiscript::var(string);
		}
		else if (value.type == native::SCRIPT_FLOAT)
		{
			return chaiscript::var(value.u.floatValue);
		}
		else if (value.type == native::SCRIPT_INTEGER)
		{
			return chaiscript::var(value.u.intValue);
		}
		else if (value.type == native::SCRIPT_OBJECT)
		{
			return chaiscript::var(entity(this->context_, value.u.entityId));
		}
		else if (value.type == native::SCRIPT_VECTOR)
		{
			std::vector<chaiscript::Boxed_Value> values;
			values.push_back(chaiscript::var(value.u.vectorValue[0]));
			values.push_back(chaiscript::var(value.u.vectorValue[1]));
			values.push_back(chaiscript::var(value.u.vectorValue[2]));

			return chaiscript::var(values);
		}

		return {};
	}

	void parameters::push(const chaiscript::Boxed_Value& value) const
	{
		if (native::scr_VmPub->outparamcount)
		{
			native::Scr_ClearOutParams();
		}

		if (native::scr_VmPub->top == native::scr_VmPub->maxstack)
		{
			throw std::runtime_error("Internal script stack overflow");
		}

		native::VariableValue* value_ptr = ++native::scr_VmPub->top;
		++native::scr_VmPub->inparamcount;

		value_ptr->type = native::SCRIPT_NONE;
		value_ptr->u.intValue = 0;

		if (value.get_type_info() == typeid(float))
		{
			const auto real_value = this->context_->get_chai()->boxed_cast<float>(value);
			value_ptr->type = native::SCRIPT_FLOAT;
			value_ptr->u.floatValue = real_value;
		}
		else if (value.get_type_info() == typeid(double))
		{
			const auto real_value = this->context_->get_chai()->boxed_cast<double>(value);
			value_ptr->type = native::SCRIPT_FLOAT;
			value_ptr->u.floatValue = static_cast<float>(real_value);
		}
		else if (value.get_type_info() == typeid(int))
		{
			const auto real_value = this->context_->get_chai()->boxed_cast<int>(value);
			value_ptr->type = native::SCRIPT_INTEGER;
			value_ptr->u.intValue = real_value;
		}
		else if (value.get_type_info() == typeid(bool))
		{
			const auto real_value = this->context_->get_chai()->boxed_cast<bool>(value);
			value_ptr->type = native::SCRIPT_INTEGER;
			value_ptr->u.intValue = real_value;
		}
		else if (value.get_type_info() == typeid(entity))
		{
			const auto real_value = this->context_->get_chai()->boxed_cast<entity>(value);
			value_ptr->type = native::SCRIPT_OBJECT;
			value_ptr->u.entityId = real_value.get_entity_id();

			game::native::AddRefToValue(value_ptr);
		}
		else if (value.get_type_info() == typeid(std::string))
		{
			const auto real_value = this->context_->get_chai()->boxed_cast<std::string>(value);
			value_ptr->type = native::SCRIPT_STRING;
			value_ptr->u.stringValue = game::native::SL_GetString(real_value.data(), 0);
		}
		else if (value.get_type_info() == typeid(std::vector<chaiscript::Boxed_Value>))
		{
			float values[3];
			const auto real_value = this->context_->get_chai()->boxed_cast<std::vector<chaiscript::Boxed_Value>
			>(value);
			if (real_value.size() != 3)
			{
				throw std::runtime_error("Invalid vector length. Size must be exactly 3");
			}

			const auto unbox_float = [&real_value, this](const size_t index) -> float
			{
				const auto value = real_value[index];
				if (value.get_type_info() == typeid(float))
				{
					return this->context_->get_chai()->boxed_cast<float>(value);
				}
				if (value.get_type_info() == typeid(double))
				{
					return float(this->context_->get_chai()->boxed_cast<double>(value));
				}
				if (value.get_type_info() == typeid(int))
				{
					return float(this->context_->get_chai()->boxed_cast<int>(value));
				}

				throw std::runtime_error("Vector element at index " + std::to_string(index) + " is not a number");
			};

			values[0] = unbox_float(0);
			values[1] = unbox_float(1);
			values[2] = unbox_float(2);

			value_ptr->type = native::SCRIPT_VECTOR;
			value_ptr->u.vectorValue = native::Scr_AllocVector(values);
		}
		else
		{
			throw std::runtime_error("Unable to unbox value of type '" + value.get_type_info().bare_name() + "'");
		}
	}

	chaiscript::Boxed_Value parameters::get_return_value() const
	{
		if (native::scr_VmPub->inparamcount == 0) return {};

		native::Scr_ClearOutParams();
		native::scr_VmPub->outparamcount = native::scr_VmPub->inparamcount;
		native::scr_VmPub->inparamcount = 0;

		return this->load(native::scr_VmPub->top[1 - native::scr_VmPub->outparamcount]);
	}
}
