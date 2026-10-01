#include "stdafx.h"
#include "specific_character.h"

#ifdef  XRGAME_EXPORTS
#include "PhraseDialog.h"
#include "string_table.h"

#include "ai_space.h"
#include "script_engine.h"
#include "Script_Game_Object.h"

namespace
{
struct SSpecificCharacterLoadProfileBatch
{
	bool active = false;
	u32 depth = 0;
	u32 loads = 0;
	u64 flags_xml = 0;
	u64 dialogs_xml = 0;
	u64 dialogs_lua = 0;
	u64 dialogs_lua_build = 0;
	u64 dialogs_lua_call = 0;
	u64 dialogs_lua_result = 0;
	u64 fields_xml = 0;
	u64 init_lua = 0;
	u64 init_lua_build = 0;
	u64 init_lua_call = 0;
	u64 init_lua_result = 0;
	u64 load_total = 0;
};

thread_local SSpecificCharacterLoadProfileBatch g_specific_character_load_profile_batch;

class SLuaStackRestore final
{
public:
	explicit SLuaStackRestore(lua_State* state) : m_state(state), m_top(lua_gettop(state)) {}
	~SLuaStackRestore() { lua_settop(m_state, m_top); }

private:
	lua_State* m_state;
	int m_top;
};

inline ::luabind::object make_lua_table(lua_State* state, const int array_size, const int record_size)
{
	lua_createtable(state, array_size, record_size);
	::luabind::detail::lua_reference reference;
	reference.set(state);
	return ::luabind::object(state, reference, true);
}

inline void set_lua_string_field(lua_State* state, const int table_index, LPCSTR key, LPCSTR value)
{
	lua_pushstring(state, value);
	lua_setfield(state, table_index, key);
}

inline void set_lua_number_field(lua_State* state, const int table_index, LPCSTR key, const lua_Number value)
{
	lua_pushnumber(state, value);
	lua_setfield(state, table_index, key);
}

inline void set_lua_bool_field(lua_State* state, const int table_index, LPCSTR key, const bool value)
{
	lua_pushboolean(state, value);
	lua_setfield(state, table_index, key);
}

template <typename T>
T cast_lua_stack_value(lua_State* state, const int index)
{
	lua_pushvalue(state, index);
	::luabind::detail::lua_reference reference;
	reference.set(state);
	const ::luabind::object value(state, reference, true);
	return ::luabind::object_cast<T>(value);
}

inline LPCSTR get_lua_string_field(lua_State* state, const int table_index, LPCSTR key)
{
	lua_getfield(state, table_index, key);
	LPCSTR result = nullptr;
	if (lua_type(state, -1) == LUA_TSTRING)
		result = lua_tostring(state, -1);
	else
		result = cast_lua_stack_value<LPCSTR>(state, -1);
	lua_pop(state, 1);
	return result;
}

inline int get_lua_int_field(lua_State* state, const int table_index, LPCSTR key)
{
	lua_getfield(state, table_index, key);
	int result = 0;
	const int type = lua_type(state, -1);
	if (type == LUA_TNIL || type == LUA_TNUMBER)
		result = static_cast<int>(lua_tonumber(state, -1));
	else
		result = cast_lua_stack_value<int>(state, -1);
	lua_pop(state, 1);
	return result;
}

inline float get_lua_float_field(lua_State* state, const int table_index, LPCSTR key)
{
	lua_getfield(state, table_index, key);
	float result = 0.f;
	const int type = lua_type(state, -1);
	if (type == LUA_TNIL || type == LUA_TNUMBER)
		result = static_cast<float>(lua_tonumber(state, -1));
	else
		result = cast_lua_stack_value<float>(state, -1);
	lua_pop(state, 1);
	return result;
}

inline bool get_lua_bool_field(lua_State* state, const int table_index, LPCSTR key)
{
	lua_getfield(state, table_index, key);
	bool result = false;
	const int type = lua_type(state, -1);
	if (type == LUA_TNIL || type == LUA_TBOOLEAN)
		result = lua_toboolean(state, -1) != 0;
	else
		result = cast_lua_stack_value<bool>(state, -1);
	lua_pop(state, 1);
	return result;
}
}

