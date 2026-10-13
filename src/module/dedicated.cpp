#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"
#include "utils/string.hpp"
#include "utils/io.hpp"
#include "module/scheduler.hpp"
#include "steam/steam.hpp"

// Joining a dedicated server with "connect <ip>:<net_queryPort>", and a match a player hosts with "connect <ip>"
// (its net_port, 27016 by default: see "a match hosted by a player" below).
// MW3 PC sends all game traffic to an internet address over DemonWare connections (Sys_SendPacket: type 4 -> the DW
// router 0x614C50 / dedicated 0x57CB20); raw UDP on the server's open port (net_queryPort, socket 0x5948AC4) only
// carries 0xF1FF "XNet" queries. So connecting by address (CL_Connect_f 0x4933A0: not loopback -> clc.state 4) first
// asks the server for its session info: CL_CheckForResend (0x48FF30) sends a query with a random nonce every 3 s
// (0x5CE5B0). The dedicated server checks it (0x53A180), turns it into "querysessioninfo <xuid> <nonce> <address>"
// (0x53A530) and answers with a 0x60 byte blob (0x4F4D00: its XNADDR - security id, local and public address, NAT
// type - and key, a key for the player, the nonce) sent as a DemonWare instant message to the player's xuid (0x538B40).
// The client gets that through the DemonWare lobby (dwLobbyEventHandler::onInstantMessage 0x63B6A0 -> 0x5CB510): nonce
// checked, the server's key registered, clc.serverAddress made a DemonWare connection to the XNADDR (0x494290), then
// getchallenge (state 3; SV_GetChallenge 0x4F6520 wants the player key from the session info, 0x538F50) and connect (5)
// over it.
// Under open-iw5 that can't work: DemonWare is emulated inside each process (the instant message never leaves the
// server), the fake STUN server gives everybody the public address 127.0.0.1:3074, the server's own address list comes
// from gethostbyname(hostname) (a Docker container's or WSL adapter's address, 127.0.1.1 on Ubuntu), and the dedicated
// server's DemonWare lobby never comes up (the emulator's dedicated server auth: "bdLobbyConnection::
// getMessageToDispatch: Decryption failed"), so its clock never syncs (the query parser drops everything until it is)
// and it never pumps its DemonWare socket (net_port): a DemonWare connection to it can't come up. Even with that fixed,
// a DemonWare connection needs NAT traversal, which the fake STUN can't give (Docker, NAT, Tailscale).
// So a dedicated server skips DemonWare: the game traffic goes as plain UDP to and from the server's open port, the one
// the player connects to:
// - server: the clock counts as synced, with the local time (queries carry the client's time, 60 s of slack); the open
//   port listens on every address (net_ip "localhost" bound it to the first adapter's address); the query checks that
//   the client's own public address, the address and port it sent to are the server's are skipped (none can hold behind
//   the fake STUN, NAT or Docker); the session info goes back raw on the open port, out of band ("sessioninfo <hex>" +
//   the XNet checksum the client's Sys_GetPacket 0x5CD3B0 wants) to where the query came from, at most once a second
//   per address. Every other packet on the open port (Sys_GetPacket 0x53A600 gave all of them to the query parser) goes
//   to the game as an internet address (type 4) with a raw handle (below)
// - client: takes the session info only from the server it is connecting to while it waits for it (state 4) and hands
//   it to the game's own instant message handler (nonce checked, the server's key registered), but the server's
//   address gets a raw handle instead of a DemonWare connection; getchallenge, connect and the game then go raw over the
//   socket the query went out on (0x5AA302C)
// Raw handles: netadr_t's handle (+0x14) is a DemonWare connection index (0 .. 0x99, 0x9A = none) and it is all
// NET_CompareAdr (MP 0x55C110, dedicated 0x4E0A90) and NET_CompareBaseAdr compare for internet addresses, so every
// address that talks raw gets a handle of its own from raw_handle_base on (kept while the address sent something within
// raw_peer_idle). Sys_SendPacket sends to those raw (its call to the DemonWare router: MP 0x5CD631, dedicated 0x53A8D1)
// and the client's connection status (0x614730, CL_CheckForResend) is "connected" for them. The client only gives raw
// handles to the servers it connects to raw, so a listen host's traffic is as before. On every map spawn the dedicated
// server publishes each player's XNADDR from its DemonWare connection (0x57D350) and drops whoever has none: a raw player
// gets a marker XNADDR there, which clients don't try to connect to.
// Logs: <game>\open-iw5\dedicated.log (the dedicated server: who sent "connect", from where) / connect.log (the client).
// Players need different Steam ids (steam\interfaces\user.cpp: <game>\open-iw5\steam_id.txt): the server gives a joining
// player the slot of the one with its xuid.
namespace
{
#pragma pack(push, 1)
	struct netadr_t
	{
		int type; // 0 bot, 2 loopback, 4 ip
		unsigned char ip[4];
		unsigned short port; // network order
		char pad[0xA];
		int handle;
	};
#pragma pack(pop)

	static_assert(sizeof(netadr_t) == 0x18);

	constexpr char session_info_command[] = "sessioninfo";
	constexpr unsigned short xnet_magic = 0xF1FF;
	constexpr unsigned short session_info_type = 1;
	constexpr unsigned int session_info_size = 0x60;

