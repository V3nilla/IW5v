#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "scheduler.hpp"
#include "dedi_native.hpp"
#include "server_addresses.hpp"
#include "game/game.hpp"
#include "utils/io.hpp"
#include "utils/string.hpp"

#include <ctime>

// Match history for the dedicated server only (the admin website reads it; MP is not touched). Once a second, on the
// server's own loop, it reads every client slot and the match:
// - <game>\open-iw5\matches.jsonl, one JSON object a line:
//   {"type":"join"|"leave", "time", "slot", "name", "xuid", "ip", "bot"}            a player (or bot) came or went
//   {"type":"match", "start", "end", "duration", "map", "gametype", "ended", "end_reason", "rounds",
//    "team_scores": {"axis", "allies"}, "winner", "players": [{"name", "xuid", "ip", "team", "score", "kills",
//    "deaths", "assists", "bot", "ping_avg", "seconds"}]}
//   written when the next game starts (or a minute into the intermission): "ended" true and "end_reason" "match end"
//   when the players were seen in intermission (the end of the match), else "ended" false ("map change", "restart").
//   Round-based modes (S&D ...) restart the map between rounds with the players' scores kept (and may show the
//   intermission state at a round's end): a restart on the same map and mode whose players still have points is the
//   same match ("rounds" + 1). A match nobody played in
//   isn't written. A server stopped during a match loses that match.
// - <game>\open-iw5\live.json, every 2 s: the map, mode, team scores, hostname, sv_bots_fill / bots / bots_skill
//   (-1 when a dvar doesn't exist) and the players now (slot, name, xuid, ip, ping,
//   score, kills, deaths, assists, team, bot), written to a temporary file and renamed over the old one.
// Times are UTC (ISO 8601).
namespace
{
	// client_t (as MP's up to +0x41C9C)
	constexpr DWORD cl_state = 0x0; // 0 free, 1 dropped, 2+ connected
	constexpr DWORD cl_address_type = 0x28; // 0 bot, 2 loopback, 4 ip
	constexpr DWORD cl_address_ip = 0x2C;
	constexpr DWORD cl_name = 0x2142C; // 0x12 bytes
	constexpr DWORD cl_ping = 0x21480;
	constexpr DWORD cl_xuid_dedi = 0x452C0;
	constexpr DWORD sv_running_dedi = 0x1B9CEEC; // dvar_t*, enabled at +0xC

	// gentity_s -> gclient_s (the same fields as MP's up to +0x3624)
	constexpr DWORD gentity_size = 0x274;
	constexpr DWORD ent_client = 0x158;
	constexpr DWORD gclient_session_state = 0x330C; // 3 intermission
	constexpr DWORD gclient_score = 0x3324;
	constexpr DWORD gclient_deaths = 0x3328;
	constexpr DWORD gclient_kills = 0x332C;
	constexpr DWORD gclient_assists = 0x3330;
	constexpr DWORD gclient_team = 0x33D0; // 0 none (free for all), 1 axis, 2 allies, 3 spectator
	constexpr int session_intermission = 3;

	// level_locals_t (dedi_native::level): startTime @+0x4B4, teamScores[4] @+0x4B8
	constexpr DWORD level_team_scores = dedi_native::level + 0x4B8;

	constexpr DWORD dvar_current = 0xC;
	constexpr int max_slots = 18;

	std::string matches_path, live_path;

	struct player_stats
	{
		std::string name, xuid, ip;
		bool bot = false;
		int team = 0, score = 0, kills = 0, deaths = 0, assists = 0;
		long long ping_sum = 0;
		int ping_count = 0, seconds = 0;
	};

	struct match_state
	{
		bool active = false;
		int start_time = 0; // level.startTime of its (last) game
		std::time_t start = 0, last_seen = 0;
		std::string map, gametype;
		int rounds = 1;
		int axis = 0, allies = 0;
		std::vector<std::string> order; // the players' keys, as they joined
		std::unordered_map<std::string, player_stats> players;
		bool ended = false; // written at its end
		std::time_t intermission_since = 0; // the players were seen in intermission (the end of the match, or of a round)
	};

	match_state match;
	match_state pending; // a match whose map restarted on the same map and mode (a new round or a new match)
	std::time_t pending_since = 0;

	struct slot_info
	{
		bool used = false;
		std::string name, xuid, ip;
		bool bot = false;
	};

	slot_info slots[max_slots];

	template <typename T>
	T& field(char* base, const DWORD offset)
	{
		return *reinterpret_cast<T*>(base + offset);
	}

	std::string iso_time(const std::time_t t)
	{
		tm utc{};
		gmtime_s(&utc, &t);
		char text[32]{};
		std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
		return text;
	}

