#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "game/game.hpp"
#include "scheduler.hpp"
#include "utils/flags.hpp"

class console final : public module
{
public:
	console()
	{
		ShowWindow(GetConsoleWindow(), SW_HIDE);

		_pipe(this->handles_, 1024, _O_TEXT);
		_dup2(this->handles_[1], 1);
		_dup2(this->handles_[1], 2);

		// a Windows program (no console) starts with stdout / stderr on no file (fd -2), which the _dup2 of fds 1 / 2
		// doesn't change: open the streams on a real fd and point it at the pipe, so printf still reaches the console
		FILE* stream = nullptr;
		if (_fileno(stdout) < 0 && !freopen_s(&stream, "NUL", "w", stdout)) _dup2(this->handles_[1], _fileno(stdout));
		if (_fileno(stderr) < 0 && !freopen_s(&stream, "NUL", "w", stderr)) _dup2(this->handles_[1], _fileno(stderr));

		//setvbuf(stdout, nullptr, _IONBF, 0);
		//setvbuf(stderr, nullptr, _IONBF, 0);
	}

	void post_start() override
	{
		scheduler::on_frame(std::bind(&console::log_messages, this));
		this->console_runner_ = std::thread(std::bind(&console::runner, this));
	}

	void pre_destroy() override
	{
		this->terminate_runner_ = true;

		// wake the runner's _read through the pipe itself (stdout may be no file) and wait for it before closing the
		// read end: closing it while _read waits on it deadlocks the quit
		_flushall();
		_write(this->handles_[1], "\r\n", 2);

		if (this->console_runner_.joinable())
		{
			this->console_runner_.join();
		}

		_close(this->handles_[0]);
		_close(this->handles_[1]);
	}

	void post_load() override
	{
		// the separate console window only with -console (the game's own console opens with ` like IW4x's)
		window_ = game::is_dedi() || utils::flags::has_flag("console");
		if (!game::is_dedi() && window_)
		{
			game::native::Sys_ShowConsole();
		}

		std::lock_guard _(this->mutex_);
		this->console_initialized_ = true;
	}

private:
	static inline bool window_ = false;
	bool console_initialized_ = false;
	bool terminate_runner_ = false;

	std::mutex mutex_;
	std::thread console_runner_;
	std::queue<std::string> message_queue_;

	int handles_[2]{};

	void log_messages()
	{
		while (this->console_initialized_ && !this->message_queue_.empty())
		{
			std::queue<std::string> message_queue_copy;

			{
				std::lock_guard _(this->mutex_);
				message_queue_copy = this->message_queue_;
				this->message_queue_ = {};
			}

			while (!message_queue_copy.empty())
			{
				log_message(message_queue_copy.front());
				message_queue_copy.pop();
			}
		}

		fflush(stdout);
		fflush(stderr);
	}

	static void log_message(const std::string& message)
	{
		OutputDebugStringA(message.data());
		if (window_)
		{
			game::native::Conbuf_AppendText(message.data());
		}
		else if (game::is_mp())
		{
			// CL_ConsolePrint (localClientNum, channel, text, duration, pixelWidth, flags): the in-game console
			reinterpret_cast<void(*)(int, int, const char*, int, int, int)>(0x487B00)(0, 0, message.data(), 0, 0, 0);
		}
	}

	void runner()
	{
		char buffer[1024];

		while (!this->terminate_runner_ && this->handles_[0])
		{
			const auto len = _read(this->handles_[0], buffer, sizeof(buffer));
			if (len > 0)
			{
				std::lock_guard _(this->mutex_);
				this->message_queue_.push(std::string(buffer, len));
			}
			else
			{
				std::this_thread::sleep_for(10ms);
			}
		}

		std::this_thread::yield();
	}
};

REGISTER_MODULE(console)