SSpecificCharacterData::SSpecificCharacterData()
{
	m_sGameName.clear();
	m_sBioText = NULL;
	m_sVisual.clear();
	m_sSupplySpawn.clear();
	m_sNpcConfigSect.clear();


	m_StartDialog = NULL;
	m_ActorDialogs.clear();

	m_Rank = NO_RANK;
	m_Reputation = NO_REPUTATION;

	money_def.inf_money = false;
	money_def.max_money = 0;
	money_def.min_money = 0;

	rank_def.min = NO_RANK;
	rank_def.max = NO_RANK;

	reputation_def.min = NO_REPUTATION;
	reputation_def.max = NO_REPUTATION;

	m_bNoRandom = false;
	m_bDefaultForCommunity = false;
	m_fPanic_threshold = 0.0f;
	m_fHitProbabilityFactor = 1.f;
	m_crouch_type = 0;
	m_upgrade_mechanic = false;
}

SSpecificCharacterData::~SSpecificCharacterData()
{
}

#endif

CSpecificCharacter::CSpecificCharacter()
{
	m_OwnId = NULL;
}


CSpecificCharacter::~CSpecificCharacter()
{
}

namespace
{
	xr_vector<shared_str> g_specific_character_random_ids;
	xr_unordered_flat_map<shared_str, xr_vector<shared_str>> g_specific_character_random_ids_by_class;
	bool g_specific_character_selection_index_ready = false;
	bool g_specific_character_shared_data_prewarmed = false;

	void build_specific_character_selection_index()
	{
		if (g_specific_character_selection_index_ready)
			return;

		g_specific_character_random_ids.clear();
		g_specific_character_random_ids_by_class.clear();
		const int max_index = CSpecificCharacter::GetMaxIndex();
		if (max_index >= 0)
			g_specific_character_random_ids.reserve(static_cast<size_t>(max_index) + 1u);

		for (int i = 0; i <= max_index; ++i)
		{
			const ITEM_DATA* const item_data = CSpecificCharacter::GetByIndex(i);
			VERIFY(item_data && item_data->_xml && item_data->_node);
			CUIXml* const xml = item_data->_xml;
			XML_NODE* const node = item_data->_node;
			if (xml->ReadAttribInt(node, "no_random", 0) == 1)
				continue;

			g_specific_character_random_ids.push_back(item_data->id);

			// CSpecificCharacter::load_shared() historically treats the first <class>
			// value as authoritative even when several nodes are present. Mirror that
			// exact behavior here so the optimization cannot change candidate sets.
			XML_NODE* const class_node = node->FirstChild("class");
			LPCSTR class_name = class_node ? xml->Read(class_node, "") : nullptr;
			if (!class_name || !class_name[0])
				continue;

			char* normalized = xr_strdup(class_name);
			xr_strlwr(normalized);
			g_specific_character_random_ids_by_class[shared_str(normalized)].push_back(item_data->id);
			xr_free(normalized);
		}

		g_specific_character_selection_index_ready = true;
	}
}

void CSpecificCharacter::PrewarmSelectionCache(const bool load_shared_data)
{
	build_specific_character_selection_index();
	if (!load_shared_data || g_specific_character_shared_data_prewarmed)
		return;

	CTimer timer;
	timer.Start();
	CSpecificCharacter character;
	for (const shared_str& id : g_specific_character_random_ids)
		character.Load(id);
	g_specific_character_shared_data_prewarmed = true;
	Msg("* Specific-character cache: indexed=%u prewarmed=%u time=%.1f ms",
		static_cast<u32>(g_specific_character_random_ids.size()),
		static_cast<u32>(g_specific_character_random_ids.size()), timer.GetElapsed_sec() * 1000.f);
}

void CSpecificCharacter::ClearSelectionCache()
{
	g_specific_character_random_ids.clear_and_free();
	g_specific_character_random_ids_by_class.clear();
	g_specific_character_selection_index_ready = false;
	g_specific_character_shared_data_prewarmed = false;
}

const xr_vector<shared_str>& CSpecificCharacter::RandomSelectionIds(const shared_str& character_class)
{
	build_specific_character_selection_index();
	if (!character_class.size())
		return g_specific_character_random_ids;

	const auto it = g_specific_character_random_ids_by_class.find(character_class);
	if (it != g_specific_character_random_ids_by_class.end())
		return it->second;

	static const xr_vector<shared_str> empty;
	return empty;
}


