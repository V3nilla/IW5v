#pragma once
#include "loader/module_loader.hpp"
#include "game/demonware/stun_server.hpp"
#include "game/demonware/service_server.hpp"

#define TCP_BLOCKING true
#define UDP_BLOCKING false

namespace demonware
{
	class dw final : public module
	{
	public:
		dw();

		void post_load() override;
		void pre_destroy() override;

		template <typename... Args>
		static std::shared_ptr<service_server> register_server(Args ... args)
		{
			std::lock_guard _(server_mutex_);
			auto server = std::make_shared<service_server>(args...);
			servers_[server->get_address()] = server;
			return server;
		}

		static std::shared_ptr<stun_server> register_stun_server(const std::string& name)
		{
			std::lock_guard _(server_mutex_);
			auto server = std::make_shared<stun_server>(name);
			stun_servers_[server->get_address()] = server;
			return server;
		}

		static int recv_datagam_packet(SOCKET s, char* buf, int len, sockaddr* from, int* fromlen);
		static void send_datagram_packet(SOCKET s, const std::string& data, const sockaddr* to, int tolen);

		static bool is_blocking_socket(SOCKET s, bool def);
		static void remove_blocking_socket(SOCKET s);
		static void set_blocking_socket(SOCKET s, bool blocking);

		static std::shared_ptr<stun_server> find_stun_server_by_name(const std::string& name);
		static std::shared_ptr<stun_server> find_stun_server_by_address(unsigned long address);

		static std::shared_ptr<service_server> find_server_by_name(const std::string& name);
		static std::shared_ptr<service_server> find_server_by_address(unsigned long address);
		static std::shared_ptr<service_server> find_server_by_socket(SOCKET s);
		static bool link_socket(SOCKET sock, unsigned long address);
		static void unlink_socket(SOCKET sock);

		static void set_key(bool encrypt, uint8_t* key);
		static uint8_t* get_key(bool encrypt);

	private:
		static bool terminate_;
		static std::thread message_thread_;
		static std::recursive_mutex server_mutex_;

		static uint8_t encryption_key_[24];
		static uint8_t decryption_key_[24];

		static std::map<SOCKET, bool> blocking_sockets_;
		static std::map<SOCKET, std::shared_ptr<service_server>> socket_links_;
		static std::map<unsigned long, std::shared_ptr<service_server>> servers_;
		static std::map<unsigned long, std::shared_ptr<stun_server>> stun_servers_;
		static std::map<SOCKET, std::queue<std::pair<std::string, std::string>>> datagram_packets_;

		static void server_thread();

		static void bd_logger_stub(int /*type*/, const char* /*channelName*/, const char*, const char* /*file*/,
		                           const char* function, unsigned int /*line*/, const char* msg, ...);
	};
}
