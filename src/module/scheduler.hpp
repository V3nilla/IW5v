#pragma once
#include "loader/module_loader.hpp"
#include "utils/concurrent_list.hpp"

class scheduler final : public module
{
public:
	static void on_frame(const std::function<void()>& callback);
	static void once(const std::function<void()>& callback);

	static void error(const std::string& message, int level);

	void post_load() override;
	void pre_destroy() override;

private:
	static std::mutex mutex_;
	static std::queue<std::pair<std::string, int>> errors_;
	static utils::concurrent_list<std::function<void()>> callbacks_;
	static utils::concurrent_list<std::function<void()>> single_callbacks_;

	static void frame_stub();

	static void execute();
	static void execute_safe();
	static void execute_error();
	static bool get_next_error(const char** error_message, int* error_level);
};
