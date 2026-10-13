#include <std_include.hpp>
#include "input.hpp"
#include "command.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"
#include "utils/io.hpp"
#include "utils/string.hpp"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// A friend's "mouse fix" for the 64-bit Steam MW2 (iw4mp.exe file offsets 0x2987E8: NOP the window procedure's
// SetThreadExecutionState call; 0x1F2F63: Sleep(1) -> Sleep(0) in Com_Frame's com_maxfps wait loop). The MW2 bytes
// are never applied: in this exe (MW3 1.9.388110, 32-bit) those offsets are unrelated code.
// - The window procedure's call is here too (0x5D0605): kept, but rate limited (a plain NOP lets the display sleep
//   during controller play, and NOPing only the 6 byte call leaves the push: esp 4 bytes off).
// - The Sleep is not: MW3's Com_Frame has no such loop (0x55655F..0x556593 only works out the time left), the
//   com_maxfps wait is WaitForSingleObject(event, 1) in 0x690F20 (0x55FBE0), unchanged, so a capped frame still waits
//   in 1 ms steps. What this exe has instead is an unconditional Sys_Sleep(1) before every frame (0x66F5C2), whatever
//   the cap: that nap is skipped (com_lowLatency, default 1). Removing it helps uncapped play most.
namespace
{
	// ---- iw5mp 1.9.388110 ----
	// MainWndProc 0x5D0600 starts with push ES_DISPLAY_REQUIRED; call [SetThreadExecutionState] (8 bytes), for every
	// message, before it looks at which one it is
	constexpr DWORD wnd_proc_execution_state = 0x5D0605;
	// the frame wait 0x66F5C0 (once per frame, right before IN_Frame samples the mouse) starts with Sys_Sleep(1)
	// (0x55F460, cdecl), before it takes its start time: 1-3 ms the com_maxfps limiter doesn't count
	constexpr DWORD frame_wait_sleep = 0x66F5C2;
	// Sys_GetEvent 0x5CC730 (Com_EventLoop, twice a frame) pumps window messages until the queue is empty:
	// PeekMessageA(&msg = ebp-4Ch, 0, 0, 0, PM_NOREMOVE) from here to the test of its result at 0x5CC7B3
	constexpr DWORD sys_get_event_peek = 0x5CC7A5;
	// the same pump while DB_FindXAssetHeader waits for a zone (0x4CA77C, then Sleep(1)): msg = ebp-30h, test at 0x5CC6BA
	constexpr DWORD sys_pump_peek = 0x5CC6B0;
	constexpr DWORD dvar_register_bool_addr = 0x5BE9F0;
	constexpr DWORD dvar_register_int_addr = 0x5BEA40;
	constexpr DWORD main_window = 0x5AA3930; // g_wv.hWnd

	game::native::dvar_t* com_low_latency = nullptr;
	game::native::dvar_t* com_max_messages = nullptr;
	game::native::dvar_t* m_rawinput = nullptr;

	game::native::dvar_t* register_bool(const char* name, const bool value, const char* description)
	{
		return reinterpret_cast<game::native::dvar_t*(*)(const char*, bool, int, const char*)>(dvar_register_bool_addr)(
			name, value, 1, description);
	}

	game::native::dvar_t* register_int(const char* name, const int value, const int min, const int max, const char* description)
	{
		return reinterpret_cast<game::native::dvar_t*(*)(const char*, int, int, int, int, const char*)>(dvar_register_int_addr)(
			name, value, min, max, 1, description);
	}

	HWND game_window()
	{
		return *reinterpret_cast<HWND*>(main_window);
	}

	bool game_in_front()
	{
		auto* window = game_window();
		return window && GetForegroundWindow() == window;
	}

	std::string log_path()
	{
		char exe[MAX_PATH]{};
		GetModuleFileNameA(nullptr, exe, sizeof(exe));
		return std::filesystem::path(exe).parent_path().string() + "\\open-iw5\\input.log";
	}

	// ---- statistics (in_stats): once a second to the console and <game>\open-iw5\input.log ----
	bool stats_on = false;
	bool stock_wnd_proc = false; // in_stockWndProc 1: SetThreadExecutionState for every message again (to compare)
	unsigned int window_messages = 0; // main thread only
	unsigned int capped_frames = 0;
	unsigned int skipped_sleeps = 0;
	unsigned int execution_state_calls = 0;
	std::atomic<unsigned int> raw_reports{0};
	std::atomic<unsigned int> raw_reads{0};
	std::atomic<unsigned int> raw_counts{0}; // |dx| + |dy|
	std::atomic<unsigned int> raw_device_reports{0}; // with a device (not injected)

