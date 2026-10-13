#include <std_include.hpp>
#include "launcher/launcher.hpp"
#include "loader/loader.hpp"
#include "loader/module_loader.hpp"
#include "game/game.hpp"
#include "loader/binary_loader.hpp"
#include "utils/string.hpp"
#include "utils/flags.hpp"

//#define GENERATE_DIFFS

DECLSPEC_NORETURN void WINAPI exit_hook(const int code)
{
	module_loader::pre_destroy();
	exit(code);
}

void verify_tls()
{
	const utils::nt::module self;
	const auto self_tls = reinterpret_cast<PIMAGE_TLS_DIRECTORY>(self.get_ptr() + self
	                                                                              .get_optional_header()
	                                                                              ->
	                                                                              DataDirectory
		[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);

	const auto ref = DWORD(&tls_data);
	const auto tls_index = *reinterpret_cast<DWORD*>(self_tls->AddressOfIndex);
	const auto tls_vector = *reinterpret_cast<DWORD*>(__readfsdword(0x2C) + 4 * tls_index);
	const auto offset = ref - tls_vector;

	if (offset != 0 && offset != 8) // Actually 8 is bad, but I think msvc places custom stuff before
	{
		throw std::runtime_error(utils::string::va("TLS payload is at offset 0x%X, but should be at 0!",
		                                           offset));
	}
}

launcher::mode detect_mode_from_arguments()
{
	if (utils::flags::has_flag("dedicated"))
	{
		return launcher::mode::server;
	}

	if (utils::flags::has_flag("multiplayer"))
	{
		return launcher::mode::multiplayer;
	}

	if (utils::flags::has_flag("singleplayer"))
	{
		return launcher::mode::singleplayer;
	}

	return launcher::mode::none;
}

FARPROC load_binary(const launcher::mode mode)
{
	loader loader(mode);
	utils::nt::module self;

	loader.set_import_resolver([self](const std::string& module, const std::string& function) -> FARPROC
	{
		if (module == "steam_api.dll")
		{
			return self.get_proc<FARPROC>(function);
		}
		else if (function == "ExitProcess")
		{
			return FARPROC(exit_hook);
		}

		return FARPROC(module_loader::load_import(module, function));
	});

	return loader.load(self);
}

int main()
{
	FARPROC entry_point;
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	{
		auto premature_shutdown = true;
		const auto _ = gsl::finally([&premature_shutdown]()
		{
			if (premature_shutdown)
			{
				module_loader::pre_destroy();
			}
		});

		try
		{
#ifdef GENERATE_DIFFS
			binary_loader::create();
			return 0;
#endif

			verify_tls();
			if (!module_loader::post_start()) return 0;

			auto mode = detect_mode_from_arguments();
			if (mode == launcher::mode::none)
			{
				const launcher launcher;
				mode = launcher.run();
				if (mode == launcher::mode::none) return 0;
			}

			entry_point = load_binary(mode);
			if (!entry_point)
			{
				throw std::runtime_error("Unable to load binary into memory");
			}

			game::initialize(mode);
			if (!module_loader::post_load()) return 0;

			premature_shutdown = false;
		}
		catch (std::exception& e)
		{
			MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
			return 1;
		}
	}

	return entry_point();
}
