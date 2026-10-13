#include <std_include.hpp>
#include "steam/steam.hpp"
#include "module/dw.hpp"
#include "game/game.hpp"
#include "utils/io.hpp"
#include "utils/string.hpp"

#include <random>

namespace steam
{
	std::string auth_ticket;

	int user::GetHSteamUser()
	{
		return NULL;
	}

	bool user::LoggedOn()
	{
		return true;
	}

	namespace
	{
		// universe public (1), account type individual (1), desktop instance (1); the low 32 bits are the account id
		constexpr unsigned long long individual_id = 0x0110000100000000;
		// the account every install had before ids were made per install (and the dedicated server's without a file)
		constexpr unsigned int shared_account = 0x1377;

		// steam_id.txt: an account id (decimal or 0x hex) or a whole individual Steam id (76561198... / 0x011000010...),
		// 0 when it's neither
		unsigned int parse_account(const std::string& text)
		{
			const auto* start = text.data();
			if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) start += 3; // saved by Notepad

			char* end = nullptr;
			const auto value = std::strtoull(start, &end, 0);
			if (end == start) return 0;
			for (; *end; ++end)
			{
				if (!std::isspace(static_cast<unsigned char>(*end))) return 0;
			}

			if (value > 0 && value <= 0xFFFFFFFF) return static_cast<unsigned int>(value);
			if ((value >> 32) == (individual_id >> 32)) return static_cast<unsigned int>(value);
			return 0;
		}

		// a random account id, never one of the small ones (0x1377, ids typed in by hand for tests)
		unsigned int random_account()
		{
			std::random_device random;
			while (true)
			{
				const auto account = random() & 0x7FFFFFFF;
				if (account >= 0x100000) return account;
			}
		}

