////////////////////////////////////////////////////////////////////////////
//	Module 		: sound_collection_storage.cpp
//	Created 	: 13.10.2005
//  Modified 	: 13.10.2005
//	Author		: Dmitriy Iassenev
//	Description : sound collection storage
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "sound_collection_storage.h"
#include "object_broker.h"

CSoundCollectionStorage* g_sound_collection_storage = 0;

CSoundCollectionStorage::~CSoundCollectionStorage()
{
	delete_data(m_objects);
}

void CSoundCollectionStorage::reserve_collections(const u32 count)
{
	if (!count)
		return;
	if (m_objects.capacity() < count)
		m_objects.reserve(count);
	m_collection_index.reserve(count);
}

u64 CSoundCollectionStorage::sound_name_hash(LPCSTR name)
{
	// FNV-1a over the normalized, case-insensitive virtual FS name. LocatorAPI
	// stores file names in lower case; folding here preserves FS.exist() behavior
	// for configs that use upper-case characters without allocating a temporary
	// string for each candidate lookup.
	u64 hash = 14695981039346656037ull;
	for (const unsigned char* c = reinterpret_cast<const unsigned char*>(name); *c; ++c)
	{
		unsigned char value = *c;
		if (value >= 'A' && value <= 'Z')
			value = static_cast<unsigned char>(value - 'A' + 'a');
		hash ^= value;
		hash *= 1099511628211ull;
	}
	return hash;
}

void CSoundCollectionStorage::prewarm_file_index()
{
	if (m_sound_files_ready)
		return;

	FS_FileSet files;
	FS.file_list(files, "$game_sounds$", FS_ListFiles);
	m_sound_files.reserve(files.size());

	for (const FS_File& file : files)
	{
		string_path name;
		xr_strcpy(name, sizeof(name), file.name.c_str());
		LPSTR extension = strext(name);
		if (!extension || stricmp(extension, ".ogg"))
			continue;

		*extension = 0;
		xr_strlwr(name);
		m_sound_files[sound_name_hash(name)].emplace_back(name);
	}

	m_sound_files_ready = true;
}

bool CSoundCollectionStorage::sound_exists(LPCSTR name)
{
	if (!m_sound_files_ready)
		prewarm_file_index();

	const auto bucket = m_sound_files.find(sound_name_hash(name));
	if (bucket == m_sound_files.end())
		return false;

	for (const xr_string& file_name : bucket->second)
		if (!stricmp(file_name.c_str(), name))
			return true;

	return false;
}

const CSoundCollectionStorage::SOUND_COLLECTION_PAIR&
CSoundCollectionStorage::object(const CSoundCollectionParams& params)
{
	const collection_key key(params);
	const auto found = m_collection_index.find(key);
	if (found != m_collection_index.end())
		return m_objects[found->second];

	const u32 index = m_objects.size();
	m_objects.push_back(std::make_pair(params, xr_new<CSoundCollection>(params)));
	m_collection_index.emplace(key, index);
	return m_objects.back();
}
