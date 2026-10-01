////////////////////////////////////////////////////////////////////////////
//	Module 		: sound_collection_storage.h
//	Created 	: 13.10.2005
//  Modified 	: 13.10.2005
//	Author		: Dmitriy Iassenev
//	Description : sound collection storage
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "sound_player.h"

class CSoundCollectionStorage
{
public:
	typedef CSoundPlayer::CSoundCollectionParams CSoundCollectionParams;
	typedef CSoundPlayer::CSoundCollection CSoundCollection;
	typedef std::pair<CSoundCollectionParams, CSoundCollection*> SOUND_COLLECTION_PAIR;
	typedef xr_vector<SOUND_COLLECTION_PAIR> OBJECTS;

private:
	struct collection_key
	{
		shared_str sound_prefix;
		shared_str sound_player_prefix;
		u32 max_count;
		u32 type;

		collection_key(const CSoundCollectionParams& params)
			: sound_prefix(params.m_sound_prefix), sound_player_prefix(params.m_sound_player_prefix),
			  max_count(params.m_max_count), type(static_cast<u32>(params.m_type))
		{
		}

		bool operator==(const collection_key& other) const
		{
			return sound_prefix == other.sound_prefix &&
				sound_player_prefix == other.sound_player_prefix &&
				max_count == other.max_count && type == other.type;
		}
	};

	struct collection_key_hash
	{
		size_t operator()(const collection_key& key) const
		{
			size_t hash = std::hash<shared_str>()(key.sound_prefix);
			const auto combine = [&hash](size_t value)
			{
				hash ^= value + static_cast<size_t>(0x9e3779b9u) + (hash << 6) + (hash >> 2);
			};
			combine(std::hash<shared_str>()(key.sound_player_prefix));
			combine(std::hash<u32>()(key.max_count));
			combine(std::hash<u32>()(key.type));
			return hash;
		}
	};

	typedef xr_unordered_map<collection_key, u32, collection_key_hash> COLLECTION_INDEX;
	typedef xr_unordered_map<u64, xr_vector<xr_string>> SOUND_FILE_INDEX;

	static u64 sound_name_hash(LPCSTR name);

	OBJECTS m_objects;
	COLLECTION_INDEX m_collection_index;
	SOUND_FILE_INDEX m_sound_files;
	bool m_sound_files_ready = false;

public:
	virtual ~CSoundCollectionStorage();
	const SOUND_COLLECTION_PAIR& object(const CSoundCollectionParams& params);
	void reserve_collections(u32 count);
	void prewarm_file_index();
	bool sound_exists(LPCSTR name);
};

extern CSoundCollectionStorage* g_sound_collection_storage;

IC CSoundCollectionStorage& sound_collection_storage();

#include "sound_collection_storage_inline.h"
