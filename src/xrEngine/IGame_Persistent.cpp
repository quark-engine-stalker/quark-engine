#include "stdafx.h"
#pragma hdrstop

#include "IGame_Persistent.h"
#include "../xrCore/adaptive_memory_policy.h"

#ifndef _EDITOR
#include "environment.h"
# include "x_ray.h"
# include "IGame_Level.h"
# include "XR_IOConsole.h"
# include "Render.h"
# include "ps_instance.h"
# include "CustomHUD.h"
# include "perlin.h"
#endif

#ifdef _EDITOR
bool g_dedicated_server = false;
#endif

#ifdef INGAME_EDITOR
# include "editor_environment_manager.hpp"
#endif // INGAME_EDITOR

extern Fvector4 ps_ssfx_grass_interactive;

ENGINE_API IGame_Persistent* g_pGamePersistent = NULL;
ENGINE_API BOOL psTextureWarmupManifest = TRUE;
ENGINE_API BOOL psTextureWarmupUiOnly = TRUE;
ENGINE_API int psTextureWarmupSlowMs = 4;
ENGINE_API int psTextureWarmupMax = 1024;

namespace
{
constexpr u32 texture_warmup_magic = 0x4D575554; // TUWM
constexpr u32 texture_warmup_version = 1;
constexpr u32 texture_warmup_max_file_size = 1024u * 1024u;

void build_texture_warmup_path(string_path& result)
{
	FS.update_path(result, "$app_data_root$", "cache\\texture_prefetch\\runtime_ui.pmf");
}

bool read_texture_name_safe(IReader& reader, xr_string& value)
{
	const int remaining = reader.elapsed();
	if (remaining <= 0)
		return false;

	const char* const begin = static_cast<const char*>(reader.pointer());
	const char* const terminator = static_cast<const char*>(memchr(begin, 0, static_cast<size_t>(remaining)));
	if (!terminator)
		return false;

	const size_t length = static_cast<size_t>(terminator - begin);
	if (!length || length >= sizeof(string_path))
		return false;

	value.assign(begin, length);
	reader.advance(static_cast<int>(length + 1));
	return true;
}

void normalize_texture_warmup_name(LPCSTR source, string_path& destination)
{
	xr_strcpy(destination, source ? source : "");
	xr_strlwr(destination);
	LPSTR extension = strext(destination);
	if (extension &&
		(!xr_strcmp(extension, ".dds") || !xr_strcmp(extension, ".tga") ||
		 !xr_strcmp(extension, ".bmp") || !xr_strcmp(extension, ".ogm") ||
		 !xr_strcmp(extension, ".gif")))
	{
		*extension = 0;
	}
}

bool runtime_ui_texture_exists(LPCSTR name)
{
	string_path resolved;
	return name && name[0] && FS.exist(resolved, "$game_textures$", name, ".dds");
}

bool is_runtime_ui_texture(LPCSTR name)
{
	if (!name || !name[0] || name[0] == '$' || strstr(name, "..") || strchr(name, ':'))
		return false;
	if (!psTextureWarmupUiOnly)
		return true;

	// Several GAMMA main-menu atlases are deliberately stored at the texture
	// root (for example gammamainmenulogo/logogamma) instead of under ui\\.
	// Restrict the exception to root-level logo names so world texture retention
	// remains bounded when texture_warmup_ui_only is enabled.
	const bool root_level_logo = !strchr(name, '\\') && !strchr(name, '/') && strstr(name, "logo");
	return !strncmp(name, "ui\\", 3) || !strncmp(name, "ui_", 3) ||
		strstr(name, "\\ui\\") || strstr(name, "\\ui_") ||
		!strncmp(name, "catsy\\", 6) || root_level_logo;
}
}

bool IsMainMenuActive()
{
	return g_pGamePersistent && g_pGamePersistent->m_pMainMenu && g_pGamePersistent->m_pMainMenu->IsActive();
} //ECO_RENDER add

