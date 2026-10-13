#pragma once
#include "utils/concurrent_list.hpp"

namespace game::scripting
{
	class context;

	class task_handle
	{
	public:
		unsigned long long id = 0;
	};

	class task final : public task_handle
	{
	public:
		std::chrono::steady_clock::time_point last_execution{};
		std::function<void()> callback{};
		std::chrono::milliseconds delay{};
		bool is_volatile = false;
	};

	class scheduler final
	{
	public:
		explicit scheduler(context* context);

		void run_frame();
		void clear();

	private:
		context* context_;

		utils::concurrent_list<task> tasks_;
		std::atomic_int64_t current_task_id_ = 0;

		task_handle add(const std::function<void()>& callback, long long milliseconds, bool is_volatile);
		task_handle add(const std::function<void()>& callback, std::chrono::milliseconds delay, bool is_volatile);

		void remove(const task_handle& handle);
	};
}
