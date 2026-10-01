#include "stdafx.h"
#pragma hdrstop
#include "TextureDescrManager.h"
#include "ETextureParams.h"
#include "profiler.h"
#include "../../xrCore/job_system.h"

// eye-params
float r__dtex_range = 50;

class cl_dt_scaler : public R_constant_setup
{
public:
	float scale;

	cl_dt_scaler(float s) : scale(s)
	{
	};

	virtual void setup(R_constant* C)
	{
		RCache.set_c(C, scale, scale, scale, 1 / r__dtex_range);
	}
};

void fix_texture_thm_name(LPSTR fn)
{
	LPSTR _ext = strext(fn);
	if (_ext &&
		(0 == stricmp(_ext, ".tga") ||
			0 == stricmp(_ext, ".thm") ||
			0 == stricmp(_ext, ".dds") ||
			0 == stricmp(_ext, ".bmp") ||
			0 == stricmp(_ext, ".ogm") ||
            0 == stricmp(_ext, ".gif")))
		*_ext = 0;
}

void CTextureDescrMngr::LoadTHMFile(LPCSTR initial, LPCSTR file_name, map_TD& s_texture_details,
    map_CS& s_detail_scalers)
{
	string_path fn;
	FS.update_path(fn, initial, file_name);
	IReader* F = FS.r_open(fn);
	R_ASSERT3(F, "Cannot open texture metadata", fn);

	xr_strcpy(fn, file_name);
	fix_texture_thm_name(fn);

	R_ASSERT(F->find_chunk(THM_CHUNK_TYPE));
	F->r_u32();

	STextureParams tp;
	tp.Clear();
	tp.Load(*F);
	FS.r_close(F);

	if (STextureParams::ttImage != tp.type && STextureParams::ttTerrain != tp.type &&
		STextureParams::ttNormalMap != tp.type)
		return;

	texture_desc& desc = s_texture_details[fn];
	cl_dt_scaler*& dts = s_detail_scalers[fn];

	if (tp.detail_name.size() && tp.flags.is_any(STextureParams::flDiffuseDetail | STextureParams::flBumpDetail))
	{
		xr_delete(desc.m_assoc);
		desc.m_assoc = xr_new<texture_assoc>();
		desc.m_assoc->detail_name = tp.detail_name;

		if (dts)
			dts->scale = tp.detail_scale;
		else
			dts = xr_new<cl_dt_scaler>(tp.detail_scale);

		desc.m_assoc->usage = 0;
		if (tp.flags.is(STextureParams::flDiffuseDetail))
			desc.m_assoc->usage |= (1 << 0);
		if (tp.flags.is(STextureParams::flBumpDetail))
			desc.m_assoc->usage |= (1 << 1);
	}

	xr_delete(desc.m_spec);
	desc.m_spec = xr_new<texture_spec>();
	desc.m_spec->m_material = tp.material + (tp.material < 4 ? tp.material_weight : 0);
	desc.m_spec->m_use_steep_parallax = false;

	if (tp.bump_mode == STextureParams::tbmUse)
	{
		desc.m_spec->m_bump_name = tp.bump_name;
	}
	else if (tp.bump_mode == STextureParams::tbmUseParallax)
	{
		desc.m_spec->m_bump_name = tp.bump_name;
		desc.m_spec->m_use_steep_parallax = true;
	}
}

void CTextureDescrMngr::MergeTHM(map_TD& texture_details, map_CS& detail_scalers)
{
	for (auto& source : texture_details)
	{
		auto destination = m_texture_details.find(source.first);
		if (destination == m_texture_details.end())
		{
			m_texture_details.insert(mk_pair(source.first, source.second));
			source.second.m_assoc = nullptr;
			source.second.m_spec = nullptr;
			continue;
		}

		// Match the original sequential override semantics: an absent detail
		// association inherits the previous/global association, while material and
		// bump metadata are always replaced by the later THM.
		if (source.second.m_assoc)
		{
			xr_delete(destination->second.m_assoc);
			destination->second.m_assoc = source.second.m_assoc;
			source.second.m_assoc = nullptr;
		}

		xr_delete(destination->second.m_spec);
		destination->second.m_spec = source.second.m_spec;
		source.second.m_spec = nullptr;
	}

	for (auto& source : detail_scalers)
	{
		if (!source.second)
			continue;

		auto destination = m_detail_scalers.find(source.first);
		if (destination == m_detail_scalers.end())
		{
			m_detail_scalers.insert(mk_pair(source.first, source.second));
		}
		else if (destination->second)
		{
			// Shader constant bindings keep this setup pointer. Preserve its identity
			// across metadata reloads and update only the scale value.
			destination->second->scale = source.second->scale;
			xr_delete(source.second);
		}
		else
		{
			destination->second = source.second;
		}

		source.second = nullptr;
	}
}