IGame_Persistent::IGame_Persistent()
{
	grass_shader_data.render_bender_count = 0;
	grass_shader_data.render_frame = u32(-1);
	RDEVICE.seqAppStart.Add(this);
	RDEVICE.seqAppEnd.Add(this);
	RDEVICE.seqFrame.Add(this, REG_PRIORITY_HIGH + 1);
	RDEVICE.seqAppActivate.Add(this);
	RDEVICE.seqAppDeactivate.Add(this);
	m_pGShaderConstants = new ShadersExternalData(); //--#SM+#--

	m_pMainMenu = NULL;
	m_texture_warmup_dirty = false;
	m_texture_warmup_unique.reserve(1024);
	m_texture_warmup_order.reserve(1024);
	m_texture_warmup_pending_pins.reserve(64);

	PerlinNoise1D = xr_new<CPerlinNoise1D>(Random.randI(0, 0xFFFF));
	PerlinNoise1D->SetOctaves(2);
	PerlinNoise1D->SetAmplitude(0.66666f);

#ifndef INGAME_EDITOR
#ifndef _EDITOR
	pEnvironment = xr_new<CEnvironment>();
#endif
#else // #ifdef INGAME_EDITOR
    if (RDEVICE.editor())
        pEnvironment = xr_new<editor::environment::manager>();
    else
        pEnvironment = xr_new<CEnvironment>();
#endif // #ifdef INGAME_EDITOR
}

IGame_Persistent::~IGame_Persistent()
{
	xr_delete(PerlinNoise1D);
	RDEVICE.seqFrame.Remove(this);
	RDEVICE.seqAppStart.Remove(this);
	RDEVICE.seqAppEnd.Remove(this);
	RDEVICE.seqAppActivate.Remove(this);
	RDEVICE.seqAppDeactivate.Remove(this);
#ifndef _EDITOR
	xr_delete(pEnvironment);
#endif
	xr_delete(m_pGShaderConstants); //--#SM+#--

	VERIFY(m_textures_prefetch_config);
	CInifile::Destroy(m_textures_prefetch_config);
	m_textures_prefetch_config = 0;
}

void IGame_Persistent::OnAppActivate()
{}

void IGame_Persistent::OnAppDeactivate()
{}

void IGame_Persistent::OnAppStart()
{
#ifndef _EDITOR
	Environment().load();
#endif

	// Texture Prefetch Config
	string_path file_name;
	m_textures_prefetch_config =
		xr_new<CInifile>(
			FS.update_path(
				file_name,
				"$game_config$",
				"prefetch\\textures.ltx"
			),
			TRUE,
			TRUE,
			FALSE
			);
}

void IGame_Persistent::OnAppEnd()
{
	SaveTextureWarmupManifest();
#ifndef _EDITOR
	Environment().unload();
#endif
	OnGameEnd();

#ifndef _EDITOR
	DEL_INSTANCE(g_hud);
#endif
}

void IGame_Persistent::PreStart(LPCSTR op)
{
	string256 prev_type;
	params new_game_params;
	xr_strcpy(prev_type, m_game_params.m_game_type);
	new_game_params.parse_cmd_line(op);

	// change game type
	if (0 != xr_strcmp(prev_type, new_game_params.m_game_type))
	{
		OnGameEnd();
	}
}

void IGame_Persistent::Start(LPCSTR op)
{
	string256 prev_type;
	xr_strcpy(prev_type, m_game_params.m_game_type);
	m_game_params.parse_cmd_line(op);
	// change game type
	if ((0 != xr_strcmp(prev_type, m_game_params.m_game_type)))
	{
		if (*m_game_params.m_game_type)
			OnGameStart();
#ifndef _EDITOR
		if (g_hud)
		DEL_INSTANCE(g_hud);
#endif
	}
	else UpdateGameType();

	VERIFY(ps_destroy.empty());
}

void IGame_Persistent::Disconnect()
{
	InvalidateGrassFrameState();
#ifndef _EDITOR
	// clear "need to play" particles
	destroy_particles(true);

	if (g_hud)
	DEL_INSTANCE(g_hud);
	//. g_hud->OnDisconnected ();
#endif
}

void IGame_Persistent::OnGameStart()
{
	InvalidateGrassFrameState();
#ifndef _EDITOR
	// LoadTitle("st_prefetching_objects");
	LoadTitle();
	if (!strstr(Core.Params, "-noprefetch"))
		Prefetch();
#endif
}

