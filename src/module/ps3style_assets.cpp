#include <std_include.hpp>
#include "ps3style_assets.hpp"
#include "utils/compression.hpp"
#include "utils/io.hpp"
#include "utils/string.hpp"

#include <d3d9.h>
#include <rapidjson/document.h>

using namespace game::native::ui;

namespace ps3style_assets
{
	namespace
	{
		std::string root;
		std::string font_suffix; // fonts\<font><suffix>.json: "" the Japanese PS3 disc's fonts, "_en" the English one's
		std::recursive_mutex mutex;
		std::set<std::string> image_names;
		std::unordered_map<std::string, Material*> materials;
		std::unordered_map<std::string, Font_s*> fonts;
		std::unordered_map<std::string, GfxImage*> images;
		IDirect3DDevice9* cached_device = nullptr; // the renderer's device (dropped by reset(): vid_restart makes a new one)
		const void* made_with_white = nullptr; // the "white" material (and its texture) the cached assets were made from
		const void* made_with_texture = nullptr;

		void drop_cached()
		{
			materials.clear();
			fonts.clear();
			images.clear();
			cached_device = nullptr;
		}
		std::unordered_map<std::string, std::pair<int, int>> image_sizes; // {0, 0}: no image
		bool xbox_icons = false;

		// the file of an image: with the Xbox buttons on, ps3_button_<name> is read from xbox_button_<name>.ps3z when
		// there is one (the material keeps the PS3 name, which the prompts and the button code use)
		std::string image_file(const std::string& key)
		{
			constexpr char prefix[] = "ps3_button_";
			if (xbox_icons && key.compare(0, sizeof(prefix) - 1, prefix) == 0)
			{
				const auto xbox = "xbox_button_" + key.substr(sizeof(prefix) - 1);
				if (image_names.count(xbox)) return root + "\\images\\" + xbox + ".ps3z";
			}
			return root + "\\images\\" + key + ".ps3z";
		}

		// DB_FindXAssetEntry (edi = type, name on the stack): the asset entry {type, header, ...} or nullptr
		__declspec(naked) void* db_find_x_asset_entry(int /*type*/, const char* /*name*/)
		{
			__asm
			{
				push edi
				push ebx
				mov edi, [esp + 0Ch]
				push [esp + 10h]
				mov eax, 4CA450h
				call eax
				add esp, 4
				pop ebx
				pop edi
				retn
			}
		}

		template <typename T>
		T* make(const size_t count = 1)
		{
			// lives as long as the game
			return static_cast<T*>(calloc(count, sizeof(T)));
		}

		char* copy_string(const std::string& s)
		{
			auto* out = static_cast<char*>(calloc(1, s.size() + 1));
			std::memcpy(out, s.data(), s.size());
			return out;
		}

		IDirect3DDevice9* device()
		{
			auto*& dev = cached_device;
			if (dev) return dev;

			const auto* white = static_cast<Material*>(find_loaded(ASSET_TYPE_MATERIAL, "white"));
			if (!white || !white->textureTable || !white->textureTable[0].image) return nullptr;

			auto* texture = static_cast<IDirect3DTexture9*>(white->textureTable[0].image->texture);
			if (texture) texture->GetDevice(&dev);
			return dev;
		}

		// every mip level gets filled (box filter, alpha weighted), else smaller draws sample garbage
		void upload(IDirect3DTexture9* texture, unsigned int w, unsigned int h, std::string level)
		{
			const auto levels = texture->GetLevelCount();
			for (DWORD l = 0; l < levels; ++l)
			{
				D3DLOCKED_RECT locked;
				if (SUCCEEDED(texture->LockRect(l, &locked, nullptr, 0)))
				{
					for (unsigned int y = 0; y < h; ++y)
					{
						std::memcpy(static_cast<char*>(locked.pBits) + y * locked.Pitch, level.data() + y * w * 4, w * 4);
					}
					texture->UnlockRect(l);
				}

				if (w == 1 && h == 1) break;

				const auto nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
				std::string next(nw * nh * 4, '\0');
				for (unsigned int y = 0; y < nh; ++y)
				{
					for (unsigned int x = 0; x < nw; ++x)
					{
						unsigned int sum[3]{}, alpha = 0, count = 0;
						for (unsigned int dy = 0; dy < 2; ++dy)
						{
							for (unsigned int dx = 0; dx < 2; ++dx)
							{
								const auto sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy);
								const auto* p = reinterpret_cast<const unsigned char*>(level.data()) + (sy * w + sx) * 4;
								for (auto c = 0; c < 3; ++c) sum[c] += p[c] * p[3];
								alpha += p[3];
								++count;
							}
						}
						auto* q = reinterpret_cast<unsigned char*>(next.data()) + (y * nw + x) * 4;
						for (auto c = 0; c < 3; ++c) q[c] = static_cast<unsigned char>(alpha ? sum[c] / alpha : 0);
						q[3] = static_cast<unsigned char>(alpha / count);
					}
				}
				level = std::move(next);
				w = nw;
				h = nh;
			}
		}

