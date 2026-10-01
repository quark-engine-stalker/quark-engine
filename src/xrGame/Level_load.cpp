#include "stdafx.h"
#include "LevelGameDef.h"
#include "ai_space.h"
#include "ParticlesObject.h"
#include "script_process.h"
#include "script_engine.h"
#include "script_engine_space.h"
#include "level.h"
#include "game_cl_base.h"
#include "../xrEngine/x_ray.h"
#include "../xrEngine/gamemtllib.h"
#include "../xrphysics/PhysicsCommon.h"
#include "level_sounds.h"
#include "GamePersistent.h"
#include "../xrEngine/Rain.h"
#include "../xrCore/job_system.h"
#include "character_community.h"
#include "character_rank.h"
#include "character_reputation.h"
#include "monster_community.h"
#include "HudManager.h"

extern ENGINE_API bool g_dedicated_server;

bool CLevel::Load_GameSpecific_Before()
{
	// AI space
	//	g_pGamePersistent->LoadTitle		("st_loading_ai_objects");
	g_pGamePersistent->LoadTitle();
	string_path fn_game;

	if (GamePersistent().GameType() == eGameIDSingle && !ai().get_alife() && FS.exist(fn_game, "$level$", "level.ai") &&
		!net_Hosts.empty())
		ai().load(net_SessionName());

	if (!g_dedicated_server && !ai().get_alife() && ai().get_game_graph())
	{
		IReader* stream = FS.r_open("$level$", "level.game");
		if (stream)
		{
			ai().patrol_path_storage_raw(*stream);
			FS.r_close(stream);
		}
	}

	CHARACTER_COMMUNITY::Reset();
	CHARACTER_RANK::Reset();
	CHARACTER_REPUTATION::Reset();
	MONSTER_COMMUNITY::Reset();

	return (TRUE);
}

bool CLevel::Load_GameSpecific_After()
{
	R_ASSERT(m_StaticParticles.empty());
	// loading static particles
	string_path fn_game;
	IReader* F = FS.r_open("$level$", "level.ps_static");
	if (F)
	{
		CParticlesObject* pStaticParticles;
		u32 chunk = 0;
		string256 ref_name;
		Fmatrix transform;
		Fvector zero_vel = {0.f, 0.f, 0.f};
		u32 ver = 0;
		for (IReader* OBJ = F->open_chunk_iterator(chunk); OBJ; OBJ = F->open_chunk_iterator(chunk, OBJ))
		{
			if (chunk == 0)
			{
				if (OBJ->length() == sizeof(u32))
				{
					ver = OBJ->r_u32();
#ifndef MASTER_GOLD
					Msg		("PS new version, %d", ver);
#endif // #ifndef MASTER_GOLD
					continue;
				}
			}
			u16 gametype_usage = 0;
			if (ver > 0)
			{
				gametype_usage = OBJ->r_u16();
			}
			OBJ->r_stringZ(ref_name, sizeof(ref_name));
			OBJ->r(&transform, sizeof(Fmatrix));
			transform.c.y += 0.01f;


			if ((g_pGamePersistent->m_game_params.m_e_game_type & EGameIDs(gametype_usage)) || (ver == 0))
			{
				pStaticParticles = CParticlesObject::Create(ref_name,FALSE, false);
				pStaticParticles->UpdateParent(transform, zero_vel);
				pStaticParticles->Play(false);
				m_StaticParticles.push_back(pStaticParticles);
			}
		}
		FS.r_close(F);
	}

	if (!g_dedicated_server)
	{
		// loading static sounds
		VERIFY(m_level_sound_manager);
		m_level_sound_manager->Load();

		// loading sound environment
		F = FS.r_open("$level$", "level.snd_env");
		if (F)
		{
			::Sound->set_geometry_env(F);
			FS.r_close(F);
		}
		else
		{
			// demonized: reset sound environment if the map doesn't have it, so that the next map won't be using environment of the previous one
			::Sound->set_geometry_env(nullptr);
		}
		// loading SOM
		F = FS.r_open("$level$", "level.som");
		if (F)
		{
			::Sound->set_geometry_som(F);
			FS.r_close(F);
		}
		else
		{
			// demonized: same here
			::Sound->set_geometry_som(nullptr);
		}

		// loading random (around player) sounds
		if (pSettings->section_exist("sounds_random"))
		{
			CInifile::Sect& S = pSettings->r_section("sounds_random");
			Sounds_Random.reserve(S.Data.size());
			for (CInifile::SectCIt I = S.Data.begin(); S.Data.end() != I; ++I)
			{
				Sounds_Random.push_back(ref_sound());
				Sound->create(Sounds_Random.back(), *I->first, st_Effect, sg_SourceType);
			}
			Sounds_Random_dwNextTime = Device.TimerAsync() + 50000;
			Sounds_Random_Enabled = FALSE;
		}

		if (g_pGamePersistent->pEnvironment)
		{
			if (CEffect_Rain* rain = g_pGamePersistent->pEnvironment->eff_Rain)
			{
				rain->InvalidateState();
			}
		}

		// level.fog_vol was parsed into temporary matrices and immediately discarded.
		// No runtime state consumed the data, so the no-op file traversal is omitted.
	}

	if (!g_dedicated_server)
	{
		// loading scripts
		ai().script_engine().remove_script_process(ScriptEngine::eScriptProcessorLevel);

		if (pLevel->section_exist("level_scripts") && pLevel->line_exist("level_scripts", "script"))
			ai().script_engine().add_script_process(ScriptEngine::eScriptProcessorLevel,
			                                        xr_new<CScriptProcess>(
				                                        "level", pLevel->r_string("level_scripts", "script")));
		else
			ai().script_engine().add_script_process(ScriptEngine::eScriptProcessorLevel,
			                                        xr_new<CScriptProcess>("level", ""));
	}

	BlockCheatLoad();

	g_pGamePersistent->Environment().SetGameTime(GetEnvironmentGameDayTimeSec(), game->GetEnvironmentGameTimeFactor());

	HUD().SetRenderable(true);

	return TRUE;
}