void IGame_Persistent::LoadTextureWarmupManifest(xr_vector<shared_str>& names)
{
	if (!psTextureWarmupManifest)
		return;

	string_path manifest_path;
	build_texture_warmup_path(manifest_path);
	IReader* manifest = FS.r_open(manifest_path);
	if (!manifest)
		return;

	const u32 maximum = static_cast<u32>(_max(psTextureWarmupMax, 1));
	if (manifest->length() < static_cast<int>(sizeof(u32) * 3) ||
		static_cast<u32>(manifest->length()) > texture_warmup_max_file_size)
	{
		FS.r_close(manifest);
		return;
	}

	const u32 magic = manifest->r_u32();
	const u32 version = manifest->r_u32();
	const u32 count = manifest->r_u32();
	if (magic != texture_warmup_magic || version != texture_warmup_version || count > maximum)
	{
		FS.r_close(manifest);
		return;
	}

	names.reserve(names.size() + count);
	for (u32 index = 0; index < count; ++index)
	{
		xr_string value;
		if (!read_texture_name_safe(*manifest, value))
			break;

		string_path normalized;
		normalize_texture_warmup_name(value.c_str(), normalized);
		if (!is_runtime_ui_texture(normalized) || !runtime_ui_texture_exists(normalized))
			continue;

		const shared_str name = normalized;
		bool inserted = false;
		{
			xrCriticalSectionGuard guard(m_texture_warmup_guard);
			inserted = m_texture_warmup_unique.emplace(name).second;
			if (inserted)
				m_texture_warmup_order.push_back(name);
		}
		if (inserted)
			names.push_back(name);
	}
	FS.r_close(manifest);
}

void IGame_Persistent::SaveTextureWarmupManifest()
{
	if (!psTextureWarmupManifest)
		return;

	xr_vector<shared_str> names;
	{
		xrCriticalSectionGuard guard(m_texture_warmup_guard);
		if (!m_texture_warmup_dirty)
			return;
		names = m_texture_warmup_order;
		m_texture_warmup_dirty = false;
	}

	const size_t maximum = static_cast<size_t>(_max(psTextureWarmupMax, 1));
	if (names.size() > maximum)
		names.resize(maximum);

	string_path manifest_path;
	build_texture_warmup_path(manifest_path);
	VerifyPath(manifest_path);
	IWriter* writer = FS.w_open(manifest_path);
	if (!writer)
	{
		xrCriticalSectionGuard guard(m_texture_warmup_guard);
		m_texture_warmup_dirty = true;
		return;
	}

	writer->w_u32(texture_warmup_magic);
	writer->w_u32(texture_warmup_version);
	writer->w_u32(static_cast<u32>(names.size()));
	for (const shared_str& name : names)
		writer->w_stringZ(name.c_str());
	FS.w_close(writer);
	Msg("* [texture-warmup] saved %u slow UI textures", static_cast<u32>(names.size()));
}

void IGame_Persistent::RecordSlowTexture(LPCSTR texture_name, u32 elapsed_ms)
{
	if (!psTextureWarmupManifest || elapsed_ms < static_cast<u32>(_max(psTextureWarmupSlowMs, 1)))
		return;

	string_path normalized;
	normalize_texture_warmup_name(texture_name, normalized);
	if (!is_runtime_ui_texture(normalized))
		return;

	const shared_str name = normalized;
	{
		xrCriticalSectionGuard guard(m_texture_warmup_guard);
		if (m_texture_warmup_unique.size() >= static_cast<size_t>(_max(psTextureWarmupMax, 1)))
			return;
		if (!m_texture_warmup_unique.emplace(name).second)
			return;

		m_texture_warmup_order.push_back(name);
		m_texture_warmup_pending_pins.push_back(name);
		m_texture_warmup_dirty = true;
	}
	Msg("* [texture-warmup] learned '%s' (%u ms)", normalized, elapsed_ms);
}

void IGame_Persistent::ProcessTextureWarmupPins()
{
#ifndef _EDITOR
	if (!Device.m_pRender)
		return;

	xr_vector<shared_str> pending;
	{
		xrCriticalSectionGuard guard(m_texture_warmup_guard);
		pending.swap(m_texture_warmup_pending_pins);
	}
	for (const shared_str& name : pending)
		Device.m_pRender->ResourcesPinTexture(name.c_str());
#endif
}