		// the images are drawn with linear filtering and straight alpha (SRCALPHA / INVSRCALPHA), which mixes the colour of
		// the transparent texels next to the visible ones into the edges (dark rims around the icons' outlines): each
		// transparent texel gets its visible neighbours' colour (alpha weighted), then those one texel further out get
		// theirs; the alpha stays 0
		void bleed_alpha(std::string& pixels, const unsigned int w, const unsigned int h)
		{
			auto* p = reinterpret_cast<unsigned char*>(pixels.data());
			std::vector<unsigned char> coloured(w * h);
			for (unsigned int i = 0; i < w * h; ++i) coloured[i] = p[i * 4 + 3] != 0;

			for (auto pass = 0; pass < 2; ++pass)
			{
				// read from the texels as they were before this pass
				const std::string source(pixels.data(), w * h * 4);
				const auto* s = reinterpret_cast<const unsigned char*>(source.data());
				const auto had = coloured;
				auto changed = false;

				for (unsigned int y = 0; y < h; ++y)
				{
					for (unsigned int x = 0; x < w; ++x)
					{
						if (had[y * w + x]) continue;

						unsigned int sum[3]{}, weight = 0;
						for (auto ny = y ? y - 1 : 0u; ny <= y + 1 && ny < h; ++ny)
						{
							for (auto nx = x ? x - 1 : 0u; nx <= x + 1 && nx < w; ++nx)
							{
								if (!had[ny * w + nx]) continue; // (also the texel itself)

								// a visible texel by its alpha, one coloured in the pass before as 1
								const auto* n = s + (ny * w + nx) * 4;
								const unsigned int a = n[3] ? n[3] : 1;
								for (auto c = 0; c < 3; ++c) sum[c] += n[c] * a;
								weight += a;
							}
						}
						if (!weight) continue;

						auto* q = p + (y * w + x) * 4;
						for (auto c = 0; c < 3; ++c) q[c] = static_cast<unsigned char>(sum[c] / weight);
						coloured[y * w + x] = 1;
						changed = true;
					}
				}
				if (!changed) break;
			}
		}

		GfxImage* load_image(const std::string& name)
		{
			const auto itr = images.find(name);
			if (itr != images.end()) return itr->second;
			images[name] = nullptr;

			std::string data;
			if (!utils::io::read_file(image_file(name), &data) || data.size() < 12 || data.compare(0, 4, "PS3Z") != 0)
			{
				return nullptr;
			}

			const auto w = *reinterpret_cast<const unsigned int*>(data.data() + 4);
			const auto h = *reinterpret_cast<const unsigned int*>(data.data() + 8);
			auto pixels = utils::compression::zlib::decompress(data.substr(12));
			if (!w || !h || w > 8192 || h > 8192 || pixels.size() < w * h * 4) return nullptr;

			auto* dev = device();
			const auto* white = static_cast<Material*>(find_loaded(ASSET_TYPE_MATERIAL, "white"));
			if (!dev || !white) return nullptr;

			IDirect3DTexture9* texture = nullptr;
			if (FAILED(dev->CreateTexture(w, h, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)) || !texture)
			{
				return nullptr;
			}
			bleed_alpha(pixels, w, h);
			upload(texture, w, h, std::move(pixels));

			auto* image = make<GfxImage>();
			*image = *white->textureTable[0].image;
			image->texture = texture;
			image->width = static_cast<unsigned short>(w);
			image->height = static_cast<unsigned short>(h);
			image->depth = 1;
			image->cardMemory[0] = image->cardMemory[1] = static_cast<int>(w * h * 4);
			image->name = copy_string("ps3style_" + name);
			images[name] = image;
			return image;
		}

