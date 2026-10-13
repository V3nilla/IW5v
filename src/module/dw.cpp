#include <std_include.hpp>
#include "dw.hpp"
#include "utils/hook.hpp"
#include "game/game.hpp"
#include "utils/nt.hpp"
#include "utils/cryptography.hpp"

#include "game/demonware/services/bdLSGHello.hpp"       // 7
#include "game/demonware/services/bdStorage.hpp"        // 10
#include "game/demonware/services/bdDediAuth.hpp"       // 12
#include "game/demonware/services/bdTitleUtilities.hpp" // 12
#include "game/demonware/services/bdDML.hpp"            // 27
#include "game/demonware/services/bdDediRSAAuth.hpp"    // 26
#include "game/demonware/services/bdSteamAuth.hpp"      // 28

namespace demonware
{
	namespace io
	{
		int __stdcall send_to(const SOCKET s, const char* buf, const int len, const int flags, const sockaddr* to,
		                      const int tolen)
		{
			if (tolen == sizeof(sockaddr_in))
			{
				const auto in_addr = reinterpret_cast<const sockaddr_in*>(to);
				const auto server = dw::find_stun_server_by_address(in_addr->sin_addr.s_addr);
				if (server) return server->send(s, buf, len, to, tolen);
			}

			return sendto(s, buf, len, flags, to, tolen);
		}

		int __stdcall recv_from(const SOCKET s, char* buf, const int len, const int flags, sockaddr* from, int* fromlen)
		{
			auto res = dw::recv_datagam_packet(s, buf, len, from, fromlen);
			if (res != 0) return res;

			res = recvfrom(s, buf, len, flags, from, fromlen);

			return res;
		}

		int __stdcall send(const SOCKET s, const char* buf, const int len, const int flags)
		{
			auto server = dw::find_server_by_socket(s);
			if (server) return server->send(buf, len);

			return ::send(s, buf, len, flags);
		}

		int __stdcall recv(const SOCKET s, char* buf, const int len, const int flags)
		{
			auto server = dw::find_server_by_socket(s);
			if (server)
			{
				const auto blocking = dw::is_blocking_socket(s, TCP_BLOCKING);

				int result;
				do
				{
					result = server->recv(buf, len);
					if (blocking && result < 0) std::this_thread::sleep_for(1ms);
				}
				while (blocking && result < 0);

				if (!blocking && result < 0)
				{
					WSASetLastError(WSAEWOULDBLOCK);
				}

				return result;
			}

			return ::recv(s, buf, len, flags);
		}

		int __stdcall connect(const SOCKET s, const sockaddr* addr, const int len)
		{
			if (len == sizeof(sockaddr_in))
			{
				const auto* in_addr = reinterpret_cast<const sockaddr_in*>(addr);
				if (dw::link_socket(s, in_addr->sin_addr.s_addr)) return 0;
			}

			return ::connect(s, addr, len);
		}

		int __stdcall close_socket(const SOCKET s)
		{
			dw::remove_blocking_socket(s);
			dw::unlink_socket(s);
			return closesocket(s);
		}

		int __stdcall ioctl_socket(const SOCKET s, const long cmd, u_long* argp)
		{
			if (static_cast<unsigned long>(cmd) == (FIONBIO))
			{
				dw::set_blocking_socket(s, *argp == 0);
			}

			return ioctlsocket(s, cmd, argp);
		}

		hostent* __stdcall get_host_by_name(char* name)
		{
			static std::mutex mutex;
			std::lock_guard<std::mutex> _(mutex);

			unsigned long addr = 0;
			const auto server = dw::find_server_by_name(name);
			if (server) addr = server->get_address();

			const auto stun_server = dw::find_stun_server_by_name(name);
			if (stun_server) addr = stun_server->get_address();

			if (server || stun_server)
			{
				static in_addr address;
				address.s_addr = addr;

				static in_addr* addr_list[2];
				addr_list[0] = &address;
				addr_list[1] = nullptr;

				static hostent host;
				host.h_name = name;
				host.h_aliases = nullptr;
				host.h_addrtype = AF_INET;
				host.h_length = sizeof(in_addr);
				host.h_addr_list = reinterpret_cast<char **>(addr_list);

				return &host;
			}

#pragma warning(push)
#pragma warning(disable: 4996)

			// ReSharper disable once CppDeprecatedEntity
			return gethostbyname(name);

#pragma warning(pop)
		}

