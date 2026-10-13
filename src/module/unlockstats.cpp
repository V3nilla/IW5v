#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "command.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"
#include "utils/io.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <ctime>
#include <set>

// unlockstats: unlocks everything in MP for the local player (the stats are players2\user\mpdata, the DemonWare "mpdata"
// file): max level (the prestige is kept), every weapon at its class's max weapon level (attachments, camos with gold,
// proficiencies, reticles), every challenge of mp/allChallengesTable.csv complete (pro perks, specialist pro streaks,
// challenge titles and emblems; not ch_prestige / ch_prestige_10, which follow the real prestige), every prestige shop
// item bought (no tokens spent), all 15 custom classes (the 5 prestige classes and the 5 entitlement ones, which in the
// real game only an entitlements file sets: 0x6443A0, and open-iw5 serves none; Find Game / PLAY only: Private Match uses
// its own privateMatchCustomClasses[5], which nothing unlocks, as on the console), the DLC camo / reticle bits
// and every title and emblem ("unlockstats elite", which the entitlement classes once needed, does the same). token_22 /
// token_23 (rank 9999) stay locked, as in the game.
// The stats are only changed in the menus: in a match the server has its own copy (written with modified flags it drops
// the host, "Too many stats changed at once") and the match's GSC its own XP, so there it is queued until the client is
// back in the menu (after the game's own end-of-match upload). Before any change the file and the stats in memory are
// backed up to players2\backup (not players2\user: bdStorage lists that folder); the upload (LiveStorage_UploadStats) is
// retried until it went through, as the end-of-match upload can still be running.
// Only the raw StructuredData calls are used (the game's playerdata wrappers Com_Error on a bad path), and the type of
// every field is checked before it is written.
namespace
{
	// ---- the game ----

	// StructuredData (mp/playerdata.def, the IW5 layout)
	enum sd_category
	{
		category_int = 0,
		category_byte = 1,
		category_bool = 2,
		category_struct = 5,
		category_indexed_array = 6,
		category_enum_array = 7,
	};

	struct sd_type
	{
		int category;
		int index; // the struct / indexed array / enum array
	};

	struct sd_enum_entry
	{
		const char* name;
		unsigned short index;
	};

	struct sd_enum
	{
		int entry_count;
		int reserved;
		const sd_enum_entry* entries; // sorted by name
	};

	struct sd_indexed_array
	{
		int size;
		sd_type element;
		int element_size; // bits for a bool
	};

	struct sd_enum_array
	{
		int enum_index;
		sd_type element;
		int element_size;
	};

	struct sd_def
	{
		int version;
		unsigned int checksum;
		int enum_count;
		const sd_enum* enums;
		int struct_count;
		const void* structs;
		int indexed_array_count;
		const sd_indexed_array* indexed_arrays;
		int enum_array_count;
		const sd_enum_array* enum_arrays;
		sd_type root;
		unsigned int size;
	};

	static_assert(sizeof(sd_def) == 0x34);

	struct sd_lookup
	{
		const sd_def* def;
		const sd_type* type;
		unsigned int offset; // bits for a bool
		int error; // 1 wrong operation for the type, 2 index out of range, 3 no such member, 4 no such enum key
	};

	struct sd_buffer
	{
		unsigned char* data;
		unsigned int size;
	};

	struct string_table
	{
		const char* name;
		int columns;
		int rows;
		const void* cells;
	};

