#include <std_include.hpp>
#include "ps3style_menus.hpp"
#include "ps3style_assets.hpp"
#include "utils/io.hpp"

#include <rapidjson/document.h>
#include <unordered_set>

using namespace game::native::ui;

namespace ps3style_menus
{
	namespace
	{
		constexpr DWORD db_find_x_asset_header_addr = 0x4CA620; // hooked: PS3 images apply to menu materials
		constexpr DWORD dvar_find_var_addr = 0x5BDC40; // dvar_t* (edi = name)

		__declspec(naked) void* dvar_find_var(const char* /*name*/)
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

		void* find_asset(const int type, const char* name)
		{
			return reinterpret_cast<void*(*)(int, const char*, int)>(db_find_x_asset_header_addr)(type, name, 1);
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

		// an asset that is loaded, without waiting for the loader: DB_FindXAssetHeader waits while zones load, and the
		// loader needs the main thread, which is the one building the menus
		void* find_loaded_asset(const int type, const char* name)
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

		// asset names the PS3 menus use that the PC game doesn't have loaded (ps3style_menuReport)
		std::set<std::string> missing_assets;

		// zeroed memory that lives as long as the game (the game keeps pointers into its menus)
		class arena
		{
		public:
			void* alloc(size_t size)
			{
				size = (size + 7) & ~7u;
				if (this->used_ + size > this->capacity_)
				{
					const auto capacity = std::max<size_t>(size, 1 << 20);
					this->block_ = static_cast<char*>(calloc(1, capacity));
					this->used_ = 0;
					this->capacity_ = capacity;
				}

				auto* data = this->block_ + this->used_;
				this->used_ += size;
				return data;
			}

			template <typename T>
			T* make(const size_t count = 1)
			{
				// a little more than asked: room for fields the PC structs might have past the known ones
				return static_cast<T*>(this->alloc(sizeof(T) * count + 16));
			}

		private:
			char* block_ = nullptr;
			size_t used_ = 0;
			size_t capacity_ = 0;
		};

		using value = rapidjson::Value;

		std::recursive_mutex mutex;
		std::unique_ptr<rapidjson::Document> document;
		std::unique_ptr<arena> memory;
		std::unordered_map<std::string, const char*> strings;
		std::unordered_map<std::string, menuDef_t*> menus;
		std::unordered_map<std::string, MenuList*> lists;

		const char* str(const value& v)
		{
			if (!v.IsString()) return nullptr;

			const std::string s(v.GetString(), v.GetStringLength());
			const auto itr = strings.find(s);
			if (itr != strings.end()) return itr->second;

			auto* copy = static_cast<char*>(memory->alloc(s.size() + 1));
			std::memcpy(copy, s.data(), s.size());
			strings[s] = copy;
			return copy;
		}

		const value& member(const value& object, const char* key)
		{
			static const value null_value;
			if (!object.IsObject()) return null_value;
			const auto itr = object.FindMember(key);
			return itr == object.MemberEnd() ? null_value : itr->value;
		}

		const char* str(const value& object, const char* key)
		{
			return str(member(object, key));
		}

		int integer(const value& object, const char* key, const int fallback = 0)
		{
			const auto& v = member(object, key);
			if (v.IsInt()) return v.GetInt();
			if (v.IsUint()) return static_cast<int>(v.GetUint());
			if (v.IsNumber()) return static_cast<int>(v.GetDouble());
			if (v.IsBool()) return v.GetBool() ? 1 : 0;
			return fallback;
		}

		float number(const value& object, const char* key, const float fallback = 0.0f)
		{
			const auto& v = member(object, key);
			return v.IsNumber() ? static_cast<float>(v.GetDouble()) : fallback;
		}

		void floats(const value& object, const char* key, float* out, const int count)
		{
			const auto& v = member(object, key);
			if (!v.IsArray()) return;
			for (rapidjson::SizeType i = 0; i < v.Size() && static_cast<int>(i) < count; ++i)
			{
				if (v[i].IsNumber()) out[i] = static_cast<float>(v[i].GetDouble());
			}
		}

		Material* material(const char* name, const bool report = true)
		{
			if (!name || !*name) return nullptr;

			// a PS3 image of that name comes first (ps3style\images), then what the game has loaded
			if (ps3style_assets::has_image(name))
			{
				if (auto* m = ps3style_assets::material(name)) return m;
			}
			if (auto* m = find_loaded_asset(ASSET_TYPE_MATERIAL, name)) return static_cast<Material*>(m);
			if (report) missing_assets.emplace(std::string("material ") + name);
			return nullptr;
		}

		// the built menus' pointers to assets of the game's zones (materials, focus sounds): the zones are freed and
		// loaded again (the UI zones when a level starts / ends, the map's zones per level), so refresh() looks them up
		// again; a pointer kept from before draws whatever is at that address now (checkered / stretched pictures)
		struct asset_slot
		{
			void** slot;
			int type;
			const char* name; // one of strings (the same name is the same pointer)
		};

		std::vector<asset_slot> asset_slots;

		void set_material(Material** slot, const char* name)
		{
			*slot = material(name);
			if (name && *name) asset_slots.push_back({reinterpret_cast<void**>(slot), ASSET_TYPE_MATERIAL, name});
		}

		void rect(const value& v, rectDef_s* out)
		{
			if (!v.IsArray() || v.Size() < 6) return;
			out->x = static_cast<float>(v[0].GetDouble());
			out->y = static_cast<float>(v[1].GetDouble());
			out->w = static_cast<float>(v[2].GetDouble());
			out->h = static_cast<float>(v[3].GetDouble());
			out->horzAlign = static_cast<unsigned char>(v[4].GetInt());
			out->vertAlign = static_cast<unsigned char>(v[5].GetInt());
		}

		// a statement: an index into the file's shared "statements" table, whose entries are operators (numbers),
		// strings, {"i": int}, {"f": float} or {"fn": statement index}; every use gets its own copy (the engine
		// caches a statement's last result)
		Statement_s* statement(const value& v, ExpressionSupportingData* sd, const int depth = 0)
		{
			if (!v.IsInt() || depth > 32) return nullptr;

			const auto& table = member(*document, "statements");
			const auto index = v.GetInt();
			if (!table.IsArray() || index < 0 || static_cast<rapidjson::SizeType>(index) >= table.Size()) return nullptr;
			const auto& entries = table[index];
			if (!entries.IsArray()) return nullptr;

			auto* st = memory->make<Statement_s>();
			st->numEntries = static_cast<int>(entries.Size());
			st->entries = memory->make<expressionEntry>(entries.Size());
			st->supportingData = sd;

			for (rapidjson::SizeType i = 0; i < entries.Size(); ++i)
			{
				const auto& e = entries[i];
				auto& out = st->entries[i];

				if (e.IsInt())
				{
					out.type = 0;
					out.data.op = e.GetInt();
					continue;
				}

				out.type = 1;
				if (e.IsString())
				{
					out.data.operand.dataType = VAL_STRING;
					out.data.operand.internals.stringVal.string = str(e);
				}
				else if (e.HasMember("i"))
				{
					out.data.operand.dataType = VAL_INT;
					out.data.operand.internals.intVal = integer(e, "i");
				}
				else if (e.HasMember("f"))
				{
					out.data.operand.dataType = VAL_FLOAT;
					out.data.operand.internals.floatVal = number(e, "f");
				}
				else if (e.HasMember("fn"))
				{
					out.data.operand.dataType = VAL_FUNCTION;
					out.data.operand.internals.function = statement(e["fn"], sd, depth + 1);
				}
			}

			return st;
		}

		// where each built statement is used ("menu / item n / key"), for crash reports
		std::string building;
		std::unordered_map<const void*, std::string> statement_names;

		Statement_s* statement(const value& object, const char* key, ExpressionSupportingData* sd)
		{
			auto* st = statement(member(object, key), sd);
			if (st) statement_names[st] = building + " " + key;
			return st;
		}

		MenuEventHandlerSet* handler_set(const value& v, ExpressionSupportingData* sd, const int depth = 0)
		{
			if (!v.IsArray() || v.Empty() || depth > 16) return nullptr;

			auto* set = memory->make<MenuEventHandlerSet>();
			set->eventHandlers = memory->make<MenuEventHandler*>(v.Size());

			for (rapidjson::SizeType i = 0; i < v.Size(); ++i)
			{
				const auto& h = v[i];
				if (!h.IsArray() || h.Empty() || !h[0].IsString()) continue;

				const std::string kind = h[0].GetString();
				auto* handler = memory->make<MenuEventHandler>();

				if (kind == "script")
				{
					handler->eventType = EVENT_UNCONDITIONAL;
					handler->eventData.unconditionalScript = h.Size() > 1 ? str(h[1]) : nullptr;
					if (!handler->eventData.unconditionalScript) continue;
				}
				else if (kind == "if" && h.Size() > 2)
				{
					auto* cs = memory->make<ConditionalScript>();
					cs->eventExpression = statement(h[1], sd);
					cs->eventHandlerSet = handler_set(h[2], sd, depth + 1);
					handler->eventType = EVENT_IF;
					handler->eventData.conditionalScript = cs;
				}
				else if (kind == "else" && h.Size() > 1)
				{
					handler->eventType = EVENT_ELSE;
					handler->eventData.elseScript = handler_set(h[1], sd, depth + 1);
				}
				else if (kind.rfind("setvar", 0) == 0 && h.Size() > 2)
				{
					auto* data = memory->make<SetLocalVarData>();
					data->localVarName = str(h[1]);
					data->expression = statement(h[2], sd);
					handler->eventType = static_cast<unsigned char>(std::atoi(kind.data() + 6));
					handler->eventData.setLocalVarData = data;
				}
				else
				{
					continue;
				}

				set->eventHandlers[set->eventHandlerCount++] = handler;
			}

			return set->eventHandlerCount ? set : nullptr;
		}

		MenuEventHandlerSet* handler_set(const value& object, const char* key, ExpressionSupportingData* sd)
		{
			return handler_set(member(object, key), sd);
		}

		ItemKeyHandler* key_handlers(const value& v, ExpressionSupportingData* sd)
		{
			if (!v.IsArray()) return nullptr;

			ItemKeyHandler* first = nullptr;
			ItemKeyHandler* last = nullptr;
			for (rapidjson::SizeType i = 0; i < v.Size(); ++i)
			{
				const auto& k = v[i];
				if (!k.IsArray() || k.Size() < 2) continue;

				auto* handler = memory->make<ItemKeyHandler>();
				handler->key = k[0].GetInt();
				handler->action = handler_set(k[1], sd);
				if (last) last->next = handler;
				else first = handler;
				last = handler;
			}

			return first;
		}

		void window(const value& v, windowDef_t* w)
		{
			w->name = str(v, "name");
			rect(member(v, "rect"), &w->rect);
			rect(member(v, "rectClient"), &w->rectClient);
			w->group = str(v, "group");
			w->style = integer(v, "style");
			w->border = integer(v, "border");
			w->ownerDraw = integer(v, "ownerDraw");
			w->ownerDrawFlags = integer(v, "ownerDrawFlags");
			w->borderSize = number(v, "borderSize");
			w->staticFlags = integer(v, "staticFlags");
			w->dynamicFlags[0] = integer(v, "dynamicFlags");
			floats(v, "foreColor", w->foreColor, 4);
			floats(v, "backColor", w->backColor, 4);
			floats(v, "borderColor", w->borderColor, 4);
			floats(v, "outlineColor", w->outlineColor, 4);
			floats(v, "disableColor", w->disableColor, 4);
			set_material(&w->background, str(v, "background"));
		}

		void type_data(const value& v, itemDef_s* item, ExpressionSupportingData* sd)
		{
			const auto& t = member(v, "typeData");
			if (!t.IsObject()) return;

			const std::string kind = str(t, "kind") ? str(t, "kind") : "";
			if (kind == "listBox")
			{
				auto* lb = memory->make<listBoxDef_s>();
				lb->drawPadding = integer(t, "drawPadding");
				lb->elementWidth = number(t, "elementWidth");
				lb->elementHeight = number(t, "elementHeight");
				lb->elementStyle = integer(t, "elementStyle");

				const auto& columns = member(t, "columns");
				if (columns.IsArray())
				{
					for (rapidjson::SizeType i = 0; i < columns.Size() && i < 16; ++i)
					{
						const auto& c = columns[i];
						auto& out = lb->columnInfo[lb->numColumns++];
						if (c.Size() >= 6)
						{
							out = {c[0].GetInt(), c[1].GetInt(), c[2].GetInt(), c[3].GetInt(), c[4].GetInt(), c[5].GetInt()};
						}
						else if (c.Size() >= 4)
						{
							out = {c[0].GetInt(), 0, c[1].GetInt(), 0, c[2].GetInt(), c[3].GetInt()};
						}
					}
				}

				lb->onDoubleClick = handler_set(t, "onDoubleClick", sd);
				lb->notselectable = integer(t, "notselectable");
				lb->noScrollBars = integer(t, "noScrollBars");
				lb->usePaging = integer(t, "usePaging");
				floats(t, "selectBorder", lb->selectBorder, 4);
				set_material(&lb->selectIcon, str(t, "selectIcon"));
				lb->elementHeightExp = statement(t, "elementHeightExp", sd);
				item->typeData.listBox = lb;
			}
			else if (kind == "multi")
			{
				auto* multi = memory->make<multiDef_s>();
				multi->strDef = integer(t, "strDef");
				const auto& names = member(t, "names");
				const auto& values = member(t, "values");
				if (names.IsArray())
				{
					for (rapidjson::SizeType i = 0; i < names.Size() && i < 32; ++i)
					{
						multi->dvarList[i] = str(names[i]);
						if (values.IsArray() && i < values.Size())
						{
							if (multi->strDef) multi->dvarStr[i] = str(values[i]);
							else if (values[i].IsNumber()) multi->dvarValue[i] = static_cast<float>(values[i].GetDouble());
						}
						multi->count = static_cast<int>(i) + 1;
					}
				}
				item->typeData.multi = multi;
			}
			else if (kind == "enumDvar")
			{
				item->typeData.enumDvarName = str(t, "dvar");
			}
			else if (kind == "newsTicker")
			{
				auto* ticker = memory->make<newsTickerDef_s>();
				ticker->feedId = integer(t, "feedId");
				ticker->speed = integer(t, "speed");
				ticker->spacing = integer(t, "spacing");
				item->typeData.ticker = ticker;
			}
			else if (kind == "textScroll")
			{
				item->typeData.scroll = memory->make<textScrollDef_s>();
			}
			else if (kind == "editField")
			{
				// the same words as on PS3 (the reader turned them into native order)
				auto* words = static_cast<unsigned int*>(memory->alloc(64));
				const auto& raw = member(t, "raw");
				if (raw.IsArray())
				{
					for (rapidjson::SizeType i = 0; i < raw.Size() && i < 9; ++i) words[i] = raw[i].GetUint();
				}
				item->typeData.data = words;
			}
		}

		itemDef_s* item(const value& v, menuDef_t* parent, ExpressionSupportingData* sd)
		{
			auto* item = memory->make<itemDef_s>();
			window(v, &item->window);
			rect(member(v, "textRect"), &item->textRect[0]);
			item->type = integer(v, "type");
			item->dataType = integer(v, "dataType");
			item->alignment = integer(v, "alignment");
			item->fontEnum = integer(v, "fontEnum");
			item->textAlignMode = integer(v, "textAlignMode");
			item->textalignx = number(v, "textalignx");
			item->textaligny = number(v, "textaligny");
			item->textscale = number(v, "textscale");
			item->textStyle = integer(v, "textStyle");
			item->gameMsgWindowIndex = integer(v, "gameMsgWindowIndex");
			item->gameMsgWindowMode = integer(v, "gameMsgWindowMode");
			item->text = str(v, "text");
			item->itemFlags = integer(v, "itemFlags");
			item->parent = parent;
			item->mouseEnterText = handler_set(v, "mouseEnterText", sd);
			item->mouseExitText = handler_set(v, "mouseExitText", sd);
			item->mouseEnter = handler_set(v, "mouseEnter", sd);
			item->mouseExit = handler_set(v, "mouseExit", sd);
			item->action = handler_set(v, "action", sd);
			item->accept = handler_set(v, "accept", sd);
			item->onFocus = handler_set(v, "onFocus", sd);
			item->hasFocus = handler_set(v, "hasFocus", sd);
			item->leaveFocus = handler_set(v, "leaveFocus", sd);
			item->dvar = str(v, "dvar");
			item->dvarTest = str(v, "dvarTest");
			item->onKey = key_handlers(member(v, "onKey"), sd);
			item->enableDvar = str(v, "enableDvar");
			item->localVar = str(v, "localVar");
			item->dvarFlags = integer(v, "dvarFlags");
			if (const auto* sound = str(v, "focusSound"))
			{
				item->focusSound = find_loaded_asset(ASSET_TYPE_SOUND, sound);
				if (!item->focusSound) missing_assets.emplace(std::string("sound ") + sound);
				asset_slots.push_back({&item->focusSound, ASSET_TYPE_SOUND, sound});
			}
			item->special = number(v, "special");
			type_data(v, item, sd);

			const auto& fe = member(v, "floatExpressions");
			if (fe.IsArray() && !fe.Empty())
			{
				item->floatExpressions = memory->make<ItemFloatExpression>(fe.Size());
				for (rapidjson::SizeType i = 0; i < fe.Size(); ++i)
				{
					if (!fe[i].IsArray() || fe[i].Size() < 2) continue;
					auto& out = item->floatExpressions[item->floatExpressionCount++];
					out.target = fe[i][0].GetInt();
					out.expression = statement(fe[i][1], sd);
				}
			}

			item->visibleExp = statement(v, "visibleExp", sd);
			item->disabledExp = statement(v, "disabledExp", sd);
			item->textExp = statement(v, "textExp", sd);
			item->materialExp = statement(v, "materialExp", sd);
			item->textAlignYExp = statement(v, "textAlignYExp", sd);
			floats(v, "glowColor", item->glowColor, 4);
			return item;
		}

		menuDef_t* build_menu(const value& v)
		{
			auto* menu = memory->make<menuDef_t>();
			auto* data = memory->make<menuData_t>();
			menu->data = data;

			// one supporting data per menu: the static dvars its expressions index, its UI strings
			auto* sd = memory->make<ExpressionSupportingData>();
			const auto& static_dvars = member(v, "staticDvars");
			if (static_dvars.IsArray() && !static_dvars.Empty())
			{
				sd->staticDvars = memory->make<StaticDvar*>(static_dvars.Size());
				for (rapidjson::SizeType i = 0; i < static_dvars.Size(); ++i)
				{
					auto* sdv = memory->make<StaticDvar>();
					sdv->dvarName = const_cast<char*>(str(static_dvars[i]));
					sdv->dvar = sdv->dvarName ? dvar_find_var(sdv->dvarName) : nullptr;
					if (!sdv->dvar && sdv->dvarName)
					{
						// a dvar the PC game doesn't have (console / dev ones): created empty, as "set" would (the
						// expressions read the dvar without checking it)
						reinterpret_cast<void(*)(const char*, const char*)>(0x5BF740)(sdv->dvarName, "");
						sdv->dvar = dvar_find_var(sdv->dvarName);
						missing_assets.emplace(std::string("dvar ") + sdv->dvarName);
					}
					sd->staticDvars[sd->numStaticDvars++] = sdv;
				}
			}

			const auto& string_tables = member(*document, "stringTables");
			const auto& ui_strings_index = member(v, "uiStrings");
			static const value no_strings(rapidjson::kArrayType);
			const auto& ui_strings = ui_strings_index.IsInt() && string_tables.IsArray()
				                         && static_cast<rapidjson::SizeType>(ui_strings_index.GetInt()) < string_tables.Size()
				                         ? string_tables[ui_strings_index.GetInt()]
				                         : no_strings;
			if (ui_strings.IsArray() && !ui_strings.Empty())
			{
				sd->strings = memory->make<const char*>(ui_strings.Size());
				for (rapidjson::SizeType i = 0; i < ui_strings.Size(); ++i)
				{
					sd->strings[sd->totalStrings++] = str(ui_strings[i]) ? str(ui_strings[i]) : "";
				}
			}
			data->expressionData = sd;

			window(v, &menu->window);
			data->fullScreen = integer(v, "fullScreen");
			data->fadeCycle = integer(v, "fadeCycle");
			data->fadeClamp = number(v, "fadeClamp");
			data->fadeAmount = number(v, "fadeAmount");
			data->fadeInAmount = number(v, "fadeInAmount");
			data->blurRadius = number(v, "blurRadius");
			data->onOpen = handler_set(v, "onOpen", sd);
			data->onCloseRequest = handler_set(v, "onCloseRequest", sd);
			data->onClose = handler_set(v, "onClose", sd);
			data->onESC = handler_set(v, "onESC", sd);
			data->onFocusDueToClose = handler_set(v, "onFocusDueToClose", sd);
			data->onKey = key_handlers(member(v, "onKey"), sd);
			data->visibleExp = statement(v, "visibleExp", sd);
			data->allowedBinding = str(v, "allowedBinding");
			data->soundName = str(v, "soundName");
			floats(v, "focusColor", data->focusColor, 4);
			data->rectXExp = statement(v, "rectXExp", sd);
			data->rectYExp = statement(v, "rectYExp", sd);
			data->rectWExp = statement(v, "rectWExp", sd);
			data->rectHExp = statement(v, "rectHExp", sd);
			data->openSoundExp = statement(v, "openSoundExp", sd);
			data->closeSoundExp = statement(v, "closeSoundExp", sd);
			data->soundLoopExp = statement(v, "soundLoopExp", sd);
			data->cursorItem[0] = -1;
			data->priority = static_cast<unsigned char>(integer(v, "priority"));

			const auto& items = member(v, "items");
			const auto menu_name = building;
			if (items.IsArray() && !items.Empty())
			{
				menu->items = memory->make<itemDef_s*>(items.Size());
				for (rapidjson::SizeType i = 0; i < items.Size(); ++i)
				{
					if (!items[i].IsObject()) continue;
					building = menu_name + " item " + std::to_string(i);
					auto* built = item(items[i], menu, sd);
					statement_names[built] = building;
					menu->items[menu->itemCount++] = built;
				}
			}
			building = menu_name;

			return menu;
		}

		// set_xbox: kept when the file is read again (reload)
		bool xbox_menus = false;

		const value* menu_value(const char* name)
		{
			if (!document || !name) return nullptr;

			// the Xbox 360 controller's menus (STICK / BUTTON LAYOUT, their in-game copies, the theater's controls) when
			// tools\make_menus.py had its pictures, and only while they are there; else the PS3 ones
			if (xbox_menus && ps3style_assets::has_image("xenon_controller_top")
				&& ps3style_assets::has_image("xenon_controller_lines_classic_mp")
				&& ps3style_assets::has_image("xenon_controller_lines_demo"))
			{
				const auto& xbox = member(member(*document, "menus_xbox"), name);
				if (xbox.IsObject()) return &xbox;
			}

			const auto& menus_value = member(*document, "menus");
			const auto& v = member(menus_value, name);
			return v.IsObject() ? &v : nullptr;
		}
	}