		// a copy of a material with its first texture replaced
		Material* copy_material(const Material* source, GfxImage* image, const std::string& name)
		{
			if (!source || !source->textureTable || !source->textureCount) return nullptr;

			auto* table = make<MaterialTextureDef>(source->textureCount);
			std::memcpy(table, source->textureTable, sizeof(MaterialTextureDef) * source->textureCount);
			table[0].image = image;

			auto* material = make<Material>();
			*material = *source;
			material->textureTable = table;
			material->info.name = copy_string(name);
			return material;
		}

		std::string lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}
	}

	void init(const std::string& folder, const bool xbox, const std::string& fonts_suffix)
	{
		std::lock_guard _(mutex);
		root = folder;
		font_suffix = fonts_suffix;
		xbox_icons = xbox;
		image_names.clear();
		image_sizes.clear();

		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(folder + "\\images", ec))
		{
			if (entry.path().extension() == ".ps3z") image_names.emplace(lower(entry.path().stem().string()));
		}
	}

	void reset()
	{
		std::lock_guard _(mutex);
		// the textures belong to the old device and the copied materials point into the old zones' techniques: all
		// made again on the next use (the old ones are left alone, their device and zones are gone)
		drop_cached();
		made_with_white = made_with_texture = nullptr;
	}

	namespace
	{
		// a renderer restart that came another way (or a lookup between reset() and the old zones going): the "white"
		// material the copies were made from is not the loaded one any more -> everything made again
		void check_renderer()
		{
			const auto* white = static_cast<Material*>(find_loaded(ASSET_TYPE_MATERIAL, "white"));
			const void* texture = white && white->textureTable && white->textureTable[0].image
				                      ? white->textureTable[0].image->texture
				                      : nullptr;
			if (white == made_with_white && texture == made_with_texture) return;
			drop_cached();
			made_with_white = white;
			made_with_texture = texture;
		}
	}

	bool xbox_buttons()
	{
		// set once at start (init), read while drawing
		return xbox_icons;
	}

	void* find_loaded(const int type, const char* name)
	{
		// the asset hash's reader / writer lock: readers @18596E4, writers @18596E8 (as DB_FindXAssetHeader)
		auto* readers = reinterpret_cast<volatile long*>(0x18596E4);
		auto* writers = reinterpret_cast<volatile long*>(0x18596E8);
		InterlockedIncrement(readers);
		while (*writers) Sleep(0);
		const auto* entry = static_cast<void**>(db_find_x_asset_entry(type, name));
		auto* header = entry ? entry[1] : nullptr;
		InterlockedDecrement(readers);
		return header;
	}

	__declspec(naked) void* find_dvar(const char* /*name*/)
	{
		__asm
		{
			push edi
			mov edi, [esp + 8]
			mov eax, 5BDC40h
			call eax
			pop edi
			retn
		}
	}

	bool has_image(const char* name)
	{
		std::lock_guard _(mutex);
		return name && image_names.count(lower(name));
	}

	Material* material(const char* name)
	{
		std::lock_guard _(mutex);
		if (!name || !image_names.count(lower(name))) return nullptr;

		check_renderer();
		const auto key = lower(name);
		const auto itr = materials.find(key);
		if (itr != materials.end()) return itr->second;

		Material* material = nullptr;
		if (auto* image = load_image(key))
		{
			material = copy_material(static_cast<Material*>(find_loaded(ASSET_TYPE_MATERIAL, "white")), image, name);

			// "white" samples point + wrap (samplerState 1): the scaled icons came out jagged. The PC's fonts and UI images
			// use E2h, linear (2) without mipmaps, clamped in u / v / w (E0h), so full-bleed pictures don't pull in the
			// opposite edge either (the copy's own texture table, not white's)
			if (material) material->textureTable[0].samplerState = 0xE2;
		}

		// not cached when the game wasn't ready (no device yet): asked again later
		if (material || device()) materials[key] = material;
		return material;
	}

	bool image_size(const char* name, int* width, int* height)
	{
		std::lock_guard _(mutex);
		if (!name || !width || !height) return false;

		const auto key = lower(name);
		auto itr = image_sizes.find(key);
		if (itr == image_sizes.end())
		{
			// the header only ("PS3Z", u32 width, u32 height): the text code asks before the renderer may be up
			std::pair<int, int> size{};
			if (image_names.count(key))
			{
				std::ifstream file(image_file(key), std::ios::binary);
				char header[12]{};
				if (file.read(header, sizeof(header)) && std::memcmp(header, "PS3Z", 4) == 0)
				{
					size = {*reinterpret_cast<const int*>(header + 4), *reinterpret_cast<const int*>(header + 8)};
				}
			}
			itr = image_sizes.emplace(key, size).first;
		}

		if (itr->second.first <= 0 || itr->second.second <= 0) return false;
		*width = itr->second.first;
		*height = itr->second.second;
		return true;
	}

	Font_s* font(const char* name)
	{
		std::lock_guard _(mutex);
		if (!name) return nullptr;

		check_renderer();
		const auto key = lower(name);
		const auto itr = fonts.find(key);
		if (itr != fonts.end()) return itr->second;
		if (key.rfind("fonts/", 0) != 0) return nullptr;

		Font_s* font = nullptr;
		std::string json;
		if (utils::io::read_file(root + "\\fonts\\" + key.substr(6) + font_suffix + ".json", &json))
		{
			rapidjson::Document def;
			def.Parse(json.data(), json.size());

			// the PC font of that name gives the material setup (font shader, glow)
			const auto* pc_font = static_cast<Font_s*>(find_loaded(ASSET_TYPE_FONT, name));
			if (!pc_font) pc_font = static_cast<Font_s*>(find_loaded(ASSET_TYPE_FONT, "fonts/normalfont"));

			auto* image = !def.HasParseError() && def.IsObject() && def.HasMember("image") && def["image"].IsString()
				              ? load_image(lower(def["image"].GetString()))
				              : nullptr;

			if (image && pc_font && def.HasMember("glyphs") && def["glyphs"].IsArray())
			{
				const auto& glyphs = def["glyphs"];
				font = make<Font_s>();
				font->fontName = copy_string(name);
				font->pixelHeight = def.HasMember("pixelHeight") ? def["pixelHeight"].GetInt() : 24;
				font->material = copy_material(pc_font->material, image, "fonts/ps3_" + key.substr(6) + font_suffix);
				font->glowMaterial = copy_material(pc_font->glowMaterial, image, "fonts/ps3_" + key.substr(6) + font_suffix + "_glow");
				font->glyphs = make<Glyph>(glyphs.Size());
				for (rapidjson::SizeType i = 0; i < glyphs.Size(); ++i)
				{
					const auto& g = glyphs[i];
					auto& out = font->glyphs[font->glyphCount++];
					out.letter = static_cast<unsigned short>(g[0].GetInt());
					out.x0 = static_cast<char>(g[1].GetInt());
					out.y0 = static_cast<char>(g[2].GetInt());
					out.dx = static_cast<unsigned char>(g[3].GetInt());
					out.pixelWidth = static_cast<unsigned char>(g[4].GetInt());
					out.pixelHeight = static_cast<unsigned char>(g[5].GetInt());
					out.s0 = static_cast<float>(g[6].GetDouble());
					out.t0 = static_cast<float>(g[7].GetDouble());
					out.s1 = static_cast<float>(g[8].GetDouble());
					out.t1 = static_cast<float>(g[9].GetDouble());
				}

				if (!font->material || !font->glowMaterial) font = nullptr;
			}
		}

		if (font || device()) fonts[key] = font;
		return font;
	}

	// ---- frame capture (development: tests while the game window is in the background) ----

	namespace
	{
		std::mutex capture_mutex;
		std::string capture_path; // the next frame goes there
		using present_t = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
		using swap_present_t = HRESULT(__stdcall*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
		present_t present_original = nullptr;
		swap_present_t swap_present_original = nullptr;
		IDirect3DDevice9* capture_device = nullptr;

		// the back buffer as a top-down 32-bit BMP (resolved first when it is multisampled)
		void save_back_buffer(IDirect3DDevice9* dev, const std::string& path)
		{
			IDirect3DSurface9* back = nullptr;
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;

			D3DSURFACE_DESC desc{};
			back->GetDesc(&desc);
			IDirect3DSurface9* source = back;
			IDirect3DSurface9* resolved = nullptr;
			if (desc.MultiSampleType != D3DMULTISAMPLE_NONE
				&& SUCCEEDED(dev->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr))
				&& SUCCEEDED(dev->StretchRect(back, nullptr, resolved, nullptr, D3DTEXF_NONE)))
			{
				source = resolved;
			}

			IDirect3DSurface9* copy = nullptr;
			if (SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr))
				&& SUCCEEDED(dev->GetRenderTargetData(source, copy)))
			{
				D3DLOCKED_RECT locked{};
				if (SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
				{
					const auto w = desc.Width, h = desc.Height;
					std::string bmp(54 + w * h * 4, '\0');
					auto* b = reinterpret_cast<unsigned char*>(bmp.data());
					const auto put = [&](const size_t at, const unsigned int v, const int bytes)
					{
						for (auto i = 0; i < bytes; ++i) b[at + i] = static_cast<unsigned char>(v >> (8 * i));
					};
					b[0] = 'B';
					b[1] = 'M';
					put(2, static_cast<unsigned int>(bmp.size()), 4);
					put(10, 54, 4);
					put(14, 40, 4);
					put(18, w, 4);
					put(22, static_cast<unsigned int>(-static_cast<int>(h)), 4); // top-down
					put(26, 1, 2);
					put(28, 32, 2);
					for (unsigned int y = 0; y < h; ++y)
					{
						auto* row = b + 54 + y * w * 4;
						std::memcpy(row, static_cast<const unsigned char*>(locked.pBits) + y * locked.Pitch, w * 4);
						for (unsigned int x = 0; x < w; ++x) row[x * 4 + 3] = 255; // X8R8G8B8
					}
					copy->UnlockRect();
					utils::io::write_file(path, bmp);
				}
			}

			if (copy) copy->Release();
			if (resolved) resolved->Release();
			back->Release();
		}

		void capture_pending(IDirect3DDevice9* dev)
		{
			std::string path;
			{
				std::lock_guard _(capture_mutex);
				path.swap(capture_path);
			}
			if (!path.empty()) save_back_buffer(dev, path);
		}

		HRESULT __stdcall present_stub(IDirect3DDevice9* dev, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty)
		{
			capture_pending(dev);
			return present_original(dev, source, dest, window, dirty);
		}

		// the game presents through the window's swap chain (IDirect3DSwapChain9::Present), not the device
		HRESULT __stdcall swap_present_stub(IDirect3DSwapChain9* chain, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty,
		                                    const DWORD flags)
		{
			capture_pending(capture_device);
			return swap_present_original(chain, source, dest, window, dirty, flags);
		}

		template <typename T>
		T patch_vtable(void* object, const int slot, void* stub)
		{
			auto** entry = *static_cast<void***>(object) + slot;
			DWORD protect = 0;
			if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &protect)) return nullptr;
			auto* original = *entry;
			*entry = stub;
			VirtualProtect(entry, sizeof(void*), protect, &protect);
			return reinterpret_cast<T>(original);
		}
	}

	bool capture_frame(const std::string& path)
	{
		auto* dev = device();
		if (!dev) return false;

		// IDirect3DDevice9::Present (vtable slot 17) and IDirect3DSwapChain9::Present (slot 3), patched once in d3d9.dll's
		// vtables
		if (!present_original)
		{
			capture_device = dev;
			present_original = patch_vtable<present_t>(dev, 17, reinterpret_cast<void*>(present_stub));
			IDirect3DSwapChain9* chain = nullptr;
			if (SUCCEEDED(dev->GetSwapChain(0, &chain)) && chain)
			{
				swap_present_original = patch_vtable<swap_present_t>(chain, 3, reinterpret_cast<void*>(swap_present_stub));
				chain->Release();
			}
			if (!present_original) return false;
		}

		std::lock_guard _(capture_mutex);
		capture_path = path;
		return true;
	}
}
