#include <std_include.hpp>
#include "bdStorage.hpp"
#include "../data_types.hpp"
#include "utils/cryptography.hpp"
#include "utils/compression.hpp"
#include "utils/nt.hpp"
#include "utils/io.hpp"
#include "module/ps3style.hpp"

namespace demonware
{
	namespace
	{
		// the fileID of a user file in bdFileInfo: the first 8 bytes of SHA1(name). The game keeps the one it gets for
		// "mpdata" and sends it back with every later stats upload (task 8, update by fileID, 0x709780)
		uint64_t file_id_of(const std::string& name)
		{
			return *reinterpret_cast<const uint64_t*>(utils::cryptography::sha1::compute(name).data());
		}

		// PS3 style: <game>\ps3style\playlists.aggr (ps3style\tools\make_playlists.py), the built-in playlists made
		// playable offline. The game takes a file of format 1 (header read 0x54A140) and at most 0xF000 bytes (0x54FC50,
		// "Insufficient space for playlists"); anything else falls back to the built-in one
		bool load_ps3_playlists(std::string& buffer)
		{
			const auto path = ps3style::folder() + "\\playlists.aggr";

			std::string data;
			if (!utils::io::read_file(path, &data)) return false;

			if (data.size() < 12 || data.size() > 0xF000 || *reinterpret_cast<const uint16_t*>(data.data()) != 1)
			{
				printf("DW: %s is not a playlist file the game takes, using the built-in playlists\n", path.data());
				return false;
			}

			printf("DW: PS3 style: playlists from %s\n", path.data());
			buffer = std::move(data);
			return true;
		}

		// PS3 style: <game>\ps3style\heatmap.raw, the world map with player dots (ownerDraw 381 "playlist_population").
		// The game downloads it into a 0x4000-byte buffer (0x54F360; bdByteBuffer::readBlob 0x6F22C0 keeps the full size of
		// a bigger file, so 0x54B954 would inflate past the buffer) and takes it only when it inflates to 0x10000 bytes, a
		// 256x256 map (0x54A090, checked at 0x54B95D); anything else falls back to the built-in one
		bool load_ps3_heatmap(std::string& buffer)
		{
			const auto path = ps3style::folder() + "\\heatmap.raw";

			std::string data;
			if (!utils::io::read_file(path, &data)) return false;

			if (data.empty() || data.size() > 0x4000)
			{
				printf("DW: %s is %u bytes (the game takes 1 to %u), using the built-in heatmap\n", path.data(),
				       uint32_t(data.size()), 0x4000u);
				return false;
			}

			if (utils::compression::zlib::decompress(data).size() != 0x10000)
			{
				printf("DW: %s doesn't inflate to a 256x256 map, using the built-in heatmap\n", path.data());
				return false;
			}

			printf("DW: PS3 style: heatmap from %s\n", path.data());
			buffer = std::move(data);
			return true;
		}