	// ---- iw5mp_server (dedicated) ----
	constexpr DWORD server_time_synced_dedi = 0x16186E5; // bool, set by 0x4D1BE0 when a getServerTime task (0xE) is done
	constexpr DWORD server_time_offset_dedi = 0x1B6A540; // int: DemonWare time - time()
	constexpr DWORD net_ip_default_dedi = 0x53AACE; // NET_Config 0x53AAB0: Dvar_RegisterString("net_ip", "localhost")
	constexpr DWORD check_own_address_dedi = 0x53A297; // 0x53A180: the client's public address (+0x24) = its source
	constexpr DWORD check_port_dedi = 0x53A2AA; // ... the port it sent to (+0xC) = net_queryPort
	constexpr DWORD check_address_dedi = 0x53A304; // ... the address it sent to (+8) is one of ours
	constexpr DWORD reply_call_dedi = 0x4F4E8B; // querysessioninfo (0x4F4D00) -> 0x538B40 (0, xuid low, xuid high, blob)
	constexpr DWORD sv_cmd_args_dedi = 0x1B5E7D8; // the server command tokenizer (querysessioninfo is a server command)
	constexpr DWORD sv_cmd_argc_dedi = 0x1B5E81C;
	constexpr DWORD sv_cmd_argv_dedi = 0x1B5E83C;
	constexpr DWORD ip_socket_dedi = 0x5948AC4; // SOCKET: the open port (net_queryPort)
	constexpr DWORD dw_send_dedi = 0x57CB20; // (size, data, netadr_t): to DemonWare connection netadr.handle
	constexpr DWORD dw_send_call_dedi = 0x53A8D1; // Sys_SendPacket 0x53A850, type 4
	constexpr DWORD open_port_packet_dedi = 0x53A82E; // Sys_GetPacket: a packet on the open port -> the query parser
	// sv_open_port_packet_stub: the query parser 0x53A530 (eax msg, net_from), Sys_GetPacket returning 0 0x53A839 / 1 0x53A842
	constexpr unsigned int query_magic = 0xFFFDFDFF; // an XNet query: 0x30 bytes from this
	constexpr int query_size = 0x30;
	// (XNADDR out (0x19 bytes, may be null), ?, netadr_t): a remote's XNADDR from its DemonWare connection. Fails for a raw
	// handle, and 0x4FAF60 (the session members' configstrings 0x1A3 + n, sent on every map spawn) then drops the player
	// with EXE_TRANSMITERROR
	constexpr DWORD remote_xnaddr_dedi = 0x57D350;
	constexpr DWORD remote_xnaddr_calls_dedi[] = {0x4AB005, 0x4F9114, 0x4FB0F3, 0x53B8B2};
	constexpr size_t xnaddr_size = 0x19;

	// ---- iw5mp (client) ----
	constexpr DWORD cl_packet_hook = 0x4912FB; // CL_ConnectionlessPacket (0x4912A0): the "openport" compare
	constexpr DWORD connection_state = 0x10625F4; // clc.state
	constexpr DWORD server_address = 0xB3F740; // clc.serverAddress
	constexpr int ca_session_info = 4; // waiting for the session info (CL_Connect_f to an internet address)
	constexpr DWORD instant_message_addr = 0x5CB510; // (controller, xuid low, xuid high, sender name, message)
	constexpr DWORD ip_socket = 0x5AA302C; // SOCKET: the one the query goes out on (NET_SendPacket type 3)
	constexpr DWORD dw_send = 0x614C50; // as the dedicated server's
	constexpr DWORD dw_send_call = 0x5CD631; // Sys_SendPacket 0x5CD5A0, type 4
	constexpr DWORD sockadr_to_netadr = 0x5B7730; // (sockaddr*, netadr_t*): type 4, ip, port; the handle stays as it was
	constexpr DWORD sockadr_to_netadr_call = 0x5CD508; // Sys_GetPacket 0x5CD3B0
	constexpr DWORD connection_status = 0x614730; // (handle) -> 0 pending, 1 connected, 2 failed
	constexpr DWORD connection_status_call = 0x48FFA7; // CL_CheckForResend 0x48FF30
	constexpr DWORD connect_xnaddr = 0x6152B0; // (netadr_t* out, XNADDR + 8, XNADDR*): a DemonWare connection to the XNADDR
	// its callers: the session info handler 0x494290, XNetXnAddrToInAddr 0x5CE250 (the session join 0x632A60 after it),
	// 0x63A8A0
	constexpr DWORD connect_xnaddr_calls[] = {0x4942CE, 0x5CE262, 0x63A901};
	constexpr size_t blob_xnaddr = 8; // the session info: the server's XNADDR, its security id first
	constexpr size_t security_id_size = 8;
	// a raw player's XNADDR as the dedicated server gives it (a session member without a DemonWare connection: the client
	// side 0x469F2A copes with one it can't connect to), the raw handle after it
	constexpr unsigned char raw_xnaddr_marker[security_id_size] = {'I', 'W', '5', 'V', 'R', 'A', 'W', 0};
	constexpr DWORD open_connection = 0x614810; // (netadr_t*): opens netadr.handle's DemonWare connection
	constexpr DWORD open_connection_calls[] = {0x4942E3, 0x498AF3, 0x499B55, 0x49D98E, 0x5CE2F4};