	constexpr DWORD sd_def_get_addr = 0x5B9D10; // (name, buffer size): defs[0] of the set
	constexpr DWORD sd_init_lookup_addr = 0x5B88A0; // (def, lookup): the root struct
	constexpr DWORD sd_lookup_int_addr = 0x5B88C0; // (lookup, index): indexed arrays
	constexpr DWORD sd_lookup_string_addr = 0x5B8BD0; // (lookup, name): struct members and enum array keys
	constexpr DWORD sd_get_int_addr = 0x5B8FA0; // (lookup, buffer): an int, anything else as a byte
	constexpr DWORD sd_get_bool_addr = 0x5B8FF0; // (lookup, buffer)
	constexpr DWORD sd_set_int_addr = 0x5B9140; // (lookup, buffer, modified flags, value): 0 changed, 1 the same, 2+ failed
	constexpr DWORD sd_set_bool_addr = 0x5B9210; // the same
	constexpr DWORD string_table_find_addr = 0x5B87D0; // (name, StringTable** out)
	constexpr DWORD string_table_cell_addr = 0x5B86A0; // (table, row, column): bounds-checked, "" when empty
	constexpr DWORD rank_for_xp_addr = 0x497350; // (xp): mp/rankTable.csv
	constexpr DWORD crc32_addr = 0x610180; // (data, size, 0): the mpdata header (LiveStorage_UploadStats)
	constexpr DWORD console_print_addr = 0x487B00;

	// the stats of controller 0 (0x5C4A70, the PC's controller, is always 0)
	constexpr DWORD get_stats_addr = 0x54BE60; // (StructuredDataBuffer*, controller): false until fetched
	constexpr DWORD stats_source_addr = 0x548310; // (controller): 0 online (mpdata), 1 systemlink (a profile file)
	constexpr DWORD stats_changed_addr = 0x549060; // (controller): writeNeeded and the UI refresh
	constexpr DWORD stats_upload_addr = 0x548430; // (controller): LiveStorage_UploadStats 0x553150 for source 0
	constexpr DWORD playerdata_read_addr = 0x54BFA0; // (controller, source): the game's "stats changed" (rank / presence
	                                                 // 0x49C710, "playerdataread", the card checks, party member data)
	constexpr DWORD challenge_cache_dirty_addr = 0x548E00; // challengeFilterCacheDirty
	constexpr DWORD sorted_challenge_filter = 0x1CE4368; // char[0x20]: the filter the challenge tracker's sorted list
	                                                     // (getsortedchallengecount / name, 0x54D420) was built for
	constexpr DWORD sorted_challenge_count = 0x1CBAF2C; // that list's length

	constexpr DWORD stats_block = 0x1CE439C; // controller 0: 2 sources x 0x3005 {crc, playerdata, fetched, retry, writeNeeded}
	constexpr DWORD stats_fetched = stats_block + 0x3000; // source 0
	constexpr DWORD stats_write_needed = stats_block + 0x3002; // cleared when an upload starts
	constexpr DWORD stats_upload_busy = stats_block + 0x600A; // set until the upload's DemonWare task is done
	constexpr DWORD theater_playerdata = 0x66148C0; // demo playback: 0x54BE60 returns the demo's copy
	constexpr DWORD connection_state = 0x10625F4; // 0..2 not connected
	constexpr DWORD unlock_snapshot = 0x106273A; // taken at match start (0x470880), cleared by the end-of-match diff 0x470990

	constexpr unsigned int playerdata_size = 0x2FFC;
	constexpr auto playerdata_def = "mp/playerdata.def";
	constexpr auto mpdata_path = "players2/user/mpdata"; // bdStorage::get_user_file_path
	constexpr auto backup_dir = "players2/backup/";
	constexpr int prestige_classes = 5; // extraCustomClassesPrestige: the shop's "already purchased" at 5
	constexpr int entitlement_classes = 5; // extraCustomClassesEntitlement: 5 + prestige + entitlement classes, at most 15

	void print(const std::string& text)
	{
		reinterpret_cast<void(*)(int, int, const char*, int, int, int)>(console_print_addr)(0, 0, (text + "\n").data(), 0, 0, 0);
	}

	bool get_stats(sd_buffer* stats)
	{
		return reinterpret_cast<bool(*)(sd_buffer*, int)>(get_stats_addr)(stats, 0);
	}

	int stats_source()
	{
		return reinterpret_cast<int(*)(int)>(stats_source_addr)(0);
	}

	bool in_match()
	{
		return *reinterpret_cast<int*>(connection_state) > 2;
	}

