#pragma once

namespace game::scripting
{
	class context;

	class executer final
	{
	public:
		explicit executer(context* context);

		void set_entity_field(const std::string& field, unsigned int entity_id,
		                      const chaiscript::Boxed_Value& value);
		chaiscript::Boxed_Value get_entity_field(const std::string& field, unsigned int entity_id);

		void notify(const std::string& event, unsigned int entity_id,
		            std::vector<chaiscript::Boxed_Value> arguments) const;

		chaiscript::Boxed_Value call(const std::string& function, unsigned int entity_id,
		                             std::vector<chaiscript::Boxed_Value> arguments) const;

	private:
		context* context_;

		std::unordered_map<unsigned int, std::unordered_map<std::string, chaiscript::Boxed_Value>> entity_fields_;

		int get_field_id(int classnum, const std::string& field) const;

		static int find_function_index(const std::string& function, bool prefer_global);
	};
}
