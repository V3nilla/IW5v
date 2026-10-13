#include "std_include.hpp"
#include "stack_isolation.hpp"

namespace game::scripting
{
	stack_isolation::stack_isolation()
	{
		this->in_param_count_ = native::scr_VmPub->inparamcount;
		this->out_param_count_ = native::scr_VmPub->outparamcount;
		this->top_ = native::scr_VmPub->top;
		this->max_stack_ = native::scr_VmPub->maxstack;

		native::scr_VmPub->top = this->stack_;
		native::scr_VmPub->maxstack = &this->stack_[ARRAYSIZE(this->stack_) - 1];
		native::scr_VmPub->inparamcount = 0;
		native::scr_VmPub->outparamcount = 0;
	}

	stack_isolation::~stack_isolation()
	{
		native::Scr_ClearOutParams();
		native::scr_VmPub->inparamcount = this->in_param_count_;
		native::scr_VmPub->outparamcount = this->out_param_count_;
		native::scr_VmPub->top = this->top_;
		native::scr_VmPub->maxstack = this->max_stack_;
	}
}
