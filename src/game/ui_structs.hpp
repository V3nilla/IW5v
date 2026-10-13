#pragma once

// MW3 PC (iw5mp 1.9.388110) menu, font and material structures. Layouts as in OpenAssetTools' IW5 headers with the PC's
// [1] per-client arrays (the console builds have [4]); checked against the PC zone data with ps3style_dumpPCMenus.

namespace game
{
	namespace native
	{
		namespace ui
		{
			enum asset_type
			{
				ASSET_TYPE_MATERIAL = 5,
				ASSET_TYPE_IMAGE = 10,
				ASSET_TYPE_SOUND = 11,
				ASSET_TYPE_FONT = 24,
				ASSET_TYPE_MENULIST = 25,
				ASSET_TYPE_MENU = 26,
				ASSET_TYPE_LOCALIZE_ENTRY = 27,
				ASSET_TYPE_RAWFILE = 38,
				ASSET_TYPE_STRINGTABLE = 40,
			};

			// 0x20 bytes (checked on "white", "background_image", the PC font atlas)
			struct GfxImage
			{
				void* texture; // IDirect3DTexture9*
				unsigned char mapType;
				unsigned char semantic;
				unsigned char category;
				unsigned char flags;
				int unknown8;
				int cardMemory[2];
				unsigned short width;
				unsigned short height;
				unsigned short depth;
				unsigned short pad;
				const char* name;
			};

			static_assert(sizeof(GfxImage) == 0x20);

			struct Material;

			struct MaterialTextureDef
			{
				unsigned int nameHash;
				char nameStart;
				char nameEnd;
				unsigned char samplerState;
				unsigned char semantic;
				GfxImage* image;
			};

			static_assert(sizeof(MaterialTextureDef) == 12);

			struct MaterialInfo
			{
				const char* name;
				unsigned char gameFlags;
				unsigned char sortKey;
				unsigned char textureAtlasRowCount;
				unsigned char textureAtlasColumnCount;
				unsigned long long drawSurf;
				unsigned int surfaceTypeBits;
			};

			static_assert(sizeof(MaterialInfo) == 0x18);

			struct Material
			{
				MaterialInfo info;
				unsigned char stateBitsEntry[54];
				unsigned char textureCount;
				unsigned char constantCount;
				unsigned char stateBitsCount;
				unsigned char stateFlags;
				unsigned char cameraRegion;
				void* techniqueSet;
				MaterialTextureDef* textureTable;
				void* constantTable;
				void* stateBitsTable;
				const char** subMaterials;
			};

			static_assert(sizeof(Material) == 0x68);

			struct Glyph
			{
				unsigned short letter;
				char x0;
				char y0;
				unsigned char dx;
				unsigned char pixelWidth;
				unsigned char pixelHeight;
				float s0;
				float t0;
				float s1;
				float t1;
			};

			static_assert(sizeof(Glyph) == 0x18);

			struct Font_s
			{
				const char* fontName;
				int pixelHeight;
				int glyphCount;
				Material* material;
				Material* glowMaterial;
				Glyph* glyphs;
			};

			struct LocalizeEntry
			{
				const char* value;
				const char* name;
			};

			struct rectDef_s
			{
				float x;
				float y;
				float w;
				float h;
				unsigned char horzAlign;
				unsigned char vertAlign;
			};

			static_assert(sizeof(rectDef_s) == 0x14);

			struct windowDef_t
			{
				const char* name;
				rectDef_s rect;
				rectDef_s rectClient;
				const char* group;
				int style;
				int border;
				int ownerDraw;
				int ownerDrawFlags;
				float borderSize;
				int staticFlags;
				int dynamicFlags[1];
				int nextTime;
				float foreColor[4];
				float backColor[4];
				float borderColor[4];
				float outlineColor[4];
				float disableColor[4];
				Material* background;
			};

			static_assert(sizeof(windowDef_t) == 0xA4);

			struct Statement_s;

			struct ExpressionString
			{
				const char* string;
			};

			union operandInternalDataUnion
			{
				int intVal;
				float floatVal;
				ExpressionString stringVal;
				Statement_s* function;
			};

			enum expDataType
			{
				VAL_INT = 0,
				VAL_FLOAT = 1,
				VAL_STRING = 2,
				VAL_FUNCTION = 3,
			};

			struct Operand
			{
				int dataType;
				operandInternalDataUnion internals;
			};