	// ---- 1. the window procedure's SetThreadExecutionState ----
	void __cdecl window_message()
	{
		++window_messages;
		if (stock_wnd_proc)
		{
			SetThreadExecutionState(ES_DISPLAY_REQUIRED);
			++execution_state_calls;
			return;
		}
		input::keep_display_awake();
	}

	// ---- 2. the frame wait's nap ----
	void __cdecl frame_sleep(const int ms)
	{
		if (!com_low_latency || com_low_latency->current.enabled)
		{
			// Sleep(0): other ready threads of the same priority still get the core, but no 1-3 ms timer wait
			Sleep(0);
			++skipped_sleeps;
			return;
		}
		Sleep(static_cast<DWORD>(ms));
	}

	// ---- 3. window messages per frame ----
	int frame_messages = 0;
	DWORD frame_messages_reset = 0;
	bool frame_capped = false;

	int message_cap()
	{
		return com_max_messages ? com_max_messages->current.integer : 128;
	}

	// the pump of the frame (Sys_GetEvent): what is left waits for the next frame
	BOOL __cdecl peek_frame_message(MSG* msg)
	{
		// a pump outside the frames (the scheduler resets the count every frame) still gets its share
		if (GetTickCount() - frame_messages_reset > 250)
		{
			frame_messages = 0;
			frame_messages_reset = GetTickCount();
		}

		const auto cap = message_cap();
		if (cap > 0 && frame_messages >= cap)
		{
			if (!frame_capped) ++capped_frames;
			frame_capped = true;
			return FALSE;
		}

		const auto result = PeekMessageA(msg, nullptr, 0, 0, PM_NOREMOVE);
		if (result) ++frame_messages;
		return result;
	}

	// the pump while a zone loads: called in a wait loop (pump, Sleep(1), check), so the cap is per pass
	BOOL __cdecl peek_loading_message(MSG* msg)
	{
		static auto handled = 0;
		const auto cap = message_cap();
		if (cap > 0 && handled >= cap)
		{
			handled = 0;
			return FALSE;
		}

		const auto result = PeekMessageA(msg, nullptr, 0, 0, PM_NOREMOVE);
		handled = result ? handled + 1 : 0;
		return result;
	}

	__declspec(naked) void sys_get_event_peek_stub()
	{
		__asm
		{
			lea eax, [ebp - 4Ch]
			push eax
			call peek_frame_message
			add esp, 4
			retn
		}
	}

	__declspec(naked) void sys_pump_peek_stub()
	{
		__asm
		{
			lea eax, [ebp - 30h]
			push eax
			call peek_loading_message
			add esp, 4
			retn
		}
	}

	// ---- 4. raw mouse input ----
	// A thread of its own with a message-only window (RIDEV_INPUTSINK, no RIDEV_NOLEGACY: the game window keeps its
	// normal mouse messages, so the buttons, the wheel and the menus' cursor work as before). It sleeps until raw
	// input arrives, then reads everything queued with GetRawInputBuffer in one go and adds the moves up; the game
	// takes the sum once per frame (IN_MouseMove -> CL_MouseEvent, gamepad.cpp). An 8 kHz mouse never puts a message
	// into the game's queue: the legacy WM_MOUSEMOVE is one coalesced message, raw input goes to this thread.
	namespace raw
	{
		std::atomic<int> sum_x{0};
		std::atomic<int> sum_y{0};
		std::atomic<DWORD> real_move{0}; // GetTickCount() of the last move with a device (SendInput's have none)
		std::atomic<DWORD> last_move{0}; // GetTickCount() of the last move raw input saw (any)
		std::atomic<DWORD> absolute_move{0}; // absolute positions (pen tablets, remote desktop): the cursor is used then
		std::atomic<bool> wanted{false};
		std::atomic<bool> registered{false};
		bool taken = false; // the game took the moves since the last frame (main thread)
		std::atomic<bool> quit{false};
		HANDLE wake = nullptr; // wanted changed / quit
		bool started = false;
		bool background_test = false; // in_rawTest 1: read in the background too (only the counts of in_stats are used)

