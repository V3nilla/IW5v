#include "std_include.hpp"
#include "context.hpp"

namespace game::scripting
{
	event_handler::event_handler(context* context) : context_(context)
	{
		const auto chai = this->context_->get_chai();

		chai->add(chaiscript::user_type<event_listener_handle>(), "_event_listener_handle");
		chai->add(chaiscript::constructor<event_listener_handle()>(), "_event_listener_handle");
		chai->add(chaiscript::constructor<event_listener_handle(const event_listener_handle&)>(),
		          "_event_listener_handle");

		chai->add(chaiscript::fun(
			          [](event_listener_handle& lhs, const event_listener_handle& rhs) -> event_listener_handle&
			          {
				          return lhs = rhs;
			          }), "=");

		chai->add(chaiscript::fun([this](const event_listener_handle& handle)
		{
			this->remove(handle);
		}), "clear");
	}

	void event_handler::dispatch(event* event)
	{
		try
		{
			std::vector<chaiscript::Boxed_Value> arguments;

			for (auto argument : event->arguments)
			{
				arguments.push_back(this->context_->get_parameters()->load(argument));
			}

			this->dispatch_to_specific_listeners(event, arguments);
			this->dispatch_to_generic_listeners(event, arguments);
		}
		catch (chaiscript::exception::eval_error& e)
		{
			throw std::runtime_error(e.pretty_print());
		}
	}

	void event_handler::dispatch_to_specific_listeners(event* event,
	                                                   const std::vector<chaiscript::Boxed_Value>& arguments)
	{
		for (auto listener : this->event_listeners_)
		{
			if (listener->event == event->name && listener->entity_id == event->entity_id)
			{
				if (listener->is_volatile)
				{
					this->event_listeners_.remove(listener);
				}

				listener->callback(arguments);
			}
		}
	}

	void event_handler::dispatch_to_generic_listeners(event* event,
	                                                  const std::vector<chaiscript::Boxed_Value>& arguments)
	{
		for (auto listener : this->generic_event_listeners_)
		{
			if (listener->event == event->name)
			{
				if (listener->is_volatile)
				{
					this->generic_event_listeners_.remove(listener);
				}

				listener->callback(entity(this->context_, event->entity_id), arguments);
			}
		}
	}

	event_listener_handle event_handler::add_event_listener(event_listener listener)
	{
		listener.id = ++this->current_listener_id_;
		this->event_listeners_.add(listener);
		return {listener.id};
	}

	event_listener_handle event_handler::add_event_listener(generic_event_listener listener)
	{
		listener.id = ++this->current_listener_id_;
		this->generic_event_listeners_.add(listener);
		return {listener.id};
	}

	void event_handler::clear()
	{
		this->event_listeners_.clear();
		this->generic_event_listeners_.clear();
	}

	void event_handler::remove(const event_listener_handle& handle)
	{
		for (const auto task : this->event_listeners_)
		{
			if (task->id == handle.id)
			{
				this->event_listeners_.remove(task);
				return;
			}
		}

		for (const auto task : this->generic_event_listeners_)
		{
			if (task->id == handle.id)
			{
				this->generic_event_listeners_.remove(task);
				return;
			}
		}
	}
}
