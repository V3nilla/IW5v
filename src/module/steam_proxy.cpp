#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "game/game.hpp"
#include "utils/nt.hpp"
#include "steam/steam.hpp"
#include "steam/interface.hpp"
#include "utils/string.hpp"
#include "scheduler.hpp"

class steam_proxy final : public module
{
public:
	void post_start() override
	{
		this->run_mod();
	}

	void post_load() override
	{
		if (game::is_dedi()) return;

		this->load_client();

		this->clean_up_on_error();

		try
		{
			if (game::is_sp())
			{
				this->start_mod("\xF0\x9F\x90\x8D Open-IW5 Singleplayer", 42680);
			}
			else if (game::is_mp())
			{
				this->start_mod("\xF0\x9F\x90\x8D Open-IW5 Multiplayer", 42690);
			}
		}
		catch (std::exception& e)
		{
			printf("Steam: %s\n", e.what());
		}
	}

	void pre_destroy() override
	{
		if (this->steam_client_module_)
		{
			if (this->steam_pipe_)
			{
				if (this->global_user_)
				{
					this->steam_client_module_.invoke<void>("Steam_ReleaseUser", this->steam_pipe_, this->global_user_);
				}

				this->steam_client_module_.invoke<bool>("Steam_BReleaseSteamPipe", this->steam_pipe_);
			}
		}
	}

private:
	utils::nt::module steam_client_module_;

	steam::interface client_engine_;
	steam::interface client_user_;
	steam::interface client_utils_;

	void* steam_pipe_ = nullptr;
	void* global_user_ = nullptr;

	void run_mod() const
	{
		const auto command = "-proc ";
		const char* parent_proc = strstr(GetCommandLineA(), command);

		if (parent_proc)
		{
			const auto pid = atoi(parent_proc + strlen(command));
			const auto process_handle = OpenProcess(SYNCHRONIZE, FALSE, pid);
			if (process_handle && process_handle != INVALID_HANDLE_VALUE)
			{
				WaitForSingleObject(process_handle, INFINITE);
				CloseHandle(process_handle);
			}

			module_loader::trigger_premature_shutdown();
		}
	}

	void* load_client_engine() const
	{
		if (!this->steam_client_module_) return nullptr;

		for (auto i = 1; i > 0; ++i)
		{
			std::string name = utils::string::va("CLIENTENGINE_INTERFACE_VERSION%03i", i);
			const auto client_engine = this->steam_client_module_
			                               .invoke<void*>("CreateInterface", name.data(), nullptr);
			if (client_engine) return client_engine;
		}

		return nullptr;
	}

	void load_client()
	{
		const auto steam_path = ::steam::get_steam_install_directory();
		if (steam_path.empty()) return;

		utils::nt::module::load(steam_path + "tier0_s.dll");
		utils::nt::module::load(steam_path + "vstdlib_s.dll");
		this->steam_client_module_ = utils::nt::module::load(steam_path + "steamclient.dll");
		if (!this->steam_client_module_) return;

		this->client_engine_ = load_client_engine();
		if (!this->client_engine_) return;

		this->steam_pipe_ = this->steam_client_module_.invoke<void*>("Steam_CreateSteamPipe");
		this->global_user_ = this->steam_client_module_.invoke<void*>("Steam_ConnectToGlobalUser", this->steam_pipe_);
		this->client_user_ = this->client_engine_.invoke<void*>(8, this->steam_pipe_, this->global_user_); // GetIClientUser
		this->client_utils_ = this->client_engine_.invoke<void*>(13, this->steam_pipe_); // GetIClientUtils
	}

	void start_mod(const std::string& title, size_t app_id)
	{
		if (!this->client_utils_ || !this->client_user_) return;

		if (!this->client_user_.invoke<bool>("BIsSubscribedApp", app_id))
		{
			app_id = 480; // Spacewar
		}

		this->client_utils_.invoke<void>("SetAppIDForCurrentPipe", app_id, false);

		const utils::nt::module self;
		const auto path = self.get_path();

		char our_directory[MAX_PATH] = {0};
		GetCurrentDirectoryA(sizeof(our_directory), our_directory);

		const std::string cmdline = utils::string::va("\"%s\" -proc %d", path.data(), GetCurrentProcessId());

		game_id game_id;
		game_id.raw.type = 1; // k_EGameIDTypeGameMod
		game_id.raw.app_id = app_id & 0xFFFFFF;

		const auto mod_id = "OIW5";
		game_id.raw.mod_id = *reinterpret_cast<const unsigned int*>(mod_id) | 0x80000000;

		this->client_user_.invoke<bool>("SpawnProcess", self.get_path().data(), cmdline.data(), 0, our_directory,
		                                game_id.bits, title.data(), app_id, 0, 0);
	}

	void clean_up_on_error()
	{
		if (this->steam_client_module_
			&& this->steam_pipe_
			&& this->global_user_
			&& this->steam_client_module_.invoke<bool>("Steam_BConnected", this->global_user_, this->steam_pipe_)
			&& this->steam_client_module_.invoke<bool>("Steam_BLoggedOn", this->global_user_, this->steam_pipe_))
		{
			scheduler::once(std::bind(&steam_proxy::clean_up_on_error, this));
			return;
		}

		this->client_engine_ = nullptr;
		this->client_user_ = nullptr;
		this->client_utils_ = nullptr;

		this->steam_pipe_ = nullptr;
		this->global_user_ = nullptr;

		this->steam_client_module_ = utils::nt::module{nullptr};
	}
};

REGISTER_MODULE(steam_proxy)