	std::string display_path(std::string path)
	{
		std::replace(path.begin(), path.end(), '/', '\\');
		return path;
	}

	// ---- the string tables ----

	const string_table* find_table(const char* name)
	{
		const string_table* table = nullptr;
		reinterpret_cast<void(*)(const char*, const string_table**)>(string_table_find_addr)(name, &table);

		// a missing table comes back as the default asset
		return table && table->name && _stricmp(table->name, name) == 0 ? table : nullptr;
	}

	const char* cell(const string_table* table, const int row, const int column)
	{
		return reinterpret_cast<const char*(*)(const string_table*, int, int)>(string_table_cell_addr)(table, row, column);
	}

	// the first row whose column is value (ignoring case, as the game's 0x5B8610), -1 when none is
	int find_row(const string_table* table, const int column, const char* value)
	{
		for (auto row = 0; row < table->rows; ++row)
		{
			if (_stricmp(cell(table, row, column), value) == 0) return row;
		}
		return -1;
	}

	// ---- the playerdata ----

	enum class result
	{
		already,
		changed,
		missing, // the playerdata has no slot for it (a weapon or challenge not in its enum)
		failed,
	};

	// an item of several fields: the worst of them
	result operator|(const result a, const result b)
	{
		return std::max(a, b);
	}

	struct key
	{
		key(const char* member) : name(member)
		{
		}

		key(const std::string& member) : name(member.data())
		{
		}

		key(const int element) : index(element)
		{
		}

		const char* name = nullptr;
		int index = 0;
	};

	struct field_write
	{
		sd_lookup lookup;
		int value;
	};

	// one run: the changes are gathered first, then written after the backup
	const sd_def* def = nullptr;
	sd_buffer buffer{};
	std::vector<field_write> writes;

	bool find_field(sd_lookup& lookup, const std::initializer_list<key> path)
	{
		reinterpret_cast<void(*)(const sd_def*, sd_lookup*)>(sd_init_lookup_addr)(def, &lookup);
		for (const auto& k : path)
		{
			const auto category = lookup.type->category;
			if (k.name && (category == category_struct || category == category_enum_array))
			{
				reinterpret_cast<void(*)(sd_lookup*, const char*)>(sd_lookup_string_addr)(&lookup, k.name);
			}
			else if (!k.name && category == category_indexed_array)
			{
				reinterpret_cast<void(*)(sd_lookup*, int)>(sd_lookup_int_addr)(&lookup, k.index);
			}
			else
			{
				lookup.error = 1;
			}

			if (lookup.error) return false;
		}
		return true;
	}

	int get_int(const sd_lookup& lookup)
	{
		return reinterpret_cast<int(*)(const sd_lookup*, const sd_buffer*)>(sd_get_int_addr)(&lookup, &buffer);
	}

	bool get_bool(const sd_lookup& lookup)
	{
		return reinterpret_cast<int(*)(const sd_lookup*, const sd_buffer*)>(sd_get_bool_addr)(&lookup, &buffer) != 0;
	}

	// an int or byte field, fallback when there is none
	int read_int(const std::initializer_list<key> path, const int fallback = 0)
	{
		sd_lookup lookup{};
		if (!find_field(lookup, path)) return fallback;
		const auto category = lookup.type->category;
		return category == category_int || category == category_byte ? get_int(lookup) : fallback;
	}

	// an int or byte field raised to at least value (never lowered)
	result raise_field(const std::initializer_list<key> path, const int value)
	{
		sd_lookup lookup{};
		if (!find_field(lookup, path)) return lookup.error == 4 ? result::missing : result::failed;

		const auto category = lookup.type->category;
		if (category != category_int && category != category_byte) return result::failed;
		if (category == category_byte && (value < 0 || value > 0xFF)) return result::failed;

		if (get_int(lookup) >= value) return result::already;
		writes.push_back({lookup, value});
		return result::changed;
	}