	// ---- iw5mp: a match hosted by a player (listen host) ----
	constexpr DWORD net_get_packet = 0x55C3D0; // jmp Sys_GetPacket: the client's (0x553D30) and the server's packet loop
	constexpr DWORD sys_get_packet = 0x5CD3B0; // (netadr_t* from, msg_t*): 1 = a packet (its XNet checksum checked)
	constexpr DWORD connection_status_netadr = 0x614800; // (netadr_t*): 0x614730 for netadr.handle (the party code's)
	// (bdReference<bdConnection>* out, netadr_t*): the DemonWare connection of netadr.handle. Only the reliable out of
	// band channels (0x5C4280: party messages like pa_joinfailed, pa_memberjoin, partystate; 0x24 channels at 0x5A63968)
	// ask, to see that the address has one, and drop the message when it hasn't
	constexpr DWORD reliable_connection_call = 0x5C428D;
	constexpr DWORD reliable_connection = 0x614320;
	// (netadr_t, XNADDR out, size, ?): a remote's XNADDR from its DemonWare connection (ours for the "none" handle).
	// 0x574E00 publishes every active player's as the session configstrings 0x1A3 + n on a map start and on map_restart
	// (S&D rounds, 0x56F050) and drops the ones it can't get with EXE_TRANSMITERROR
	constexpr DWORD remote_xnaddr = 0x614400;
	constexpr DWORD remote_xnaddr_calls[] = {0x48D6D8, 0x5CE337, 0x5CE387};
	constexpr DWORD session_xnaddr_call = 0x574F60; // 0x574E00's, see sv_session_xnaddr
	constexpr DWORD steam_lobby = 0x5CF0500; // the game's Steam lobby object (0x63A200)
	// in it: the session an accepted invite joins (0x64F53E copies 0x31 bytes: id, XNADDR, key), filled from the lobby
	// data like the lobby enter handler 0x6399E0 does
	constexpr int lobby_session_id = 0x1EF;
	constexpr int lobby_xnaddr = 0x1F7;
	constexpr int lobby_key = 0x210;
	constexpr int lobby_common_addr = 0x220; // bdCommonAddr: dw_serialized_addr read, then serialized to the XNADDR
	constexpr DWORD read_session_id = 0x55CCA0; // (hex text, out 8 bytes)
	constexpr DWORD read_session_key = 0x55CBF0; // (hex text, out 16 bytes)
	constexpr DWORD read_common_addr = 0x638A30; // (text, bdCommonAddr*)
	constexpr DWORD common_addr_to_xnaddr = 0x6EF110; // bdCommonAddr::serialize (this, XNADDR out)
	constexpr DWORD invite_state = 0x5AA2D58; // 2: an invite to accept (0x64F2E0, every frame)
	constexpr DWORD invite_address = 0x5AA2D64; // empty: join the Steam lobby's session, else "connect" to it
	// 1: a friend's invite (the Steam overlay's join, 0x637910 -> 0x5C9DF0): the party join asks as join type 2
	// (0x4B1050 -> party +4, sent in pa_joinrequest); a private party's host turns type 1 down (pa_joinfailed 0x11)
	constexpr DWORD invite_from_friend = 0x5AA2DF1;
	constexpr DWORD online_state = 0x8B80E0; // 3 .. 6: online (0x5CAB00 errors otherwise)
	constexpr DWORD own_xnaddr = 0x5CAB00; // -> our XNADDR (xnaddr_size)
	constexpr char lobby_command[] = "iw5vlobby"; // a host's answer to the query: its lobby data
	constexpr char hello_command[] = "iw5vhello"; // a joining player to its host: its XNADDR
	constexpr DWORD hello_interval = 2000; // ms, while the host talks to us
	constexpr DWORD host_silent = 30000; // ms without a packet from the host: stop

	// ---- raw connections (both) ----
	constexpr int raw_handle_base = 0x1000; // above DemonWare's connection indexes
	constexpr int max_raw_peers = 0x1000;
	constexpr DWORD raw_peer_idle = 120000; // ms

	struct raw_peer
	{
		unsigned int ip; // network order, as in netadr_t
		unsigned short port;
		DWORD last_seen;
	};

	std::mutex raw_mutex;
	std::vector<raw_peer> raw_peers; // [handle - raw_handle_base]
	std::unordered_map<unsigned long long, int> raw_indexes;

	struct raw_server
	{
		unsigned char security_id[security_id_size];
		netadr_t address;
	};

	std::vector<raw_server> raw_servers; // the client: the servers it connected to raw (under raw_mutex)

	// a listen host's players and a player's host: XNADDR -> raw address (under raw_mutex)
	struct raw_xnaddr
	{
		unsigned char xnaddr[xnaddr_size];
		netadr_t address;
	};

	std::vector<raw_xnaddr> raw_xnaddrs;

	struct
	{
		bool active;
		netadr_t address; // with its raw handle
		DWORD last_hello;
	} raw_host{}; // the player: the listen host it joined raw (main thread)

	std::string log_path;

	std::string to_hex(const unsigned char* data, size_t size);

	// <game>\open-iw5\dedicated.log (the dedicated server) / connect.log (the client)
	void net_log(const std::string& text)
	{
		static std::mutex mutex;
		SYSTEMTIME time;
		GetLocalTime(&time);
		std::lock_guard _(mutex);
		utils::io::write_file(log_path, utils::string::va("%04d-%02d-%02d %02d:%02d:%02d ", time.wYear, time.wMonth, time.wDay,
		                                                  time.wHour, time.wMinute, time.wSecond) + text + "\n", true);
	}

	std::string address_text(const unsigned int ip, const unsigned short port)
	{
		const auto* b = reinterpret_cast<const unsigned char*>(&ip);
		return utils::string::va("%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], ntohs(port));
	}

	bool is_raw_handle(const int handle)
	{
		return handle >= raw_handle_base && handle < raw_handle_base + max_raw_peers;
	}

	// the address's raw handle (its last packet now); create: give it one when it has none (taking the one of the address
	// idle for longest, if that is raw_peer_idle or more); -1 when it has none
	int raw_handle(const netadr_t& address, const bool create)
	{
		unsigned int ip;
		std::memcpy(&ip, address.ip, sizeof(ip));
		const auto key = static_cast<unsigned long long>(ip) << 16 | address.port;
		const auto now = GetTickCount();

		std::lock_guard _(raw_mutex);
		const auto known = raw_indexes.find(key);
		if (known != raw_indexes.end())
		{
			raw_peers[known->second].last_seen = now;
			return raw_handle_base + known->second;
		}

		if (!create) return -1;

		auto index = static_cast<int>(raw_peers.size());
		if (index < max_raw_peers)
		{
			raw_peers.push_back({ip, address.port, now});
		}
		else
		{
			index = -1;
			auto idle = raw_peer_idle;
			for (auto i = 0; i < max_raw_peers; ++i)
			{
				if (now - raw_peers[i].last_seen >= idle)
				{
					idle = now - raw_peers[i].last_seen;
					index = i;
				}
			}

			if (index < 0) return -1;
			raw_indexes.erase(static_cast<unsigned long long>(raw_peers[index].ip) << 16 | raw_peers[index].port);
			raw_peers[index] = {ip, address.port, now};
		}

		raw_indexes[key] = index;
		return raw_handle_base + index;
	}

	// Sys_SendPacket's data has its checksum already; >= 0 sent, as the DemonWare router
	int send_raw(const DWORD socket_addr, const int size, const char* data, const netadr_t& to)
	{
		sockaddr_in address{};
		address.sin_family = AF_INET;
		{
			std::lock_guard _(raw_mutex);
			const auto index = to.handle - raw_handle_base;
			if (index >= static_cast<int>(raw_peers.size())) return -1;
			address.sin_addr.s_addr = raw_peers[index].ip;
			address.sin_port = raw_peers[index].port;
		}

		const auto socket = *reinterpret_cast<SOCKET*>(socket_addr);
		if (!socket || socket == INVALID_SOCKET) return -1;
		return sendto(socket, data, size, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));
	}

