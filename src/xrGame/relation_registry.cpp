//////////////////////////////////////////////////////////////////////////
// relation_registry.cpp:	реестр для хранения данных об отношении персонажа к 
//							другим персонажам
//////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "relation_registry.h"
#include "alife_registry_wrappers.h"

#include "character_community.h"
#include "character_reputation.h"
#include "character_rank.h"
#include "InventoryOwner.h"

#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "script_engine.h"


//////////////////////////////////////////////////////////////////////////

SRelation::SRelation()
{
	m_iGoodwill = NEUTRAL_GOODWILL;
}

SRelation::~SRelation()
{
}

//////////////////////////////////////////////////////////////////////////

void RELATION_DATA::clear()
{
	personal.clear();
	communities.clear();
	RELATION_REGISTRY::InvalidateAttitudeCache();
}

void RELATION_DATA::load(IReader& stream)
{
	load_data(personal, stream);
	load_data(communities, stream);
	RELATION_REGISTRY::InvalidateAttitudeCache();
}

void RELATION_DATA::save(IWriter& stream)
{
	save_data(personal, stream);
	save_data(communities, stream);
}

//////////////////////////////////////////////////////////////////////////

RELATION_REGISTRY::RELATION_MAP_SPOTS::RELATION_MAP_SPOTS()
{
	spot_names[ALife::eRelationTypeFriend] = "friend_location";
	spot_names[ALife::eRelationTypeNeutral] = "neutral_location";
	spot_names[ALife::eRelationTypeEnemy] = "enemy_location";
	spot_names[ALife::eRelationTypeWorstEnemy] = "enemy_location";
	//spot_names[ALife::eRelationTypeWorstEnemy]	= "enemy_location";
	spot_names[ALife::eRelationTypeLast] = "neutral_location";
}

//////////////////////////////////////////////////////////////////////////

CRelationRegistryWrapper* RELATION_REGISTRY::m_relation_registry = NULL;
RELATION_REGISTRY::FIGHT_VECTOR* RELATION_REGISTRY::m_fight_registry = NULL;
RELATION_REGISTRY::RELATION_MAP_SPOTS* RELATION_REGISTRY::m_spot_names = NULL;
xr_unordered_flat_map<u32, CHARACTER_GOODWILL> RELATION_REGISTRY::m_attitude_cache;


//////////////////////////////////////////////////////////////////////////


RELATION_REGISTRY::RELATION_REGISTRY()
{
}

RELATION_REGISTRY::~RELATION_REGISTRY()
{
}

//////////////////////////////////////////////////////////////////////////

extern void load_attack_goodwill();
extern bool IsGameTypeSingle();

CRelationRegistryWrapper& RELATION_REGISTRY::relation_registry()
{
	if (!m_relation_registry)
	{
		VERIFY(IsGameTypeSingle());

		m_relation_registry = xr_new<CRelationRegistryWrapper>();
		// Relation queries are extremely hot in stalker memory/enemy classification.
		// Keep pair results resident; mutations are rare and invalidate the cache.
		m_attitude_cache.reserve(4096);
		load_attack_goodwill();
	}

	return *m_relation_registry;
}


RELATION_REGISTRY::FIGHT_VECTOR& RELATION_REGISTRY::fight_registry()
{
	if (!m_fight_registry)
		m_fight_registry = xr_new<FIGHT_VECTOR>();

	return *m_fight_registry;
}

void RELATION_REGISTRY::clear_relation_registry()
{
	xr_delete(m_relation_registry);
	xr_delete(m_fight_registry);
	xr_delete(m_spot_names);
	m_attitude_cache.clear();
}

void RELATION_REGISTRY::InvalidateAttitudeCache()
{
	// clear() keeps the robin-hood allocation, so normal relation changes do not
	// create allocator churn while still providing strict invalidation semantics.
	m_attitude_cache.clear();
}

const shared_str& RELATION_REGISTRY::GetSpotName(ALife::ERelationType& type)
{
	if (!m_spot_names)
		m_spot_names = xr_new<RELATION_MAP_SPOTS>();
	return m_spot_names->GetSpotName(type);
}

//////////////////////////////////////////////////////////////////////////

