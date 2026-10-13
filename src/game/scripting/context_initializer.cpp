#include "std_include.hpp"
#include "context_initializer.hpp"

namespace game::scripting::context_initializer
{
	void initialize_entity(context* context)
	{
		const auto chai = context->get_chai();

		chai->add(chaiscript::user_type<entity>(), "_entity");
		chai->add(chaiscript::constructor<entity()>(), "_entity");
		chai->add(chaiscript::constructor<entity(const entity&)>(), "_entity");

		chai->add(chaiscript::fun([](entity& lhs, const entity& rhs) -> entity&
		{
			return lhs = rhs;
		}), "=");

		chai->add(chaiscript::fun(&entity::get), "get");
		chai->add(chaiscript::fun(&entity::set), "set");

		chai->add(chaiscript::fun(&entity::on_notify), "onNotify");
		chai->add(chaiscript::fun([](const entity& ent, const std::string& event,
		                             const std::function<void(
			                             std::vector<chaiscript::Boxed_Value>)>&
		                             callback)
		{
			return ent.on_notify(event, callback, false);
		}), "onNotify");

		chai->add(chaiscript::fun([context](const std::string& event,
		                                    const std::function<void(
			                                    entity, std::vector<chaiscript::Boxed_Value>)>&
		                                    callback)
		{
			generic_event_listener listener;
			listener.event = event;
			listener.is_volatile = false;
			listener.callback = callback;

			return context->get_event_handler()->add_event_listener(listener);
		}), "onNotify");

		chai->add(chaiscript::fun([context](const std::string& event,
		                                    const std::function<void(
			                                    entity, std::vector<chaiscript::Boxed_Value>)>&
		                                    callback, const bool is_volatile)
		{
			generic_event_listener listener;
			listener.event = event;
			listener.is_volatile = is_volatile;
			listener.callback = callback;

			return context->get_event_handler()->add_event_listener(listener);
		}), "onNotify");

		// Notification
		chai->add(chaiscript::fun(&entity::notify), "vectorNotify");
		chai->add(chaiscript::fun([](const entity& ent, const std::string& event)
		{
			return ent.notify(event, {});
		}), "notify");

		chai->add(chaiscript::fun(
			          [](const entity& ent, const std::string& event,
			             const chaiscript::Boxed_Value& a1)
			          {
				          return ent.notify(event, {a1});
			          }), "notify");

		chai->add(chaiscript::fun(
			          [](const entity& ent, const std::string& event,
			             const chaiscript::Boxed_Value& a1,
			             const chaiscript::Boxed_Value& a2)
			          {
				          return ent.notify(event, {a1, a2});
			          }), "notify");

		chai->add(chaiscript::fun(
			          [](const entity& ent, const std::string& event,
			             const chaiscript::Boxed_Value& a1,
			             const chaiscript::Boxed_Value& a2,
			             const chaiscript::Boxed_Value& a3)
			          {
				          return ent.notify(event, {a1, a2, a3});
			          }), "notify");

		chai->add(chaiscript::fun(
			          [](const entity& ent, const std::string& event,
			             const chaiscript::Boxed_Value& a1,
			             const chaiscript::Boxed_Value& a2,
			             const chaiscript::Boxed_Value& a3,
			             const chaiscript::Boxed_Value& a4)
			          {
				          return ent.notify(event, {a1, a2, a3, a4});
			          }), "notify");

		chai->add(chaiscript::fun(
			          [](const entity& ent, const std::string& event,
			             const chaiscript::Boxed_Value& a1,
			             const chaiscript::Boxed_Value& a2,
			             const chaiscript::Boxed_Value& a3,
			             const chaiscript::Boxed_Value& a4,
			             const chaiscript::Boxed_Value& a5)
			          {
				          return ent.notify(event, {a1, a2, a3, a4, a5});
			          }), "notify");

		// Instance call
		chai->add(chaiscript::fun(&entity::call), "vectorCall");
		chai->add(chaiscript::fun([](const entity& ent, const std::string& function)
		{
			return ent.call(function, {});
		}), "call");

		chai->add(chaiscript::fun(
			          [](const entity& ent, const std::string& function,
			             const chaiscript::Boxed_Value& a1)
			          {
				          return ent.call(function, {a1});
			          }), "call");

		chai->add(chaiscript::fun(
			          [](const entity& ent, const std::string& function,
			             const chaiscript::Boxed_Value& a1,
			             const chaiscript::Boxed_Value& a2)
			          {
				          return ent.call(function, {a1, a2});
			          }), "call");

		chai->add(chaiscript::fun(
			          [](const entity& ent, const std::string& function,
			             const chaiscript::Boxed_Value& a1,
			             const chaiscript::Boxed_Value& a2,
			             const chaiscript::Boxed_Value& a3)
			          {
				          return ent.call(function, {a1, a2, a3});
			          }), "call");

		chai->add(chaiscript::fun(
			          [](const entity& ent, const std::string& function,
			             const chaiscript::Boxed_Value& a1,
			             const chaiscript::Boxed_Value& a2,
			             const chaiscript::Boxed_Value& a3,
			             const chaiscript::Boxed_Value& a4)
			          {
				          return ent.call(function, {a1, a2, a3, a4});
			          }), "call");

		chai->add(chaiscript::fun(
			          [](const entity& ent, const std::string& function,
			             const chaiscript::Boxed_Value& a1,
			             const chaiscript::Boxed_Value& a2,
			             const chaiscript::Boxed_Value& a3,
			             const chaiscript::Boxed_Value& a4,
			             const chaiscript::Boxed_Value& a5)
			          {
				          return ent.call(function, {a1, a2, a3, a4, a5});
			          }), "call");

		// Global call
		chai->add(chaiscript::fun(
			          [context](const std::string& function,
			                    const std::vector<chaiscript::Boxed_Value>& arguments)
			          {
				          return context->get_executer()->call(function, 0, arguments);
			          }), "vectorCall");
		chai->add(chaiscript::fun([context](const std::string& function)
		{
			return context->get_executer()->call(function, 0, {});
		}), "call");

		chai->add(chaiscript::fun(
			          [context](const std::string& function,
			                    const chaiscript::Boxed_Value& a1)
			          {
				          return context->get_executer()->call(function, 0, {a1});
			          }), "call");

		chai->add(chaiscript::fun(
			          [context](const std::string& function,
			                    const chaiscript::Boxed_Value& a1,
			                    const chaiscript::Boxed_Value& a2)
			          {
				          return context->get_executer()->call(function, 0, {a1, a2});
			          }), "call");

		chai->add(chaiscript::fun(
			          [context](const std::string& function,
			                    const chaiscript::Boxed_Value& a1,
			                    const chaiscript::Boxed_Value& a2,
			                    const chaiscript::Boxed_Value& a3)
			          {
				          return context->get_executer()->call(function, 0, {a1, a2, a3});
			          }), "call");

		chai->add(chaiscript::fun(
			          [context](const std::string& function,
			                    const chaiscript::Boxed_Value& a1,
			                    const chaiscript::Boxed_Value& a2,
			                    const chaiscript::Boxed_Value& a3,
			                    const chaiscript::Boxed_Value& a4)
			          {
				          return context->get_executer()->call(function, 0, {a1, a2, a3, a4});
			          }), "call");

		chai->add(chaiscript::fun(
			          [context](const std::string& function,
			                    const chaiscript::Boxed_Value& a1,
			                    const chaiscript::Boxed_Value& a2,
			                    const chaiscript::Boxed_Value& a3,
			                    const chaiscript::Boxed_Value& a4,
			                    const chaiscript::Boxed_Value& a5)
			          {
				          return context->get_executer()->call(function, 0, {a1, a2, a3, a4, a5});
			          }), "call");
	}

	void initialize(context* context)
	{
		initialize_entity(context);

		const auto chai = context->get_chai();

		chai->add(chaiscript::fun([](const std::string& string)
		{
			printf("%s\n", string.data());
		}), "print");

		chai->add(chaiscript::fun([](const std::string& string)
		{
			MessageBoxA(nullptr, string.data(), nullptr, 0);
		}), "alert");

		const auto level_id = *native::levelEntityId;
		chai->add_global(chaiscript::var(entity(context, level_id)), "level");
	}
}