#ifdef XRGAME_EXPORTS
void CSpecificCharacter::BeginLoadProfileBatch()
{
	SSpecificCharacterLoadProfileBatch& batch = g_specific_character_load_profile_batch;
	if (batch.depth)
	{
		++batch.depth;
		return;
	}

	batch = SSpecificCharacterLoadProfileBatch{};
	batch.depth = 1;
	batch.active = ai().script_engine().lua_recording() && CPU::qpc_freq;
}

void CSpecificCharacter::EndLoadProfileBatch(LPCSTR object_name, const u32 object_id)
{
	SSpecificCharacterLoadProfileBatch& batch = g_specific_character_load_profile_batch;
	if (!batch.depth)
		return;
	if (--batch.depth)
		return;
	if (!batch.active)
		return;

	CScriptEngine& script_engine = ai().script_engine();
	const u32 loads = batch.loads;
	script_engine.record_native_profile("alife_specific_batch", "specific.flags_xml", object_name, object_id, batch.flags_xml, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.dialogs_xml", object_name, object_id, batch.dialogs_xml, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.dialogs_lua", object_name, object_id, batch.dialogs_lua, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.dialogs_lua.build", object_name, object_id, batch.dialogs_lua_build, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.dialogs_lua.call", object_name, object_id, batch.dialogs_lua_call, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.dialogs_lua.result", object_name, object_id, batch.dialogs_lua_result, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.fields_xml", object_name, object_id, batch.fields_xml, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.init_lua", object_name, object_id, batch.init_lua, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.init_lua.build", object_name, object_id, batch.init_lua_build, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.init_lua.call", object_name, object_id, batch.init_lua_call, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.init_lua.result", object_name, object_id, batch.init_lua_result, loads);
	script_engine.record_native_profile("alife_specific_batch", "specific.load_total", object_name, object_id, batch.load_total, loads);
	batch.active = false;
}
#endif

void CSpecificCharacter::InitXmlIdToIndex()
{
	if (!id_to_index::tag_name)
		id_to_index::tag_name = "specific_character";
	if (!id_to_index::file_str)
		id_to_index::file_str = pSettings->r_string("profiles", "specific_characters_files");
}


void CSpecificCharacter::Load(shared_str id)
{
	R_ASSERT(id.size());
	m_OwnId = id;
	inherited_shared::load_shared(m_OwnId, NULL);
}