#ifndef _EDITOR
void IGame_Persistent::Prefetch()
{
	if (!strstr(Core.Params, "-no_adaptive_memory") &&
		(psTextureWarmupMax == 1024 || psTextureWarmupSlowMs == 4))
	{
		const xr_memory_policy::snapshot memory_policy = xr_memory_policy::query();
		if (psTextureWarmupMax == 1024)
			psTextureWarmupMax = static_cast<int>(xr_memory_policy::texture_warmup_max(memory_policy));
		if (psTextureWarmupSlowMs == 4)
			psTextureWarmupSlowMs = static_cast<int>(xr_memory_policy::texture_warmup_slow_ms(memory_policy));
		Msg("* [memory-policy] texture warmup retention=%d entries (%s)",
			psTextureWarmupMax, xr_memory_policy::tier_name(memory_policy.mode));
		Msg("* [memory-policy] texture warmup slow threshold=%d ms", psTextureWarmupSlowMs);
	}

	Msg("* [x-ray]: Prefetching Data");
	// Detailed heap statistics use _heapwalk and are intentionally disabled in
	// normal builds because they serialize the allocator during startup.
	const bool load_diagnostics = IsLoadDiagnosticsEnabled();
	const float p_time_start = load_diagnostics ? 1000.f * Device.GetTimerGlobal()->GetElapsed_sec() : 0.f;
	const size_t mem_0 = load_diagnostics ? Memory.mem_usage() : 0;

	Log("Loading objects...");
	ObjectPool.prefetch();
	Log("Loading models...");
	Render->models_Prefetch();
	Log("Loading textures...");

	// Folder masks and explicit prefetch entries often overlap. Preserve the
	// original first-seen order while avoiding repeated registry lookups and
	// temporary texture references for the same normalized name.
	xr_vector<shared_str> texture_prefetch_names;
	xr_unordered_set<shared_str> texture_prefetch_unique;
	texture_prefetch_names.reserve(4096);
	texture_prefetch_unique.reserve(4096);

	const auto queueTexture = [&](LPCSTR texture_name)
	{
		if (!texture_name || !texture_name[0])
			return;

		string_path normalized;
		xr_strcpy(normalized, texture_name);
		strlwr(normalized);
		LPSTR extension = strext(normalized);
		if (extension &&
			(!xr_strcmp(extension, ".dds") || !xr_strcmp(extension, ".tga") ||
			 !xr_strcmp(extension, ".bmp") || !xr_strcmp(extension, ".ogm") ||
			 !xr_strcmp(extension, ".gif")))
		{
			*extension = 0;
		}

		const shared_str key = normalized;
		if (texture_prefetch_unique.emplace(key).second)
			texture_prefetch_names.push_back(key);
	};

	xr_vector<shared_str> learned_texture_names;
	LoadTextureWarmupManifest(learned_texture_names);
	for (const shared_str& texture_name : learned_texture_names)
		queueTexture(texture_name.c_str());

	const auto loadFileFolder = [&](LPCSTR _folder)
	{
		string_path folder;
		strconcat(sizeof(folder), folder, _folder, "\\*.dds");

		FS_FileSet fset;
		FS.file_list(fset, "$game_textures$", FS_ListFiles, folder);

		for (FS_FileSet::iterator it = fset.begin(); it != fset.end(); ++it)
			queueTexture(it->name.c_str());
	};

	if (m_textures_prefetch_config->section_exist("prefetch_folders"))
	{
		CInifile::Sect const& sect_f = m_textures_prefetch_config->r_section("prefetch_folders");
		for (CInifile::SectCIt I = sect_f.Data.begin(); I != sect_f.Data.end(); I++)
		{
			if (I->second.size() && !xr_strcmp(*I->second, "*"))
			{
				string_path folder;
				FS.update_path(folder, "$game_textures$", *I->first);
				xr_strcat(folder, sizeof(folder), "\\");

				xr_vector<LPSTR> *subfolders = FS.file_list_open(folder, FS_ListFolders);

				if (subfolders == nullptr)
					continue;

				for (LPSTR subfolder : *subfolders)
				{
					string_path path;
					strconcat(sizeof(path), path, folder, subfolder);

					loadFileFolder(path);
				}

				FS.file_list_close(subfolders);
			}

			loadFileFolder(*I->first);
		}
	}

	if (m_textures_prefetch_config->section_exist("prefetch_textures"))
	{
		CInifile::Sect const& sect = m_textures_prefetch_config->r_section("prefetch_textures");
		for (CInifile::SectCIt I = sect.Data.begin(); I != sect.Data.end(); ++I)
			queueTexture(I->first.c_str());
	}

	for (const shared_str& texture_name : texture_prefetch_names)
		Device.m_pRender->ResourcesPrefetchCreateTexture(texture_name.c_str());
	for (const shared_str& texture_name : learned_texture_names)
		Device.m_pRender->ResourcesPinTexture(texture_name.c_str());

	Device.m_pRender->ResourcesDeferredUpload();
	if (!learned_texture_names.empty())
		Msg("* [texture-warmup] prefetched and retained %u learned UI textures",
			static_cast<u32>(learned_texture_names.size()));

	Msg("* [x-ray]: Prefetched Data");
	if (load_diagnostics)
	{
		const float p_time = 1000.f * Device.GetTimerGlobal()->GetElapsed_sec() - p_time_start;
		const size_t p_mem = Memory.mem_usage() - mem_0;
		Msg("* [prefetch] time:   %d ms", iFloor(p_time));
		Msg("* [prefetch] memory: %lldKb", p_mem / 1024);
	}
}
#endif