	// a bool field set
	result unlock_field(const std::initializer_list<key> path)
	{
		sd_lookup lookup{};
		if (!find_field(lookup, path)) return lookup.error == 4 ? result::missing : result::failed;
		if (lookup.type->category != category_bool) return result::failed;

		if (get_bool(lookup)) return result::already;
		writes.push_back({lookup, 1});
		return result::changed;
	}

	// the element count of an indexed array field, 0 when it is none
	int array_size(const char* field)
	{
		sd_lookup lookup{};
		if (!find_field(lookup, {field}) || lookup.type->category != category_indexed_array) return 0;
		return def->indexed_arrays[lookup.type->index].size;
	}

	// the enum an enum array field is keyed by
	const sd_enum* array_enum(const char* field)
	{
		sd_lookup lookup{};
		if (!find_field(lookup, {field}) || lookup.type->category != category_enum_array) return nullptr;
		const auto enum_index = def->enum_arrays[lookup.type->index].enum_index;
		return enum_index >= 0 && enum_index < def->enum_count ? &def->enums[enum_index] : nullptr;
	}

	// the number of writes that failed
	int apply_writes()
	{
		auto failed = 0;
		for (auto& w : writes)
		{
			const auto status = w.lookup.type->category == category_bool
				                    ? reinterpret_cast<int(*)(sd_lookup*, sd_buffer*, unsigned char*, bool)>(sd_set_bool_addr)(&w.lookup, &buffer, nullptr, w.value != 0)
				                    : reinterpret_cast<int(*)(sd_lookup*, sd_buffer*, unsigned char*, int)>(sd_set_int_addr)(&w.lookup, &buffer, nullptr, w.value);
			if (status >= 2) ++failed;
		}
		writes.clear();
		return failed;
	}

	struct group
	{
		int done = 0; // unlocked now (changed or already)
		int changed = 0;
		int skipped = 0;
		int failed = 0;

		void add(const result r)
		{
			if (r == result::missing) ++skipped;
			else if (r == result::failed) ++failed;
			else
			{
				++done;
				if (r == result::changed) ++changed;
			}
		}

		std::string counts() const
		{
			std::string text = utils::string::va("%d changed", changed);
			if (skipped) text += utils::string::va(", %d not in playerdata", skipped);
			if (failed) text += utils::string::va(", %d FAILED", failed);
			return text;
		}
	};

	// ---- backup and save ----

	std::string timestamp()
	{
		const auto now = std::time(nullptr);
		tm local{};
		localtime_s(&local, &now);
		char text[32]{};
		std::strftime(text, sizeof(text), "%Y%m%d_%H%M%S", &local);
		return text;
	}

	// players2/backup/mpdata_<time> (the file as it is) and mpdata_<time>_live (the stats in memory, as a valid mpdata: newer
	// than the file while an upload is pending); the name, "" when the live copy couldn't be written (a fresh profile has
	// no file yet, that is fine)
	std::string backup(const sd_buffer& stats)
	{
		try
		{
			std::error_code ec;
			std::filesystem::create_directories(backup_dir, ec);

			const auto stem = std::string(backup_dir) + "mpdata_" + timestamp();
			auto base = stem;
			for (auto n = 2; utils::io::file_exists(base) || utils::io::file_exists(base + "_live"); ++n)
			{
				base = stem + utils::string::va("_%d", n);
			}

			if (utils::io::file_exists(mpdata_path) && !CopyFileA(mpdata_path, base.data(), TRUE))
			{
				print("unlockstats: couldn't copy " + display_path(mpdata_path) + " (the _live copy is the backup)");
			}

			std::string live(4 + playerdata_size, '\0');
			const auto crc = reinterpret_cast<unsigned int(*)(const void*, unsigned int, unsigned int)>(crc32_addr)(stats.data, playerdata_size, 0);
			std::memcpy(live.data(), &crc, 4);
			std::memcpy(live.data() + 4, stats.data, playerdata_size);

			const auto live_path = base + "_live";
			if (!utils::io::write_file(live_path, live) || utils::io::file_size(live_path) != live.size()) return {};
			return base;
		}
		catch (const std::exception&)
		{
			return {};
		}
	}