void CSpecificCharacter::load_shared(LPCSTR)
{
#if 0
	CTimer			timer;
	timer.Start();
#endif
	const ITEM_DATA& item_data = *id_to_index::GetById(m_OwnId);
	SSpecificCharacterData* const shared_data = data();

#ifdef XRGAME_EXPORTS
	CScriptEngine& script_engine = ai().script_engine();
	SSpecificCharacterLoadProfileBatch& profile_batch = g_specific_character_load_profile_batch;
	const bool record_profile = profile_batch.active;
	const u64 total_started = record_profile ? CPU::QPC() : 0;
	auto accumulate_stage = [&](u64& target, const u64 started)
	{
		if (started)
			target += CPU::QPC() - started;
	};
#endif

	CUIXml* pXML = item_data._xml;

	pXML->SetLocalRoot(pXML->GetRoot());


	XML_NODE* item_node = item_data._node;
	R_ASSERT3(item_node, "specific_character id=", *item_data.id);

	pXML->SetLocalRoot(item_node);
	XML_NODE* const local_root = item_node;


#ifdef XRGAME_EXPORTS
	const u64 flags_started = record_profile ? CPU::QPC() : 0;
#endif
	int norandom = pXML->ReadAttribInt(item_node, "no_random", 0);
	if (1 == norandom)
		shared_data->m_bNoRandom = true;
	else
		shared_data->m_bNoRandom = false;

	int team_default = pXML->ReadAttribInt(item_node, "team_default", 0);
	if (1 == team_default)
		shared_data->m_bDefaultForCommunity = true;
	else
		shared_data->m_bDefaultForCommunity = false;

	R_ASSERT3(!(shared_data->m_bNoRandom && shared_data->m_bDefaultForCommunity),
	          "cannot set 'no_random' and 'team_default' flags simultaneously, profile id", *shared_str(item_data.id));

#ifdef XRGAME_EXPORTS
	accumulate_stage(profile_batch.flags_xml, flags_started);
#endif

#ifdef  XRGAME_EXPORTS

	LPCSTR start_dialog = pXML->Read(local_root->FirstChild("start_dialog"), NULL);
	if (start_dialog)
	{
		shared_data->m_StartDialog = start_dialog;
	}
	else
		shared_data->m_StartDialog = NULL;

	const u64 dialogs_xml_started = record_profile ? CPU::QPC() : 0;
	const int dialogs_num = pXML->GetNodesNum(local_root, "actor_dialog");
	shared_data->m_ActorDialogs.clear();
	shared_data->m_ActorDialogs.reserve(static_cast<size_t>(dialogs_num));
	XML_NODE* dialog_node = dialogs_num ? local_root->FirstChild("actor_dialog") : nullptr;
	for (int i = 0; i < dialogs_num; ++i)
	{
		R_ASSERT(dialog_node);
		shared_data->m_ActorDialogs.emplace_back(pXML->Read(dialog_node, ""));
		dialog_node = local_root->IterateChildren("actor_dialog", dialog_node);
	}
	accumulate_stage(profile_batch.dialogs_xml, dialogs_xml_started);

	const u64 dialogs_lua_started = record_profile ? CPU::QPC() : 0;
	static cached_script_functor<::luabind::object> dialog_list_cache("_G.CSpecificCharacterDialogList");
	if (const auto* funct = dialog_list_cache.get(script_engine))
	{
		lua_State* const lua = script_engine.lua();
		const u64 build_started = record_profile ? CPU::QPC() : 0;
		::luabind::object table = make_lua_table(lua, static_cast<int>(shared_data->m_ActorDialogs.size()), 0);
		{
			SLuaStackRestore stack_restore(lua);
			table.pushvalue();
			const int table_index = lua_gettop(lua);
			int i = 1;
			for (const auto& dialog : shared_data->m_ActorDialogs)
			{
				lua_pushstring(lua, dialog.c_str());
				lua_rawseti(lua, table_index, i++);
			}
		}
		accumulate_stage(profile_batch.dialogs_lua_build, build_started);

		const LPCSTR character_name = item_data.id.c_str();
		const u64 call_started = record_profile ? CPU::QPC() : 0;
		::luabind::object output = (*funct)(character_name, table);
		accumulate_stage(profile_batch.dialogs_lua_call, call_started);

		const u64 result_started = record_profile ? CPU::QPC() : 0;
		if (output && output.type() == LUA_TTABLE)
		{
			SLuaStackRestore stack_restore(lua);
			output.pushvalue();
			const int output_index = lua_gettop(lua);
			shared_data->m_ActorDialogs.clear();
			const size_t array_size = static_cast<size_t>(lua_objlen(lua, output_index));
			if (array_size > shared_data->m_ActorDialogs.capacity())
				shared_data->m_ActorDialogs.reserve(array_size);

			lua_pushnil(lua);
			while (lua_next(lua, output_index) != 0)
			{
				if (lua_type(lua, -1) == LUA_TSTRING)
					shared_data->m_ActorDialogs.emplace_back(lua_tostring(lua, -1));
				lua_pop(lua, 1);
			}
		}
		accumulate_stage(profile_batch.dialogs_lua_result, result_started);
	}
	accumulate_stage(profile_batch.dialogs_lua, dialogs_lua_started);

	const u64 fields_xml_started = record_profile ? CPU::QPC() : 0;
	shared_data->m_icon_name = pXML->Read(local_root->FirstChild("icon"), "ui_npc_u_barman");


	//игровое имя персонажа
	shared_data->m_sGameName = pXML->Read(local_root->FirstChild("name"), "");
	CStringTable string_table;
	shared_data->m_sBioText = string_table.translate(pXML->Read(local_root->FirstChild("bio"), ""));


	shared_data->m_fPanic_threshold = pXML->ReadFlt(local_root->FirstChild("panic_threshold"), 0.f);
	shared_data->m_fHitProbabilityFactor = pXML->ReadFlt(local_root->FirstChild("hit_probability_factor"), 1.f);
	shared_data->m_crouch_type = pXML->ReadInt(local_root->FirstChild("crouch_type"), 0);
	shared_data->m_upgrade_mechanic = (pXML->ReadInt(local_root->FirstChild("mechanic_mode"), 0) == 1);

	shared_data->m_critical_wound_weights = pXML->Read(local_root->FirstChild("critical_wound_weights"), "1");

#endif

	shared_data->m_sVisual = pXML->Read(local_root->FirstChild("visual"), "");


#ifdef  XRGAME_EXPORTS
	shared_data->m_sSupplySpawn = pXML->Read(local_root->FirstChild("supplies"), "");

	if (!shared_data->m_sSupplySpawn.empty())
	{
		xr_string& str = shared_data->m_sSupplySpawn;
		xr_string::size_type pos = str.find("\\n");

		while (xr_string::npos != pos)
		{
			str.replace(pos, 2, "\n");
			pos = str.find("\\n", pos + 1);
		}
	}

	shared_data->m_sNpcConfigSect = pXML->Read(local_root->FirstChild("npc_config"), "");
	shared_data->m_sound_voice_prefix = pXML->Read(local_root->FirstChild("snd_config"), "");

	shared_data->m_terrain_sect = pXML->Read(local_root->FirstChild("terrain_sect"), "");

#endif

	shared_data->m_Classes.clear();
	const int classes_num = pXML->GetNodesNum(local_root, "class");
	shared_data->m_Classes.reserve(static_cast<size_t>(classes_num));
	if (classes_num)
	{
		LPCSTR char_class = pXML->Read(local_root->FirstChild("class"), "");
		if (char_class)
		{
			char* buf_str = xr_strdup(char_class);
			xr_strlwr(buf_str);
			const CHARACTER_CLASS normalized_class = buf_str;
			xr_free(buf_str);
			for (int i = 0; i < classes_num; ++i)
				shared_data->m_Classes.push_back(normalized_class);
		}
	}


#ifdef  XRGAME_EXPORTS

	LPCSTR team = pXML->Read(local_root->FirstChild("community"), NULL);
	R_ASSERT3(team != NULL, "'community' field not fulfiled for specific character", *m_OwnId);

	char* buf_str = xr_strdup(team);
	xr_strlwr(buf_str);
	shared_data->m_Community.set(buf_str);
	xr_free(buf_str);

	if (shared_data->m_Community.index() == NO_COMMUNITY_INDEX)
		Debug.fatal(DEBUG_INFO, "wrong 'community' '%s' in specific character %s ", team, *m_OwnId);


	XML_NODE* const rank_node = local_root->FirstChild("rank");
	const int min_rank = pXML->ReadAttribInt(rank_node, "min", NO_RANK);
	const int max_rank = pXML->ReadAttribInt(rank_node, "max", NO_RANK);
	if (min_rank != NO_RANK && max_rank != NO_RANK)
	{
		shared_data->rank_def.min = _min(min_rank, max_rank);
		shared_data->rank_def.max = _max(max_rank, min_rank);
	}
	else
	{
		const int rank = pXML->ReadInt(rank_node, NO_RANK);
		R_ASSERT3(rank != NO_RANK, "'rank' field not fulfiled for specific character", *m_OwnId);
		shared_data->rank_def.min = rank;
		shared_data->rank_def.max = rank;
	}

	XML_NODE* const reputation_node = local_root->FirstChild("reputation");
	const int min_reputation = pXML->ReadAttribInt(reputation_node, "min", NO_REPUTATION);
	const int max_reputation = pXML->ReadAttribInt(reputation_node, "max", NO_REPUTATION);
	if (min_reputation != NO_REPUTATION && max_reputation != NO_REPUTATION)
	{
		shared_data->reputation_def.min = _min(min_reputation, max_reputation);
		shared_data->reputation_def.max = _max(max_reputation, min_reputation);
	}
	else
	{
		const int rep = pXML->ReadInt(reputation_node, NO_REPUTATION);
		R_ASSERT3(rep != NO_REPUTATION, "'reputation' field not fulfiled for specific character", *m_OwnId);
		shared_data->reputation_def.min = rep;
		shared_data->reputation_def.max = rep;
	}

	XML_NODE* const money_node = local_root->FirstChild("money");
	if (money_node)
	{
		shared_data->money_def.min_money = pXML->ReadAttribInt(money_node, "min", 0);
		shared_data->money_def.max_money = pXML->ReadAttribInt(money_node, "max", 0);
		shared_data->money_def.inf_money = !!pXML->ReadAttribInt(money_node, "infinitive", 0);
		shared_data->money_def.max_money = _max(shared_data->money_def.max_money, shared_data->money_def.min_money); // :)
	}
	else
	{
		shared_data->money_def.min_money = 0;
		shared_data->money_def.max_money = 0;
		shared_data->money_def.inf_money = false;
	}

	accumulate_stage(profile_batch.fields_xml, fields_xml_started);

	const u64 init_lua_started = record_profile ? CPU::QPC() : 0;
	static cached_script_functor<::luabind::object> init_cache("_G.CSpecificCharacterInit");
	if (const auto* init_funct = init_cache.get(script_engine))
	{
		lua_State* const lua = script_engine.lua();
		const u64 build_started = record_profile ? CPU::QPC() : 0;
		::luabind::object table = make_lua_table(lua, 0, 22);
		{
			SLuaStackRestore stack_restore(lua);
			table.pushvalue();
			const int table_index = lua_gettop(lua);
			set_lua_string_field(lua, table_index, "name", shared_data->m_sGameName.c_str());
			set_lua_string_field(lua, table_index, "bio", shared_data->m_sBioText.c_str());
			set_lua_string_field(lua, table_index, "community", shared_data->m_Community.id().c_str());
			set_lua_string_field(lua, table_index, "icon", shared_data->m_icon_name.c_str());
			set_lua_string_field(lua, table_index, "start_dialog", shared_data->m_StartDialog.c_str());
			set_lua_number_field(lua, table_index, "panic_threshold", shared_data->m_fPanic_threshold);
			set_lua_number_field(lua, table_index, "hit_probability_factor", shared_data->m_fHitProbabilityFactor);
			set_lua_number_field(lua, table_index, "crouch_type", shared_data->m_crouch_type);
			set_lua_bool_field(lua, table_index, "mechanic_mode", shared_data->m_upgrade_mechanic);
			set_lua_string_field(lua, table_index, "critical_wound_weights", shared_data->m_critical_wound_weights.c_str());
			set_lua_string_field(lua, table_index, "supplies", shared_data->m_sSupplySpawn.c_str());
			set_lua_string_field(lua, table_index, "visual", shared_data->m_sVisual.c_str());
			set_lua_string_field(lua, table_index, "npc_config", shared_data->m_sNpcConfigSect.c_str());
			set_lua_string_field(lua, table_index, "snd_config", shared_data->m_sound_voice_prefix.c_str());
			set_lua_string_field(lua, table_index, "terrain_sect", shared_data->m_terrain_sect.c_str());
			set_lua_number_field(lua, table_index, "rank_min", shared_data->rank_def.min);
			set_lua_number_field(lua, table_index, "rank_max", shared_data->rank_def.max);
			set_lua_number_field(lua, table_index, "reputation_min", shared_data->reputation_def.min);
			set_lua_number_field(lua, table_index, "reputation_max", shared_data->reputation_def.max);
			set_lua_number_field(lua, table_index, "money_min", shared_data->money_def.min_money);
			set_lua_number_field(lua, table_index, "money_max", shared_data->money_def.max_money);
			set_lua_bool_field(lua, table_index, "money_infinitive", shared_data->money_def.inf_money);
		}
		accumulate_stage(profile_batch.init_lua_build, build_started);

		const LPCSTR character_name = item_data.id.c_str();
		const u64 call_started = record_profile ? CPU::QPC() : 0;
		::luabind::object output = (*init_funct)(character_name, table);
		accumulate_stage(profile_batch.init_lua_call, call_started);

		const u64 result_started = record_profile ? CPU::QPC() : 0;
		if (output && output.type() == LUA_TTABLE)
		{
			SLuaStackRestore stack_restore(lua);
			output.pushvalue();
			const int output_index = lua_gettop(lua);

			shared_data->m_sGameName = get_lua_string_field(lua, output_index, "name");
			shared_data->m_sBioText = string_table.translate(get_lua_string_field(lua, output_index, "bio"));

			const LPCSTR output_community = get_lua_string_field(lua, output_index, "community");
			shared_data->m_Community.set(output_community);
			if (shared_data->m_Community.index() == NO_COMMUNITY_INDEX)
				Debug.fatal(DEBUG_INFO, "wrong 'community' '%s' in specific character %s ", output_community, *m_OwnId);

			shared_data->m_icon_name = get_lua_string_field(lua, output_index, "icon");
			lua_getfield(lua, output_index, "start_dialog");
			shared_data->m_StartDialog = lua_type(lua, -1) == LUA_TSTRING ? lua_tostring(lua, -1) : NULL;
			lua_pop(lua, 1);
			shared_data->m_fPanic_threshold = get_lua_float_field(lua, output_index, "panic_threshold");
			shared_data->m_fHitProbabilityFactor = get_lua_float_field(lua, output_index, "hit_probability_factor");
			shared_data->m_crouch_type = get_lua_int_field(lua, output_index, "crouch_type");
			shared_data->m_upgrade_mechanic = get_lua_bool_field(lua, output_index, "mechanic_mode");
			shared_data->m_critical_wound_weights = get_lua_string_field(lua, output_index, "critical_wound_weights");
			shared_data->m_sVisual = get_lua_string_field(lua, output_index, "visual");
			shared_data->m_sNpcConfigSect = get_lua_string_field(lua, output_index, "npc_config");
			shared_data->m_sound_voice_prefix = get_lua_string_field(lua, output_index, "snd_config");
			shared_data->m_terrain_sect = get_lua_string_field(lua, output_index, "terrain_sect");

			shared_data->m_sSupplySpawn = get_lua_string_field(lua, output_index, "supplies");
			if (!shared_data->m_sSupplySpawn.empty())
			{
				xr_string& str = shared_data->m_sSupplySpawn;
				xr_string::size_type pos = str.find("\\n");
				while (xr_string::npos != pos)
				{
					str.replace(pos, 2, "\n");
					pos = str.find("\\n", pos + 1);
				}
			}

			const int output_rank_min = get_lua_int_field(lua, output_index, "rank_min");
			const int output_rank_max = get_lua_int_field(lua, output_index, "rank_max");
			shared_data->rank_def.min = _min(output_rank_min, output_rank_max);
			shared_data->rank_def.max = _max(output_rank_min, output_rank_max);

			const int output_reputation_min = get_lua_int_field(lua, output_index, "reputation_min");
			const int output_reputation_max = get_lua_int_field(lua, output_index, "reputation_max");
			shared_data->reputation_def.min = _min(output_reputation_min, output_reputation_max);
			shared_data->reputation_def.max = _max(output_reputation_min, output_reputation_max);

			const int output_money_min = get_lua_int_field(lua, output_index, "money_min");
			const int output_money_max = get_lua_int_field(lua, output_index, "money_max");
			shared_data->money_def.min_money = _min(output_money_min, output_money_max);
			shared_data->money_def.max_money = _max(output_money_min, output_money_max);
			shared_data->money_def.inf_money = get_lua_bool_field(lua, output_index, "money_infinitive");
		}
		accumulate_stage(profile_batch.init_lua_result, result_started);
	}
	accumulate_stage(profile_batch.init_lua, init_lua_started);
	accumulate_stage(profile_batch.load_total, total_started);
	if (record_profile)
		++profile_batch.loads;

#endif

#if 0
	Msg("CSpecificCharacter::load_shared() takes %f milliseconds", timer.GetElapsed_sec()*1000.f);
#endif
}