	int __cdecl sv_send_packet(const int size, const char* data, const netadr_t to)
	{
		if (is_raw_handle(to.handle)) return send_raw(ip_socket_dedi, size, data, to);
		return reinterpret_cast<int(*)(int, const char*, netadr_t)>(dw_send_dedi)(size, data, to);
	}

	bool __cdecl sv_remote_xnaddr(unsigned char* xnaddr, void* other, const netadr_t address)
	{
		if (!is_raw_handle(address.handle))
		{
			return reinterpret_cast<bool(*)(unsigned char*, void*, netadr_t)>(remote_xnaddr_dedi)(xnaddr, other, address);
		}

		if (xnaddr)
		{
			std::memset(xnaddr, 0, xnaddr_size);
			std::memcpy(xnaddr, raw_xnaddr_marker, sizeof(raw_xnaddr_marker));
			std::memcpy(xnaddr + sizeof(raw_xnaddr_marker), &address.handle, sizeof(address.handle));
		}
		return true;
	}

	int __cdecl cl_send_packet(const int size, const char* data, const netadr_t to)
	{
		if (is_raw_handle(to.handle)) return send_raw(ip_socket, size, data, to);
		return reinterpret_cast<int(*)(int, const char*, netadr_t)>(dw_send)(size, data, to);
	}

	// the dedicated server: a packet on the open port that isn't a query is the game's (true), with a raw handle
	bool __cdecl sv_open_port_packet(const game::native::msg_t* msg, netadr_t* from)
	{
		if (msg->cursize >= query_size && *reinterpret_cast<const unsigned int*>(msg->data) == query_magic) return false;
		if (from->type != 4) return false;

		const auto handle = raw_handle(*from, true);
		if (handle < 0) return false; // the query parser drops it
		from->handle = handle;

		// a player joining: "connect <xuid> <userinfo>", logged at most 20 times a minute
		static constexpr char connect[] = "\xFF\xFF\xFF\xFF" "connect ";
		if (msg->cursize > static_cast<int>(sizeof(connect)) && std::memcmp(msg->data, connect, sizeof(connect) - 1) == 0)
		{
			static auto minute = 0ull;
			static auto lines = 0;
			const auto now = GetTickCount64() / 60000;
			if (now != minute)
			{
				minute = now;
				lines = 0;
			}
			if (++lines <= 20)
			{
				unsigned int ip;
				std::memcpy(&ip, from->ip, sizeof(ip));
				net_log(utils::string::va("%s (raw handle %d): connect %.16s", address_text(ip, from->port).data(), handle,
				                          std::string(msg->data + sizeof(connect) - 1, msg->cursize - sizeof(connect) + 1).data()));
			}
		}
		return true;
	}

	__declspec(naked) void sv_open_port_packet_stub()
	{
		__asm
		{
			pushad
			push edi // net_from
			push ebx // msg
			call sv_open_port_packet
			add esp, 8
			test al, al
			popad
			jnz taken

			push edi // the replaced code: the query parser, then return 0
			mov eax, ebx
			mov ecx, 53A530h
			call ecx
			add esp, 4
			push 53A839h
			retn

		taken:
			push 53A842h // return 1
			retn
		}
	}

	// MP: every packet that came in raw (not over a DemonWare connection: those come from 0x614D60) gets the handle of
	// its address (the handle is whatever the last one was otherwise): a raw server's for its players, a listen host's
	// for the players who joined it raw
	void __cdecl cl_sockadr_to_netadr(const sockaddr_in* address, netadr_t* from)
	{
		reinterpret_cast<void(*)(const sockaddr_in*, netadr_t*)>(sockadr_to_netadr)(address, from);
		if (from->type != 4) return;

		const auto handle = raw_handle(*from, true);
		if (handle >= 0) from->handle = handle;
	}

	int __cdecl cl_connection_status(const int handle)
	{
		if (is_raw_handle(handle)) return 1;
		return reinterpret_cast<int(*)(int)>(connection_status)(handle);
	}

	int __cdecl cl_connection_status_netadr(const netadr_t* address)
	{
		return cl_connection_status(address->handle);
	}

	// a raw player's XNADDR: the one it sent in iw5vhello, else (a player on a raw dedicated server) the marker
	bool __cdecl cl_remote_xnaddr(const netadr_t address, unsigned char* xnaddr, const unsigned int size, void* other)
	{
		if (!is_raw_handle(address.handle))
		{
			return reinterpret_cast<bool(*)(netadr_t, unsigned char*, unsigned int, void*)>(remote_xnaddr)(
				address, xnaddr, size, other);
		}

		if (!xnaddr || size < xnaddr_size) return false;

		std::lock_guard _(raw_mutex);
		for (const auto& peer : raw_xnaddrs)
		{
			if (peer.address.handle == address.handle)
			{
				std::memcpy(xnaddr, peer.xnaddr, xnaddr_size);
				return true;
			}
		}

		std::memset(xnaddr, 0, xnaddr_size);
		std::memcpy(xnaddr, raw_xnaddr_marker, sizeof(raw_xnaddr_marker));
		std::memcpy(xnaddr + sizeof(raw_xnaddr_marker), &address.handle, sizeof(address.handle));
		return true;
	}