		// GetRawInputBuffer gives a 32-bit process on 64-bit Windows the 64-bit layout: the header has 64-bit handles
		// (8 bytes more before the data) and the blocks are 8-byte aligned
		size_t data_offset = sizeof(RAWINPUTHEADER);
		size_t block_align = sizeof(DWORD);

		void add(const RAWINPUTHEADER& header, const RAWMOUSE& mouse)
		{
			++raw_reports;
			if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE)
			{
				absolute_move = GetTickCount();
				return;
			}
			if (!mouse.lLastX && !mouse.lLastY) return; // buttons / wheel only (the legacy messages bring those)

			sum_x += mouse.lLastX;
			sum_y += mouse.lLastY;
			last_move = GetTickCount();
			raw_counts += static_cast<unsigned int>(std::abs(mouse.lLastX) + std::abs(mouse.lLastY));
			if (header.hDevice)
			{
				real_move = GetTickCount();
				++raw_device_reports;
			}
		}

		void read_buffer(std::vector<unsigned long long>& buffer)
		{
			for (auto loops = 0; loops < 64; ++loops)
			{
				auto size = static_cast<UINT>(buffer.size() * sizeof(buffer[0]));
				auto* data = reinterpret_cast<BYTE*>(buffer.data());
				const auto count = GetRawInputBuffer(reinterpret_cast<RAWINPUT*>(data), &size, sizeof(RAWINPUTHEADER));
				if (count == 0) return;
				if (count == static_cast<UINT>(-1))
				{
					if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && buffer.size() < 0x20000)
					{
						buffer.resize(buffer.size() * 2);
						continue;
					}
					return;
				}

				++raw_reads;
				const auto* end = data + buffer.size() * sizeof(buffer[0]);
				for (UINT i = 0; i < count && data + data_offset + sizeof(RAWMOUSE) <= end; ++i)
				{
					const auto* header = reinterpret_cast<const RAWINPUTHEADER*>(data);
					if (header->dwType == RIM_TYPEMOUSE)
					{
						add(*header, *reinterpret_cast<const RAWMOUSE*>(data + data_offset));
					}
					const auto size_of = std::max<size_t>(header->dwSize, data_offset);
					data += (size_of + block_align - 1) & ~(block_align - 1);
				}
			}
		}

		// a WM_INPUT that came through the message queue anyway
		void read_message(const LPARAM l_param)
		{
			RAWINPUT input{};
			UINT size = sizeof(input);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l_param), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) ==
				static_cast<UINT>(-1)) return;
			if (input.header.dwType == RIM_TYPEMOUSE) add(input.header, input.data.mouse);
		}

		bool set_registration(HWND window, const bool on)
		{
			RAWINPUTDEVICE device{};
			device.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
			device.usUsage = 0x02; // HID_USAGE_GENERIC_MOUSE
			device.dwFlags = on ? RIDEV_INPUTSINK : RIDEV_REMOVE;
			device.hwndTarget = on ? window : nullptr;
			if (!RegisterRawInputDevices(&device, 1, sizeof(device)))
			{
				printf("Raw mouse input: RegisterRawInputDevices failed (%lu)\n", GetLastError());
				return false;
			}
			return true;
		}

		// ~1 ms, or less when wanted changes / quit (wake)
		void batch_wait(HANDLE timer)
		{
			LARGE_INTEGER due{};
			due.QuadPart = -10000; // 1 ms (100 ns units, relative)
			if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
			{
				const HANDLE handles[] = {wake, timer};
				WaitForMultipleObjects(2, handles, FALSE, 5);
				return;
			}
			Sleep(1);
		}

		void run()
		{
			// the moves are summed for the next frame: this only has to keep up, never to be first
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

			// an 8 kHz mouse would wake this thread for every report (8000 times a second, each one taking a core from
			// the game's threads): after a wake it waits ~1 ms first and reads ~8 reports at once. The game takes the
			// sum once per frame, so that millisecond is never seen. A high resolution timer where Windows has one
			// (10 1803 and later), else Sleep(1)
			auto* batch_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

			BOOL wow64 = FALSE;
			if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64)
			{
				data_offset = sizeof(RAWINPUTHEADER) + 8;
				block_align = 8;
			}

			WNDCLASSEXA wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = DefWindowProcA;
			wc.hInstance = GetModuleHandleA(nullptr);
			wc.lpszClassName = "IW5V raw mouse";
			RegisterClassExA(&wc);
			auto* window = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
			if (!window)
			{
				printf("Raw mouse input: no window (%lu)\n", GetLastError());
				return;
			}

			std::vector<unsigned long long> buffer(2048); // 16 KB: ~340 reports a read
			while (!quit)
			{
				const auto want = wanted.load();
				if (want != registered.load() && set_registration(window, want))
				{
					sum_x = 0;
					sum_y = 0;
					registered = want;
				}

				// raw input already waiting (MsgWaitForMultipleObjects only wakes for input that came after the
				// last look at the queue), else sleep until some comes or wanted changes
				if (!(HIWORD(GetQueueStatus(QS_RAWINPUT)) & QS_RAWINPUT))
				{
					const auto result = MsgWaitForMultipleObjects(1, &wake, FALSE, INFINITE, QS_RAWINPUT | QS_SENDMESSAGE | QS_POSTMESSAGE);
					if (result == WAIT_OBJECT_0) continue;
					if (result != WAIT_OBJECT_0 + 1) break;
				}

				batch_wait(batch_timer);
				read_buffer(buffer);

				MSG msg;
				while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
				{
					if (msg.message == WM_INPUT) read_message(msg.lParam);
					DispatchMessageA(&msg);
				}
			}

			if (registered) set_registration(window, false);
			registered = false;
			DestroyWindow(window);
			if (batch_timer) CloseHandle(batch_timer);
		}

		// every frame (main thread): read only while the game is in front and m_rawinput is on
		void update()
		{
			const auto in_front = game_in_front();
			const auto want = (in_front || background_test) && m_rawinput && m_rawinput->current.enabled;

			// moves the game didn't take (in the background, or IN_Frame skips the mouse while it is released: the
			// console, a window without focus) don't count when it takes the mouse again: no jump of the view
			if (!in_front || !taken)
			{
				sum_x = 0;
				sum_y = 0;
			}
			taken = false;

			if (want && !started)
			{
				started = true;
				wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
				std::thread(run).detach();
			}

			if (want != wanted.load())
			{
				wanted = want;
				if (wake) SetEvent(wake);
			}
		}
	}

	// ---- statistics ----
	std::vector<float> frame_times; // ms, this second
	LARGE_INTEGER last_frame{};
	DWORD stats_start = 0;

	void log(const std::string& text)
	{
		printf("%s", text.data());
		utils::io::write_file(log_path(), text, true);
	}

	void update_stats()
	{
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		if (last_frame.QuadPart)
		{
			frame_times.push_back(static_cast<float>(static_cast<double>(now.QuadPart - last_frame.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)));
		}
		last_frame = now;

		const auto tick = GetTickCount();
		if (tick - stats_start < 1000) return;
		const auto seconds = static_cast<float>(tick - stats_start) / 1000.0f;
		stats_start = tick;

		auto average = 0.0f, high = 0.0f, worst = 0.0f;
		if (!frame_times.empty())
		{
			for (const auto t : frame_times) average += t;
			average /= static_cast<float>(frame_times.size());
			std::sort(frame_times.begin(), frame_times.end());
			high = frame_times[std::min(frame_times.size() - 1, frame_times.size() * 99 / 100)];
			worst = frame_times.back();
		}

		log(utils::string::va("input: %.0f fps, frame %.2f ms (99%% %.2f, max %.2f) | window messages %.0f/s, SetThreadExecutionState %u, "
		                      "capped frames %u | raw %s: %.0f reports/s (%.0f from a device, %u counts) in %.0f reads/s | frame naps skipped %u\n",
		                      static_cast<float>(frame_times.size()) / seconds, average, high, worst,
		                      static_cast<float>(window_messages) / seconds, execution_state_calls, capped_frames,
		                      input::raw_active() ? "on" : "off", static_cast<float>(raw_reports.exchange(0)) / seconds,
		                      static_cast<float>(raw_device_reports.exchange(0)) / seconds, raw_counts.exchange(0),
		                      static_cast<float>(raw_reads.exchange(0)) / seconds, skipped_sleeps));

		frame_times.clear();
		window_messages = 0;
		execution_state_calls = 0;
		capped_frames = 0;
		skipped_sleeps = 0;
	}

	// in_floodTest <messages a second> <seconds>: posts WM_NULL to the game window at that rate (the worst case of an
	// 8 kHz device: posted messages aren't merged like mouse moves; the window procedure runs for each). No real input.
	void flood_test(const int rate, const int seconds)
	{
		std::thread([rate, seconds]
		{
			auto* window = game_window();
			if (!window || rate <= 0) return;

			LARGE_INTEGER start, now, frequency;
			QueryPerformanceFrequency(&frequency);
			QueryPerformanceCounter(&start);
			long long posted = 0, failed = 0;
			for (;;)
			{
				QueryPerformanceCounter(&now);
				const auto elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
				if (elapsed >= seconds) break;

				const auto due = static_cast<long long>(elapsed * rate);
				for (; posted < due; ++posted)
				{
					if (!PostMessageA(window, WM_NULL, 0, 0)) ++failed;
				}
				Sleep(1);
			}
			printf("in_floodTest: %lld messages in %d s (%lld not posted: queue full)\n", posted, seconds, failed);
		}).detach();
	}
}

