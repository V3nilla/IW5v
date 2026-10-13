#pragma once
#include "executer.hpp"
#include "scheduler.hpp"
#include "parameters.hpp"
#include "event_handler.hpp"

namespace game::scripting
{
	class context final
	{
	public:
		context();

		chaiscript::ChaiScript* get_chai();

		executer* get_executer();
		scheduler* get_scheduler();
		parameters* get_parameters();
		event_handler* get_event_handler();

	private:
		chaiscript::ChaiScript chai_;

		executer executer_;
		scheduler scheduler_;
		parameters parameters_;
		event_handler event_handler_;
	};
}