	// 0x574E00 (map start, map_restart) also asks for a Find Game lobby member's bot: it has a session
	// xuid, but its NA_BOT address has no handle, so the lookup failed and the game dropped every member's bot with
	// EXE_TRANSMITERROR at each S&D round (0x574F78) and freed its member 2 s later. Its XNADDR is its member's, all zero,
	// as the configstring had it when the bot connected (0x5755E0). Plain bots have no session xuid and never get here.
	// One hook for this call
	bool __cdecl sv_session_xnaddr(const netadr_t address, unsigned char* xnaddr, const unsigned int size, void* other)
	{
		if (address.type == 0)
		{
			if (xnaddr) std::memset(xnaddr, 0, std::min<size_t>(size, xnaddr_size));
			return true;
		}

		return cl_remote_xnaddr(address, xnaddr, size, other);
	}

	// a stand-in bdConnection reference for raw handles: 0x5C4280 only checks it is there and releases it (refcount at +4,
	// the vtable's first function deletes it at 0)
	void __fastcall raw_connection_delete(void*, void*, int)
	{
	}

	struct
	{
		const void* vtable;
		volatile long refs;
	} raw_connection{};

	const void* const raw_connection_vtable[] = {reinterpret_cast<const void*>(raw_connection_delete)};

	void** __cdecl cl_reliable_connection(void** connection, const netadr_t* address)
	{
		if (address && is_raw_handle(address->handle))
		{
			raw_connection.vtable = raw_connection_vtable;
			raw_connection.refs = 0x40000000;
			*connection = &raw_connection;
			return connection;
		}
		return reinterpret_cast<void**(*)(void**, const netadr_t*)>(reliable_connection)(connection, address);
	}

	// a raw server's XNADDR is its address with its raw handle, no DemonWare connection; a raw player's (another player
	// on a raw server) has none. A listen host's players and a player's listen host: the address their XNADDR came
	// from (iw5vhello / iw5vlobby)
	bool __cdecl cl_connect_xnaddr(netadr_t* address, const unsigned char* common_addr, const unsigned char* xnaddr)
	{
		if (xnaddr && std::memcmp(xnaddr, raw_xnaddr_marker, sizeof(raw_xnaddr_marker)) == 0)
		{
			if (address)
			{
				*address = {};
				address->handle = -1;
			}
			return false;
		}

		if (address && common_addr)
		{
			std::lock_guard _(raw_mutex);
			for (const auto& peer : raw_xnaddrs)
			{
				if (std::memcmp(peer.xnaddr, common_addr, xnaddr_size) == 0)
				{
					*address = peer.address;
					return true;
				}
			}
		}

		if (address && xnaddr)
		{
			std::lock_guard _(raw_mutex);
			for (const auto& server : raw_servers)
			{
				if (std::memcmp(server.security_id, xnaddr, security_id_size) == 0)
				{
					*address = server.address;
					return true;
				}
			}
		}

		return reinterpret_cast<bool(*)(netadr_t*, const void*, const void*)>(connect_xnaddr)(address, common_addr, xnaddr);
	}

	bool __cdecl cl_open_connection(netadr_t* address)
	{
		if (address && is_raw_handle(address->handle)) return true;
		return reinterpret_cast<bool(*)(netadr_t*)>(open_connection)(address);
	}

	// the XNet checksum (dedicated 0x539FD0, MP 0x5CD200): RFC 1071 over big endian words, an odd last byte added as is;
	// sent big endian after the data
	unsigned short xnet_checksum(const unsigned char* data, size_t size)
	{
		unsigned int sum = 0;
		for (; size > 1; data += 2, size -= 2) sum += static_cast<unsigned int>(data[0] << 8 | data[1]);
		if (size) sum += data[0];
		while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
		return static_cast<unsigned short>(~sum);
	}

	const char* sv_cmd_argv(const int index)
	{
		const auto args = *reinterpret_cast<int*>(sv_cmd_args_dedi);
		if (index >= reinterpret_cast<int*>(sv_cmd_argc_dedi)[args]) return "";
		return reinterpret_cast<const char***>(sv_cmd_argv_dedi)[args][index];
	}

	// "a.b.c.d:port" (NET_AdrToString's text in querysessioninfo)
	bool parse_address(const char* text, sockaddr_in& address)
	{
		unsigned int a, b, c, d, port;
		char end;
		if (sscanf_s(text, "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &port, &end, 1) != 5 || a > 255 || b > 255 || c > 255 ||
			d > 255 || !port || port > 0xFFFF)
		{
			return false;
		}

		address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<unsigned short>(port));
		address.sin_addr.s_addr = htonl(a << 24 | b << 16 | c << 8 | d);
		return true;
	}

	std::string to_hex(const unsigned char* data, const size_t size)
	{
		static constexpr char digits[] = "0123456789abcdef";
		std::string text;
		text.reserve(size * 2);
		for (size_t i = 0; i < size; ++i)
		{
			text.push_back(digits[data[i] >> 4]);
			text.push_back(digits[data[i] & 0xF]);
		}
		return text;
	}

	bool from_hex(const char* text, std::vector<unsigned char>& data)
	{
		const auto digit = [](const char c) -> int
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};