	struct
	{
		bool active = false;
		bool warned = false;
		DWORD started = 0;
		DWORD last_try = 0;
		int rewrites = 0;
		std::filesystem::file_time_type file_time{};
	} save;

	std::filesystem::file_time_type mpdata_time()
	{
		std::error_code ec;
		const auto time = std::filesystem::last_write_time(mpdata_path, ec);
		return ec ? std::filesystem::file_time_type{} : time;
	}

	// the file's time before an upload that started, to see that it wrote the file
	void upload()
	{
		const auto file_time = mpdata_time();
		reinterpret_cast<void(*)(int)>(stats_upload_addr)(0);
		if (!*reinterpret_cast<char*>(stats_write_needed)) save.file_time = file_time;
	}

	void start_save()
	{
		const auto now = GetTickCount();
		save.active = true;
		save.warned = false;
		save.started = now;
		save.last_try = now;
		save.rewrites = 0;
		save.file_time = mpdata_time();
		upload();
	}

	// the upload does nothing while another one is running (busy) or DemonWare isn't ready: again until it started, and
	// done when its task is (busy cleared)
	void poll_save(const DWORD now)
	{
		if (!save.active) return;

		if (!*reinterpret_cast<char*>(stats_fetched))
		{
			save.active = false;
			print("unlockstats: the stats were reloaded before they were saved; run unlockstats again");
			return;
		}

		const auto busy = *reinterpret_cast<char*>(stats_upload_busy) != 0;
		const auto dirty = *reinterpret_cast<char*>(stats_write_needed) != 0;

		if (!busy && !dirty)
		{
			if (mpdata_time() != save.file_time)
			{
				save.active = false;
				print("unlockstats: saved to " + display_path(mpdata_path));
				return;
			}

			// the task finished without the file changing (bdStorage couldn't write it): again, a few times
			if (++save.rewrites > 3)
			{
				save.active = false;
				print("unlockstats: the upload finished but " + display_path(mpdata_path) + " didn't change; the stats are "
					"unlocked in memory and the next stats upload (a match end, a class edit) writes them");
				return;
			}
			reinterpret_cast<void(*)(int)>(stats_changed_addr)(0);
		}

		if (!busy && *reinterpret_cast<char*>(stats_write_needed) && now - save.last_try >= 500)
		{
			save.last_try = now;
			upload();
		}

		if (!save.warned && now - save.started >= 30000)
		{
			save.warned = true;
			print("unlockstats: save still pending (DemonWare busy or offline); it will be written by the next stats upload. "
				"Don't quit before a match ends or a class is edited.");
		}
	}

	// ---- unlockstats ----

	// nullptr when the stats can be changed now, else why not
	const char* blocker(sd_buffer& stats)
	{
		if (!get_stats(&stats)) return "stats not loaded yet";
		if (*reinterpret_cast<DWORD*>(theater_playerdata)) return "theater playerdata active";
		if (stats_source() != 0) return "systemlink stats in use, not supported";
		return nullptr;
	}