void CTextureDescrMngr::Load()
{
	FS_FileSet game_files;
	FS_FileSet level_files;
	FS.file_list(game_files, "$game_textures$", FS_ListFiles, "*.thm");
	FS.file_list(level_files, "$level$", FS_ListFiles, "*.thm");

	struct thm_work_item
	{
		LPCSTR initial;
		shared_str file_name;
		bool level_override;
	};

	xr_vector<thm_work_item> items;
	items.reserve(game_files.size() + level_files.size());
	for (const FS_File& file : game_files)
		items.push_back({"$game_textures$", file.name.c_str(), false});
	for (const FS_File& file : level_files)
		items.push_back({"$level$", file.name.c_str(), true});

	if (items.empty())
		return;

	constexpr u32 max_jobs = 8;
	const u32 useful_jobs = _max(1u, static_cast<u32>((items.size() + 31) / 32));
	const bool parallel_thm = !(Core.Params && strstr(Core.Params, "-no_mt_thm"));
	const u32 job_count = parallel_thm
		? _min(max_jobs, _min(xr_jobs::available_thread_count(), useful_jobs))
		: 1u;

	struct thm_job_context
	{
		const xr_vector<thm_work_item>* items = nullptr;
		volatile LONG* next_item = nullptr;
		map_TD game_details;
		map_CS game_scalers;
		map_TD level_details;
		map_CS level_scalers;
	};

	thm_job_context contexts[max_jobs];
	__declspec(align(64)) volatile LONG next_item = 0;
	for (u32 index = 0; index < job_count; ++index)
	{
		contexts[index].items = &items;
		contexts[index].next_item = &next_item;
	}

	auto load_job = [](void* parameter)
	{
		thm_job_context& context = *static_cast<thm_job_context*>(parameter);
		for (;;)
		{
			const LONG item_index = InterlockedIncrement(context.next_item) - 1;
			if (item_index < 0 || static_cast<size_t>(item_index) >= context.items->size())
				break;

			const thm_work_item& item = (*context.items)[item_index];
			if (item.level_override)
				CTextureDescrMngr::LoadTHMFile(item.initial, item.file_name.c_str(),
					context.level_details, context.level_scalers);
			else
				CTextureDescrMngr::LoadTHMFile(item.initial, item.file_name.c_str(),
					context.game_details, context.game_scalers);
		}
	};

	xr_jobs::task_group group;
	for (u32 index = 1; index < job_count; ++index)
		xr_jobs::submit(load_job, &contexts[index], &group, xr_jobs::priority::normal);

	load_job(&contexts[0]);
	xr_jobs::wait(group);

	// Preserve the original override contract regardless of worker completion order:
	// global metadata is merged first, then level-specific metadata replaces it.
	for (u32 index = 0; index < job_count; ++index)
		MergeTHM(contexts[index].game_details, contexts[index].game_scalers);
	for (u32 index = 0; index < job_count; ++index)
		MergeTHM(contexts[index].level_details, contexts[index].level_scalers);
}

void CTextureDescrMngr::UnLoad()
{
	for (auto& it : m_texture_details)
	{
		xr_delete(it.second.m_assoc);
		xr_delete(it.second.m_spec);
	}
	m_texture_details.clear();
}

CTextureDescrMngr::~CTextureDescrMngr()
{
	for (auto& it : m_detail_scalers)
		xr_delete(it.second);
	m_detail_scalers.clear();
}

shared_str CTextureDescrMngr::GetBumpName(const shared_str& tex_name) const
{
	map_TD::const_iterator I = m_texture_details.find(tex_name);
	if (I != m_texture_details.end())
	{
		if (I->second.m_spec)
		{
			return I->second.m_spec->m_bump_name;
		}
	}
	return "";
}

BOOL CTextureDescrMngr::UseSteepParallax(const shared_str& tex_name) const
{
	map_TD::const_iterator I = m_texture_details.find(tex_name);
	if (I != m_texture_details.end())
	{
		if (I->second.m_spec)
		{
			return I->second.m_spec->m_use_steep_parallax;
		}
	}
	return FALSE;
}

float CTextureDescrMngr::GetMaterial(const shared_str& tex_name) const
{
	map_TD::const_iterator I = m_texture_details.find(tex_name);
	if (I != m_texture_details.end())
	{
		if (I->second.m_spec)
		{
			return I->second.m_spec->m_material;
		}
	}
	return 1.0f;
}

void CTextureDescrMngr::GetTextureUsage(const shared_str& tex_name, BOOL& bDiffuse, BOOL& bBump) const
{
	map_TD::const_iterator I = m_texture_details.find(tex_name);
	if (I != m_texture_details.end())
	{
		if (I->second.m_assoc)
		{
			u8 usage = I->second.m_assoc->usage;
			bDiffuse = !!(usage & (1 << 0));
			bBump = !!(usage & (1 << 1));
		}
	}
}

BOOL CTextureDescrMngr::GetDetailTexture(const shared_str& tex_name, LPCSTR& res, R_constant_setup* & CS) const
{
	map_TD::const_iterator I = m_texture_details.find(tex_name);
	if (I != m_texture_details.end())
	{
		if (I->second.m_assoc)
		{
			texture_assoc* TA = I->second.m_assoc;
			res = TA->detail_name.c_str();
			map_CS::const_iterator It2 = m_detail_scalers.find(tex_name);
			CS = It2 == m_detail_scalers.end() ? 0 : It2->second; //TA->cs;
			return TRUE;
		}
	}
	return FALSE;
}