		data.clear();
		for (; text[0] && text[1]; text += 2)
		{
			const auto high = digit(text[0]);
			const auto low = digit(text[1]);
			if (high < 0 || low < 0) return false;
			data.push_back(static_cast<unsigned char>(high << 4 | low));
		}
		return !text[0];
	}

	// one answer a second per address (a client asks every 3 s; the query's source can be forged)
	bool may_answer(const sockaddr_in& to)
	{
		static std::mutex mutex;
		std::lock_guard _(mutex);
		static std::unordered_map<unsigned long long, DWORD> last_answers;
		const auto now = GetTickCount();
		const auto key = static_cast<unsigned long long>(to.sin_addr.s_addr) << 16 | to.sin_port;

		if (last_answers.size() > 1024)
		{
			for (auto i = last_answers.begin(); i != last_answers.end();)
			{
				i = now - i->second > 10000 ? last_answers.erase(i) : std::next(i);
			}
		}

		const auto last = last_answers.find(key);
		if (last != last_answers.end() && now - last->second < 1000) return false;
		last_answers[key] = now;
		return true;
	}

	// the dedicated server: querysessioninfo's answer, raw on the open port to the address in its third argument (where
	// the query came from)
	void __cdecl send_session_info(int /*controller*/, unsigned int /*xuid_low*/, unsigned int /*xuid_high*/,
	                               const unsigned char* blob)
	{
		const auto* text = sv_cmd_argv(3);
		sockaddr_in address{};
		if (!parse_address(text, address) || *reinterpret_cast<const unsigned int*>(blob + 4) != session_info_size ||
			!may_answer(address))
		{
			return;
		}

		const auto socket = *reinterpret_cast<SOCKET*>(ip_socket_dedi);
		if (!socket || socket == INVALID_SOCKET) return;

		std::string packet("\xFF\xFF\xFF\xFF", 4);
		packet += session_info_command;
		packet += ' ';
		packet += to_hex(blob, session_info_size);
		const auto checksum = xnet_checksum(reinterpret_cast<const unsigned char*>(packet.data()), packet.size());
		packet.push_back(static_cast<char>(checksum >> 8));
		packet.push_back(static_cast<char>(checksum & 0xFF));

		if (sendto(socket, packet.data(), static_cast<int>(packet.size()), 0, reinterpret_cast<sockaddr*>(&address),
		           sizeof(address)) < 0)
		{
			printf("querysessioninfo: no answer to %s (error %d)\n", text, WSAGetLastError());
			return;
		}

		printf("querysessioninfo: session info sent to %s\n", text);
	}

	// ---- a match hosted by a player (listen host) ----
	// On Steam a friend joins a player's party through the Steam lobby the party's host made: its lobby data
	// (dw_serialized_addr, dw_sec_kid, dw_sec_key: the host's address and its party session's id and key, set by
	// 0x6398E0) goes to the lobby enter handler (0x639BB0 -> 0x6399E0, into the Steam lobby object), then the invite is
	// taken like any (0x64F2E0: 0x4B1050 joins the party session, pa_joinrequest to the host over a DemonWare connection
	// to its XNADDR). open-iw5's Steam lobbies are stubs: nobody else sees the lobby data, and DemonWare connections don't
	// come up (the fake STUN). So "connect <host address>" asks the host for it instead: CL_Connect_f sends the XNet query
	// it would send a dedicated server to the address (state 4), a listen host answers it with its lobby data
	// (iw5vlobby, raw from its game port; the game set it on its open-iw5 Steam lobby), and the player fills the Steam
	// lobby object with it and takes it as an accepted invite: the game's own party join, the host's XNADDR mapped to its
	// raw address. The host learns the player's address from iw5vhello (the player's XNADDR, sent raw every 2 s while the
	// host talks to it) and maps the player's XNADDR to it: the party and later the match go raw both ways.
	// Also needed for raw players: a stand-in DemonWare connection for the reliable party messages (0x5C4280), "connected"
	// for the party code's connection checks (0x614800), their XNADDR for the session configstrings (0x614400, else
	// EXE_TRANSMITERROR on a map start or S&D round), the invite flagged as a friend's (a private party refuses the other
	// join type), and Steam's peer authentication answered (steam\interfaces\user.cpp: "Disconnected due to VAC.").
	// Logs: connect.log, the host's (queries answered, hellos) and the player's (the lobby data taken).

	// a raw out of band packet with the XNet checksum MP's Sys_GetPacket checks on everything it gets raw
	bool send_oob_raw(const netadr_t& to, const std::string& text)
	{
		std::string packet("\xFF\xFF\xFF\xFF", 4);
		packet += text;
		const auto checksum = xnet_checksum(reinterpret_cast<const unsigned char*>(packet.data()), packet.size());
		packet.push_back(static_cast<char>(checksum >> 8));
		packet.push_back(static_cast<char>(checksum & 0xFF));

		sockaddr_in address{};
		address.sin_family = AF_INET;
		std::memcpy(&address.sin_addr, to.ip, sizeof(to.ip));
		address.sin_port = to.port;

		const auto socket = *reinterpret_cast<SOCKET*>(ip_socket);
		if (!socket || socket == INVALID_SOCKET) return false;
		return sendto(socket, packet.data(), static_cast<int>(packet.size()), 0, reinterpret_cast<sockaddr*>(&address),
		              sizeof(address)) >= 0;
	}

	std::string text_to_hex(const std::string& text)
	{
		return to_hex(reinterpret_cast<const unsigned char*>(text.data()), text.size());
	}

	bool hex_to_text(const char* hex, std::string& text)
	{
		std::vector<unsigned char> data;
		if (!from_hex(hex, data) || data.empty() || data.size() > 0x100) return false;
		text.assign(data.begin(), data.end());
		return text.find('\0') == std::string::npos;
	}

	void remember_xnaddr(const unsigned char* xnaddr, const netadr_t& address)
	{
		std::lock_guard _(raw_mutex);
		for (auto& peer : raw_xnaddrs)
		{
			if (std::memcmp(peer.xnaddr, xnaddr, xnaddr_size) == 0)
			{
				peer.address = address;
				return;
			}
		}

		if (raw_xnaddrs.size() >= 64) raw_xnaddrs.erase(raw_xnaddrs.begin());
		raw_xnaddr peer{};
		std::memcpy(peer.xnaddr, xnaddr, xnaddr_size);
		peer.address = address;
		raw_xnaddrs.push_back(peer);
	}

	// the host: a player's XNet query (connect <our address>) -> our lobby data, if we host a party
	void answer_query(const netadr_t& from)
	{
		const auto addr = steam::get_own_lobby_data("dw_serialized_addr");
		const auto id = steam::get_own_lobby_data("dw_sec_kid");
		const auto key = steam::get_own_lobby_data("dw_sec_key");
		unsigned int ip;
		std::memcpy(&ip, from.ip, sizeof(ip));

		if (addr.empty() || id.empty() || key.empty())
		{
			static auto lines = 0;
			if (++lines <= 20) net_log(address_text(ip, from.port) + ": query, but we host no party");
			return;
		}

		sockaddr_in to{};
		to.sin_addr.s_addr = ip;
		to.sin_port = from.port;
		if (!may_answer(to)) return;

		const auto sent = send_oob_raw(from, utils::string::va("%s %s %s %s", lobby_command, text_to_hex(addr).data(),
		                                                       text_to_hex(id).data(), text_to_hex(key).data()));
		net_log(utils::string::va("%s: query, lobby data sent (%d): %s / %s", address_text(ip, from.port).data(), sent,
		                          addr.data(), id.data()));
	}

	// the host: a joining player's XNADDR (iw5vhello <hex>): its packets go to where this came from
	void take_hello(const netadr_t& from, const char* text)
	{
		std::vector<unsigned char> xnaddr;
		if (!from_hex(text, xnaddr) || xnaddr.size() != xnaddr_size) return;

		auto address = from;
		address.handle = raw_handle(from, true);
		if (address.handle < 0) return;

		auto known = false;
		{
			std::lock_guard _(raw_mutex);
			for (const auto& peer : raw_xnaddrs)
			{
				if (std::memcmp(peer.xnaddr, xnaddr.data(), xnaddr_size) == 0 && peer.address.handle == address.handle)
				{
					known = true;
					break;
				}
			}
		}

		remember_xnaddr(xnaddr.data(), address);
		if (!known)
		{
			unsigned int ip;
			std::memcpy(&ip, from.ip, sizeof(ip));
			net_log(utils::string::va("%s: hello, XNADDR %s, raw handle %d", address_text(ip, from.port).data(),
			                          to_hex(xnaddr.data(), xnaddr_size).data(), address.handle));
		}
	}

	// every packet MP gets (both packet loops: the client's, or the server thread's while a server runs): the raw join's
	// own packets (true) don't go to the game
	bool raw_join_packet(const netadr_t& from, const game::native::msg_t* msg)
	{
		if (from.type != 4 || !msg->data) return false;

		if (msg->cursize == query_size && *reinterpret_cast<const unsigned int*>(msg->data) == query_magic)
		{
			answer_query(from);
			return true;
		}

		static const std::string hello = std::string("\xFF\xFF\xFF\xFF", 4) + hello_command + " ";
		if (msg->cursize > static_cast<int>(hello.size()) && msg->cursize < 0x200 &&
			std::memcmp(msg->data, hello.data(), hello.size()) == 0)
		{
			const std::string text(msg->data + hello.size(), msg->cursize - hello.size());
			take_hello(from, text.data());
			return true;
		}

		return false;
	}

	int __cdecl net_get_packet_stub(netadr_t* from, game::native::msg_t* msg)
	{
		while (reinterpret_cast<int(*)(netadr_t*, game::native::msg_t*)>(sys_get_packet)(from, msg))
		{
			if (!raw_join_packet(*from, msg)) return 1;
		}
		return 0;
	}

	// the player: our XNADDR to the host we joined raw, while it talks to us
	void send_hello()
	{
		if (!raw_host.active) return;

		const auto now = GetTickCount();
		if (now - raw_host.last_hello < hello_interval) return;
		raw_host.last_hello = now;

		DWORD last_seen;
		{
			std::lock_guard _(raw_mutex);
			const auto index = raw_host.address.handle - raw_handle_base;
			if (index < 0 || index >= static_cast<int>(raw_peers.size())) return;
			last_seen = raw_peers[index].last_seen;
		}

		unsigned int ip;
		std::memcpy(&ip, raw_host.address.ip, sizeof(ip));
		if (now - last_seen > host_silent)
		{
			raw_host.active = false;
			net_log(address_text(ip, raw_host.address.port) + ": the host went quiet, no more hellos");
			return;
		}

		const auto state = *reinterpret_cast<int*>(online_state);
		if (state < 3 || state > 6) return;
		const auto* xnaddr = reinterpret_cast<const unsigned char*(*)()>(own_xnaddr)();
		if (!xnaddr) return;

		send_oob_raw(raw_host.address, std::string(hello_command) + " " + to_hex(xnaddr, xnaddr_size));
	}

	// the player: the host's lobby data (iw5vlobby <addr> <id> <key>, hex) -> the Steam lobby object, taken as an invite
	void join_listen_host(const netadr_t& server)
	{
		unsigned int ip;
		std::memcpy(&ip, server.ip, sizeof(ip));
		const auto host = address_text(ip, server.port);

		const auto index = *game::native::cmd_args;
		std::string addr, id, key;
		if (game::native::cmd_argc[index] < 4 || !hex_to_text(game::native::cmd_argv[index][1], addr) ||
			!hex_to_text(game::native::cmd_argv[index][2], id) || !hex_to_text(game::native::cmd_argv[index][3], key))
		{
			net_log(host + ": bad lobby data");
			return;
		}

		auto* lobby = *reinterpret_cast<unsigned char**>(steam_lobby);
		if (!lobby)
		{
			net_log(host + ": lobby data, but the game has no Steam lobby object");
			return;
		}

		auto address = server;
		address.handle = raw_handle(server, true);
		if (address.handle < 0) return;

		reinterpret_cast<void(*)(const char*, void*)>(read_session_id)(id.data(), lobby + lobby_session_id);
		reinterpret_cast<void(*)(const char*, void*)>(read_session_key)(key.data(), lobby + lobby_key);
		reinterpret_cast<void(*)(const char*, void*)>(read_common_addr)(addr.data(), lobby + lobby_common_addr);
		reinterpret_cast<void(__thiscall*)(void*, void*)>(common_addr_to_xnaddr)(lobby + lobby_common_addr,
		                                                                          lobby + lobby_xnaddr);

		remember_xnaddr(lobby + lobby_xnaddr, address);
		raw_host.active = true;
		raw_host.address = address;
		raw_host.last_hello = GetTickCount() - hello_interval;
		send_hello();

		*reinterpret_cast<char*>(invite_address) = 0;
		*reinterpret_cast<bool*>(invite_from_friend) = true;
		*reinterpret_cast<int*>(invite_state) = 2;

		net_log(utils::string::va("%s: lobby data taken, raw handle %d, joining its party: %s / %s, XNADDR %s",
		                          host.data(), address.handle, addr.data(), id.data(),
		                          to_hex(lobby + lobby_xnaddr, xnaddr_size).data()));
	}

	// the client: the session info (a dedicated server) or the lobby data (a listen host) of the server it is connecting
	// to (true: handled)
	bool __cdecl cl_connectionless_packet(const char* command, const int local_client_num, const netadr_t* from)
	{
		if (!command) return false;
		const auto lobby_data = _stricmp(command, lobby_command) == 0;
		if (!lobby_data && _stricmp(command, session_info_command) != 0) return false;

		const auto& server = *reinterpret_cast<netadr_t*>(server_address);
		if (local_client_num != 0 || *reinterpret_cast<int*>(connection_state) != ca_session_info ||
			from->type != server.type || std::memcmp(from->ip, server.ip, sizeof(server.ip)) != 0 || from->port != server.port)
		{
			return true;
		}

		if (lobby_data)
		{
			join_listen_host(server);
			return true;
		}

		const auto index = *game::native::cmd_args;
		if (game::native::cmd_argc[index] < 2) return true;

		std::vector<unsigned char> blob;
		if (!from_hex(game::native::cmd_argv[index][1], blob) || blob.size() != session_info_size ||
			*reinterpret_cast<unsigned short*>(&blob[0]) != xnet_magic ||
			*reinterpret_cast<unsigned short*>(&blob[2]) != session_info_type ||
			*reinterpret_cast<unsigned int*>(&blob[4]) != session_info_size)
		{
			return true;
		}

		// 0x494290: the server's address gets a raw handle instead of a DemonWare connection (cl_connect_xnaddr)
		raw_server entry{};
		std::memcpy(entry.security_id, &blob[blob_xnaddr], security_id_size);
		entry.address = server;
		entry.address.handle = raw_handle(server, true);
		if (entry.address.handle < 0) return true;

		{
			std::lock_guard _(raw_mutex);
			auto known = false;
			for (auto& other : raw_servers)
			{
				if (std::memcmp(other.security_id, entry.security_id, security_id_size) == 0)
				{
					other = entry;
					known = true;
				}
			}
			if (!known) raw_servers.push_back(entry);
		}

		const auto result = reinterpret_cast<bool(*)(int, unsigned int, unsigned int, const char*, const unsigned char*)>(
			instant_message_addr)(0, 0, 0, "", blob.data());
		unsigned int ip;
		std::memcpy(&ip, server.ip, sizeof(ip));
		net_log(utils::string::va("%s: session info taken (%d), raw handle %d, state %d", address_text(ip, server.port).data(),
		                          result, entry.address.handle, *reinterpret_cast<int*>(connection_state)));
		return true;
	}

	__declspec(naked) void cl_connectionless_packet_stub()
	{
		__asm
		{
			pushad
			lea eax, [esp + 0C7Ch] // from: [esp + 0C5Ch] at the hook, + pushad
			push eax
			push esi // localClientNum
			push edi // the command
			call cl_connectionless_packet
			add esp, 0Ch
			test al, al
			popad
			jnz handled

			push 7F5260h // the replaced push ("openport")
			push 491300h
			retn

		handled:
			push 4912EEh // al = 1, the epilogue
			retn
		}
	}
}

