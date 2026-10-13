#include "std_include.hpp"
#include "game/game.hpp"
#include "utils/string.hpp"
#include "functions.hpp"
#include "stack_isolation.hpp"
#include "safe_executer.hpp"
#include "context.hpp"

namespace game::scripting
{
	executer::executer(context* context) : context_(context)
	{
	}

	int executer::get_field_id(const int classnum, const std::string& field) const
	{
		const auto field_name = utils::string::to_lower(field);
		const auto class_id = native::g_classMap[classnum].id;
		const auto field_str = native::SL_GetString(field_name.data(), 1);
		const auto _ = gsl::finally([field_str]()
		{
			native::RemoveRefToValue(native::SCRIPT_STRING, {int(field_str)});
		});

		const auto offset = native::FindVariable(class_id, field_str);
		if (offset)
		{
			const auto index = 4 * (offset + 0xC800 * (class_id & 1));
			return PINT(SELECT_VALUE(0x1A3BC80, 0x1EFE180, 0x1DC8800))[index];
		}

		return -1;
	}

	void executer::set_entity_field(const std::string& field, const unsigned int entity_id,
	                                const chaiscript::Boxed_Value& value)
	{
		const auto entref = native::Scr_GetEntityIdRef(entity_id);
		const int id = get_field_id(entref.raw.classnum, field);

		if (id != -1)
		{
			stack_isolation _;
			this->context_->get_parameters()->push(value);

			native::scr_VmPub->outparamcount = native::scr_VmPub->inparamcount;
			native::scr_VmPub->inparamcount = 0;

			if (!safe_executer::set_entity_field(entref, id))
			{
				throw std::runtime_error("Failed to set value for field '" + field + "'");
			}
		}
		else
		{
			this->entity_fields_[entity_id][field] = value;
		}
	}

	chaiscript::Boxed_Value executer::get_entity_field(const std::string& field, const unsigned int entity_id)
	{
		const auto entref = native::Scr_GetEntityIdRef(entity_id);
		const auto id = this->get_field_id(entref.raw.classnum, field);

		if (id != -1)
		{
			stack_isolation _;

			native::VariableValue value{};
			if (!safe_executer::get_entity_field(entref, id, &value))
			{
				throw std::runtime_error("Failed to get value for field '" + field + "'");
			}

			const auto $ = gsl::finally([value]()
			{
				native::RemoveRefToValue(value.type, value.u);
			});

			return this->context_->get_parameters()->load(value);
		}
		else
		{
			const auto& map = this->entity_fields_[entity_id];
			const auto value = map.find(field);
			if (value != map.end())
			{
				return value->second;
			}
		}

		return {};
	}

	void executer::notify(const std::string& event, const unsigned int entity_id,
	                      std::vector<chaiscript::Boxed_Value> arguments) const
	{
		stack_isolation _;

		std::reverse(arguments.begin(), arguments.end());
		for (auto argument : arguments)
		{
			this->context_->get_parameters()->push(argument);
		}

		const auto event_id = native::SL_GetString(event.data(), 0);
		native::Scr_NotifyId(entity_id, event_id, native::scr_VmPub->inparamcount);
	}

	chaiscript::Boxed_Value executer::call(const std::string& function, const unsigned int entity_id,
	                                       std::vector<chaiscript::Boxed_Value> arguments) const
	{
		const auto function_index = find_function_index(function, entity_id == 0);
		if (function_index < 0)
		{
			throw std::runtime_error("No function found for name '" + function + "'");
		}

		const auto entity = function_index > 0x1C7
			                    ? native::Scr_GetEntityIdRef(entity_id)
			                    : native::scr_entref_t{~0u};

		const auto function_ptr = native::Scr_GetFunc(function_index);

		stack_isolation _;

		std::reverse(arguments.begin(), arguments.end());
		for (const auto& argument : arguments)
		{
			this->context_->get_parameters()->push(argument);
		}

		native::scr_VmPub->outparamcount = native::scr_VmPub->inparamcount;
		native::scr_VmPub->inparamcount = 0;

		if (!safe_executer::call(function_ptr, entity))
		{
			throw std::runtime_error("Error executing function '" + function + "'");
		}

		return this->context_->get_parameters()->get_return_value();
	}

	int executer::find_function_index(const std::string& function, const bool prefer_global)
	{
		const auto target = utils::string::to_lower(function);

		const auto primary_map = prefer_global
			                         ? &global_function_map
			                         : &instance_function_map;
		const auto secondary_map = !prefer_global
			                           ? &global_function_map
			                           : &instance_function_map;

		auto function_entry = primary_map->find(target);
		if (function_entry != primary_map->end())
		{
			return function_entry->second;
		}

		function_entry = secondary_map->find(target);
		if (function_entry != secondary_map->end())
		{
			return function_entry->second;
		}

		return -1;
	}
}