		// the game's JPEG reader (an old stb_image) takes only a baseline 8-bit frame (SOF0 / SOF1, 0x6B7D90) with 1 or 3
		// components, and before it only DHT, DQT, DRI, APPn and COM segments (0x6B7270; SOF2 is "progressive jpeg"):
		// component ids 1..n or 0..n-1 (0x6B7A8E), 8-bit DQT tables 0-3 (0x6B7355), DHT class 0-1 / table 0-3 (0x6B746D),
		// DRI length 4 (0x6B72C5). iotd_image is 256x256 and 0x59B2F0 keeps bigger pictures out (0x59B32E / 0x59B33C)
		bool is_loadable_jpeg(const std::string& data)
		{
			const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
			const auto size = data.size();

			if (size < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) return false;

			size_t pos = 2;
			while (true)
			{
				if (pos >= size || bytes[pos] != 0xFF) return false;
				while (pos < size && bytes[pos] == 0xFF) ++pos;
				if (pos + 3 > size) return false;

				const auto marker = bytes[pos];
				const auto* segment = bytes + pos + 1;
				const size_t length = (segment[0] << 8) | segment[1];
				if (length < 2 || pos + 1 + length > size) return false;

				if (marker == 0xC0 || marker == 0xC1)
				{
					if (length < 8) return false;

					const auto height = (segment[3] << 8) | segment[4];
					const auto width = (segment[5] << 8) | segment[6];
					const auto components = segment[7];

					if (segment[2] != 8 || (components != 1 && components != 3)) return false;
					if (length != 8 + 3u * components) return false;
					if (height < 1 || height > 256 || width < 1 || width > 256) return false;

					for (auto i = 0; i < components; ++i)
					{
						if (segment[8 + 3 * i] != i + 1 && segment[8 + 3 * i] != i) return false;

						const auto horizontal = segment[9 + 3 * i] >> 4;
						const auto vertical = segment[9 + 3 * i] & 15;
						const auto table = segment[10 + 3 * i];

						if (horizontal < 1 || horizontal > 4 || vertical < 1 || vertical > 4 || table > 3) return false;
					}

					return true;
				}

				const auto skipped = marker == 0xC4 || marker == 0xDB || marker == 0xDD || marker == 0xFE
					|| (marker >= 0xE0 && marker <= 0xEF);
				if (!skipped) return false;

				if (marker == 0xDB)
				{
					size_t i = 2;
					for (; i < length; i += 65)
					{
						if ((segment[i] >> 4) != 0 || (segment[i] & 15) > 3) return false;
					}

					if (i != length) return false;
				}
				else if (marker == 0xC4)
				{
					size_t i = 2;
					while (i < length)
					{
						if (i + 17 > length || (segment[i] >> 4) > 1 || (segment[i] & 15) > 3) return false;

						size_t count = 0;
						for (auto j = 1; j <= 16; ++j) count += segment[i + j];
						i += 17 + count;
					}

					if (i != length) return false;
				}
				else if (marker == 0xDD && length != 4) return false;

				pos += 1 + length;
			}
		}

		// PS3 style: <game>\ps3style\iotd-<language>.jpg / .txt (the image of the day and its caption, the main menu's
		// ownerDraw 387) and motd-<language>.txt (the message of the day in the PSN menu), else the -english file.
		// - jpg: below 0x8000 bytes (0x546C50 turns the picture off at 0x8000) and loadable (is_loadable_jpeg)
		// - txt: at most 0xFF bytes after the CR / LF at the end, which the game trims too (0x547D80 / 0x547C10 take 0x100,
		//   but the NUL of a 0x100-byte caption lands on the MOTD-done flag 0x1CBA990)
		bool load_ps3_daily(const std::string& name, std::string& buffer)
		{
			static const std::regex daily{"(iotd|motd)-([a-z]+)\\.(jpg|txt)"};

			std::smatch match;
			if (!std::regex_match(name, match, daily)) return false;

			const auto image = match[3].str() == "jpg";

			std::vector<std::string> files{name};
			const auto english = match[1].str() + "-english." + match[3].str();
			if (english != name) files.push_back(english);

			for (const auto& file : files)
			{
				const auto path = ps3style::folder() + "\\" + file;

				std::string data;
				if (!utils::io::read_file(path, &data)) continue;

				if (image)
				{
					if (data.empty() || data.size() >= 0x8000)
					{
						printf("DW: %s is %u bytes (the game takes 1 to %u), skipped\n", path.data(), uint32_t(data.size()),
						       0x7FFFu);
						continue;
					}

					if (!is_loadable_jpeg(data))
					{
						printf("DW: %s isn't a baseline 8-bit JPEG of at most 256x256 the game can load, skipped\n",
						       path.data());
						continue;
					}
				}
				else
				{
					while (!data.empty() && (data.back() == '\r' || data.back() == '\n')) data.pop_back();

					if (data.empty() || data.size() > 0xFF)
					{
						printf("DW: %s is %u bytes (the game takes 1 to %u), skipped\n", path.data(), uint32_t(data.size()),
						       0xFFu);
						continue;
					}
				}

				printf("DW: PS3 style: %s from %s\n", name.data(), path.data());
				buffer = std::move(data);
				return true;
			}

			return false;
		}
	}

