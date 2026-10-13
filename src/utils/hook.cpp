#include <std_include.hpp>
#include "hook.hpp"

namespace utils
{
	namespace
	{
		std::atomic<hook::write_listener> write_listener_{nullptr};
	}

	void hook::signature::process()
	{
		if (this->signatures_.empty()) return;

		const auto start = reinterpret_cast<char*>(this->start_);

		const unsigned int sig_count = this->signatures_.size();
		const auto containers = this->signatures_.data();

		for (size_t i = 0; i < this->length_; ++i)
		{
			const auto address = start + i;

			for (unsigned int k = 0; k < sig_count; ++k)
			{
				const auto container = &containers[k];

				unsigned int j;
				for (j = 0; j < strlen(container->mask); ++j)
				{
					if (container->mask[j] != '?' && container->signature[j] != address[j])
					{
						break;
					}
				}

				if (j == strlen(container->mask))
				{
					container->callback(address);
				}
			}
		}
	}

	void hook::signature::add(const container& container)
	{
		signatures_.push_back(container);
	}

	hook::~hook()
	{
		if (this->initialized_)
		{
			this->uninstall();
		}
	}

	hook* hook::initialize(const DWORD place, void (*stub)(), const bool use_jump)
	{
		return this->initialize(place, reinterpret_cast<void*>(stub), use_jump);
	}

	hook* hook::initialize(const DWORD place, void* stub, const bool use_jump)
	{
		return this->initialize(reinterpret_cast<void*>(place), stub, use_jump);
	}

	hook* hook::initialize(void* place, void* stub, const bool use_jump)
	{
		if (this->initialized_) return this;
		this->initialized_ = true;

		this->use_jump_ = use_jump;
		this->place_ = place;
		this->stub_ = stub;

		this->original_ = static_cast<char*>(this->place_) + 5 + *reinterpret_cast<DWORD*>((static_cast<char*>(this->
			place_) + 1));

		return this;
	}

	hook* hook::install(const bool unprotect, const bool keep_unprotected)
	{
		std::lock_guard _(this->state_mutex_);

		if (!this->initialized_ || this->installed_)
		{
			return this;
		}

		this->installed_ = true;

		if (unprotect) VirtualProtect(this->place_, sizeof(this->buffer_), PAGE_EXECUTE_READWRITE, &this->protection_);
		std::memcpy(this->buffer_, this->place_, sizeof(this->buffer_));

		const auto code = static_cast<char*>(this->place_);

		*code = static_cast<char>(this->use_jump_ ? 0xE9 : 0xE8);

		*reinterpret_cast<size_t*>(code + 1) = reinterpret_cast<size_t>(this->stub_) - (reinterpret_cast<size_t>(this->
			place_) + 5);

		if (unprotect && !keep_unprotected)
			VirtualProtect(this->place_, sizeof(this->buffer_), this->protection_,
			               &this->protection_);

		FlushInstructionCache(GetCurrentProcess(), this->place_, sizeof(this->buffer_));
		notify_write(this->place_, sizeof(this->buffer_));

		return this;
	}

	void hook::quick()
	{
		if (this->installed_)
		{
			this->installed_ = false;
		}
	}

	bool hook::iat(const nt::module module, const std::string& target_module, const std::string& process, void* stub)
	{
		if (!module.is_valid()) return false;

		auto ptr = module.get_iat_entry(target_module, process);
		if (!ptr) return false;

		DWORD protect;
		VirtualProtect(ptr, sizeof(*ptr), PAGE_EXECUTE_READWRITE, &protect);

		*ptr = stub;

		VirtualProtect(ptr, sizeof(*ptr), protect, &protect);
		notify_write(ptr, sizeof(*ptr));
		return true;
	}

	hook* hook::uninstall(const bool unprotect)
	{
		std::lock_guard _(this->state_mutex_);

		if (!this->initialized_ || !this->installed_)
		{
			return this;
		}

		this->installed_ = false;

		if (unprotect) VirtualProtect(this->place_, sizeof(this->buffer_), PAGE_EXECUTE_READWRITE, &this->protection_);

		std::memcpy(this->place_, this->buffer_, sizeof(this->buffer_));

		if (unprotect) VirtualProtect(this->place_, sizeof(this->buffer_), this->protection_, &this->protection_);

		FlushInstructionCache(GetCurrentProcess(), this->place_, sizeof(this->buffer_));
		notify_write(this->place_, sizeof(this->buffer_));

		return this;
	}

	void* hook::get_address() const
	{
		return this->place_;
	}

	void* hook::get_original() const
	{
		return this->original_;
	}

	void hook::nop(void* place, const size_t length)
	{
		DWORD old_protect;
		VirtualProtect(place, length, PAGE_EXECUTE_READWRITE, &old_protect);

		memset(place, 0x90, length);

		VirtualProtect(place, length, old_protect, &old_protect);
		FlushInstructionCache(GetCurrentProcess(), place, length);
		notify_write(place, length);
	}

	void hook::nop(const DWORD place, const size_t length)
	{
		nop(reinterpret_cast<void*>(place), length);
	}

	void hook::set_write_listener(const write_listener listener)
	{
		write_listener_ = listener;
	}

	void hook::notify_write(const void* place, const size_t length)
	{
		const auto listener = write_listener_.load();
		if (listener) listener(place, length);
	}
}
