#pragma once
#include "game/game.hpp"

namespace game::scripting
{
	class context;
	class event_listener_handle;

	class entity final
	{
	public:
		entity();
		entity(const entity& other);
		entity(entity&& other) noexcept;
		entity(context* context, unsigned int entity_id);
		~entity();

		entity& operator=(const entity& other);
		entity& operator=(entity&& other) noexcept;

		event_listener_handle on_notify(const std::string& event,
		                                const std::function<void(std::vector<chaiscript::Boxed_Value>)>& callback,
		                                bool is_volatile) const;

		unsigned int get_entity_id() const;
		game::native::scr_entref_t get_entity_reference() const;

		chaiscript::Boxed_Value call(const std::string& function,
		                             const std::vector<chaiscript::Boxed_Value>& arguments) const;
		void notify(const std::string& event, const std::vector<chaiscript::Boxed_Value>& arguments) const;

		void set(const std::string& field, const chaiscript::Boxed_Value& value) const;
		chaiscript::Boxed_Value get(const std::string& field) const;

	private:
		context* context_;
		unsigned int entity_id_;

		void add() const;
		void release() const;
	};
}