void IGame_Persistent::OnGameEnd()
{
	InvalidateGrassFrameState();
	SaveTextureWarmupManifest();
#ifndef _EDITOR
	ObjectPool.clear();
	Render->models_Clear(TRUE);
#endif
}

void IGame_Persistent::OnFrame()
{
#ifndef _EDITOR

	ProcessTextureWarmupPins();

	if (!Device.Paused() || Device.dwPrecacheFrame)
		Environment().OnFrame();

	Device.Statistic->Particles_starting = ps_needtoplay.size();
	Device.Statistic->Particles_active = ps_active.size();
	Device.Statistic->Particles_destroy = ps_destroy.size();

	// Play req particle systems
	while (ps_needtoplay.size())
	{
		CPS_Instance* psi = ps_needtoplay.back();
		ps_needtoplay.pop_back();
		psi->Play(false);
	}

	// Destroy inactive particle systems
	while (ps_destroy.size())
	{
		// u32 cnt = ps_destroy.size();
		CPS_Instance* psi = ps_destroy.back();
		VERIFY(psi);
		if (psi->Locked())
		{
			Log("--locked");
			break;
		}
		ps_destroy.pop_back();
		psi->PSI_internal_delete();
	}

#endif
}

void IGame_Persistent::destroy_particles(const bool& all_particles)
{
#ifndef _EDITOR
	ps_needtoplay.clear();

	while (ps_destroy.size())
	{
		CPS_Instance* psi = ps_destroy.back();
		VERIFY(psi);
		VERIFY(!psi->Locked());
		ps_destroy.pop_back();
		psi->PSI_internal_delete();
	}

	// delete active particles
	if (all_particles)
	{
		for (; !ps_active.empty();)
			(*ps_active.begin())->PSI_internal_delete();
	}
	else
	{
		u32 active_size = ps_active.size();
		CPS_Instance** I = (CPS_Instance**)_alloca(active_size * sizeof(CPS_Instance*));
		std::copy(ps_active.begin(), ps_active.end(), I);

		struct destroy_on_game_load
		{
			static IC bool predicate(CPS_Instance* const& object)
			{
				return (!object->destroy_on_game_load());
			}
		};

		CPS_Instance** E = std::remove_if(I, I + active_size, &destroy_on_game_load::predicate);
		for (; I != E; ++I)
			(*I)->PSI_internal_delete();
	}

	VERIFY(ps_needtoplay.empty() && ps_destroy.empty() && (!all_particles || ps_active.empty()));
#endif
}

void IGame_Persistent::OnAssetsChanged()
{
#ifndef _EDITOR
	Device.m_pRender->OnAssetsChanged(); //Resources->m_textures_description.Load();
#endif
}