	void unlock_all(const sd_buffer& stats)
	{
		def = reinterpret_cast<const sd_def*(*)(const char*, unsigned int)>(sd_def_get_addr)(playerdata_def, playerdata_size);
		buffer = stats;
		writes.clear();

		const auto* header = reinterpret_cast<const unsigned int*>(stats.data);
		if (!def || header[0] != static_cast<unsigned int>(def->version) || header[1] != def->checksum)
		{
			print(utils::string::va("unlockstats: the stats are version %u / %08X, %s is %d / %08X: nothing changed", header[0], header[1],
			                        playerdata_def, def ? def->version : 0, def ? def->checksum : 0));
			return;
		}

		std::vector<std::string> lines;
		std::vector<std::string> missing_tables;
		const auto table = [&](const char* name)
		{
			const auto* t = find_table(name);
			if (!t) missing_tables.emplace_back(name);
			return t;
		};

		// the level: the max XP (rankTable "maxrank" col 1, that rank's col 7) when below the max rank; the prestige is kept
		group level;
		const auto xp_before = read_int({"experience"});
		auto xp_after = xp_before;
		const auto prestige = read_int({"prestige"});
		auto max_rank = 0;
		if (const auto* ranks = table("mp/rankTable.csv"))
		{
			const auto max_row = find_row(ranks, 0, "maxrank");
			const std::string max_rank_text = max_row >= 0 ? cell(ranks, max_row, 1) : "";
			const auto rank_row = max_rank_text.empty() ? -1 : find_row(ranks, 0, max_rank_text.data());
			max_rank = std::atoi(max_rank_text.data());
			const auto max_xp = rank_row >= 0 ? std::atoi(cell(ranks, rank_row, 7)) : 0;

			if (max_rank <= 0 || max_xp <= 0) level.add(result::failed);
			else if (reinterpret_cast<int(*)(int)>(rank_for_xp_addr)(xp_before) >= max_rank) level.add(result::already);
			else
			{
				const auto r = raise_field({"experience"}, max_xp);
				level.add(r);
				if (r == result::changed) xp_after = max_xp;
			}
		}
		else level.add(result::failed);

		// the weapons: every statstable weapon (col 4) whose class (col 2) has a weaponRankTable row gets that class's max
		// weapon rank (col 1) and its XP (col 1 of that rank's row): attachments, camos with gold, proficiencies, reticles
		group weapons;
		{
			const auto* stats_table = table("mp/statstable.csv");
			const auto* weapon_ranks = table("mp/weaponRankTable.csv");
			std::set<std::string> seen;
			for (auto row = 0; stats_table && weapon_ranks && row < stats_table->rows; ++row)
			{
				const std::string weapon = cell(stats_table, row, 4);
				const auto* weapon_class = cell(stats_table, row, 2);
				if (weapon.empty() || !*weapon_class) continue;

				const auto class_row = find_row(weapon_ranks, 0, weapon_class);
				if (class_row < 0) continue;
				if (!seen.insert(utils::string::to_lower(weapon)).second) continue;

				const std::string max_text = cell(weapon_ranks, class_row, 1);
				const auto max_row = max_text.empty() ? -1 : find_row(weapon_ranks, 0, max_text.data());
				const auto max_weapon_rank = std::atoi(max_text.data());
				const auto max_weapon_xp = max_row >= 0 ? std::atoi(cell(weapon_ranks, max_row, 1)) : 0;
				if (max_weapon_rank <= 0 || max_weapon_xp <= 0)
				{
					weapons.add(result::failed);
					continue;
				}

				weapons.add(raise_field({"weaponXP", weapon}, max_weapon_xp) | raise_field({"weaponRank", weapon}, max_weapon_rank));
			}
			if (!stats_table || !weapon_ranks) weapons.add(result::failed);
		}

		// the challenges: every tier done (state = tiers + 1, progress = the last tier's target; the targets are cols 6, 8 ..
		// 22); ch_prestige / ch_prestige_10 stay with the real prestige (the prestige reset rewrites them)
		group challenges;
		if (const auto* all_challenges = table("mp/allChallengesTable.csv"))
		{
			std::set<std::string> seen;
			for (auto row = 0; row < all_challenges->rows; ++row)
			{
				const std::string name = cell(all_challenges, row, 0);
				if (name.empty() || _stricmp(name.data(), "ch_prestige") == 0 || _stricmp(name.data(), "ch_prestige_10") == 0) continue;

				auto tiers = 0, last = 0;
				for (auto column = 6; column <= 22; column += 2)
				{
					const auto* target = cell(all_challenges, row, column);
					if (!*target) continue;
					++tiers;
					last = std::atoi(target);
				}
				if (!tiers || !seen.insert(utils::string::to_lower(name)).second) continue;

				challenges.add(raise_field({"challengeState", name}, tiers + 1) | raise_field({"challengeProgress", name}, last));
			}
		}
		else challenges.add(result::failed);

		// the prestige shop, as its menu buys (state 2): the permanent unlocks progress 2, the titles and ch_prPurchase1..3
		// progress 1; no tokens are spent
		group shop;
		if (const auto* challenge_enum = array_enum("challengeState"))
		{
			for (auto i = 0; i < challenge_enum->entry_count; ++i)
			{
				const auto* name = challenge_enum->entries[i].name;
				if (!name || _strnicmp(name, "ch_prPurchase", 13) != 0) continue;

				const auto title = _strnicmp(name, "ch_prPurchase_title", 19) == 0;
				const auto permanent = !title && _strnicmp(name, "ch_prPurchase_", 14) == 0;
				shop.add(raise_field({"challengeState", name}, 2) | raise_field({"challengeProgress", name}, permanent ? 2 : 1));
			}
		}
		else shop.add(result::failed);

		// the classes: 5 + prestige + entitlement, all 15 (every one is already set up by stats_init)
		group classes;
		classes.add(raise_field({"extraCustomClassesPrestige"}, prestige_classes));
		classes.add(raise_field({"extraCustomClassesEntitlement"}, entitlement_classes));
		const auto class_count = std::min(15, 5 + std::max(read_int({"extraCustomClassesPrestige"}), prestige_classes)
		                                  + std::max(read_int({"extraCustomClassesEntitlement"}), entitlement_classes));

		// the DLC camos and reticles (camoTable col 6: marine, winter), entitlement bits in the real game
		group dlc;
		for (const auto* field : {"unlockedCamo", "unlockedReticles"})
		{
			const auto size = array_size(field);
			if (!size) dlc.add(result::failed);
			for (auto i = 0; i < size; ++i) dlc.add(unlock_field({field, i}));
		}

		// every title and emblem: the bit of its card table row (kept by a prestige)
		group titles, icons;
		const auto card_bits = [&](group& g, const char* table_name, const char* field)
		{
			const auto* cards = table(table_name);
			const auto size = array_size(field);
			if (!cards || !size)
			{
				g.add(result::failed);
				return;
			}
			for (auto i = 0; i < std::min(cards->rows, size); ++i) g.add(unlock_field({field, i}));
		};
		card_bits(titles, "mp/cardTitleTable.csv", "titleUnlocked");
		card_bits(icons, "mp/cardIconTable.csv", "iconUnlocked");

		const auto rank_after = xp_after != xp_before ? max_rank : reinterpret_cast<int(*)(int)>(rank_for_xp_addr)(xp_before);
		if (level.failed) lines.push_back(utils::string::va("  level %d (xp %d): FAILED, prestige %d kept", rank_after + 1, xp_before, prestige));
		else if (level.changed) lines.push_back(utils::string::va("  level %d (xp %d -> %d), prestige %d kept", rank_after + 1, xp_before, xp_after, prestige));
		else lines.push_back(utils::string::va("  level %d (xp %d, already the max), prestige %d kept", rank_after + 1, xp_before, prestige));
		lines.push_back(utils::string::va("  weapons: %d at max weapon level (%s)", weapons.done, weapons.counts().data()));
		lines.push_back(utils::string::va("  challenges: %d complete, with the pro perks, titles and emblems (%s)", challenges.done,
		                                  challenges.counts().data()));
		lines.push_back(utils::string::va("  prestige shop: %d bought, no tokens spent (%s)", shop.done, shop.counts().data()));
		lines.push_back(utils::string::va("  classes: %d, the prestige and the entitlement ones (%s); Private Match has its own 5 (as on the console)", class_count,
		                                  classes.counts().data()));
		lines.push_back(utils::string::va("  titles %d, emblems %d (%s / %s); in Find Game 510 titles are selectable (2 have no unlock row)",
		                                  titles.done, icons.done, titles.counts().data(), icons.counts().data()));
		lines.push_back(utils::string::va("  DLC camos / reticles: %d (%s)", dlc.done, dlc.counts().data()));
		lines.push_back("  token_22 / token_23 stay locked (rank 9999, as in the game)");
		for (const auto& name : missing_tables) lines.push_back("  missing table " + name + ": FAILED");

		const auto failed = level.failed + weapons.failed + challenges.failed + shop.failed + classes.failed + dlc.failed + titles.failed + icons.failed;
		if (writes.empty())
		{
			print(utils::string::va("unlockstats: nothing to change (0 changed, %d failed)", failed));
			for (const auto& line : lines) print(line);
			return;
		}

		const auto backup_name = backup(stats);
		if (backup_name.empty())
		{
			writes.clear();
			print("unlockstats: couldn't write the backup to " + display_path(backup_dir) + ": nothing changed");
			return;
		}

		const auto change_count = static_cast<int>(writes.size());
		const auto write_failed = apply_writes();

		reinterpret_cast<void(*)()>(challenge_cache_dirty_addr)();
		// the lobby's challenge tracker keeps the challenges it picked before the unlock (its list is only rebuilt when the
		// filter changes, 0x54D670) and showed a finished one as "Marksman IX 1000/0" (no target past the last tier): the
		// next frame rebuilds it with every challenge complete, so the tracker is hidden, as for a fully completed player
		*reinterpret_cast<char*>(sorted_challenge_filter) = 0;
		*reinterpret_cast<int*>(sorted_challenge_count) = 0;
		reinterpret_cast<void(*)(int)>(stats_changed_addr)(0);
		reinterpret_cast<void(*)(int, int)>(playerdata_read_addr)(0, 0);

		print(utils::string::va("unlockstats: backup %s (+ _live) | %d fields changed, %d failed", display_path(backup_name).data(),
		                        change_count - write_failed, failed + write_failed));
		for (const auto& line : lines) print(line);
		print("unlockstats: saving to " + display_path(mpdata_path) + "...");

		start_save();
	}