	std::string json_string(const std::string& text)
	{
		std::string out = "\"";
		for (const auto ch : text)
		{
			const auto c = static_cast<unsigned char>(ch);
			if (c == '"' || c == '\\') out += '\\', out += ch;
			else if (c < 0x20 || c >= 0x7F) out += utils::string::va("\\u%04x", c);
			else out += ch;
		}
		return out + "\"";
	}

	const char* team_name(const int team)
	{
		switch (team)
		{
		case 1: return "axis";
		case 2: return "allies";
		case 3: return "spectator";
		default: return "none";
		}
	}

	bool server_running()
	{
		const auto* running = *reinterpret_cast<const char* const*>(sv_running_dedi);
		return running && running[0xC];
	}

	std::string dvar_text(const char* name)
	{
		auto* dvar = static_cast<char*>(dedi_native::dvar_find_var(name));
		const auto* value = dvar ? *reinterpret_cast<const char**>(dvar + dvar_current) : nullptr;
		return value ? std::string(value) : std::string();
	}

	int dvar_int(const char* name)
	{
		auto* dvar = static_cast<char*>(dedi_native::dvar_find_var(name));
		return dvar ? *reinterpret_cast<int*>(dvar + dvar_current) : -1;
	}

	char* client(const int num)
	{
		const auto& sv = server_addresses::dedi;
		return reinterpret_cast<char*>(sv.svs_clients) + num * sv.client_size;
	}

	char* gclient(const int num)
	{
		auto* ent = reinterpret_cast<char*>(server_addresses::dedi.g_entities) + num * gentity_size;
		return *reinterpret_cast<char**>(ent + ent_client);
	}

	int num_clients()
	{
		return std::clamp(*reinterpret_cast<int*>(server_addresses::dedi.svs_num_clients), 0, max_slots);
	}

	std::string client_name(char* cl)
	{
		std::string name;
		const auto* text = cl + cl_name;
		for (auto i = 0; i < 0x12 && text[i]; ++i)
		{
			const auto c = static_cast<unsigned char>(text[i]);
			name += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
		}
		return name;
	}