bool input::take_raw_mouse(int& dx, int& dy, const bool cursor_moved)
{
	if (!raw_active()) return false;
	raw::taken = true;
	dx = raw::sum_x.exchange(0);
	dy = raw::sum_y.exchange(0);

	// the cursor moves but raw input has seen nothing for a while: it doesn't reach this process (a remote session,
	// a program that takes the mouse for itself): the cursor's move counts, as in the stock game
	return !cursor_moved || dx || dy || GetTickCount() - raw::last_move.load() <= 500;
}

bool input::raw_active()
{
	return raw::registered && m_rawinput && m_rawinput->current.enabled && GetTickCount() - raw::absolute_move.load() > 500;
}

unsigned long input::raw_real_move_time()
{
	return raw::real_move;
}

void input::keep_display_awake()
{
	// main thread only (the window procedure, the controller's frame)
	static DWORD last = 0;
	const auto now = GetTickCount();
	if (last && now - last < 5000) return;
	last = now;
	SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
	++execution_state_calls;
}

void input::post_load()
{
	if (!game::is_mp()) return;

	// the 8 bytes push + call -> call + 3 nops: the stub takes no arguments, so the stack stays as it was
	utils::hook(wnd_proc_execution_state, window_message, HOOK_CALL).install()->quick();
	utils::hook::nop(wnd_proc_execution_state + 5, 3);

	utils::hook(frame_wait_sleep, frame_sleep, HOOK_CALL).install()->quick();

	utils::hook(sys_get_event_peek, sys_get_event_peek_stub, HOOK_CALL).install()->quick();
	utils::hook::nop(sys_get_event_peek + 5, 9);
	utils::hook(sys_pump_peek, sys_pump_peek_stub, HOOK_CALL).install()->quick();
	utils::hook::nop(sys_pump_peek + 5, 5);

	scheduler::once([]
	{
		com_low_latency = register_bool("com_lowLatency", true,
		                                "Skip the game's 1 ms sleep before every frame: steadier frame times, less input lag (0 = as the stock game)");
		com_max_messages = register_int("com_maxMessagesPerFrame", 128, 0, 100000,
		                                "Window messages handled per frame at most, the rest waits for the next frame (0 = no limit)");
		m_rawinput = register_bool("m_rawinput", true, "Raw mouse input: the mouse's own movement, read in batches, without Windows' pointer speed or acceleration");

		command::add("in_stats", [](const std::vector<std::string>& params)
		{
			stats_on = params.size() < 2 || params[1] != "0";
			stats_start = GetTickCount();
			last_frame = {};
			frame_times.clear();
			raw_reports = 0;
			raw_reads = 0;
			window_messages = execution_state_calls = capped_frames = skipped_sleeps = 0;
		});
		command::add("in_rawTest", [](const std::vector<std::string>& params)
		{
			raw::background_test = params.size() < 2 || params[1] != "0";
		});
		command::add("in_stockWndProc", [](const std::vector<std::string>& params)
		{
			stock_wnd_proc = params.size() < 2 || params[1] != "0";
		});
		command::add("in_floodTest", [](const std::vector<std::string>& params)
		{
			flood_test(params.size() > 1 ? std::atoi(params[1].data()) : 8000, params.size() > 2 ? std::atoi(params[2].data()) : 10);
		});

		scheduler::on_frame([]
		{
			frame_messages = 0;
			frame_messages_reset = GetTickCount();
			frame_capped = false;
			raw::update();
			if (stats_on) update_stats();
		});
	});
}

void input::pre_destroy()
{
	raw::quit = true;
	if (raw::wake) SetEvent(raw::wake);
}

REGISTER_MODULE(input)