		// written through .tmp, so a crash never leaves a cut-off file
		bool write_account(const std::string& path, const unsigned int account)
		{
			const auto tmp = path + ".tmp";
			if (!::utils::io::write_file(tmp, ::utils::string::va("0x%08X\r\n", account))) return false;
			return MoveFileExA(tmp.data(), path.data(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		}

		// <game>\open-iw5\steam_id.txt; the game (not the dedicated server) makes it on its first start with a random id
		unsigned int load_account()
		{
			char exe[MAX_PATH]{};
			GetModuleFileNameA(nullptr, exe, sizeof(exe));
			const auto path = std::filesystem::path(exe).parent_path().string() + "\\open-iw5\\steam_id.txt";

			std::string text;
			const auto found = ::utils::io::read_file(path, &text);
			if (const auto account = found ? parse_account(text) : 0) return account;
			if (::game::is_dedi()) return shared_account;

			const auto account = random_account();
			if (found) printf("Steam: %s doesn't hold a Steam id, it gets a new one\n", path.data());
			if (write_account(path, account)) printf("Steam: new Steam id %016llX in %s\n", individual_id | account, path.data());
			else printf("Steam: can't write %s, the Steam id %016llX is for this start only\n", path.data(), individual_id | account);
			return account;
		}
	}

	// Every install has its own Steam id (load_account): a server tells players apart by it (the dedicated server's
	// SV_DirectConnect gives a joining player the slot of the party member with its xuid, dropping that one, and the
	// listen host's party is keyed by it), so with one id for everyone the second player on a server kicked the first.
	// The stats (players2\user\mpdata) aren't tied to it: the game only checks their CRC and version (0x550720), and
	// bdStorage keeps user files by name.
	steam_id user::GetSteamID()
	{
		static const auto account = load_account();

		steam_id id;
		id.bits = individual_id | account;
		return id;
	}

	int user::InitiateGameConnection(void* pAuthBlob, int cbMaxAuthBlob, steam_id steamIDGameServer,
	                                 unsigned int unIPServer, unsigned short usPortServer, bool bSecure)
	{
		return 0;
	}

	void user::TerminateGameConnection(unsigned int unIPServer, unsigned short usPortServer)
	{
	}

	void user::TrackAppUsageEvent(steam_id gameID, int eAppUsageEvent, const char* pchExtraInfo)
	{
	}

	bool user::GetUserDataFolder(char* pchBuffer, int cubBuffer)
	{
		return false;
	}

	void user::StartVoiceRecording()
	{
	}

	void user::StopVoiceRecording()
	{
	}

	int user::GetAvailableVoice(unsigned int* pcbCompressed, unsigned int* pcbUncompressed,
	                            unsigned int nUncompressedVoiceDesiredSampleRate)
	{
		return 0;
	}

	int user::GetVoice(bool bWantCompressed, void* pDestBuffer, unsigned int cbDestBufferSize,
	                   unsigned int* nBytesWritten, bool bWantUncompressed, void* pUncompressedDestBuffer,
	                   unsigned int cbUncompressedDestBufferSize, unsigned int* nUncompressBytesWritten,
	                   unsigned int nUncompressedVoiceDesiredSampleRate)
	{
		return 0;
	}

	int user::DecompressVoice(void* pCompressed, unsigned int cbCompressed, void* pDestBuffer,
	                          unsigned int cbDestBufferSize, unsigned int* nBytesWritten)
	{
		return 0;
	}

	unsigned int user::GetVoiceOptimalSampleRate()
	{
		return 0;
	}

	// Steam's peer authentication (MW3's p2pAuth, 0x6C0CB0: party members check each other's tickets and drop or kick the
	// ones that fail; "Disconnected due to VAC." for a player joining a party): there is no Steam to ask, so every ticket
	// is good, as Steam would answer for a real one
	unsigned int user::GetAuthSessionTicket(void* pTicket, int cbMaxTicket, unsigned int* pcbTicket)
	{
		static unsigned int handle = 0;
		const auto id = this->GetSteamID();
		const std::string ticket = std::string("IW5V", 4) + std::string(reinterpret_cast<const char*>(&id.bits), sizeof(id.bits));
		if (!pTicket || cbMaxTicket < static_cast<int>(ticket.size())) return 0;

		std::memcpy(pTicket, ticket.data(), ticket.size());
		if (pcbTicket) *pcbTicket = static_cast<unsigned int>(ticket.size());
		return ++handle;
	}

	int user::BeginAuthSession(const void* pAuthTicket, int cbAuthTicket, steam_id steamID)
	{
		if (!pAuthTicket || cbAuthTicket <= 0) return 1; // k_EBeginAuthSessionResultInvalidTicket

		// k_EAuthSessionResponseOK, as a ValidateAuthTicketResponse_t callback on the next SteamAPI_RunCallbacks
		auto* response = static_cast<validate_auth_ticket_response*>(calloc(1, sizeof(validate_auth_ticket_response)));
		response->m_steam_id = steamID;
		response->m_e_auth_session_response = 0;
		callbacks::return_call(response, sizeof(validate_auth_ticket_response), validate_auth_ticket_response::callback_id,
		                       0);
		return 0; // k_EBeginAuthSessionResultOK
	}

	void user::EndAuthSession(steam_id steamID)
	{
	}

	void user::CancelAuthTicket(unsigned int hAuthTicket)
	{
	}

	unsigned int user::UserHasLicenseForApp(steam_id steamID, unsigned int appID)
	{
		return 0;
	}

	bool user::BIsBehindNAT()
	{
		return false;
	}

	void user::AdvertiseGame(steam_id steamIDGameServer, unsigned int unIPServer, unsigned short usPortServer)
	{
	}

	unsigned __int64 user::RequestEncryptedAppTicket(void* pUserData, int cbUserData)
	{
		// Generate the authentication ticket
		const auto id = this->GetSteamID();

		auth_ticket = "Open-IW5";
		auth_ticket.resize(32);
		auth_ticket.append(reinterpret_cast<char*>(pUserData), cbUserData);
		auth_ticket.append(reinterpret_cast<const char*>(&id.bits), sizeof(id.bits));

		// Create the call response
		const auto result = callbacks::register_call();
		auto retvals = static_cast<encrypted_app_ticket_response*>(calloc(1, sizeof(encrypted_app_ticket_response)));
		//::Utils::Memory::AllocateArray<EncryptedAppTicketResponse>();
		retvals->m_e_result = 1;

		// Return the call response
		callbacks::return_call(retvals, sizeof(encrypted_app_ticket_response),
		                       encrypted_app_ticket_response::callback_id, result);

		return result;
	}

	bool user::GetEncryptedAppTicket(void* pTicket, int cbMaxTicket, unsigned int* pcbTicket)
	{
		if (cbMaxTicket < 0 || auth_ticket.empty()) return false;

		const auto size = std::min(size_t(cbMaxTicket), auth_ticket.size());
		std::memcpy(pTicket, auth_ticket.data(), size);
		*pcbTicket = size;

		return true;
	}
}