void RELATION_REGISTRY::ClearRelations(u16 person_id)
{
	const RELATION_DATA* relation_data = relation_registry().registry().objects_ptr(person_id);
	if (relation_data)
	{
		relation_registry().registry().objects(person_id).clear();
	}
}


//////////////////////////////////////////////////////////////////////////
CHARACTER_GOODWILL RELATION_REGISTRY::GetGoodwill(u16 from, u16 to) const
{
	const RELATION_DATA* relation_data = relation_registry().registry().objects_ptr(from);

	if (relation_data)
	{
		PERSONAL_RELATION_MAP::const_iterator it = relation_data->personal.find(to);
		if (relation_data->personal.end() != it)
		{
			const SRelation& relation = (*it).second;
			return relation.Goodwill();
		}
	}
	//если отношение еще не задано, то возвращаем нейтральное
	return NEUTRAL_GOODWILL;
}

CHARACTER_GOODWILL RELATION_REGISTRY::GetAttitude(const CInventoryOwner* from, const CInventoryOwner* to) const
{
	VERIFY(from);
	VERIFY(to);
	const u16 from_id = from->object_id();
	const u16 to_id = to->object_id();
	const u32 cache_key = (static_cast<u32>(from_id) << 16) | static_cast<u32>(to_id);
	const auto cached = m_attitude_cache.find(cache_key);
	if (cached != m_attitude_cache.end())
		return cached->second;

	const CCharacterInfo& from_info = from->CharacterInfo();
	const CCharacterInfo& to_info = to->CharacterInfo();

	const CHARACTER_GOODWILL personal_goodwill = GetGoodwill(from_id, to_id);
	VERIFY(personal_goodwill != NO_GOODWILL);

	// Live inventory owners already keep these indices in sync whenever the
	// underlying values change. Reusing them avoids constructing four temporary
	// rank/reputation objects and linearly scanning each threshold table twice
	// for every relation query in NPC visibility and combat code.
	const CHARACTER_GOODWILL reputation_goodwill = CHARACTER_REPUTATION::relation(
		from_info.Reputation().index(), to_info.Reputation().index());
	const CHARACTER_GOODWILL rank_goodwill =
		CHARACTER_RANK::relation(from_info.Rank().index(), to_info.Rank().index());

	const CHARACTER_COMMUNITY_INDEX from_community = from_info.Community().index();
	const CHARACTER_COMMUNITY_INDEX to_community = to_info.Community().index();
	const CHARACTER_GOODWILL community_goodwill = GetCommunityGoodwill(from_community, to_id);
	VERIFY(community_goodwill != NO_GOODWILL);
	const CHARACTER_GOODWILL community_to_community = GetCommunityRelation(from_community, to_community);

	const CHARACTER_GOODWILL attitude = personal_goodwill + reputation_goodwill + rank_goodwill +
		community_goodwill + community_to_community;
	m_attitude_cache[cache_key] = attitude;
	return attitude;
}

CHARACTER_GOODWILL RELATION_REGISTRY::GetAttitude(CInventoryOwner* from, CInventoryOwner* to) const
{
	return GetAttitude(static_cast<const CInventoryOwner*>(from), static_cast<const CInventoryOwner*>(to));
}

void RELATION_REGISTRY::SetGoodwill(u16 from, u16 to, CHARACTER_GOODWILL goodwill)
{
	RELATION_DATA& relation_data = relation_registry().registry().objects(from);

	static Ivector2 gw_limits = pSettings->r_ivector2(ACTIONS_POINTS_SECT, "personal_goodwill_limits");
	clamp(goodwill, gw_limits.x, gw_limits.y);

	relation_data.personal[to].SetGoodwill(goodwill);
	InvalidateAttitudeCache();
}

