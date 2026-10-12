#include "stdafx.h"
#include "../../xrEngine/IGame_Persistent.h"
#pragma hdrstop

#include "ResourceManager.h"
#include "VideoMemoryPolicy.h"
#include "blenders\blender.h"

void fix_texture_name(LPSTR fn);

void CResourceManager::OnDeviceDestroy(BOOL)
{
	if (RDEVICE.b_is_Ready) return;
	m_textures_description.UnLoad();

	// Matrices
	for (map_Matrix::iterator m = m_matrices.begin(); m != m_matrices.end(); m++)
	{
		R_ASSERT(1==m->second->dwReference.load(std::memory_order_relaxed));
		xr_delete(m->second);
	}
	m_matrices.clear();

	// Constants
	for (map_Constant::iterator c = m_constants.begin(); c != m_constants.end(); c++)
	{
		R_ASSERT(1==c->second->dwReference.load(std::memory_order_relaxed));
		xr_delete(c->second);
	}
	m_constants.clear();

	// Release blenders
	for (map_BlenderIt b = m_blenders.begin(); b != m_blenders.end(); b++)
	{
		xr_free((char*&)b->first);
		IBlender::Destroy(b->second);
	}
	m_blenders.clear();

	// destroy TD
	for (map_TDIt _t = m_td.begin(); _t != m_td.end(); _t++)
	{
		xr_free((char*&)_t->first);
		xr_free((char*&)_t->second.T);
		xr_delete(_t->second.cs);
	}
	m_td.clear();

	// scripting
#ifndef _EDITOR
	LS_Unload();
#endif
}

void CResourceManager::OnDeviceCreate(IReader* F)
{
	if (!RDEVICE.b_is_Ready) return;

	string256 name;

#ifndef _EDITOR
	// scripting
	LS_Load();
#endif
	IReader* fs = 0;
	// Load constants
	fs = F->open_chunk(0);
	if (fs)
	{
		while (!fs->eof())
		{
			fs->r_stringZ(name, sizeof(name));
			CConstant* C = _CreateConstant(name);
			C->Load(fs);
		}
		fs->close();
	}

	// Load matrices
	fs = F->open_chunk(1);
	if (fs)
	{
		while (!fs->eof())
		{
			fs->r_stringZ(name, sizeof(name));
			CMatrix* M = _CreateMatrix(name);
			M->Load(fs);
		}
		fs->close();
	}

	// Load blenders
	fs = F->open_chunk(2);
	if (fs)
	{
		IReader* chunk = NULL;
		int chunk_id = 0;

		while ((chunk = fs->open_chunk(chunk_id)) != NULL)
		{
			CBlender_DESC desc;
			chunk->r(&desc, sizeof(desc));
			if (desc.CLS == B_SHADOW_WORLD)
			{
				chunk->close();
				chunk_id += 1;
				continue;
			}
			IBlender* B = IBlender::Create(desc.CLS);
			if (0 == B)
			{
				Msg("! Renderer doesn't support blender '%s'", desc.cName);
			}
			else
			{
				if (B->getDescription().version != desc.version)
				{
					Msg("! Version conflict in shader '%s'", desc.cName);
				}

				chunk->seek(0);
				B->Load(*chunk, desc.version);

				std::pair<map_BlenderIt, bool> I = m_blenders.insert(mk_pair(xr_strdup(desc.cName), B));
				R_ASSERT2(I.second, "shader.xr - found duplicate name!!!");
			}
			chunk->close();
			chunk_id += 1;
		}
		fs->close();
	}

	m_textures_description.Load();
}

void CResourceManager::OnDeviceCreate(LPCSTR shName)
{
#ifdef _EDITOR
	if (!FS.exist(shName)) return;
#endif

	// Check if file is compressed already
	string32 ID = "shENGINE";
	string32 id;
	IReader* F = FS.r_open(shName);
	R_ASSERT2(F, shName);
	F->r(&id, 8);
	if (0 == strncmp(id, ID, 8))
	{
		FATAL("Unsupported blender library. Compressed?");
	}
	OnDeviceCreate(F);
	FS.r_close(F);
}

void CResourceManager::StoreNecessaryTextures()
{
	xr_vector<shared_str> names;
	{
		xrCriticalSectionGuard guard(textureGuard);
		if (!m_necessary.empty())
			return;

		names.reserve(m_textures.size());
		for (map_TextureIt it = m_textures.begin(); it != m_textures.end(); ++it)
		{
			LPCSTR texture_name = it->first;
			if (strstr(texture_name, "\\levels\\")) continue;
			if (!strchr(texture_name, '\\')) continue;
			names.push_back(texture_name);
		}
	}

	xr_vector<ref_texture> necessary;
	necessary.reserve(names.size());
	for (u32 i = 0; i < names.size(); ++i)
	{
		ref_texture texture;
		texture.create(*names[i]);
		necessary.push_back(texture);
	}

	{
		xrCriticalSectionGuard guard(textureGuard);
		if (m_necessary.empty())
			m_necessary.swap(necessary);
	}
}