namespace
{
constexpr u16 invalid_material_index = u16(-1);
constexpr u32 material_id_count = 1u << 14;
constexpr u16 default_cform_material_id = static_cast<u16>(material_id_count - 1);
constexpr u32 cform_remap_block_size = 8192;

struct cform_remap_context
{
	CDB::TRI* triangles;
	u32 count;
	const u16* material_index_by_id;
	const u8* suppress_shadows;
	const u8* suppress_wallmarks;
	__declspec(align(64)) volatile LONG next;
	volatile LONG invalid_material_id;
};

void remap_cform_triangles_job(void* raw_context)
{
	cform_remap_context& context = *static_cast<cform_remap_context*>(raw_context);
	for (;;)
	{
		const LONG start_value = InterlockedExchangeAdd(&context.next, static_cast<LONG>(cform_remap_block_size));
		if (start_value < 0 || static_cast<u32>(start_value) >= context.count)
			return;

		const u32 start = static_cast<u32>(start_value);
		const u32 end = _min(start + cform_remap_block_size, context.count);
		for (u32 index = start; index < end; ++index)
		{
			CDB::TRI& triangle = context.triangles[index];
			const u16 source_material_id = static_cast<u16>(triangle.material);
			const u16 mapped_index = context.material_index_by_id[source_material_id];
			if (mapped_index == invalid_material_index)
			{
				InterlockedCompareExchange(
					&context.invalid_material_id, static_cast<LONG>(source_material_id), -1);
				continue;
			}

			triangle.material = mapped_index;
			triangle.suppress_shadows = context.suppress_shadows[mapped_index] != 0;
			triangle.suppress_wm = context.suppress_wallmarks[mapped_index] != 0;
		}
	}
}
}

void CLevel::Load_GameSpecific_CFORM(CDB::TRI* tris, u32 count)
{
	const u32 material_count = GMLib.CountMaterial();
	xr_vector<u16> material_index_by_id;
	material_index_by_id.assign(material_id_count, invalid_material_index);

	xr_vector<u8> suppress_shadows;
	xr_vector<u8> suppress_wallmarks;
	suppress_shadows.resize(material_count);
	suppress_wallmarks.resize(material_count);

	const u16 default_index = static_cast<u16>(GMLib.GetMaterialIdx("default"));
	material_index_by_id[default_cform_material_id] = default_index;

	u16 material_index = 0;
	for (GameMtlIt it = GMLib.FirstMaterial(); GMLib.LastMaterial() != it; ++it, ++material_index)
	{
		SGameMtl* material = *it;
		suppress_shadows[material_index] = material->Flags.is(SGameMtl::flSuppressShadows) ? 1 : 0;
		suppress_wallmarks[material_index] = material->Flags.is(SGameMtl::flSuppressWallmarks) ? 1 : 0;

		if (!material->Flags.test(SGameMtl::flDynamic))
		{
			const u32 material_id = material->GetID();
			VERIFY(material_id < material_id_count);
			material_index_by_id[material_id] = material_index;
		}
	}

	// The level CFORM can contain hundreds of thousands or millions of triangles.
	// Use direct material lookup and distribute independent triangle blocks across
	// the common pool. This callback already runs during asynchronous CDB build.
	cform_remap_context context
	{
		tris,
		count,
		material_index_by_id.data(),
		suppress_shadows.data(),
		suppress_wallmarks.data(),
		0,
		-1
	};

	const u32 block_count = (count + cform_remap_block_size - 1) / cform_remap_block_size;
	const bool parallel_remap = block_count > 1 && xr_jobs::worker_count() > 0 &&
		!(Core.Params && strstr(Core.Params, "-no_mt_cform"));
	const u32 lane_count = parallel_remap ?
		_min(block_count, xr_jobs::available_thread_count()) : 1;

	xr_jobs::task_group group;
	xr_jobs::submit_many(&remap_cform_triangles_job, &context,
		lane_count - 1, &group, xr_jobs::priority::normal);

	remap_cform_triangles_job(&context);
	if (lane_count > 1)
		xr_jobs::wait(group);

	const LONG invalid_id = InterlockedCompareExchange(&context.invalid_material_id, -1, -1);
	if (invalid_id >= 0)
		Debug.fatal(DEBUG_INFO, "Game material '%d' not found", invalid_id);
}

void CLevel::BlockCheatLoad()
{
#ifndef	DEBUG
	if (game && (GameID() != eGameIDSingle)) phTimefactor = 1.f;
#endif
}