void IGame_Persistent::GrassBendersUpdate(u16 id, u8& data_idx, u32& data_frame, Fvector& position, float init_radius, float init_str, bool CheckDistance)
{
	// Interactive grass disabled
	if (ps_ssfx_grass_interactive.y < 1)
		return;

	// Just update position if not NULL
	if (data_idx != NULL)
	{
		// Explosions can take the mem spot, unassign and try to get a spot later.
		if (grass_shader_data.id[data_idx] != id)
		{
			data_idx = NULL;
			data_frame = RDEVICE.dwFrame + Random.randI(10, 35);
		}
		else
		{
			grass_shader_data.pos[data_idx] = position;
		}
	}

	if (RDEVICE.dwFrame < data_frame)
		return;

	// Wait some random frames to split the checks
	data_frame = RDEVICE.dwFrame + Random.randI(10, 35);

	// Check Distance
	if (CheckDistance)
	{
		if (position.distance_to_xz_sqr(Device.vCameraPosition) > ps_ssfx_grass_interactive.z)
		{
			GrassBendersRemoveByIndex(data_idx);
			return;
		}
	}

	CFrustum& view_frust = ::Render->ViewBase;
	u32 mask = 0xff;
	float rad = data_idx == NULL ? 1.0 : std::max(1.0f, grass_shader_data.radius_curr[data_idx] + 0.5f);

	// In view frustum?
	if (!view_frust.testSphere(position, rad, mask))
	{
		GrassBendersRemoveByIndex(data_idx);
		return;
	}

	// Empty slot, let's use this
	if (data_idx == NULL)
	{
		u8 idx = grass_shader_data.index + 1;

		// Add to grass blenders array
		if (grass_shader_data.id[idx] == NULL)
		{
			data_idx = idx;
			GrassBendersSet(idx, id, position, Fvector3().set(0, -99, 0), 0, 0, 0.0f, init_radius, BENDER_ANIM_DEFAULT, true);

			grass_shader_data.str_target[idx] = init_str;
			grass_shader_data.radius_curr[idx] = init_radius;
		}
		// Back to 0 when the array limit is reached
		grass_shader_data.index = idx < ps_ssfx_grass_interactive.y ? idx : 0;
	}
	else
	{
		// Already inview, let's add more time to re-check
		data_frame += 60;
		grass_shader_data.pos[data_idx] = position;
	}
}

void IGame_Persistent::GrassBendersAddExplosion(u16 id, Fvector position, Fvector3 dir, float fade, float speed, float intensity, float radius)
{
	if (ps_ssfx_grass_interactive.y < 1)
		return;

	for (int idx = 1; idx < ps_ssfx_grass_interactive.y + 1; idx++)
	{
		// Add explosion to any spot not already taken by an explosion.
		if (grass_shader_data.anim[idx] != BENDER_ANIM_EXPLOSION)
		{
			// Add 99 to the ID to avoid conflicts between explosions and basic benders happening at the same time with the same ID.
			GrassBendersSet(idx, id + 99, position, dir, fade, speed, intensity, radius, BENDER_ANIM_EXPLOSION, true);
			grass_shader_data.str_target[idx] = intensity;
			break;
		}
	}
}

void IGame_Persistent::GrassBendersAddShot(u16 id, Fvector position, Fvector3 dir, float fade, float speed, float intensity, float radius)
{
	// Is disabled?
	if (ps_ssfx_grass_interactive.y < 1 || intensity <= 0.0f)
		return;

	// Check distance
	if (position.distance_to_xz_sqr(Device.vCameraPosition) > ps_ssfx_grass_interactive.z)
		return;

	int AddAt = -1;

	// Look for a spot
	for (int idx = 1; idx < ps_ssfx_grass_interactive.y + 1; idx++)
	{
		// Already exist, just update and increase intensity
		if (grass_shader_data.id[idx] == id)
		{
			float currentSTR = grass_shader_data.str[idx];
			GrassBendersSet(idx, id, position, dir, fade, speed, currentSTR, radius, BENDER_ANIM_EXPLOSION, false);
			grass_shader_data.str_target[idx] += intensity;
			AddAt = -1;
			break;
		}
		else
		{
			// Check all indexes and keep usable index to use later if needed...
			if (AddAt == -1 && grass_shader_data.radius[idx] == NULL)
				AddAt = idx;
		}
	}

	// We got an available index... Add bender at AddAt
	if (AddAt != -1)
	{
		GrassBendersSet(AddAt, id, position, dir, fade, speed, 0.001f, radius, BENDER_ANIM_EXPLOSION, true);
		grass_shader_data.str_target[AddAt] = intensity;
	}
}