void CResourceManager::PinTexture(LPCSTR name)
{
	if (!name || !name[0])
		return;

	string_path normalized;
	xr_strcpy(normalized, name);
	fix_texture_name(normalized);

	// Price only existing DDS files. Video/user textures stay on their normal
	// ownership path rather than becoming unpriced session-long cache entries.
	string_path resolved;
	const CLocatorAPI::file* file = FS.exist(resolved, "$game_textures$", normalized, ".dds");
	if (!file)
		return;
	const u64 bytes = file->size_real;
	const u64 budget = static_cast<u64>(_max(0, psTextureWarmupMemoryMb)) * 1024u * 1024u;

	// Runtime pinning must never turn into another synchronous texture load.
	// Retain only an object that is already present in the resource registry.
	xrCriticalSectionGuard guard(textureGuard);
	map_TextureIt found = m_textures.find(LPSTR(normalized));
	if (found == m_textures.end() || !found->second)
		return;

	CTexture* const candidate = found->second;
	for (const warm_texture_entry& retained : m_warm_textures)
		if (retained.texture._get() == candidate)
			return;
	const u64 resident = candidate->ResidentBytes();
	const u64 cost = _max(bytes, resident);
	const u64 limit = video_memory_policy::optional_texture_budget(budget,
		m_video_memory_budget, m_video_memory_usage, m_warm_texture_reclaimable_bytes);
	if (m_warm_texture_bytes > limit || cost > limit - m_warm_texture_bytes)
		return;
	m_warm_textures.push_back({ref_texture(candidate), bytes, Device.dwTimeGlobal});
	m_warm_texture_bytes += cost;
	m_warm_texture_resident_bytes += resident;
	if (candidate->dwReference.load(std::memory_order_relaxed) == 1)
		m_warm_texture_reclaimable_bytes += resident;
}

void CResourceManager::DestroyNecessaryTextures()
{
	xr_vector<ref_texture> necessary;
	{
		xrCriticalSectionGuard guard(textureGuard);
		m_necessary.swap(necessary);
	}
	// Release references outside the registry lock. The final release may call
	// back into _DeleteTexture().
	necessary.clear();
}

void CResourceManager::SetVideoMemoryBudget(u64 budget, u64 usage)
{
	xrCriticalSectionGuard guard(textureGuard);
	m_video_memory_budget = budget;
	m_video_memory_usage = usage;
	// Optional model workers must not add uploads while required resources
	// already consume the safety margin. Demand loads always remain enabled.
	m_allow_speculative_texture_upload.store(!budget || usage < budget - budget / 8u,
		std::memory_order_release);
}

void CResourceManager::TrimWarmTextures(u32 now, bool pressure, bool hard_pressure, u32 max_releases)
{
	if (!max_releases)
		return;
	xr_vector<ref_texture> retired;
	{
		xrCriticalSectionGuard guard(textureGuard);
		m_warm_texture_bytes = m_warm_texture_resident_bytes = m_warm_texture_reclaimable_bytes = 0;
		for (const warm_texture_entry& entry : m_warm_textures)
		{
			const u64 resident = entry.texture->ResidentBytes();
			m_warm_texture_bytes += _max(entry.source_bytes, resident);
			m_warm_texture_resident_bytes += resident;
			if (entry.texture->dwReference.load(std::memory_order_acquire) == 1)
				m_warm_texture_reclaimable_bytes += resident;
		}
		const u64 configured = static_cast<u64>(_max(0, psTextureWarmupMemoryMb)) * 1024u * 1024u;
		const u64 limit = video_memory_policy::optional_texture_budget(configured,
			m_video_memory_budget, m_video_memory_usage, m_warm_texture_reclaimable_bytes);
		for (auto it = m_warm_textures.begin(); it != m_warm_textures.end() && retired.size() < max_releases;)
		{
			CTexture* texture = it->texture._get();
			const u32 age = video_memory_policy::warm_age(now, it->retained_at, texture->last_used_time);
			const bool over_budget = pressure || m_warm_texture_bytes > limit;
			const u32 grace = over_budget ? (hard_pressure ? video_memory_policy::warm_hard_pressure_idle_ms :
				video_memory_policy::warm_pressure_idle_ms) : video_memory_policy::warm_idle_ms;
			// Only retire sole cache ownership. Shader/UI/scene/explicit-prefetch
			// owners are never unloaded or modified by memory maintenance.
			if (age >= grace && texture->dwReference.load(std::memory_order_acquire) == 1)
			{
				// Detach before final Release: workers must not find an entry whose
				// destructor is running. A new texture with this name can publish
				// immediately, without the retired destructor unregistering it.
				const auto registered = m_textures.find(texture->cName.c_str());
				if (registered != m_textures.end() && registered->second == texture)
					m_textures.erase(registered);
				texture->dwFlags &= ~xr_resource_flagged::RF_REGISTERED;
				const u64 resident = texture->ResidentBytes();
				m_warm_texture_bytes -= _max(it->source_bytes, resident);
				m_warm_texture_resident_bytes -= resident;
				// A worker can drop a shared owner after the first accounting pass.
				// That texture was not counted as reclaimable yet; clamp the estimate
				// instead of wrapping an unsigned counter when it now becomes idle.
				m_warm_texture_reclaimable_bytes -= _min(m_warm_texture_reclaimable_bytes, resident);
				retired.push_back(it->texture);
				it = m_warm_textures.erase(it);
			}
			else
				++it;
		}
	}
	// Final release can unregister the texture. Do not hold registry iterators
	// or its lock across destructors; this runs at the pre-render frame boundary.
	retired.clear();
}