		bool register_hook(const std::string& process, void* stub)
		{
			const utils::nt::module main;

			auto result = false;
			result = result || utils::hook::iat(main, "wsock32.dll", process, stub);
			result = result || utils::hook::iat(main, "WS2_32.dll", process, stub);
			return result;
		}
	}
}

namespace demonware
{
	bool dw::terminate_;
	std::thread dw::message_thread_;
	std::recursive_mutex dw::server_mutex_;
	std::map<SOCKET, bool> dw::blocking_sockets_;
	std::map<SOCKET, std::shared_ptr<service_server>> dw::socket_links_;
	std::map<unsigned long, std::shared_ptr<service_server>> dw::servers_;
	std::map<unsigned long, std::shared_ptr<stun_server>> dw::stun_servers_;
	std::map<SOCKET, std::queue<std::pair<std::string, std::string>>> dw::datagram_packets_;

	uint8_t dw::encryption_key_[24];
	uint8_t dw::decryption_key_[24];

	std::shared_ptr<service_server> dw::find_server_by_name(const std::string& name)
	{
		std::lock_guard _(server_mutex_);
		return find_server_by_address(utils::cryptography::jenkins_one_at_a_time::compute(name));
	}

	std::shared_ptr<service_server> dw::find_server_by_address(const unsigned long address)
	{
		std::lock_guard _(server_mutex_);

		const auto server = servers_.find(address);
		if (server != servers_.end())
		{
			return server->second;
		}

		return std::shared_ptr<service_server>();
	}

	std::shared_ptr<stun_server> dw::find_stun_server_by_name(const std::string& name)
	{
		std::lock_guard _(server_mutex_);
		return find_stun_server_by_address(utils::cryptography::jenkins_one_at_a_time::compute(name));
	}

	std::shared_ptr<stun_server> dw::find_stun_server_by_address(const unsigned long address)
	{
		std::lock_guard _(server_mutex_);

		const auto server = stun_servers_.find(address);
		if (server != stun_servers_.end())
		{
			return server->second;
		}

		return std::shared_ptr<stun_server>();
	}

	std::shared_ptr<service_server> dw::find_server_by_socket(const SOCKET s)
	{
		std::lock_guard _(server_mutex_);

		const auto server = socket_links_.find(s);
		if (server != socket_links_.end())
		{
			return server->second;
		}

		return std::shared_ptr<service_server>();
	}

	bool dw::link_socket(const SOCKET s, const unsigned long address)
	{
		std::lock_guard _(server_mutex_);

		const auto server = find_server_by_address(address);
		if (!server) return false;

		socket_links_[s] = server;
		return true;
	}

	void dw::unlink_socket(const SOCKET sock)
	{
		std::lock_guard _(server_mutex_);

		const auto server = socket_links_.find(sock);
		if (server != socket_links_.end())
		{
			socket_links_.erase(server);
		}

		const auto dgram_packets = datagram_packets_.find(sock);
		if (dgram_packets != datagram_packets_.end())
		{
			datagram_packets_.erase(dgram_packets);
		}
	}

	int dw::recv_datagam_packet(const SOCKET s, char* buf, const int len, sockaddr* from, int* fromlen)
	{
		std::unique_lock lock(server_mutex_);

		auto queue = datagram_packets_.find(s);
		if (queue != datagram_packets_.end())
		{
			const auto blocking = is_blocking_socket(s, UDP_BLOCKING);

			lock.unlock();
			while (blocking && queue->second.empty())
			{
				std::this_thread::sleep_for(1ms);
			}
			lock.lock();

			if (!queue->second.empty())
			{
				auto packet = queue->second.front();
				queue->second.pop();

				*fromlen = INT(packet.first.size());
				std::memcpy(from, packet.first.data(), *fromlen);

				const int size = std::min(len, INT(packet.second.size()));
				std::memcpy(buf, packet.second.data(), size);

				return size;
			}

			WSASetLastError(WSAEWOULDBLOCK);
			return -1;
		}

		return 0;
	}