	void set_xbox(const bool xbox)
	{
		std::lock_guard _(mutex);
		xbox_menus = xbox;
	}

	bool load(const std::string& path)
	{
		std::lock_guard _(mutex);

		std::string data;
		if (!utils::io::read_file(path, &data)) return false;

		auto doc = std::make_unique<rapidjson::Document>();
		doc->Parse(data.data(), data.size());
		if (doc->HasParseError() || !doc->IsObject()) return false;

		document = std::move(doc);
		memory = std::make_unique<arena>();
		strings.clear();
		menus.clear();
		lists.clear();
		asset_slots.clear();
		return true;
	}

	bool loaded()
	{
		std::lock_guard _(mutex);
		return document != nullptr;
	}

	menuDef_t* find_menu(const char* name)
	{
		std::lock_guard _(mutex);
		if (!name) return nullptr;

		const auto itr = menus.find(name);
		if (itr != menus.end()) return itr->second;

		const auto* v = menu_value(name);
		building = name;
		auto* menu = v ? build_menu(*v) : nullptr;
		if (menu) statement_names[menu] = name;
		menus[name] = menu;
		return menu;
	}

	MenuList* find_list(const char* name, const MenuList* pc_list)
	{
		std::lock_guard _(mutex);
		if (!document || !name) return nullptr;

		const auto itr = lists.find(name);
		if (itr != lists.end()) return itr->second;

		const auto& names = member(member(*document, "lists"), name);
		if (!names.IsArray())
		{
			// a PC list the PS3 doesn't have (e.g. the PC loads every script menu as its own list
			// "ui_mp/scriptmenus/<menu>.menu"): its menus, each the PS3 one when there is one
			MenuList* list = nullptr;
			if (pc_list && pc_list->menuCount > 0)
			{
				auto replaced = false;
				std::vector<menuDef_t*> result;
				for (auto i = 0; i < pc_list->menuCount; ++i)
				{
					auto* menu = pc_list->menus[i];
					auto* ps3 = menu && menu->window.name ? find_menu(menu->window.name) : nullptr;
					replaced |= ps3 != nullptr;
					result.push_back(ps3 ? ps3 : menu);
				}

				if (replaced)
				{
					list = memory->make<MenuList>();
					list->name = pc_list->name;
					list->menuCount = static_cast<int>(result.size());
					list->menus = memory->make<menuDef_t*>(result.size());
					std::copy(result.begin(), result.end(), list->menus);
				}
			}

			lists[name] = list;
			return list;
		}

		std::vector<menuDef_t*> result;
		std::unordered_set<std::string> have;
		for (rapidjson::SizeType i = 0; i < names.Size(); ++i)
		{
			if (!names[i].IsString()) continue;
			if (auto* menu = find_menu(names[i].GetString()))
			{
				result.push_back(menu);
				have.emplace(names[i].GetString());
			}
		}

		if (pc_list)
		{
			for (auto i = 0; i < pc_list->menuCount; ++i)
			{
				auto* menu = pc_list->menus[i];
				if (menu && menu->window.name && !have.count(menu->window.name)) result.push_back(menu);
			}
		}

		auto* list = memory->make<MenuList>();
		list->name = str(rapidjson::Value(name, static_cast<rapidjson::SizeType>(strlen(name))));
		list->menuCount = static_cast<int>(result.size());
		list->menus = memory->make<menuDef_t*>(result.size());
		std::copy(result.begin(), result.end(), list->menus);
		lists[name] = list;
		return list;
	}

