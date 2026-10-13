#include "std_include.hpp"
#include "context_initializer.hpp"

namespace game::scripting
{
	context::context() : executer_(this), scheduler_(this), parameters_(this), event_handler_(this)
	{
		context_initializer::initialize(this);
	}

	executer* context::get_executer()
	{
		return &this->executer_;
	}

	scheduler* context::get_scheduler()
	{
		return &this->scheduler_;
	}

	parameters* context::get_parameters()
	{
		return &this->parameters_;
	}

	event_handler* context::get_event_handler()
	{
		return &this->event_handler_;
	}

	chaiscript::ChaiScript* context::get_chai()
	{
		return &this->chai_;
	}
}