void RELATION_REGISTRY::ForceSetGoodwill(u16 from, u16 to, CHARACTER_GOODWILL goodwill)
{
	RELATION_DATA& relation_data = relation_registry().registry().objects(from);

	CSE_ALifeTraderAbstract* from_obj = smart_cast<CSE_ALifeTraderAbstract*>(ai().alife().objects().object(from));
	CSE_ALifeTraderAbstract* to_obj = smart_cast<CSE_ALifeTraderAbstract*>(ai().alife().objects().object(to));

	if (!from_obj || !to_obj)
	{
		ai().script_engine().script_log(ScriptStorage::eLuaMessageTypeError,
		                                "RELATION_REGISTRY::ForceSetGoodwill  : cannot convert obj to CSE_ALifeTraderAbstract!");
		return;
	}
	CHARACTER_GOODWILL community_to_obj_goodwill = GetCommunityGoodwill(from_obj->Community(), to);
	CHARACTER_GOODWILL community_to_community_goodwill = GetCommunityRelation(
		from_obj->Community(), to_obj->Community());

	relation_data.personal[to].SetGoodwill(goodwill - community_to_obj_goodwill - community_to_community_goodwill);
	InvalidateAttitudeCache();
}


void RELATION_REGISTRY::ChangeGoodwill(u16 from, u16 to, CHARACTER_GOODWILL delta_goodwill)
{
	CHARACTER_GOODWILL new_goodwill = GetGoodwill(from, to) + delta_goodwill;
	SetGoodwill(from, to, new_goodwill);
}

//////////////////////////////////////////////////////////////////////////
CHARACTER_GOODWILL RELATION_REGISTRY::GetCommunityGoodwill(CHARACTER_COMMUNITY_INDEX from_community,
                                                           u16 to_character) const
{
	const RELATION_DATA* relation_data = relation_registry().registry().objects_ptr(to_character);

	if (relation_data)
	{
		COMMUNITY_RELATION_MAP::const_iterator it = relation_data->communities.find(from_community);
		if (relation_data->communities.end() != it)
		{
			const SRelation& relation = (*it).second;
			return relation.Goodwill();
		}
	}
	//если отношение еще не задано, то возвращаем нейтральное
	return NEUTRAL_GOODWILL;
}

void RELATION_REGISTRY::SetCommunityGoodwill(CHARACTER_COMMUNITY_INDEX from_community, u16 to_character,
                                             CHARACTER_GOODWILL goodwill)
{
	static Ivector2 gw_limits = pSettings->r_ivector2(ACTIONS_POINTS_SECT, "community_goodwill_limits");
	clamp(goodwill, gw_limits.x, gw_limits.y);
	RELATION_DATA& relation_data = relation_registry().registry().objects(to_character);

	relation_data.communities[from_community].SetGoodwill(goodwill);
	InvalidateAttitudeCache();
}

void RELATION_REGISTRY::ChangeCommunityGoodwill(CHARACTER_COMMUNITY_INDEX from_community, u16 to_character,
                                                CHARACTER_GOODWILL delta_goodwill)
{
	CHARACTER_GOODWILL gw = GetCommunityGoodwill(from_community, to_character) + delta_goodwill;
	SetCommunityGoodwill(from_community, to_character, gw);
}

//////////////////////////////////////////////////////////////////////////

CHARACTER_GOODWILL RELATION_REGISTRY::GetCommunityRelation(CHARACTER_COMMUNITY_INDEX index1,
                                                           CHARACTER_COMMUNITY_INDEX index2) const
{
	return CHARACTER_COMMUNITY::relation(index1, index2);
}

CHARACTER_GOODWILL RELATION_REGISTRY::GetRankRelation(CHARACTER_RANK_VALUE rank1, CHARACTER_RANK_VALUE rank2) const
{
	CHARACTER_RANK rank_from, rank_to;
	rank_from.set(rank1);
	rank_to.set(rank2);
	return CHARACTER_RANK::relation(rank_from.index(), rank_to.index());
}

CHARACTER_GOODWILL RELATION_REGISTRY::GetReputationRelation(CHARACTER_REPUTATION_VALUE rep1,
                                                            CHARACTER_REPUTATION_VALUE rep2) const
{
	CHARACTER_REPUTATION rep_from, rep_to;
	rep_from.set(rep1);
	rep_to.set(rep2);
	return CHARACTER_REPUTATION::relation(rep_from.index(), rep_to.index());
}

//////////////////////////////////////////////////////////////////////////

void RELATION_REGISTRY::SetCommunityRelation(CHARACTER_COMMUNITY_INDEX index1, CHARACTER_COMMUNITY_INDEX index2,
                                             CHARACTER_GOODWILL goodwill)
{
	CHARACTER_COMMUNITY::set_relation(index1, index2, goodwill);
	InvalidateAttitudeCache();
}