	void dw::send_datagram_packet(const SOCKET s, const std::string& data, const sockaddr* to, const int tolen)
	{
		std::lock_guard _(server_mutex_);
		datagram_packets_[s].push({std::string(LPSTR(to), tolen), data});
	}

	bool dw::is_blocking_socket(const SOCKET s, const bool def)
	{
		std::lock_guard _(server_mutex_);

		if (blocking_sockets_.find(s) != blocking_sockets_.end())
		{
			return blocking_sockets_[s];
		}

		return def;
	}

	void dw::remove_blocking_socket(const SOCKET s)
	{
		std::lock_guard _(server_mutex_);

		const auto entry = blocking_sockets_.find(s);
		if (entry != blocking_sockets_.end())
		{
			blocking_sockets_.erase(entry);
		}
	}

	void dw::set_blocking_socket(const SOCKET s, const bool blocking)
	{
		std::lock_guard _(server_mutex_);
		blocking_sockets_[s] = blocking;
	}

	uint8_t* dw::get_key(const bool encrypt)
	{
		return encrypt ? encryption_key_ : decryption_key_;
	}

	void dw::set_key(const bool encrypt, uint8_t* key)
	{
		static_assert(sizeof encryption_key_ == sizeof decryption_key_);
		std::memcpy(encrypt ? encryption_key_ : decryption_key_, key, sizeof encryption_key_);
	}

	void dw::server_thread()
	{
		terminate_ = false;
		while (!terminate_)
		{
			std::unique_lock lock(server_mutex_);

			for (auto& server : servers_)
			{
				server.second->run_frame();
			}

			lock.unlock();

			std::this_thread::sleep_for(50ms);
		}
	}

	void dw::pre_destroy()
	{
		std::lock_guard _(server_mutex_);

		terminate_ = true;
		if (message_thread_.joinable())
		{
			message_thread_.join();
		}

		servers_.clear();
		stun_servers_.clear();
		socket_links_.clear();
		blocking_sockets_.clear();
		datagram_packets_.clear();
	}

	dw::dw()
	{
		register_stun_server("mw3-stun.us.demonware.net");
		register_stun_server("mw3-stun.eu.demonware.net");
		register_stun_server("stun.jp.demonware.net");
		register_stun_server("stun.au.demonware.net");
		register_stun_server("stun.eu.demonware.net");
		register_stun_server("stun.us.demonware.net");

		auto lsg_server = register_server("mw3-pc-lobby.prod.demonware.net");
		auto auth_server = register_server("mw3-pc-auth.prod.demonware.net");

		auth_server->register_service<bdDediAuth>();
		auth_server->register_service<bdSteamAuth>();
		auth_server->register_service<bdDediRSAAuth>();

		lsg_server->register_service<bdLSGHello>();
		lsg_server->register_service<bdStorage>();
		lsg_server->register_service<bdTitleUtilities>();
		lsg_server->register_service<bdDML>();
		/*lsg_server->register_service<bdMatchMaking>();
		lsg_server->register_service<bdBandwidthTest>();
		lsg_server->register_service<bdGroup>();
		lsg_server->register_service<bdAnticheat>();
		lsg_server->register_service<bdRelayService>();*/
	}

	void dw::post_load()
	{
		message_thread_ = std::thread(server_thread);

		io::register_hook("send", io::send);
		io::register_hook("recv", io::recv);
		io::register_hook("sendto", io::send_to);
		io::register_hook("recvfrom", io::recv_from);
		io::register_hook("connect", io::connect);
		io::register_hook("closesocket", io::close_socket);
		io::register_hook("ioctlsocket", io::ioctl_socket);
		io::register_hook("gethostbyname", io::get_host_by_name);

		utils::hook(SELECT_VALUE(0x6F40A0, 0x6EE1C0, 0x611310), bd_logger_stub, HOOK_JUMP).install()->quick();
	}

	void dw::bd_logger_stub(int /*type*/, const char* const /*channelName*/, const char*, const char* const /*file*/,
	                        const char* const function, const unsigned int /*line*/, const char* const msg, ...)
	{
		char buffer[2048];

		va_list ap;
		va_start(ap, msg);

		vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, msg, ap);
		printf("%s: %s\n", function, buffer);

		va_end(ap);
	}

	REGISTER_MODULE(dw)
}
