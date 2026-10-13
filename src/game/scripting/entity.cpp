#include "std_include.hpp"
#include "context.hpp"

namespace game::scripting
{
	entity::entity() : entity(nullptr, 0)
	{
	}

	entity::entity(const entity& other) : entity(other.context_, other.entity_id_)
	{
	}

	entity::entity(entity&& other) noexcept
	{
		if (&other == this) return;

		this->context_ = other.context_;
		this->entity_id_ = other.entity_id_;

		other.context_ = nullptr;
		other.entity_id_ = 0;
	}

	entity::entity(context* context, const unsigned int entity_id) : context_(context), entity_id_(entity_id)
	{
		this->add();
	}

	entity::~entity()
	{
		this->release();
	}

	entity& entity::operator=(const entity& other)
	{
		if (&other != this)
		{
			this->release();

			this->context_ = other.context_;
			this->entity_id_ = other.entity_id_;

			this->add();
		}

		return *this;
	}

	entity& entity::operator=(entity&& other) noexcept
	{
		if (&other != this)
		{
			this->release();

			this->context_ = other.context_;
			this->entity_id_ = other.entity_id_;

			other.context_ = nullptr;
			other.entity_id_ = 0;
		}

		return *this;
	}

	event_listener_handle entity::on_notify(const std::string& event,
	                                        const std::function<void(std::vector<chaiscript::Boxed_Value>)>&
	                                        callback,
	                                        const bool is_volatile)
	const
	{
		event_listener listener;
		listener.event = event;
		listener.callback = callback;
		listener.entity_id = this->entity_id_;
		listener.is_volatile = is_volatile;

		return this->context_->get_event_handler()->add_event_listener(listener);
	}

	unsigned int entity::get_entity_id() const
	{
		return this->entity_id_;
	}

	native::scr_entref_t entity::get_entity_reference() const
	{
		return game::native::Scr_GetEntityIdRef(this->get_entity_id());
	}

	chaiscript::Boxed_Value entity::call(const std::string& function,
	                                     const std::vector<chaiscript::Boxed_Value>& arguments) const
	{
		return this->context_->get_executer()->call(function, this->get_entity_id(), arguments);
	}

	void entity::notify(const std::string& event,
	                    const std::vector<chaiscript::Boxed_Value>& arguments) const
	{
		this->context_->get_executer()->notify(event, this->get_entity_id(), arguments);
	}

	void entity::set(const std::string& field, const chaiscript::Boxed_Value& value) const
	{
		this->context_->get_executer()->set_entity_field(field, this->get_entity_id(), value);
	}

	chaiscript::Boxed_Value entity::get(const std::string& field) const
	{
		return this->context_->get_executer()->get_entity_field(field, this->get_entity_id());
	}

	void entity::add() const
	{
		if (this->entity_id_)
		{
			native::VariableValue value;
			value.type = native::SCRIPT_OBJECT;
			value.u.entityId = this->entity_id_;
			native::AddRefToValue(&value);
		}
	}

	void entity::release() const
	{
		if (this->entity_id_)
		{
			native::RemoveRefToValue(native::SCRIPT_OBJECT, {static_cast<int>(this->entity_id_)});
		}
	}
}