	bdStorage::bdStorage()
	{
		this->register_service(1, &bdStorage::set_legacy_user_file);
		this->register_service(3, &bdStorage::get_legacy_user_file);
		this->register_service(5, &bdStorage::list_legacy_user_files);
		this->register_service(6, &bdStorage::list_publisher_files);
		this->register_service(7, &bdStorage::get_publisher_file);
		this->register_service(8, &bdStorage::update_user_file);
		this->register_service(10, &bdStorage::set_user_file);
		this->register_service(11, &bdStorage::delete_user_file);
		this->register_service(12, &bdStorage::get_user_file);

		this->map_publisher_resource("heatmap\\.raw", DW_HEATMAP);
		this->map_publisher_resource("motd-.*\\.txt", DW_MOTD);
		this->map_publisher_resource("online_mp\\.img", DW_IMG);
		this->map_publisher_resource("online_[Tt][Uu][0-9]+_mp_.+\\.wad", DW_WAD);
		this->map_publisher_resource("playlists(_.+)?\\.aggr", DW_PLAYLIST);
		this->map_publisher_resource("social_[Tt][Uu][0-9]+\\.cfg", DW_CONFIG);
		this->map_publisher_resource("iotd-.*\\.txt", DW_IOTD_TXT);
		this->map_publisher_resource("iotd-.*\\.jpg", DW_IOTD_IMG);
	}

	void bdStorage::map_publisher_resource(const std::string& expression, const INT id)
	{
		const auto res = FindResource(::utils::nt::module(), MAKEINTRESOURCE(id), RT_RCDATA);
		if (!res) return;

		const auto handle = LoadResource(nullptr, res);
		if (!handle) return;

		std::string data(LPSTR(LockResource(handle)), SizeofResource(nullptr, res));

		publisher_resources_.emplace_back(std::regex{expression}, data);
	}

	bool bdStorage::load_publisher_resource(const std::string& name, std::string& buffer)
	{
		if (ps3style::enabled())
		{
			// the offline PS3 look: the image of the day / MOTD only from <game>\ps3style (load_ps3_daily), never
			// open-iw5's skull / "Blub" / "Welcome to Open-IW5". Without a file the game gets BD_NO_FILE: it turns the
			// image download off (0x546C50 clears 0x8ACEB0), sets iotdText "" (0x547E97) and ends the MOTD download
			// (0x547C10), no retries, and keeps its own MOTD (PLATFORM_NOMOTD_MP, set at start 0x55635D).
			// Ownerdraw 387 (0x58C1E0) draws only when the picture is there: a picture without a caption shows with an
			// empty caption, a caption without a picture shows nothing. The picture is uploaded once per DW session
			// (0x59B2F0, then 0x8ACEB1 is cleared)
			static const std::regex ps3_daily{"(motd|iotd)-.*"};
			if (std::regex_match(name, ps3_daily))
			{
				if (load_ps3_daily(name, buffer)) return true;

				printf("DW: PS3 style: no %s\n", name.data());
				return false;
			}

			static const std::regex playlists{"playlists(_.+)?\\.aggr"};
			if (std::regex_match(name, playlists) && load_ps3_playlists(buffer)) return true;

			static const std::regex heatmap{"heatmap\\.raw"};
			if (std::regex_match(name, heatmap) && load_ps3_heatmap(buffer)) return true;
		}

		for (const auto& resource : this->publisher_resources_)
		{
			if (std::regex_match(name, resource.first))
			{
				buffer = resource.second;
				return true;
			}
		}

		printf("DW: Missing publisher file: %s\n", name.data());

		return false;
	}

	std::string bdStorage::get_user_file_path(const std::string& name)
	{
		return "players2/user/" + name;
	}