void IGame_Persistent::GrassBendersUpdateAnimations()
{
	for (int idx = 1; idx < ps_ssfx_grass_interactive.y + 1; idx++)
	{
		if (grass_shader_data.id[idx] != NULL)
		{
			switch (grass_shader_data.anim[idx])
			{
			case BENDER_ANIM_EXPLOSION: // Internal Only ( You can use BENDER_ANIM_PULSE for anomalies )
			{
				// Radius
				grass_shader_data.time[idx] += Device.fTimeDelta * grass_shader_data.speed[idx];
				grass_shader_data.radius_curr[idx] = grass_shader_data.radius[idx] * std::min(1.0f, grass_shader_data.time[idx]);

				grass_shader_data.str_target[idx] = std::min(1.0f, grass_shader_data.str_target[idx]);

				// Easing
				float diff = abs(grass_shader_data.str[idx] - grass_shader_data.str_target[idx]);
				diff = std::max(0.1f, diff);

				// Intensity
				if (grass_shader_data.str_target[idx] <= grass_shader_data.str[idx])
				{
					grass_shader_data.str[idx] -= Device.fTimeDelta * grass_shader_data.fade[idx] * diff;
				}
				else
				{
					grass_shader_data.str[idx] += Device.fTimeDelta * grass_shader_data.speed[idx] * diff;

					if (grass_shader_data.str[idx] >= grass_shader_data.str_target[idx])
						grass_shader_data.str_target[idx] = 0;
				}

				// Remove Bender
				if (grass_shader_data.str[idx] < 0.0f)
					GrassBendersReset(idx);
			}
			break;

			case BENDER_ANIM_WAVY:
			{
				// Anim Speed
				grass_shader_data.time[idx] += Device.fTimeDelta * 1.5f * grass_shader_data.speed[idx];

				// Curve
				float curve = sin(grass_shader_data.time[idx]);

				// Intensity using curve
				grass_shader_data.str[idx] = curve * cos(curve * 1.4f) * 1.8f * grass_shader_data.str_target[idx];
			}

			break;

			case BENDER_ANIM_SUCK:
			{
				// Anim Speed
				grass_shader_data.time[idx] += Device.fTimeDelta * grass_shader_data.speed[idx];

				// Perlin Noise
				float curve = clampr(PerlinNoise1D->GetContinious(grass_shader_data.time[idx]) + 0.5f, 0.f, 1.f) * -1.0;

				// Intensity using Perlin
				grass_shader_data.str[idx] = curve * grass_shader_data.str_target[idx];
			}
			break;

			case BENDER_ANIM_BLOW:
			{
				// Anim Speed
				grass_shader_data.time[idx] += Device.fTimeDelta * 1.2f * grass_shader_data.speed[idx];

				// Perlin Noise
				float curve = clampr(PerlinNoise1D->GetContinious(grass_shader_data.time[idx]) + 1.0f, 0.f, 2.0f) * 0.25f;

				// Intensity using Perlin
				grass_shader_data.str[idx] = curve * grass_shader_data.str_target[idx];
			}
			break;

			case BENDER_ANIM_PULSE:
			{
				// Anim Speed
				grass_shader_data.time[idx] += Device.fTimeDelta * grass_shader_data.speed[idx];

				// Radius
				grass_shader_data.radius_curr[idx] = grass_shader_data.radius[idx] * std::min(1.0f, grass_shader_data.time[idx]);

				// Diminish intensity when radius target is reached
				if (grass_shader_data.radius_curr[idx] >= grass_shader_data.radius[idx])
					grass_shader_data.str[idx] += GrassBenderToValue(grass_shader_data.str[idx], 0.0f, grass_shader_data.speed[idx] * 0.6f, true);

				// Loop when intensity is <= 0
				if (grass_shader_data.str[idx] <= 0.0f)
				{
					grass_shader_data.str[idx] = grass_shader_data.str_target[idx];
					grass_shader_data.radius_curr[idx] = 0.0f;
					grass_shader_data.time[idx] = 0.0f;
				}

			}
			break;

			case BENDER_ANIM_DEFAULT:

				// Just fade to target strength
				grass_shader_data.str[idx] += GrassBenderToValue(grass_shader_data.str[idx], grass_shader_data.str_target[idx], 2.0f, true);

				break;
			}
		}
	}
}