	void build_all()
	{
		std::lock_guard _(mutex);
		if (!document) return;

		const auto& all = member(*document, "menus");
		if (!all.IsObject()) return;
		for (auto itr = all.MemberBegin(); itr != all.MemberEnd(); ++itr)
		{
			find_menu(itr->name.GetString());
		}
	}

	std::string describe(const void* statement)
	{
		const auto itr = statement_names.find(statement);
		return itr == statement_names.end() ? "?" : itr->second;
	}

	std::vector<std::string> missing()
	{
		std::lock_guard _(mutex);
		return {missing_assets.begin(), missing_assets.end()};
	}

	void refresh()
	{
		std::lock_guard _(mutex);

		// once per name (the slots share their name pointers)
		std::unordered_map<const char*, void*> found[2];
		for (auto& s : asset_slots)
		{
			const auto sound = s.type == ASSET_TYPE_SOUND;
			auto& cache = found[sound ? 1 : 0];
			auto itr = cache.find(s.name);
			if (itr == cache.end())
			{
				itr = cache.emplace(s.name, sound ? find_loaded_asset(ASSET_TYPE_SOUND, s.name) : material(s.name, false)).first;
			}
			*s.slot = itr->second;
		}

		// the built lists hold the PC lists' own menus, which went with their zone: made again when asked for
		lists.clear();
	}

	void reload(const std::string& path)
	{
		std::lock_guard _(mutex);

		// the old arena stays alive: the game may still point into it
		static std::vector<std::unique_ptr<arena>> old;
		if (memory) old.push_back(std::move(memory));
		load(path);
	}
}