#ifdef  XRGAME_EXPORTS

LPCSTR CSpecificCharacter::Name() const
{
	return data()->m_sGameName.c_str();
}

shared_str CSpecificCharacter::Bio() const
{
	return data()->m_sBioText;
}

const CHARACTER_COMMUNITY& CSpecificCharacter::Community() const
{
	return data()->m_Community;
}

LPCSTR CSpecificCharacter::SupplySpawn() const
{
	return data()->m_sSupplySpawn.c_str();
}

LPCSTR CSpecificCharacter::NpcConfigSect() const
{
	return data()->m_sNpcConfigSect.c_str();
}

LPCSTR CSpecificCharacter::sound_voice_prefix() const
{
	return data()->m_sound_voice_prefix.c_str();
}

float CSpecificCharacter::panic_threshold() const
{
	return data()->m_fPanic_threshold;
}

float CSpecificCharacter::hit_probability_factor() const
{
	return data()->m_fHitProbabilityFactor;
}

int CSpecificCharacter::crouch_type() const
{
	return data()->m_crouch_type;
}

bool CSpecificCharacter::upgrade_mechanic() const
{
	return data()->m_upgrade_mechanic;
}

LPCSTR CSpecificCharacter::critical_wound_weights() const
{
	return data()->m_critical_wound_weights.c_str();
}

#endif

shared_str CSpecificCharacter::terrain_sect() const
{
	return data()->m_terrain_sect;
}

CHARACTER_RANK_VALUE CSpecificCharacter::Rank() const
{
	return data()->m_Rank;
}

CHARACTER_REPUTATION_VALUE CSpecificCharacter::Reputation() const
{
	return data()->m_Reputation;
}

LPCSTR CSpecificCharacter::Visual() const
{
	return data()->m_sVisual.c_str();
}
