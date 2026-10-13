#pragma once
#include "nt.hpp"

#define HOOK_JUMP true
#define HOOK_CALL false

namespace utils
{
	class hook final
	{
	public:
		class signature final
		{
		public:
			struct container final
			{
				const char* signature;
				const char* mask;
				std::function<void(char*)> callback;
			};

			signature(void* start, const size_t length) : start_(start), length_(length)
			{
			}

			signature(const DWORD start, const size_t length) : signature(reinterpret_cast<void*>(start), length)
			{
			}

			signature() : signature(0x400000, 0x800000)
			{
			}

			void process();
			void add(const container& container);

		private:
			void* start_;
			size_t length_;
			std::vector<container> signatures_;
		};

		hook() : initialized_(false), installed_(false), place_(nullptr), stub_(nullptr), original_(nullptr),
		         use_jump_(false), protection_(0)
		{
			ZeroMemory(this->buffer_, sizeof(this->buffer_));
		}

		hook(void* place, void* stub, const bool use_jump = true) : hook() { this->initialize(place, stub, use_jump); }

		hook(void* place, void (*stub)(), const bool use_jump = true) : hook(
			place, reinterpret_cast<void*>(stub), use_jump)
		{
		}

		hook(const DWORD place, void* stub, const bool use_jump = true) : hook(
			reinterpret_cast<void*>(place), stub, use_jump)
		{
		}

		hook(const DWORD place, const DWORD stub, const bool use_jump = true) : hook(
			reinterpret_cast<void*>(place), reinterpret_cast<void*>(stub), use_jump)
		{
		}

		hook(const DWORD place, void (*stub)(), const bool use_jump = true) : hook(
			reinterpret_cast<void*>(place), reinterpret_cast<void*>(stub), use_jump)
		{
		}

		hook(const hook&) = delete;
		hook(const hook&&) = delete;

		~hook();

		hook* initialize(void* place, void* stub, bool use_jump = true);
		hook* initialize(DWORD place, void* stub, bool use_jump = true);
		hook* initialize(DWORD place, void (*stub)(), bool use_jump = true); // For lambdas
		hook* install(bool unprotect = true, bool keep_unprotected = false);
		hook* uninstall(bool unprotect = true);

		void* get_address() const;
		void* get_original() const;
		void quick();

		static bool iat(nt::module module, const std::string& target_module, const std::string& process, void* stub);

		static void nop(void* place, size_t length);
		static void nop(DWORD place, size_t length);

		template <typename T>
		static void set(void* place, T value)
		{
			DWORD old_protect;
			VirtualProtect(place, sizeof(T), PAGE_EXECUTE_READWRITE, &old_protect);

			*static_cast<T*>(place) = value;

			VirtualProtect(place, sizeof(T), old_protect, &old_protect);
			FlushInstructionCache(GetCurrentProcess(), place, sizeof(T));
			notify_write(place, sizeof(T));
		}

		template <typename T>
		static void set(const DWORD place, T value)
		{
			return set<T>(reinterpret_cast<void*>(place), value);
		}

		// Writes into the game's code go through install / uninstall / set / nop / iat, which tell the write listener
		// (or call notify_write after the write). The payload is writable, so a plain store works as well.
		using write_listener = void(*)(const void* place, size_t length);
		static void set_write_listener(write_listener listener);
		static void notify_write(const void* place, size_t length);

	private:
		bool initialized_;
		bool installed_;

		void* place_;
		void* stub_;
		void* original_;
		char buffer_[5]{};
		bool use_jump_;

		DWORD protection_;

		std::mutex state_mutex_;
	};
}