			union entryInternalData
			{
				int op;
				Operand operand;
			};

			struct expressionEntry
			{
				int type; // 0 = operator, 1 = operand
				entryInternalData data;
			};

			static_assert(sizeof(expressionEntry) == 12);

			struct StaticDvar
			{
				void* dvar;
				char* dvarName;
			};

			struct ExpressionSupportingData
			{
				int totalFunctions;
				Statement_s** functions;
				int numStaticDvars;
				StaticDvar** staticDvars;
				int totalStrings;
				const char** strings;
			};

			// the evaluator (0x588730) keeps a result cache per local client: times @10 / @20, results @30
			struct Statement_s
			{
				int numEntries;
				expressionEntry* entries;
				ExpressionSupportingData* supportingData;
				int persistentState;
				int lastExecuteTime[4];
				int lastFrame[4];
				Operand lastResult[4];
			};

			static_assert(sizeof(Statement_s) == 0x50);

			struct MenuEventHandlerSet;

			struct ConditionalScript
			{
				MenuEventHandlerSet* eventHandlerSet;
				Statement_s* eventExpression;
			};

			struct SetLocalVarData
			{
				const char* localVarName;
				Statement_s* expression;
			};

			union EventData
			{
				const char* unconditionalScript;
				ConditionalScript* conditionalScript;
				MenuEventHandlerSet* elseScript;
				SetLocalVarData* setLocalVarData;
			};

			enum EventType
			{
				EVENT_UNCONDITIONAL = 0,
				EVENT_IF = 1,
				EVENT_ELSE = 2,
				EVENT_SET_LOCAL_VAR_BOOL = 3,
				EVENT_SET_LOCAL_VAR_INT = 4,
				EVENT_SET_LOCAL_VAR_FLOAT = 5,
				EVENT_SET_LOCAL_VAR_STRING = 6,
			};

			struct MenuEventHandler
			{
				EventData eventData;
				unsigned char eventType;
			};

			struct MenuEventHandlerSet
			{
				int eventHandlerCount;
				MenuEventHandler** eventHandlers;
			};

			struct ItemKeyHandler
			{
				int key;
				MenuEventHandlerSet* action;
				ItemKeyHandler* next;
			};

			struct menuTransition
			{
				int transitionType;
				int startTime;
				float startVal;
				float endVal;
				float time;
				int endTriggerType;
			};

			static_assert(sizeof(menuTransition) == 0x18);

			struct menuData_t
			{
				int fullScreen;
				int fadeCycle;
				float fadeClamp;
				float fadeAmount;
				float fadeInAmount;
				float blurRadius;
				MenuEventHandlerSet* onOpen;
				MenuEventHandlerSet* onCloseRequest;
				MenuEventHandlerSet* onClose;
				MenuEventHandlerSet* onESC;
				MenuEventHandlerSet* onFocusDueToClose;
				ItemKeyHandler* onKey;
				Statement_s* visibleExp;
				const char* allowedBinding;
				const char* soundName;
				float focusColor[4];
				Statement_s* rectXExp;
				Statement_s* rectYExp;
				Statement_s* rectWExp;
				Statement_s* rectHExp;
				Statement_s* openSoundExp;
				Statement_s* closeSoundExp;
				Statement_s* soundLoopExp;
				int cursorItem[1];
				menuTransition scaleTransition[1];
				menuTransition alphaTransition[1];
				menuTransition xTransition[1];
				menuTransition yTransition[1];
				ExpressionSupportingData* expressionData;
				unsigned char priority;
			};

			static_assert(offsetof(menuData_t, expressionData) == 0xCC);

			struct itemDef_s;

			struct menuDef_t
			{
				menuData_t* data;
				windowDef_t window;
				int itemCount;
				itemDef_s** items;
			};

			static_assert(sizeof(menuDef_t) == 0xB0);

			struct columnInfo_s
			{
				int xpos;
				int ypos;
				int width;
				int height;
				int maxChars;
				int alignment;
			};

			struct listBoxDef_s
			{
				int mousePos;
				int startPos[1];
				int endPos[1];
				int drawPadding;
				float elementWidth;
				float elementHeight;
				int elementStyle;
				int numColumns;
				columnInfo_s columnInfo[16];
				MenuEventHandlerSet* onDoubleClick;
				int notselectable;
				int noScrollBars;
				int usePaging;
				float selectBorder[4];
				Material* selectIcon;
				Statement_s* elementHeightExp;
			};