	// queued in a match: applied once back in the menu
	bool pending = false;
	bool menu_seen = false;
	DWORD menu_since = 0;

	void unlockstats_command(const std::vector<std::string>& params)
	{
		// "elite" (what the 15 classes once needed) is taken and changes nothing
		for (size_t i = 1; i < params.size(); ++i)
		{
			if (_stricmp(params[i].data(), "elite") != 0)
			{
				print("unlockstats: unlocks everything (max level, weapons, challenges, prestige shop, all 15 classes (Private Match keeps its own 5), titles, "
					"emblems); a backup of your stats goes to " + display_path(backup_dir) + " first");
				return;
			}
		}

		sd_buffer stats{};
		if (const auto* why = blocker(stats))
		{
			print(std::string("unlockstats: ") + why);
			return;
		}

		if (in_match())
		{
			print(pending ? "unlockstats: already queued" : "unlockstats: queued: applies when you are back in the menu");
			pending = true;
			menu_seen = false;
			return;
		}

		unlock_all(stats);
	}

	void frame()
	{
		const auto now = GetTickCount();

		// back in the menu for a second, after the end-of-match unlock diff (its snapshot flag; at most 10 s) and its upload
		if (pending)
		{
			if (in_match()) menu_seen = false;
			else if (!menu_seen)
			{
				menu_seen = true;
				menu_since = now;
			}
			else if (now - menu_since >= 1000 && (!*reinterpret_cast<char*>(unlock_snapshot) || now - menu_since >= 10000))
			{
				sd_buffer stats{};
				if (get_stats(&stats))
				{
					pending = false;

					if (const auto* why = blocker(stats)) print(std::string("unlockstats (queued): ") + why + ", nothing changed");
					else
					{
						print("unlockstats: applying the queued unlock");
						unlock_all(stats);
					}
				}
			}
		}

		poll_save(now);
	}
}

class unlockstats final : public module
{
public:
	void post_load() override
	{
		if (!game::is_mp()) return;

		scheduler::on_frame(frame);
		scheduler::once([]
		{
			command::add("unlockstats", unlockstats_command);
		});
	}
};

REGISTER_MODULE(unlockstats)