class dedicated final : public module
{
public:
	void post_load() override
	{
		char exe[MAX_PATH]{};
		GetModuleFileNameA(nullptr, exe, sizeof(exe));
		log_path = std::filesystem::path(exe).parent_path().string() +
			(game::is_dedi() ? "\\open-iw5\\dedicated.log" : "\\open-iw5\\connect.log");

		if (game::is_dedi())
		{
			utils::hook::set<bool>(server_time_synced_dedi, true);
			utils::hook::set<int>(server_time_offset_dedi, 0);
			utils::hook::set<const char*>(net_ip_default_dedi, "0.0.0.0");
			utils::hook::nop(check_own_address_dedi, 6);
			utils::hook::nop(check_port_dedi, 6);
			utils::hook::nop(check_address_dedi, 6);
			utils::hook(reply_call_dedi, send_session_info, HOOK_CALL).install()->quick();

			utils::hook(open_port_packet_dedi, sv_open_port_packet_stub, HOOK_JUMP).install()->quick();
			utils::hook(dw_send_call_dedi, sv_send_packet, HOOK_CALL).install()->quick();
			for (const auto call : remote_xnaddr_calls_dedi)
			{
				utils::hook(call, sv_remote_xnaddr, HOOK_CALL).install()->quick();
			}
		}
		else if (game::is_mp())
		{
			utils::hook(cl_packet_hook, cl_connectionless_packet_stub, HOOK_JUMP).install()->quick();

			utils::hook(dw_send_call, cl_send_packet, HOOK_CALL).install()->quick();
			utils::hook(sockadr_to_netadr_call, cl_sockadr_to_netadr, HOOK_CALL).install()->quick();
			utils::hook(connection_status_call, cl_connection_status, HOOK_CALL).install()->quick();
			for (const auto call : connect_xnaddr_calls)
			{
				utils::hook(call, cl_connect_xnaddr, HOOK_CALL).install()->quick();
			}
			for (const auto call : open_connection_calls)
			{
				utils::hook(call, cl_open_connection, HOOK_CALL).install()->quick();
			}

			utils::hook(net_get_packet, net_get_packet_stub, HOOK_JUMP).install()->quick();
			utils::hook(connection_status_netadr, cl_connection_status_netadr, HOOK_JUMP).install()->quick();
			utils::hook(reliable_connection_call, cl_reliable_connection, HOOK_CALL).install()->quick();
			for (const auto call : remote_xnaddr_calls)
			{
				utils::hook(call, cl_remote_xnaddr, HOOK_CALL).install()->quick();
			}
			utils::hook(session_xnaddr_call, sv_session_xnaddr, HOOK_CALL).install()->quick();
			scheduler::on_frame(send_hello);
		}
	}
};

REGISTER_MODULE(dedicated)