void IGame_Persistent::GrassBendersRemoveByIndex(u8& idx)
{
	if (idx != NULL)
	{
		GrassBendersReset(idx);
		idx = NULL;
	}
}

void IGame_Persistent::GrassBendersRemoveById(u16 id)
{
	// Search by Object ID ( Used when removing benders CPHMovementControl::DestroyCharacter() )
	for (int i = 1; i < ps_ssfx_grass_interactive.y + 1; i++)
		if (grass_shader_data.id[i] == id)
			GrassBendersReset(i);
}

void IGame_Persistent::GrassBendersReset(u8 idx)
{
	// Reset Everything
	GrassBendersSet(idx, NULL, Fvector3().set(0, 0, 0), Fvector3().set(0, -99, 0), 0, 0, 0, 0, BENDER_ANIM_DEFAULT, true);
	grass_shader_data.str_target[idx] = 0;
}

void IGame_Persistent::GrassBendersSet(u8 idx, u16 id, Fvector position, Fvector3 dir, float fade, float speed, float intensity, float radius, GrassBenders_Anim anim, bool resetTime)
{
	// Set values
	grass_shader_data.anim[idx] = anim;
	grass_shader_data.pos[idx] = position;
	grass_shader_data.id[idx] = id;
	grass_shader_data.radius[idx] = radius;
	grass_shader_data.str[idx] = intensity;
	grass_shader_data.fade[idx] = fade;
	grass_shader_data.speed[idx] = speed;
	grass_shader_data.dir[idx] = dir;

	if (resetTime)
	{
		grass_shader_data.radius_curr[idx] = 0.01f;
		grass_shader_data.time[idx] = 0;
	}
}

void IGame_Persistent::InvalidateGrassFrameState()
{
	grass_shader_data.render_bender_count = 0;
	grass_shader_data.render_frame = u32(-1);
}

void IGame_Persistent::PrepareGrassFrameState()
{
	if (grass_shader_data.render_frame == RDEVICE.dwFrame)
		return;

	const u32 bender_count = ps_ssfx_grass_interactive.y > 0 ?
		u32(_min(16, ps_ssfx_grass_interactive.y + 1)) : 0;
	grass_shader_data.render_bender_count = bender_count;
	if (bender_count)
	{
		Fvector4 player_position = { 0.f, 0.f, 0.f, 0.f };
		if (ps_ssfx_grass_interactive.x > 0)
			player_position.set(Device.vCameraPosition.x, Device.vCameraPosition.y,
				Device.vCameraPosition.z, -1.f);

		grass_shader_data.render_benders[0].set(player_position);
		grass_shader_data.render_benders[16].set(0.f, -99.f, 0.f, 1.f);
		for (u32 bend = 1; bend < bender_count; ++bend)
		{
			grass_shader_data.render_benders[bend].set(grass_shader_data.pos[bend].x,
				grass_shader_data.pos[bend].y, grass_shader_data.pos[bend].z,
				grass_shader_data.radius_curr[bend]);
			grass_shader_data.render_benders[bend + 16].set(grass_shader_data.dir[bend].x,
				grass_shader_data.dir[bend].y, grass_shader_data.dir[bend].z,
				grass_shader_data.str[bend]);
		}

		const u32 bender_bytes = bender_count * sizeof(Fvector4);
		CopyMemory(grass_shader_data.render_previous_benders, grass_shader_data.prev_pos, bender_bytes);
		CopyMemory(grass_shader_data.render_previous_benders + 16, grass_shader_data.prev_dir, bender_bytes);
	}

	grass_shader_data.render_frame = RDEVICE.dwFrame;
}

float IGame_Persistent::GrassBenderToValue(float& current, float go_to, float intensity, bool use_easing)
{
	float diff = abs(current - go_to);

	float r_value = Device.fTimeDelta * intensity * (use_easing ? std::min(0.5f, diff) : 1.0f);

	if (diff - r_value <= 0)
	{
		current = go_to;
		return 0;
	}

	return current < go_to ? r_value : -r_value;
}