	std::string client_ip(char* cl)
	{
		const auto* ip = &field<unsigned char>(cl, cl_address_ip);
		return utils::string::va("%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
	}

	void append(const std::string& line)
	{
		utils::io::write_file(matches_path, line + "\n", true);
	}

	// ---- the match record ----

	void write_match(match_state& m, const std::string& reason)
	{
		if (!m.active || m.ended) return;
		m.ended = true;

		auto played = false;
		for (const auto& [key, p] : m.players) played |= p.seconds > 0;
		if (!played) return;

		std::string winner;
		const auto team_based = std::any_of(m.players.begin(), m.players.end(), [](const auto& entry)
		{
			return entry.second.team == 1 || entry.second.team == 2;
		});
		if (team_based) winner = m.axis > m.allies ? "axis" : m.allies > m.axis ? "allies" : "tie";
		else
		{
			const player_stats* best = nullptr;
			for (const auto& [key, p] : m.players)
			{
				if (!best || p.score > best->score) best = &p;
			}
			if (best) winner = best->name;
		}

		std::string players;
		for (const auto& key : m.order)
		{
			const auto& p = m.players[key];
			if (!players.empty()) players += ",";
			players += utils::string::va("{\"name\":%s,\"xuid\":%s,\"ip\":%s,\"team\":\"%s\",\"score\":%d,\"kills\":%d,\"deaths\":%d,"
			                             "\"assists\":%d,\"bot\":%s,\"ping_avg\":%d,\"seconds\":%d}",
			                             json_string(p.name).data(), json_string(p.xuid).data(), json_string(p.ip).data(), team_name(p.team),
			                             p.score, p.kills, p.deaths, p.assists, p.bot ? "true" : "false",
			                             p.ping_count ? static_cast<int>(p.ping_sum / p.ping_count) : 0, p.seconds);
		}

		append(utils::string::va("{\"type\":\"match\",\"start\":\"%s\",\"end\":\"%s\",\"duration\":%d,\"map\":%s,\"gametype\":%s,"
		                         "\"ended\":%s,\"end_reason\":%s,\"rounds\":%d,\"team_scores\":{\"axis\":%d,\"allies\":%d},"
		                         "\"winner\":%s,\"players\":[%s]}",
		                         iso_time(m.start).data(), iso_time(m.last_seen).data(), static_cast<int>(m.last_seen - m.start),
		                         json_string(m.map).data(), json_string(m.gametype).data(), reason == "match end" ? "true" : "false",
		                         json_string(reason).data(), m.rounds, m.axis, m.allies, json_string(winner).data(), players.data()));
	}

	// a match that saw its intermission ended normally
	std::string end_reason(const match_state& m, const char* otherwise)
	{
		return m.intermission_since ? "match end" : otherwise;
	}

	void start_match(const int start_time, const std::time_t now, const std::string& map, const std::string& gametype)
	{
		match = {};
		match.active = true;
		match.start_time = start_time;
		match.start = match.last_seen = now;
		match.map = map;
		match.gametype = gametype;
	}

	bool has_points(const match_state& m)
	{
		return std::any_of(m.players.begin(), m.players.end(), [](const auto& entry)
		{
			const auto& p = entry.second;
			return p.score || p.kills || p.deaths || p.assists;
		});
	}

	// a restart on the same map and mode (pending): when the players kept their points it was the next round of that
	// match, which goes on in match; else pending was a match of its own
	void resolve_pending()
	{
		if (has_points(match) && !match.players.empty())
		{
			auto merged = std::move(pending);
			merged.start_time = match.start_time;
			merged.last_seen = match.last_seen;
			merged.axis = match.axis;
			merged.allies = match.allies;
			merged.rounds += 1;
			for (const auto& key : match.order)
			{
				const auto& p = match.players[key];
				auto [itr, added] = merged.players.try_emplace(key);
				if (added) merged.order.push_back(key);
				auto& old = itr->second;
				const auto seconds = old.seconds + p.seconds;
				const auto ping_sum = old.ping_sum + p.ping_sum;
				const auto ping_count = old.ping_count + p.ping_count;
				old = p;
				old.seconds = seconds;
				old.ping_sum = ping_sum;
				old.ping_count = ping_count;
			}
			merged.ended = match.ended;
			merged.intermission_since = match.intermission_since;
			match = std::move(merged);
		}
		else
		{
			write_match(pending, end_reason(pending, "restart"));
		}
		pending = {};
	}

	// ---- once a second ----

	struct live_player
	{
		int slot;
		std::string key;
		player_stats stats;
		int ping;
	};

	std::string player_key(const player_stats& p)
	{
		if (p.bot) return "bot:" + p.name;
		if (!p.xuid.empty()) return "xuid:" + p.xuid;
		return "ip:" + p.ip + ":" + p.name;
	}

	void track_slots(const std::vector<live_player>& now_players, const std::time_t now)
	{
		slot_info current[max_slots];
		for (const auto& lp : now_players) current[lp.slot] = {true, lp.stats.name, lp.stats.xuid, lp.stats.ip, lp.stats.bot};

		for (auto num = 0; num < max_slots; ++num)
		{
			const auto& before = slots[num];
			const auto& after = current[num];
			const auto same = before.used == after.used && before.name == after.name && before.xuid == after.xuid;
			if (same) continue;

			const auto event = [&](const char* type, const slot_info& s)
			{
				append(utils::string::va("{\"type\":\"%s\",\"time\":\"%s\",\"slot\":%d,\"name\":%s,\"xuid\":%s,\"ip\":%s,\"bot\":%s}", type,
				                         iso_time(now).data(), num, json_string(s.name).data(), json_string(s.xuid).data(),
				                         json_string(s.ip).data(), s.bot ? "true" : "false"));
			};
			if (before.used) event("leave", before);
			if (after.used) event("join", after);
			slots[num] = after;
		}
	}

	void write_live(const std::vector<live_player>& now_players, const std::time_t now, const std::string& map,
	                const std::string& gametype)
	{
		std::string players;
		for (const auto& lp : now_players)
		{
			const auto& p = lp.stats;
			if (!players.empty()) players += ",";
			players += utils::string::va("{\"slot\":%d,\"name\":%s,\"xuid\":%s,\"ip\":%s,\"ping\":%d,\"score\":%d,\"kills\":%d,\"deaths\":%d,"
			                             "\"assists\":%d,\"team\":\"%s\",\"bot\":%s}",
			                             lp.slot, json_string(p.name).data(), json_string(p.xuid).data(), json_string(p.ip).data(), lp.ping,
			                             p.score, p.kills, p.deaths, p.assists, team_name(p.team), p.bot ? "true" : "false");
		}

		const auto text = utils::string::va("{\"time\":\"%s\",\"map\":%s,\"gametype\":%s,\"match_start\":\"%s\",\"rounds\":%d,"
		                                    "\"team_scores\":{\"axis\":%d,\"allies\":%d},\"max_clients\":%d,\"hostname\":%s,\"sv_bots_fill\":%d,"
		                                    "\"bots\":%d,\"bots_skill\":%d,\"players\":[%s]}\n",
		                                    iso_time(now).data(), json_string(map).data(), json_string(gametype).data(),
		                                    iso_time(match.start).data(), match.rounds, match.axis, match.allies, num_clients(),
		                                    json_string(dvar_text("sv_hostname")).data(), dvar_int("sv_bots_fill"), dvar_int("bots"),
		                                    dvar_int("bots_skill"), players.data());
		const auto temp = live_path + ".tmp";
		if (utils::io::write_file(temp, text)) MoveFileExA(temp.data(), live_path.data(), MOVEFILE_REPLACE_EXISTING);
	}

	void frame()
	{
		static DWORD last_tick = 0;
		const auto tick = GetTickCount();
		if (tick - last_tick < 1000) return;
		last_tick = tick;

		if (!server_running()) return;
		const auto start_time = *reinterpret_cast<int*>(dedi_native::level_start_time);
		// (0 on the first map after the start: the server time starts at 0 there)

		const auto now = std::time(nullptr);
		const auto map = dvar_text("mapname");
		const auto gametype = dvar_text("g_gametype");

		// a new game: a map change, a map_restart or a fast_restart
		if (!match.active || match.start_time != start_time)
		{
			if (pending.active) resolve_pending();
			if (match.active && !match.ended && match.map == map && match.gametype == gametype)
			{
				// a new round of the same match, or a restart: decided below once the players are back
				pending = match;
				pending_since = now;
			}
			else
			{
				write_match(match, end_reason(match, "map change"));
			}
			start_match(start_time, now, map, gametype);
		}

		std::vector<live_player> players;
		auto intermission = false;
		for (auto num = 0; num < num_clients(); ++num)
		{
			auto* cl = client(num);
			if (field<int>(cl, cl_state) < 2) continue;

			live_player lp{num, {}, {}, field<int>(cl, cl_ping)};
			auto& p = lp.stats;
			p.name = client_name(cl);
			p.bot = field<int>(cl, cl_address_type) == 0;
			if (!p.bot)
			{
				p.ip = client_ip(cl);
				const auto xuid = field<unsigned long long>(cl, cl_xuid_dedi);
				if (xuid) p.xuid = utils::string::va("%016llX", xuid);
			}

			if (auto* gc = gclient(num))
			{
				p.team = field<int>(gc, gclient_team);
				p.score = field<int>(gc, gclient_score);
				p.kills = field<int>(gc, gclient_kills);
				p.deaths = field<int>(gc, gclient_deaths);
				p.assists = field<int>(gc, gclient_assists);
				intermission |= field<int>(gc, gclient_session_state) == session_intermission;
			}
			lp.key = player_key(p);
			players.push_back(std::move(lp));
		}

		track_slots(players, now);

		if (!match.ended)
		{
			match.last_seen = now;
			const auto* scores = reinterpret_cast<const int*>(level_team_scores);
			match.axis = scores[1];
			match.allies = scores[2];
			for (const auto& lp : players)
			{
				auto [itr, added] = match.players.try_emplace(lp.key);
				if (added) match.order.push_back(lp.key);
				auto& p = itr->second;
				const auto seconds = p.seconds;
				const auto ping_sum = p.ping_sum;
				const auto ping_count = p.ping_count;
				p = lp.stats;
				p.seconds = seconds + 1;
				if (!lp.stats.bot && lp.ping > 0 && lp.ping < 999)
				{
					p.ping_sum = ping_sum + lp.ping;
					p.ping_count = ping_count + 1;
				}
				else
				{
					p.ping_sum = ping_sum;
					p.ping_count = ping_count;
				}
			}
		}

		if (pending.active && now - pending_since >= 5) resolve_pending();

		// intermission: the end of the match, or (round-based modes) of a round, which a restart with the points kept
		// follows within seconds; still there after a minute: the end of the match (also written on the next map)
		if (intermission && !match.ended && !match.intermission_since) match.intermission_since = now;
		if (match.intermission_since && !match.ended && !pending.active && now - match.intermission_since >= 60)
		{
			write_match(match, "match end");
		}

		static DWORD last_live = 0;
		if (tick - last_live >= 2000)
		{
			last_live = tick;
			write_live(players, now, map, gametype);
		}
	}
}

class match_log final : public module
{
public:
	void post_load() override
	{
		if (!game::is_dedi()) return;

		char exe[MAX_PATH]{};
		GetModuleFileNameA(nullptr, exe, sizeof(exe));
		const auto dir = std::filesystem::path(exe).parent_path().string() + "\\open-iw5";
		matches_path = dir + "\\matches.jsonl";
		live_path = dir + "\\live.json";

		scheduler::on_frame(frame);
	}
};

REGISTER_MODULE(match_log)