	// the user file behind a fileID (file_id_of): "mpdata" (the stats) or another file in players2/user, "" when none is
	std::string bdStorage::find_user_file_name(const uint64_t file_id)
	{
		if (file_id_of("mpdata") == file_id) return "mpdata";

		try
		{
			for (const auto& file : std::filesystem::directory_iterator(get_user_file_path("")))
			{
				const auto extension = file.path().extension();
				if (!file.is_regular_file() || extension == ".tmp" || extension == ".bak") continue;

				const auto name = file.path().filename().string();
				if (file_id_of(name) == file_id) return name;
			}
		}
		catch (const std::exception&)
		{
		}

		return {};
	}

	// players2/user/<name> through <name>.tmp, so a failed write never leaves a cut-off file; the previous file stays as
	// <name>.bak
	bool bdStorage::write_user_file(const std::string& name, const std::string& data)
	{
		const auto path = get_user_file_path(name);
		const auto tmp = path + ".tmp";

		if (!utils::io::write_file(tmp, data)) return false;

		if (utils::io::file_size(tmp) != data.size())
		{
			DeleteFileA(tmp.data());
			return false;
		}

		if (utils::io::file_exists(path))
		{
			CopyFileA(path.data(), (path + ".bak").data(), FALSE);
		}

		return MoveFileExA(tmp.data(), path.data(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
	}

	void bdStorage::set_legacy_user_file(i_server* server, byte_buffer* buffer) const
	{
		bool priv;
		std::string filename, data;

		buffer->read_string(&filename);
		buffer->read_bool(&priv);
		buffer->read_blob(&data);

		printf("DW: Storing user file: %s\n", filename.data());

		if (!write_user_file(filename, data))
		{
			printf("DW: Failed to store user file: %s\n", filename.data());
		}

		auto info = new bdFileInfo;

		info->file_id = file_id_of(filename);
		info->filename = filename;
		info->create_time = uint32_t(time(nullptr));
		info->modified_time = info->create_time;
		info->file_size = uint32_t(data.size());
		info->owner_id = 0;
		info->priv = priv;

		auto reply = server->create_reply(this->get_sub_type());
		reply->add(info);
		reply->send();
	}

	void bdStorage::get_legacy_user_file(i_server* server, byte_buffer* buffer) const
	{
		std::string filename, data;
		buffer->read_string(&filename);

		printf("DW: Loading user file: %s\n", filename.data());

		const auto path = get_user_file_path(filename);
		if (utils::io::read_file(path, &data))
		{
			auto reply = server->create_reply(this->get_sub_type());
			reply->add(new bdFileData(data));
			reply->send();
		}
		else
		{
			server->create_reply(this->get_sub_type(), game::native::BD_NO_FILE)->send();
		}
	}

	void bdStorage::list_legacy_user_files(i_server* server, byte_buffer* buffer) const
	{
		uint64_t unk;
		uint32_t date;
		uint16_t num_results, offset;
		std::string filename, data;

		buffer->read_uint64(&unk);
		buffer->read_uint32(&date);
		buffer->read_uint16(&num_results);
		buffer->read_uint16(&offset);
		buffer->read_string(&filename);

		auto reply = server->create_reply(this->get_sub_type());

		const auto path = get_user_file_path(filename);
		if (utils::io::read_file(path, &data))
		{
			auto* info = new bdFileInfo;

			info->file_id = file_id_of(filename);
			info->filename = filename;
			info->create_time = 0;
			info->modified_time = info->create_time;
			info->file_size = uint32_t(data.size());
			info->owner_id = 0;
			info->priv = false;

			reply->add(info);
		}

		reply->send();
	}

	void bdStorage::list_publisher_files(i_server* server, byte_buffer* buffer)
	{
		uint32_t date;
		uint16_t num_results, offset;
		std::string filename, data;

		buffer->read_uint32(&date);
		buffer->read_uint16(&num_results);
		buffer->read_uint16(&offset);
		buffer->read_string(&filename);

		auto reply = server->create_reply(this->get_sub_type());

		if (this->load_publisher_resource(filename, data))
		{
			auto* info = new bdFileInfo;

			info->file_id = *reinterpret_cast<const uint64_t*>(utils::cryptography::sha1::compute(filename).data());
			info->filename = filename;
			info->create_time = 0;
			info->modified_time = info->create_time;
			info->file_size = uint32_t(data.size());
			info->owner_id = 0;
			info->priv = false;

			reply->add(info);
		}

		reply->send();
	}

	void bdStorage::get_publisher_file(i_server* server, byte_buffer* buffer)
	{
		std::string filename;
		buffer->read_string(&filename);

		printf("DW: Loading publisher file: %s\n", filename.data());

		std::string data;
		if (this->load_publisher_resource(filename, data))
		{
			auto reply = server->create_reply(this->get_sub_type());
			reply->add(new bdFileData(data));
			reply->send();
		}
		else
		{
			server->create_reply(this->get_sub_type(), game::native::BD_NO_FILE)->send();
		}
	}

	// every stats upload after the first one of a game session (LiveStorage_UploadStats 0x553150 -> 0x63ED80 -> 0x709780):
	// the fileID the game got in bdFileInfo, then the file
	void bdStorage::update_user_file(i_server* server, byte_buffer* buffer) const
	{
		uint64_t file_id{};
		std::string data;

		buffer->read_uint64(&file_id);
		buffer->read_blob(&data);

		const auto filename = find_user_file_name(file_id);
		if (filename.empty())
		{
			printf("DW: Update of unknown user file %016llX (%u bytes) dropped\n", file_id, uint32_t(data.size()));
		}
		else if (data.empty())
		{
			printf("DW: Empty update of user file %s dropped\n", filename.data());
		}
		else
		{
			printf("DW: Updating user file: %s (%u bytes)\n", filename.data(), uint32_t(data.size()));

			if (!write_user_file(filename, data))
			{
				printf("DW: Failed to update user file: %s\n", filename.data());
			}
		}

		// the game takes an empty reply as done
		server->create_reply(this->get_sub_type())->send();
	}

	void bdStorage::delete_user_file(i_server* server, byte_buffer* buffer) const
	{
		uint64_t owner;
		std::string game, filename;

		buffer->read_string(&game);
		buffer->read_string(&filename);
		buffer->read_uint64(&owner);

		// Really remove the file?

		auto reply = server->create_reply(this->get_sub_type());
		reply->send();
	}

	void bdStorage::set_user_file(i_server* server, byte_buffer* buffer) const
	{
		bool priv;
		uint64_t owner;
		std::string game, filename, data;

		buffer->read_string(&game);
		buffer->read_string(&filename);
		buffer->read_bool(&priv);
		buffer->read_blob(&data);
		buffer->read_uint64(&owner);

		if (!write_user_file(filename, data))
		{
			printf("DW: Failed to store user file: %s\n", filename.data());
		}

		auto info = new bdFileInfo;

		info->file_id = file_id_of(filename);
		info->filename = filename;
		info->create_time = uint32_t(time(nullptr));
		info->modified_time = info->create_time;
		info->file_size = uint32_t(data.size());
		info->owner_id = owner;
		info->priv = priv;

		auto reply = server->create_reply(this->get_sub_type());
		reply->add(info);
		reply->send();
	}

	void bdStorage::get_user_file(i_server* server, byte_buffer* buffer) const
	{
		uint64_t owner;
		std::string game, filename, platform, data;

		buffer->read_string(&game);
		buffer->read_string(&filename);
		buffer->read_uint64(&owner);
		buffer->read_string(&platform);

		const auto path = get_user_file_path(filename);
		if (utils::io::read_file(path, &data))
		{
			auto reply = server->create_reply(this->get_sub_type());
			reply->add(new bdFileData(data));
			reply->send();
		}
		else
		{
			server->create_reply(this->get_sub_type(), game::native::BD_NO_FILE)->send();
		}
	}
}