			static_assert(offsetof(listBoxDef_s, onDoubleClick) == 0x1A0);

			struct editFieldDef_s
			{
				float minVal;
				float maxVal;
				float stepVal;
				float defVal;
				float range;
				int maxChars;
				int maxCharsGotoNext;
				int maxPaintChars;
				int paintOffset;
			};

			struct multiDef_s
			{
				const char* dvarList[32];
				const char* dvarStr[32];
				float dvarValue[32];
				int count;
				int strDef;
			};

			struct newsTickerDef_s
			{
				int feedId;
				int speed;
				int spacing;
			};

			struct textScrollDef_s
			{
				int startTime;
			};

			union itemDefData_t
			{
				listBoxDef_s* listBox;
				editFieldDef_s* editField;
				multiDef_s* multi;
				const char* enumDvarName;
				newsTickerDef_s* ticker;
				textScrollDef_s* scroll;
				void* data;
			};

			struct ItemFloatExpression
			{
				int target;
				Statement_s* expression;
			};

			struct itemDef_s
			{
				windowDef_t window;
				rectDef_s textRect[1];
				int type;
				int dataType;
				int alignment;
				int fontEnum;
				int textAlignMode;
				float textalignx;
				float textaligny;
				float textscale;
				int textStyle;
				int gameMsgWindowIndex;
				int gameMsgWindowMode;
				const char* text;
				int itemFlags;
				menuDef_t* parent;
				MenuEventHandlerSet* mouseEnterText;
				MenuEventHandlerSet* mouseExitText;
				MenuEventHandlerSet* mouseEnter;
				MenuEventHandlerSet* mouseExit;
				MenuEventHandlerSet* action;
				MenuEventHandlerSet* accept;
				MenuEventHandlerSet* onFocus;
				MenuEventHandlerSet* hasFocus;
				MenuEventHandlerSet* leaveFocus;
				const char* dvar;
				const char* dvarTest;
				ItemKeyHandler* onKey;
				const char* enableDvar;
				const char* localVar;
				int dvarFlags;
				void* focusSound; // snd_alias_list_t*, name first
				float special;
				int cursorPos[1];
				itemDefData_t typeData;
				int floatExpressionCount;
				ItemFloatExpression* floatExpressions;
				Statement_s* visibleExp;
				Statement_s* disabledExp;
				Statement_s* textExp;
				Statement_s* materialExp;
				float glowColor[4];
				bool decayActive;
				int fxBirthTime;
				int fxLetterTime;
				int fxDecayStartTime;
				int fxDecayDuration;
				int lastSoundPlayedTime;
				Statement_s* textAlignYExp;
			};

			static_assert(offsetof(itemDef_s, type) == 0xB8);
			static_assert(offsetof(itemDef_s, typeData) == 0x138);
			static_assert(sizeof(itemDef_s) == 0x180);

			struct MenuList
			{
				const char* name;
				int menuCount;
				menuDef_t** menus;
			};

			enum item_type
			{
				ITEM_TYPE_TEXT = 0,
				ITEM_TYPE_BUTTON = 1,
				ITEM_TYPE_RADIOBUTTON = 2,
				ITEM_TYPE_CHECKBOX = 3,
				ITEM_TYPE_EDITFIELD = 4,
				ITEM_TYPE_COMBO = 5,
				ITEM_TYPE_LISTBOX = 6,
				ITEM_TYPE_MODEL = 7,
				ITEM_TYPE_OWNERDRAW = 8,
				ITEM_TYPE_NUMERICFIELD = 9,
				ITEM_TYPE_SLIDER = 10,
				ITEM_TYPE_YESNO = 11,
				ITEM_TYPE_MULTI = 12,
				ITEM_TYPE_DVARENUM = 13,
				ITEM_TYPE_BIND = 14,
				ITEM_TYPE_MENUMODEL = 15,
				ITEM_TYPE_VALIDFILEFIELD = 16,
				ITEM_TYPE_DECIMALFIELD = 17,
				ITEM_TYPE_UPREDITFIELD = 18,
				ITEM_TYPE_GAME_MESSAGE_WINDOW = 19,
				ITEM_TYPE_NEWS_TICKER = 20,
				ITEM_TYPE_TEXT_SCROLL = 21,
				ITEM_TYPE_EMAILFIELD = 22,
				ITEM_TYPE_PASSWORDFIELD = 23,
			};
		}
	}
}
