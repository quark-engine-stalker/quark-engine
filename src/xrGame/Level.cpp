#include "pch_script.h"
#include "sound_collection_storage.h"
#include <iterator>
#include "xrEngine/FDemoRecord.h"
#include "xrEngine/FDemoPlay.h"
#include "xrEngine/Environment.h"
#include "xrEngine/IGame_Persistent.h"
#include "ParticlesObject.h"
#include "Level.h"
#include "HUDManager.h"
#include "xrServer.h"
#include "NET_Queue.h"
#include "game_cl_base.h"
#include "entity_alive.h"
#include "ai_space.h"
#include "ai_debug.h"
#include "ShootingObject.h"
#include "GameTaskManager.h"
#include "Level_Bullet_Manager.h"
#include "script_process.h"
#include "script_engine.h"
#include "script_engine_space.h"
#include "../xrServerEntities/specific_character.h"
#include "../xrServerEntities/object_factory.h"
#include "team_base_zone.h"
#include "infoportion.h"
#include "patrol_path_storage.h"
#include "date_time.h"
#include "space_restriction_manager.h"
#include "seniority_hierarchy_holder.h"
#include "space_restrictor.h"
#include "client_spawn_manager.h"
#include "autosave_manager.h"
#include "ClimableObject.h"
#include "level_graph.h"
#include "mt_config.h"
#include "phcommander.h"
#include "map_manager.h"
#include "xrEngine/CameraManager.h"
#include "level_sounds.h"
#include "car.h"
#include "trade_parameters.h"
#include "game_cl_base_weapon_usage_statistic.h"
#include "MainMenu.h"
#include "xrEngine/XR_IOConsole.h"
#include "actor.h"
#include "player_hud.h"
#include "UI/UIGameTutorial.h"
#include "file_transfer.h"
#include "message_filter.h"
#include "demoplay_control.h"
#include "demoinfo.h"
#include "CustomDetector.h"
#include "xrPhysics/IPHWorld.h"
#include "xrPhysics/console_vars.h"
#include "../xrEngine/device.h"
#include "../xrCore/adaptive_memory_policy.h"

#ifdef _WIN64
extern "C"
{
#include "../3rd party/luajit-2/src/xr_alloc.h"
}
#endif

#include "UIGameCustom.h"
#include "ui/UIPdaWnd.h"
#include "UICursor.h"
#include "debug_renderer.h"
#include "LevelDebugScript.h"
#include "script_attachment_manager.h"
#include "script_light_inline.h"

#include "alife_simulator.h"
#include "alife_object_registry.h"

#ifdef DEBUG
#include "level_debug.h"
#include "ai/stalker/ai_stalker.h"
#include "PhysicObject.h"
#include "PHDebug.h"
#include "debug_text_tree.h"
#endif
extern ENGINE_API bool g_dedicated_server;
extern ENGINE_API BOOL	g_bootComplete;
extern CUISequencer* g_tutorial;
extern CUISequencer* g_tutorial2;
extern BOOL psLua_ParallelGC;
extern BOOL psLua_ParallelGC_debug;

float g_cl_lvInterp = 0.1;
u32 lvInterpSteps = 0;

namespace
{
constexpr size_t retained_net_event_capacity = 512;
#ifdef SPAWN_ANTIFREEZE
constexpr size_t retained_prefetch_event_capacity = 128;
// Spawn prefetch is already asynchronous. Keep the semantic commit on the main
// thread, but cap one frame's commit burst so a wave of prepared objects cannot
// monopolize OnFrame. Ordering inside the queue remains stable.
constexpr u32 spawn_event_max_per_frame = 4;
constexpr u32 spawn_event_budget_us = 1500;
#endif

}

#ifdef SPAWN_ANTIFREEZE
BOOL spawn_antifreeze = TRUE;
BOOL spawn_antifreeze_debug = FALSE;

u16	GetSpawnInfo(NET_Packet& P, u16& parent_id, shared_str& section)
{
    u16 dummy16, id;
    P.r_begin(dummy16);

    shared_str s_name;
    P.r_stringZ(s_name);
    section = s_name;

    string256 temp;
    P.r_stringZ(temp);

    u8 temp_gt, s_RP;
    Fvector o_Position, o_Angle;
    u16 RespawnTime;
    P.r_u8(temp_gt/*s_gameid*/);
    P.r_u8(s_RP);
    P.r_vec3(o_Position);
    P.r_vec3(o_Angle);
    P.r_u16(RespawnTime);
    P.r_u16(id);
    P.r_u16(parent_id);

    P.r_pos = 0;
    return id;
}
#endif
//-AVO

// Define a helper struct to hold the heavy data
struct ProcessNetPacket : public intrusive_base_nonatomic
{
    NET_Packet P;
};

struct ProcessGameEventsData : ProcessNetPacket
{
    NET_Packet PRespond;
};

namespace crash_saving {
    extern void(*save_impl)();
    static bool g_isSaving = false;
    int saveCountMax = 10;

    void _save_impl()
    {
        if (g_isSaving) return;
        if (saveCountMax <= 0) return;

        int saveCount = -1;
        g_isSaving = true;
        auto data = make_intrusive<ProcessNetPacket>();
        NET_Packet& net_packet = data->P;
        net_packet.w_begin(M_SAVE_GAME);

		std::string path = "fatal_ctd_save_";
		std::string path_mask(path);
		std::string path_ext = ".scop";
		path_mask.append("*").append(path_ext);

        FS_FileSet fset_temp;
        FS.file_list(fset_temp, "$game_saves$", FS_ListFiles | FS_RootOnly, path_mask.c_str());

		std::vector<FS_File> fset(fset_temp.begin(), fset_temp.end());
		struct {
			bool operator()(FS_File& a, FS_File& b) {
				return a.time_write > b.time_write;
			}
		} sortFilesDesc;
		std::sort(fset.begin(), fset.end(), sortFilesDesc);

        //Msg("save mask %s", path_mask.c_str());

		for (auto &file : fset)
		{
			string128 name;
			xr_strcpy(name, sizeof(name), file.name.c_str());
			std::string name_string(name);
			name_string.erase(name_string.length() - path_ext.length());

            //Msg("found save file %s, save_name %s", name, name_string.c_str());

			try {
				//Msg("save number %s", name_string.substr(path.length()).c_str());
				int name_count = std::stoi(name_string.substr(path.length()));
				saveCount = name_count;
				break;
			} catch (...) {
				Msg("!error getting save number from %s", name);
			}
		}

        saveCount++;
        if (saveCount >= saveCountMax) {
            saveCount = 0;
        }

        path.append(std::to_string(saveCount));
        net_packet.w_stringZ(path.c_str());
        net_packet.w_u8(1);
        CLevel& level = Level();
        if (&level != nullptr)
        {
            level.Send(net_packet, net_flags(1));
        }

    }
}

CLevel::CLevel() :
    IPureClient(Device.GetTimerGlobal())
#ifdef PROFILE_CRITICAL_SECTIONS
    , DemoCS(MUTEX_PROFILE_ID(DemoCS))
#endif
{
	g_bDebugEvents = strstr(Core.Params, "-debug_ge") != nullptr;
	game_events = xr_new<NET_Queue_Event>();

    eChangeRP = Engine.Event.Handler_Attach("LEVEL:ChangeRP", this);
    eDemoPlay = Engine.Event.Handler_Attach("LEVEL:PlayDEMO", this);
    eChangeTrack = Engine.Event.Handler_Attach("LEVEL:PlayMusic", this);
    eEnvironment = Engine.Event.Handler_Attach("LEVEL:Environment", this);
    eEntitySpawn = Engine.Event.Handler_Attach("LEVEL:spawn", this);
    m_pBulletManager = xr_new<CBulletManager>();
    if (!g_dedicated_server)
    {
        m_map_manager = xr_new<CMapManager>();
        m_game_task_manager = xr_new<CGameTaskManager>();
    }
    m_dwDeltaUpdate = u32(fixed_step * 1000);
    m_seniority_hierarchy_holder = xr_new<CSeniorityHierarchyHolder>();
    if (!g_dedicated_server)
    {
        m_level_sound_manager = xr_new<CLevelSoundManager>();
        m_space_restriction_manager = xr_new<CSpaceRestrictionManager>();
        m_client_spawn_manager = xr_new<CClientSpawnManager>();
        m_autosave_manager = xr_new<CAutosaveManager>();
        m_debug_renderer = xr_new<CDebugRenderer>();
#ifdef DEBUG
        m_level_debug = xr_new<CLevelDebug>();
#endif
    }
    m_ph_commander = xr_new<CPHCommander>();
    m_ph_commander_scripts = xr_new<CPHCommander>();
    pObjects4CrPr.clear();
    pActors4CrPr.clear();
    g_player_hud = xr_new<player_hud>();
    g_player_hud->load_default();

#ifdef SPAWN_ANTIFREEZE
	_InterlockedExchange(&closeSignal, FALSE);
	_InterlockedExchange(&prefetch_job_scheduled, FALSE);
    spawn_events = xr_new<NET_Queue_Event>();
    spawn_events_data = xr_new<spawn_events_data_map>();
    prefetch_events = xr_new<prefetch_event_queue>();
    prefetched_models = xr_new<models_set>();
	Msg("CLevel::CLevel() Spawn Antifreeze %s", spawn_antifreeze ? "initialized" : "disabled");
#endif

    Msg("%s", Core.Params);
    //crash_saving::save_impl = crash_saving::_save_impl; // CLevel ready, we can save now
}

extern CAI_Space* g_ai_space;

void CLevel::CancelLevelJobs()
{
#ifdef SPAWN_ANTIFREEZE
	_InterlockedExchange(&closeSignal, TRUE);
#endif

	// Closing the scope rejects new submissions, skips queued callbacks and waits
	// for callbacks that already entered level-owned code.
	level_job_scope.cancel_and_wait();

#ifdef SPAWN_ANTIFREEZE
	// Scope completion is recorded after task-group completion, but keep the
	// explicit wait as a local invariant for the prefetch state below.
	xr_jobs::wait(prefetch_job_group);
	_InterlockedExchange(&prefetch_job_scheduled, FALSE);
#endif
}

CLevel::~CLevel()
{
	// No level-owned callback may outlive CLevel or any of its member task groups.
	CancelLevelJobs();

#ifdef SPAWN_ANTIFREEZE
	xr_delete(spawn_events);
	xr_delete(spawn_events_data);
	xr_delete(prefetch_events);
	xr_delete(prefetched_models);
#endif

	//crash_saving::save_impl = nullptr; // CLevel not available, disable crash save
	xr_delete(g_player_hud);
	delete_data(m_script_attachments);
	delete_data(hud_zones_list);
	hud_zones_list = nullptr;
	Msg("- Destroying level");
	Engine.Event.Handler_Detach(eEntitySpawn, this);
	Engine.Event.Handler_Detach(eEnvironment, this);
	Engine.Event.Handler_Detach(eChangeTrack, this);
	Engine.Event.Handler_Detach(eDemoPlay, this);
	Engine.Event.Handler_Detach(eChangeRP, this);
	if (physics_world())
	{
		destroy_physics_world();
		xr_delete(m_ph_commander_physics_worldstep);
	}
	// destroy PSs
	for (POIt p_it = m_StaticParticles.begin(); m_StaticParticles.end() != p_it; ++p_it)
		CParticlesObject::Destroy(*p_it);
	m_StaticParticles.clear();
	// Unload sounds
	// unload prefetched sounds
	sound_registry.clear();
	// unload static sounds
	for (u32 i = 0; i < static_Sounds.size(); ++i)
	{
		static_Sounds[i]->destroy();
		xr_delete(static_Sounds[i]);
	}
	static_Sounds.clear();
	xr_delete(m_level_sound_manager);
	xr_delete(m_space_restriction_manager);
	xr_delete(m_seniority_hierarchy_holder);
	xr_delete(m_client_spawn_manager);
	xr_delete(m_autosave_manager);
    xr_delete(m_debug_renderer);
    delete_data(m_debug_render_queue);
    if (!g_dedicated_server)
        ai().script_engine().remove_script_process(ScriptEngine::eScriptProcessorLevel);
    xr_delete(game);
    xr_delete(game_events);

    xr_delete(m_pBulletManager);
    xr_delete(pStatGraphR);
    xr_delete(pStatGraphS);
    xr_delete(m_ph_commander);
    xr_delete(m_ph_commander_scripts);
    pObjects4CrPr.clear();
    pActors4CrPr.clear();
    ai().unload();
#ifdef DEBUG
    xr_delete(m_level_debug);
#endif
    xr_delete(m_map_manager);
    delete_data(m_game_task_manager);
    // here we clean default trade params
    // because they should be new for each saved/loaded game
    // and I didn't find better place to put this code in
    // XXX nitrocaster: find better place for this clean()
    CTradeParameters::clean();
    if (g_tutorial && g_tutorial->m_pStoredInputReceiver == this)
        g_tutorial->m_pStoredInputReceiver = nullptr;
    if (g_tutorial2 && g_tutorial2->m_pStoredInputReceiver == this)
        g_tutorial2->m_pStoredInputReceiver = nullptr;
    if (IsDemoPlay())
    {
        StopPlayDemo();
        if (m_reader)
        {
            FS.r_close(m_reader);
            m_reader = nullptr;
        }
    }
    xr_delete(m_msg_filter);
    xr_delete(m_demoplay_control);
    xr_delete(m_demo_info);
    if (IsDemoSave())
    {
        StopSaveDemo();
    }
    deinit_compression();
}

shared_str CLevel::name() const
{
    return map_data.m_name;
}

void CLevel::GetLevelInfo(CServerInfo* si)
{
    if (Server && game)
    {
        Server->GetServerInfo(si);
    }
}

void CLevel::PrefetchSound(LPCSTR name)
{
    // preprocess sound name
    string_path tmp;
    xr_strcpy(tmp, name);
    xr_strlwr(tmp);
    if (strext(tmp))
        *strext(tmp) = 0;
    shared_str snd_name = tmp;
    // find in registry
    SoundRegistryMapIt it = sound_registry.find(snd_name);
    // if find failed - preload sound
    if (it == sound_registry.end())
        sound_registry[snd_name].create(snd_name.c_str(), st_Effect, sg_SourceType);
}

// Game interface ////////////////////////////////////////////////////
int CLevel::get_RPID(LPCSTR /**name/**/)
{
    /*
    // Gain access to string
    LPCSTR	params = pLevel->r_string("respawn_point",name);
    if (0==params)	return -1;

    // Read data
    Fvector4	pos;
    int			team;
    sscanf		(params,"%f,%f,%f,%d,%f",&pos.x,&pos.y,&pos.z,&team,&pos.w); pos.y += 0.1f;

    // Search respawn point
    svector<Fvector4,maxRP>	&rp = Level().get_team(team).RespawnPoints;
    for (int i=0; i<(int)(rp.size()); ++i)
    if (pos.similar(rp[i],EPS_L))	return i;
    */
    return -1;
}

bool g_bDebugEvents = false;

void CLevel::cl_Process_Event(u16 dest, u16 type, NET_Packet& P)
{
    // Msg("--- event[%d] for [%d]",type,dest);
    CObject* O = Objects.net_Find(dest);
    if (0 == O)
    {
#ifdef DEBUG
        Msg("* WARNING: c_EVENT[%d] to [%d]: unknown dest", type, dest);
#endif
        return;
    }
    CGameObject* GO = smart_cast<CGameObject*>(O);
    if (!GO)
    {
#ifndef MASTER_GOLD
        Msg("! ERROR: c_EVENT[%d] : non-game-object", dest);
#endif
        return;
    }
    if (type != GE_DESTROY_REJECT)
    {
        if (type == GE_DESTROY)
        {
            Game().OnDestroy(GO);
        }
        GO->OnEvent(P, type);
    }
    else
    {
        // handle GE_DESTROY_REJECT here
        u32 pos = P.r_tell();
        u16 id = P.r_u16();
        P.r_seek(pos);
        bool ok = true;
        CObject* D = Objects.net_Find(id);
        if (0 == D)
        {
#ifndef MASTER_GOLD
            Msg("! ERROR: c_EVENT[%d] : unknown dest", id);
#endif
            ok = false;
        }
        CGameObject* GD = smart_cast<CGameObject*>(D);
        if (!GD)
        {
#ifndef MASTER_GOLD
            Msg("! ERROR: c_EVENT[%d] : non-game-object", id);
#endif
            ok = false;
        }
        GO->OnEvent(P, GE_OWNERSHIP_REJECT);
        if (ok)
        {
            Game().OnDestroy(GD);
            GD->OnEvent(P, GE_DESTROY);
        }
    }
}

//AVO: used by SPAWN_ANTIFREEZE (by alpet, edited by demonized)
#ifdef SPAWN_ANTIFREEZE
bool CLevel::PostponedSpawnFind(u16 id, const NET_Event& E) const
{
    auto data = make_intrusive<ProcessNetPacket>();
    NET_Packet& P = data->P;
    E.implication(P);
    return PostponedSpawnFind(id, P);
}

bool CLevel::PostponedSpawnFind(u16 id, NET_Packet& P) const
{
    u16 parent_id;
    shared_str section;
    return id == GetSpawnInfo(P, parent_id, section);
}

bool CLevel::PostponedSpawn(u16 id)
{
    PROF_EVENT("ProcessGameEvents PostponedSpawn");

    xrSRWLockGuard g(prefetch_lock, true);
    auto queue = prefetch_events;
    auto it = std::find_if(queue->begin(), queue->end(), [id, this](prefetch_event& E) { return PostponedSpawnFind(id, E.p); });

    auto& queue2 = spawn_events->queue;
    auto it2 = std::find_if(queue2.begin(), queue2.end(), [id, this](const NET_Event& E) { return PostponedSpawnFind(id, E); });
    return it != queue->end() || it2 != queue2.end();
}

int CLevel::GetSpawnEventPriority(const NET_Event& e) const
{
    if (e.ID == M_EVENT)
        return 0;

    if (e.ID == M_SPAWN) {
        auto data = make_intrusive<ProcessNetPacket>();
        NET_Packet& P = data->P;
        e.implication(P);

        u16 parent_id = 0;
        shared_str section;
        GetSpawnInfo(P, parent_id, section);
        if (parent_id < 0xFFFF)
            return 1;

        return 2;
    }

    return 0;
}

bool CLevel::SpawnEventCompare(const NET_Event& a, const NET_Event& b) const
{
    return GetSpawnEventPriority(a) > GetSpawnEventPriority(b);
}

// demonized: If called manually, be aware of ProcessPrefetchEvents thread, which may modify spawn_events queue at the same time, maybe fix later
void CLevel::SortSpawnEventsQueue()
{
    xrSRWLockGuard g(prefetch_lock);
    auto& queue = spawn_events->queue;
    std::stable_sort(queue.begin(), queue.end(), [this](const NET_Event& a, const NET_Event& b) { return SpawnEventCompare(a, b); });
}

void CLevel::ProcessPrefetchEvents(void* args)
{
	CLevel* level = static_cast<CLevel*>(args);
	R_ASSERT(level);

	for (;;)
	{
		prefetch_event_queue saved_prefetch_events;
		{
			xrSRWLockGuard guard(level->prefetch_lock);
			if (_InterlockedCompareExchange(&level->closeSignal, FALSE, FALSE) != FALSE ||
				level->prefetch_events->empty())
			{
				_InterlockedExchange(&level->prefetch_job_scheduled, FALSE);
				return;
			}

			saved_prefetch_events.swap(*level->prefetch_events);
		}

		bool stop_requested = false;
		for (const auto& event : saved_prefetch_events)
		{
			for (const auto& model : event.models)
			{
				if (_InterlockedCompareExchange(&level->closeSignal, FALSE, FALSE) != FALSE)
				{
					stop_requested = true;
					break;
				}

				bool not_prefetched = false;
				{
					xrSRWLockGuard guard(level->prefetch_lock, true);
					not_prefetched = level->prefetched_models->find(model) == level->prefetched_models->end();
				}

				if (not_prefetched)
				{
					if (spawn_antifreeze_debug)
						Msg("[ProcessPrefetchEvents] Prefetching model '%s' for spawn event", model.c_str());

					::Render->models_PrefetchOne(model.c_str(), false);

					{
						xrSRWLockGuard guard(level->prefetch_lock);
						level->prefetched_models->insert(model);
					}
				}
			}

			if (stop_requested)
				break;
		}

		if (stop_requested)
		{
			xrSRWLockGuard guard(level->prefetch_lock);
			_InterlockedExchange(&level->prefetch_job_scheduled, FALSE);
			return;
		}

		{
			xrSRWLockGuard guard(level->prefetch_lock);
			for (auto& event : saved_prefetch_events)
			{
				level->spawn_events->insert(event.p);
				const u16 event_id = event.id;
				level->spawn_events_data->emplace(event_id, std::move(event));
			}

			// Keep only a bounded outer-vector working set for the next burst. The
			// heavy per-event model sets are destroyed by clear(), while a normal
			// queue-size allocation can be reused instead of hitting the allocator.
			saved_prefetch_events.clear();
			if (level->prefetch_events->empty() &&
				saved_prefetch_events.capacity() <= retained_prefetch_event_capacity)
				saved_prefetch_events.swap(*level->prefetch_events);
		}
	}
}

// demonized: If called manually, be aware of ProcessPrefetchEvents thread, which may modify spawn_events queue at the same time, maybe fix later
void CLevel::ProcessSpawnEvents()
{
    PROF_EVENT("ProcessSpawnEvents");

    xr_vector<NET_Event> events_to_process;
    u32 queued_after_extract = 0;
    {
        xrSRWLockGuard g(prefetch_lock);
        auto& queue = spawn_events->queue;
        if (!queue.empty())
        {
            const u32 batch_count = std::min(queue.size(), spawn_event_max_per_frame);
            events_to_process.reserve(batch_count);
            for (u32 i = 0; i < batch_count; ++i)
                events_to_process.push_back(std::move(queue[i]));
            queue.erase(queue.begin(), queue.begin() + batch_count);
            queued_after_extract = queue.size();
        }
    }

    if (events_to_process.empty())
        return;

    const u64 started = CPU::qpc_freq ? CPU::QPC() : 0;
    u32 processed = 0;
    // Reuse the 16 KiB packet scratch storage for the whole bounded batch.
    auto data = make_intrusive<ProcessNetPacket>();

    for (; processed < events_to_process.size(); ++processed)
    {
        const auto& E = events_to_process[processed];
        u16 ID, dest, type;
        NET_Packet& P = data->P;
        ID = E.ID;
        dest = E.destination;
        type = E.type;
        E.implication(P);

        u16 parent_id;
        shared_str section;
        u16 obj_id = GetSpawnInfo(P, parent_id, section);

        if (spawn_antifreeze_debug) Msg("[ProcessSpawnEvents] spawning section %s, obj_id %d, parent_id %d, event_id %d", section.c_str(), obj_id, parent_id, dest);

        // The worker-side metadata stays in the shared map until the matching
        // event is actually committed. This is required when a large ready queue
        // is spread over several frames.
        bool had_spawn_data = false;
        bool had_alife_object = false;
        {
            xrSRWLockGuard guard(prefetch_lock);
            auto spawn_data_it = spawn_events_data->find(obj_id);
            if (spawn_data_it != spawn_events_data->end())
            {
                had_spawn_data = true;
                had_alife_object = spawn_data_it->second.hasAlifeObject;
                spawn_events_data->erase(spawn_data_it);
            }
        }

        // demonized: If item is II_BOLT class - go through anyway
        if (pSettings->line_exist(section.c_str(), "class") && strstr(pSettings->r_string(section.c_str(), "class"), "II_BOLT") != nullptr)
        {
            // demonized: this is a sin, but its an easy way
            goto spawn;
        }

        // If the object was in alife, but now its absent, skip it
        if (had_spawn_data && had_alife_object)
        {
            auto obj = ai().alife().objects().object(obj_id);
            if (!obj || !obj->m_bOnline)
            {
                if (spawn_antifreeze_debug) Msg("![ProcessSpawnEvents] object absent or offline, do not spawn, section %s, obj_id %d, parent_id %d, event_id %d", section.c_str(), obj_id, parent_id, dest);
                goto budget_check;
            }
        }

        // If there is a parent of this object, check if its still in alife
        if (parent_id != 0xffff)
        {
            auto parent_obj = ai().alife().objects().object(parent_id);
            if (!parent_obj || !parent_obj->m_bOnline)
            {
                if (spawn_antifreeze_debug) Msg("![ProcessSpawnEvents] parent object is not in alife, do not spawn, section %s, obj_id %d, parent_id %d, event_id %d", section.c_str(), obj_id, parent_id, dest);
                goto budget_check;
            }
        }

    spawn:
        {
            u16 dummy16;
            P.r_begin(dummy16);
            cl_Process_Spawn(P);
        }

    budget_check:
        // One spawn can itself exceed the budget; it cannot be preempted without
        // changing script semantics. Stop after it and carry the untouched suffix
        // to the next frame. This guarantees progress while preventing multiple
        // heavy spawn callbacks from stacking in one frame.
        if (processed + 1 < events_to_process.size() && CPU::qpc_freq)
        {
            const u64 elapsed = CPU::QPC() - started;
            if (elapsed * 1000000ull >= static_cast<u64>(spawn_event_budget_us) * CPU::qpc_freq)
            {
                ++processed;
                break;
            }
        }
    }

    if (processed < events_to_process.size())
    {
        xrSRWLockGuard guard(prefetch_lock);
        auto& queue = spawn_events->queue;
        // Put older, already-sorted events in front of any newly produced ones.
        // A stable priority sort on the next frame retains relative order among
        // equal-priority events.
        queue.insert(queue.begin(),
            std::make_move_iterator(events_to_process.begin() + processed),
            std::make_move_iterator(events_to_process.end()));
        queued_after_extract = queue.size();
    }

    if (spawn_antifreeze_debug && queued_after_extract)
        Msg("[ProcessSpawnEvents] deferred %u prepared spawn events", static_cast<u32>(queued_after_extract));

    events_to_process.clear();
    if (events_to_process.capacity() <= retained_net_event_capacity)
    {
        xrSRWLockGuard guard(prefetch_lock);
        if (spawn_events->queue.empty())
            events_to_process.swap(spawn_events->queue);
    }
}

#endif

void CLevel::ProcessGameEvents()
{
	PROF_EVENT("ProcessGameEvents");

	xr_vector<NET_Event> events_to_process;
	xr_vector<prefetch_event> events_to_prefetch;
	{
		xrSRWLockGuard g(prefetch_lock);
		if (!game_events->queue.empty())
		{
			events_to_process.swap(game_events->queue);
		}
	}

	// Game events. Keep the large packet scratch buffers alive for the whole
	// detached batch: NET_Packet is 16 KiB, so allocating three fresh buffers for
	// every event amplified allocator traffic during mass spawn/despawn bursts.
	if (!events_to_process.empty())
	{
		auto data = make_intrusive<ProcessGameEventsData>();

#ifdef SPAWN_ANTIFREEZE
		const size_t spawn_event_count = static_cast<size_t>(std::count_if(
			events_to_process.begin(), events_to_process.end(),
			[](const NET_Event& event) { return event.ID == M_SPAWN; }));
		if (spawn_event_count)
			events_to_prefetch.reserve(std::min(spawn_event_count, retained_prefetch_event_capacity));
#endif

		for (auto it = events_to_process.begin(); it != events_to_process.end(); )
		{
			PROF_EVENT("ProcessGameEvents game_events queue");
			u16 ID = it->ID;
			u16 dest = it->destination;
			u16 type = it->type;

			auto& P = data->P;
			it->implication(P);

//AVO: spawn antifreeze implementation, originally by alpet, reritten by demonized
#ifdef SPAWN_ANTIFREEZE
			if (spawn_antifreeze && g_bootComplete)
			{
				// Postpone M_EVENT for postponed spawns
				//if (M_EVENT == ID && PostponedSpawn(dest))
				//{
				//	game_events->insert(P);
				//	Msg("[ProcessGameEvents] postponed M_EVENT, object in prefetch queue: obj_id %d", dest);
				//	it = game_events->queue.erase(it); // remove current event
				//	continue;
				//}

				// add to prefetch_events queue for postponed spawn
				if (M_SPAWN == ID)
				{
					PROF_EVENT("ProcessGameEvents M_SPAWN");

					u16 parent_id;
					shared_str section;
					u16 obj_id = GetSpawnInfo(P, parent_id, section);

					static auto isValidToPrefetch = [](u16 parent_id, shared_str& section, u16 obj_id, NET_Packet& P) {
						if (pSettings->line_exist("spawn_antifreeze_ignore", section))
						{
							return false;
						}

						bool valid = true;

						if (pSettings->line_exist(section.c_str(), "class"))
						{
							auto c = pSettings->r_string(section.c_str(), "class");

							// Do not prefetch fake missiles of a weapon
							valid &= strstr(c, "G_RPG7") == nullptr;
							valid &= strstr(c, "G_FAKE") == nullptr;

							// Do not prefetch helicopters
							valid &= strstr(c, "C_HLCP") == nullptr;
						}

						return valid;
					};

					if (isValidToPrefetch(parent_id, section, obj_id, P))
					{
						models_set models;

						static auto safe_insert = [](models_set& models, LPCSTR model) {
							if (model)
							{
								string_path modelWithoutExtension;
								xr_strcpy(modelWithoutExtension, model);
								xr_strlwr(modelWithoutExtension);
								if (strext(modelWithoutExtension)) *strext(modelWithoutExtension) = 0;

								if (xr_strcmp(modelWithoutExtension, "") != 0 && xr_strcmp(modelWithoutExtension, ".ogf") != 0)
									models.insert(modelWithoutExtension);
							}
						};

						// Insert visual from ltx
						if (pSettings->line_exist(section, "visual"))
						{
							safe_insert(models, pSettings->r_string(section, "visual"));
						}

						// Corpse visual
						/*if (pSettings->line_exist(section, "corpse_visual"))
						{
							safe_insert(models, pSettings->r_string(section, "corpse_visual"));
						}*/

						// Bloodsucker visual
						/*if (pSettings->line_exist(section, "Predator_Visual"))
						{
							safe_insert(models, pSettings->r_string(section, "Predator_Visual"));
						}*/

						auto obj = ai().alife().objects().object(obj_id);

						// Actual visual from alife object
						if (obj && obj->visual())
						{
							safe_insert(models, obj->visual()->get_visual());
						}

						if (!models.empty())
						{
							events_to_prefetch.emplace_back();
							auto& E = events_to_prefetch.back();
							E.p = P;
							E.models = std::move(models);
							E.id = obj_id;
							E.hasAlifeObject = obj != nullptr;

							if (spawn_antifreeze_debug) Msg("[ProcessGameEvents] added M_SPAWN to prefetch_events: section %s, obj_id %d, parent_id %d, event_id %d", section.c_str(), obj_id, parent_id, dest);
							it++; // Move to next event
							continue;
						}
					}					
				}
			}
#endif
//-AVO
			it++; // Move to next event
			switch (ID)
			{
			case M_SPAWN:
				{
					PROF_EVENT("ProcessGameEvents M_SPAWN");

#ifdef SPAWN_ANTIFREEZE
					if (spawn_antifreeze_debug)
					{
						u16 parent_id;
						shared_str section;
						u16 obj_id = GetSpawnInfo(P, parent_id, section);
						Msg("[ProcessGameEvents] M_SPAWN: section %s, obj_id %d, parent_id %d, event_id %d", section.c_str(), obj_id, parent_id, dest);
					}
#endif

					u16 dummy16;
					P.r_begin(dummy16);
					cl_Process_Spawn(P);
					break;
				}
			case M_EVENT:
				{
					PROF_EVENT("ProcessGameEvents M_EVENT");
					cl_Process_Event(dest, type, P);
					break;
				}
			case M_MOVE_PLAYERS:
				{
					PROF_EVENT("ProcessGameEvents M_MOVE_PLAYERS");
					u8 Count = P.r_u8();
					for (u8 i = 0; i < Count; i++)
					{
						u16 ID = P.r_u16();
						Fvector NewPos, NewDir;
						P.r_vec3(NewPos);
						P.r_vec3(NewDir);
						CActor* OActor = smart_cast<CActor*>(Objects.net_Find(ID));
						if (0 == OActor)
							break;
						OActor->MoveActor(NewPos, NewDir);
					}
					auto& PRespond = data->PRespond;
					PRespond.w_begin(M_MOVE_PLAYERS_RESPOND);
					Send(PRespond, net_flags(TRUE, TRUE));
					break;
				}
			case M_STATISTIC_UPDATE:
				{
					PROF_EVENT("ProcessGameEvents M_STATISTIC_UPDATE");
					if (GameID() != eGameIDSingle)
						Game().m_WeaponUsageStatistic->OnUpdateRequest(&P);
					break;
				}
			case M_FILE_TRANSFER:
				{
					PROF_EVENT("ProcessGameEvents M_FILE_TRANSFER");
					if (m_file_transfer) // in case of net_Stop
						m_file_transfer->on_message(&P);
					break;
				}
			case M_GAMEMESSAGE:
				{
					PROF_EVENT("ProcessGameEvents M_GAMEMESSAGE");
					Game().OnGameMessage(P);
					break;
				}
			default:
				{
					VERIFY(0);
					break;
				}
			}
		}
	}

#ifdef SPAWN_ANTIFREEZE
	if (!events_to_prefetch.empty())
	{
		bool schedule_prefetch_job = false;
		{
			xrSRWLockGuard guard(prefetch_lock);
			if (prefetch_events->empty())
				events_to_prefetch.swap(*prefetch_events);
			else
			{
				prefetch_events->reserve(prefetch_events->size() + events_to_prefetch.size());
				for (auto& event : events_to_prefetch)
					prefetch_events->push_back(std::move(event));
			}
			if (_InterlockedCompareExchange(&prefetch_job_scheduled, TRUE, FALSE) == FALSE)
				schedule_prefetch_job = true;
		}

		if (schedule_prefetch_job && !xr_jobs::submit_scoped(&CLevel::ProcessPrefetchEvents, this,
			level_job_scope, &prefetch_job_group, xr_jobs::priority::background))
		{
			// The level is already stopping. Do not execute a rejected owner-bound
			// callback synchronously against teardown state.
			_InterlockedExchange(&prefetch_job_scheduled, FALSE);
		}
	}
#endif

	events_to_process.clear();
	if (events_to_process.capacity() <= retained_net_event_capacity)
	{
		xrSRWLockGuard guard(prefetch_lock);
		if (game_events->queue.empty())
			events_to_process.swap(game_events->queue);
	}

	if (OnServer() && GameID() != eGameIDSingle)
		Game().m_WeaponUsageStatistic->Send_Check_Respond();
}

#ifdef DEBUG_MEMORY_MANAGER
extern Flags32 psAI_Flags;
extern float debug_on_frame_gather_stats_frequency;

struct debug_memory_guard
{
    inline debug_memory_guard()
    {
        mem_alloc_gather_stats(!!psAI_Flags.test(aiDebugOnFrameAllocs));
        mem_alloc_gather_stats_frequency(debug_on_frame_gather_stats_frequency);
    }
};
#endif

void CLevel::MakeReconnect()
{
	if (!Engine.Event.Peek("KERNEL:disconnect"))
	{
		Engine.Event.Defer("KERNEL:disconnect");
		char const* server_options = nullptr;
		char const* client_options = nullptr;
		if (m_caServerOptions.c_str())
		{
			server_options = xr_strdup(*m_caServerOptions);
		}
		else
		{
			server_options = xr_strdup("");
		}
		if (m_caClientOptions.c_str())
		{
			client_options = xr_strdup(*m_caClientOptions);
		}
		else
		{
			client_options = xr_strdup("");
		}
		Engine.Event.Defer("KERNEL:start", size_t(server_options), size_t(client_options));
	}
}

void CLevel::OnFrame()
{
	PROF_EVENT("CLevel::OnFrame()");

#ifdef DEBUG_MEMORY_MANAGER
    debug_memory_guard __guard__;
#endif
#ifdef DEBUG
    DBG_RenderUpdate();
#endif
	{

		Fvector temp_vector;
		m_feel_deny.feel_touch_update(temp_vector, 0.f);
		if (GameID() != eGameIDSingle)
			psDeviceFlags.set(rsDisableObjectsAsCrows, true);
		else
			psDeviceFlags.set(rsDisableObjectsAsCrows, false);
	}
	// commit events from bullet manager from prev-frame
	Device.Statistic->TEST0.Begin();
	{

		BulletManager().CommitEvents();
	}
	Device.Statistic->TEST0.End();
	// Client receive
	if (net_isDisconnected())
	{
		if (OnClient() && GameID() != eGameIDSingle)
		{
#ifdef DEBUG
            Msg("--- I'm disconnected, so clear all objects...");
#endif
			ClearAllObjects();
		}
		Engine.Event.Defer("kernel:disconnect");
		return;
	}
	else
	{
		Device.Statistic->netClient1.Begin();
		{

			ClientReceive();
		}
		Device.Statistic->netClient1.End();
	}

	{

		ProcessGameEvents();
	}
#ifdef SPAWN_ANTIFREEZE
	{
		bool queueEmpty = false;
		{
			xrSRWLockGuard g(prefetch_lock);
			queueEmpty = spawn_events->queue.empty();
		}
		if (!queueEmpty)
		{
			{

				SortSpawnEventsQueue();
			}
			{

				ProcessSpawnEvents();
			}
		}
	}
#endif

	{

		if (m_bNeed_CrPr)
			make_NetCorrectionPrediction();
		if (!g_dedicated_server)
		{
			if (g_mt_config.test(mtMap))
				Device.add_to_seq_parallel(
					fastdelegate::FastDelegate0<>(m_map_manager, &CMapManager::Update), "map_manager");
			else
				MapManager().Update();
			if (IsGameTypeSingle() && Device.dwPrecacheFrame == 0)
			{
				// XXX nitrocaster: was enabled in x-ray 1.5; to be restored or removed
				//if (g_mt_config.test(mtMap))
				//{
				//    Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(
				//    m_game_task_manager,&CGameTaskManager::UpdateTasks));
				//}
				//else
				GameTaskManager().UpdateTasks();
			}
		}
	}
	// Inherited update
	{

		inherited::OnFrame();
	}
	// Draw client/server stats
	if (!g_dedicated_server && psDeviceFlags.test(rsStatistic))
	{
		CGameFont* F = UI().Font().pFontDI;
		if (!psNET_direct_connect)
		{
			if (IsServer())
			{
				const IServerStatistic* S = Server->GetStatistic();
				F->SetHeightI(0.015f);
				F->OutSetI(0.0f, 0.5f);
				F->SetColor(D3DCOLOR_XRGB(0, 255, 0));
				F->OutNext("IN:  %4d/%4d (%2.1f%%)", S->bytes_in_real, S->bytes_in,
				           100.f * float(S->bytes_in_real) / float(S->bytes_in));
				F->OutNext("OUT: %4d/%4d (%2.1f%%)", S->bytes_out_real, S->bytes_out,
				           100.f * float(S->bytes_out_real) / float(S->bytes_out));
				F->OutNext("client_2_sever ping: %d", net_Statistic.getPing());
				F->OutNext("SPS/Sended : %4d/%4d", S->dwBytesPerSec, S->dwBytesSended);
				F->OutNext("sv_urate/cl_urate : %4d/%4d", psNET_ServerUpdate, psNET_ClientUpdate);
				F->SetColor(D3DCOLOR_XRGB(255, 255, 255));
				struct net_stats_functor
				{
					xrServer* m_server;
					CGameFont* F;

					void operator()(IClient* C)
					{
						m_server->UpdateClientStatistic(C);
						F->OutNext("0x%08x: P(%d), BPS(%2.1fK), MRR(%2d), MSR(%2d), Retried(%2d), Blocked(%2d)",
						           //Server->game->get_option_s(*C->Name,"name",*C->Name),
						           C->ID.value(),
						           C->stats.getPing(),
						           float(C->stats.getBPS()), // /1024,
						           C->stats.getMPS_Receive(),
						           C->stats.getMPS_Send(),
						           C->stats.getRetriedCount(),
						           C->stats.dwTimesBlocked);
					}
				};
				net_stats_functor tmp_functor;
				tmp_functor.m_server = Server;
				tmp_functor.F = F;
				Server->ForEachClientDo(tmp_functor);
			}
			if (IsClient())
			{
				IPureClient::UpdateStatistic();
				F->SetHeightI(0.015f);
				F->OutSetI(0.0f, 0.5f);
				F->SetColor(D3DCOLOR_XRGB(0, 255, 0));
				F->OutNext("client_2_sever ping: %d", net_Statistic.getPing());
				F->OutNext("sv_urate/cl_urate : %4d/%4d", psNET_ServerUpdate, psNET_ClientUpdate);
				F->SetColor(D3DCOLOR_XRGB(255, 255, 255));
				F->OutNext("BReceivedPs(%2d), BSendedPs(%2d), Retried(%2d), Blocked(%2d)",
				           net_Statistic.getReceivedPerSec(),
				           net_Statistic.getSendedPerSec(),
				           net_Statistic.getRetriedCount(),
				           net_Statistic.dwTimesBlocked);
#ifdef DEBUG
                if (!pStatGraphR)
                {
                    pStatGraphR = xr_new<CStatGraph>();
                    pStatGraphR->SetRect(50, 700, 300, 68, 0xff000000, 0xff000000);
                    //m_stat_graph->SetGrid(0, 0.0f, 10, 1.0f, 0xff808080, 0xffffffff);
                    pStatGraphR->SetMinMax(0.0f, 65536.0f, 1000);
                    pStatGraphR->SetStyle(CStatGraph::stBarLine);
                    pStatGraphR->AppendSubGraph(CStatGraph::stBarLine);
                }
                pStatGraphR->AppendItem(float(net_Statistic.getBPS()), 0xff00ff00, 0);
                F->OutSet(20.f, 700.f);
                F->OutNext("64 KBS");
#endif
			}
		}
	}
	else
	{
#ifdef DEBUG
        if (pStatGraphR)
            xr_delete(pStatGraphR);
#endif
	}
#ifdef DEBUG
    g_pGamePersistent->Environment().m_paused = m_bEnvPaused;
#endif
	{

		g_pGamePersistent->Environment().SetGameTime(GetEnvironmentGameDayTimeSec(),
		                                             game->GetEnvironmentGameTimeFactor());
	}
	if (!g_dedicated_server)
	{

		ai().script_engine().script_process(ScriptEngine::eScriptProcessorLevel)->update();
	}
	{

		{

			m_ph_commander->update();
		}
		{

			m_ph_commander_scripts->update();
		}
	}
	Device.Statistic->TEST0.Begin();
	{

		BulletManager().CommitRenderSet();
	}
	Device.Statistic->TEST0.End();
	// update static sounds
	if (!g_dedicated_server)
	{

		if (g_mt_config.test(mtLevelSounds))
		{
			Device.add_to_seq_parallel(fastdelegate::FastDelegate0<>(
				m_level_sound_manager, &CLevelSoundManager::Update), "level_sounds");
		}
		else
			m_level_sound_manager->Update();
	}

	// The device-owned collector already runs at the serialized post-frame GC point.
	// Do not enqueue the legacy delegate when it would only enter script_gc() and
	// immediately return; on short frames even this extra queue traffic is noise.
	if (!g_dedicated_server && !(psLua_ParallelGC && Device.LuaGC))
	{
		// Raw LuaJIT GC operates on the same VM as gameplay/UI scripts. Running it
		// through seqParallel races main-thread Lua access and can corrupt the heap.

		script_gc();
	}
	if (pStatGraphR)
	{
		static float fRPC_Mult = 10.0f;
		static float fRPS_Mult = 1.0f;
		pStatGraphR->AppendItem(float(m_dwRPC) * fRPC_Mult, 0xffff0000, 1);
		pStatGraphR->AppendItem(float(m_dwRPS) * fRPS_Mult, 0xff00ff00, 0);
	}

	{

		for (auto& pair : m_script_attachments)
			pair.second->Update();
	}
}

int psLUA_GCSTEP = 300;
int psLua_ParallelGCStep = 75;
int psLua_ParallelGCMaxStepKB = 512;
int psLua_ParallelGCStepMul = 200;
extern BOOL psLua_Profile;
extern int psLua_ProfileThresholdUS;

namespace
{
SLuaAllocationStats lua_frame_allocation_begin;

u64 allocation_counter_delta(const u64 current, const u64 previous)
{
	return current >= previous ? current - previous : current;
}

// LuaJIT 2.0/x64 must keep GC objects in its low-address arena. Normal gameplay
// defers indivisible automatic-GC phases to the serialized engine point for
// frametime stability, but doing so close to arena exhaustion can strand the VM
// at atomic/finalize while the current Lua callback keeps allocating. Above 60%
// low-address pressure correctness takes priority: allow stock allocation-driven
// GC to finish those phases synchronously until pressure falls again.
bool lua_gc_allow_host_managed_deferral(
	const u32 total_kb, const u32 pool_reserved_kb, const u32 pool_committed_kb,
	const u32 pool_fallback_kb, const u32 pool_allocation_failures)
{
	if (!psLua_ParallelGC)
		return false;

	if (pool_fallback_kb || pool_allocation_failures)
		return false;

	if (!pool_reserved_kb)
		return true;

	const u32 pressure_kb = _max(total_kb, pool_committed_kb);
	return static_cast<u64>(pressure_kb) * 5ull <
		static_cast<u64>(pool_reserved_kb) * 3ull;
}
}

void CLevel::script_gc()
{
	if (!(psLua_ParallelGC && Device.LuaGC))
	{
		PROF_EVENT();
		lua_State* lua_state = ai().script_engine().lua();
		luaJIT_setgchostmanaged(lua_state, 0);
		lua_gc(lua_state, LUA_GCSTEP, psLUA_GCSTEP);
	}
}

// demonized: bind LuaGC call to be available in device.cpp
bool CLevel::Load(u32 dwNum)
{
    inherited::Load(dwNum);

	// Build the immutable class index before gameplay starts. By default also
	// materialize shared specific-character data while the level is still loading,
	// moving the former multi-thousand-entry cold Lua/XML scan out of an NPC
	// registration frame. The switch keeps a compatibility path for mods whose
	// character-init hook intentionally depends on later runtime state.
	const bool prewarm_specific_character_data = !strstr(Core.Params, "-specific_character_lazy_load");
	CSpecificCharacter::PrewarmSelectionCache(prewarm_specific_character_data);

	// Force the object-factory CLSID table into its final sorted/indexed form at
	// the loading boundary. Later mod registrations still invalidate m_actual and
	// rebuild normally, so this does not freeze or alter the registration API.
	object_factory().prewarm_script_clsid_index();

	// Sound collections used by stalkers historically probed up to 101 candidate
	// filenames per collection during net_Spawn(). Build the immutable .ogg name
	// index while the level is loading so cold NPC voice profiles cannot turn that
	// filesystem/index probing into a gameplay hitch. Reserve more collection slots
	// only when RAM headroom allows it; the reservation changes no collection keys.
	const xr_memory_policy::snapshot memory_policy = xr_memory_policy::query();
	u32 collection_reserve = 1024;
	switch (memory_policy.mode)
	{
	case xr_memory_policy::tier::constrained: collection_reserve = 512; break;
	case xr_memory_policy::tier::balanced: collection_reserve = 1024; break;
	case xr_memory_policy::tier::performance: collection_reserve = 4096; break;
	case xr_memory_policy::tier::abundant: collection_reserve = 8192; break;
	}
	sound_collection_storage().reserve_collections(collection_reserve);
	sound_collection_storage().prewarm_file_index();

    Msg("Device.LuaGC bind");
    Device.LuaGC = fastdelegate::FastDelegate0<int>(&CLevel::LuaGC);
	Device.LuaGCMemory = fastdelegate::FastDelegate0<u32>(&CLevel::LuaGCMemory);
	Device.LuaGCStatus = fastdelegate::FastDelegate1<SLuaGCStatus*>(&CLevel::LuaGCStatus);
    Device.LuaGCDebug = fastdelegate::FastDelegate0<void>(&CLevel::LuaGCDebug);
	Device.LuaProfileFrameBegin = fastdelegate::FastDelegate0<void>(&CLevel::LuaProfileFrameBegin);
	Device.LuaProfileFrameEnd = fastdelegate::FastDelegate0<void>(&CLevel::LuaProfileFrameEnd);
	luaJIT_setgchostmanaged(ai().script_engine().lua(), psLua_ParallelGC ? 1 : 0);
	Device.ResetLuaGCAdaptiveState();
    return true;
}

// Called by the device only at serialized GC points. LuaJIT returns 2 at its
// indivisible atomic boundary and 3 around arbitrary finalizer callbacks; the
// device resumes these phases only after the rendered frame and jobs finish.
int CLevel::LuaGC()
{
	CScriptLuaStateGuard lua_guard(ai().script_engine(), "deferred_lua_gc");
	lua_State* lua_state = ai().script_engine().lua();
	const bool allow_host_managed_deferral = lua_gc_allow_host_managed_deferral(
		Device.LuaGCMemoryKB, Device.LuaGCPoolReservedKB, Device.LuaGCPoolCommittedKB,
		Device.LuaGCPoolFallbackKB, Device.LuaGCPoolAllocationFailures);
	luaJIT_setgchostmanaged(lua_state, allow_host_managed_deferral ? 1 : 0);
	const u32 configured_step_kb = static_cast<u32>(_max(psLua_ParallelGCStep, 1));
	const u32 configured_max_step_kb = static_cast<u32>(_max(psLua_ParallelGCMaxStepKB, psLua_ParallelGCStep));
	const u32 requested_step_kb = Device.LuaGCStepKB ? Device.LuaGCStepKB : configured_step_kb;
	if (Device.LuaGCEmergencyStep)
	{
		const u32 low_address_pressure_kb = _max(
			Device.LuaGCMemoryKB, Device.LuaGCPoolCommittedKB);
		const bool severe_pool_pressure =
			(Device.LuaGCPoolReservedKB &&
				static_cast<u64>(low_address_pressure_kb) * 5ull >=
				static_cast<u64>(Device.LuaGCPoolReservedKB) * 4ull) ||
			Device.LuaGCPoolFallbackKB >= 64u * 1024u ||
			Device.LuaGCPoolAllocationFailures != 0;
		const u32 emergency_floor_kb = severe_pool_pressure ? 256u : 128u;
		const u32 emergency_cap_kb = _max(configured_max_step_kb, severe_pool_pressure ? 1024u : 512u);
		Device.LuaGCStepKB = _min(_max(requested_step_kb, emergency_floor_kb), emergency_cap_kb);
	}
	else
	{
		// psLua_ParallelGCStep is the starting quantum, not a permanent ceiling.
		// The device controller scales it to consume the measured time window, while
		// this explicit max remains the gameplay safety clamp.
		Device.LuaGCStepKB = _min(_max(requested_step_kb, 8u), configured_max_step_kb);
	}

	// lua_gc(LUA_GCSTEP, n) is not a time/work budget: n lowers the allocation
	// threshold and a mostly-live heap can make one call finish an entire sweep.
	// Use the engine-only exact-quantum API, while leaving collectgarbage() and the
	// public Lua 5.1 API untouched for mods. A 1 MB minimum allocation headroom
	// prevents the scheduler's many small allocations from immediately chaining
	// more automatic steps before the next serialized maintenance slice. Normal
	// gameplay gets enough headroom to keep allocation-driven GC out of the script
	// update path; emergency mode tightens it again so collection catches bursts.
	const int previous_step_multiplier = lua_gc(
		lua_state, LUA_GCSETSTEPMUL, _max(psLua_ParallelGCStepMul, 200));
	const size_t allocation_headroom_kb = Device.LuaGCEmergencyStep ?
		_max<size_t>(static_cast<size_t>(Device.LuaGCStepKB) * 4u, 1024u) :
		_max<size_t>(static_cast<size_t>(Device.LuaGCStepKB) * 16u, 8192u);
	const size_t work_bytes = static_cast<size_t>(Device.LuaGCStepKB) << 10;

	// LuaJIT 2.0 atomic is normally one indivisible stop-the-world operation.
	// Before committing it, spend bounded post-frame slices on the same root/gray
	// marking work that atomic() would otherwise have to drain in one call. The
	// actual stock atomic commit still runs afterwards, so collector semantics are
	// unchanged. Require two consecutive quiescent probes to avoid committing just
	// after a frame that repopulated gray/grayagain. Critical pool pressure bypasses
	// this smoothing and finishes the collector immediately to protect against OOM.
	if (Device.LuaGCNativeState == 2u && !Device.LuaGCEmergencyStep && allow_host_managed_deferral)
	{
		const size_t preatomic_work_bytes = _min<size_t>(work_bytes, 64u * 1024u);
		const bool preatomic_ready = luaJIT_gcpreatomic(lua_state, preatomic_work_bytes) != 0;
		if (!preatomic_ready)
		{
			Device.LuaGCAtomicPrepared = false;
			lua_gc(lua_state, LUA_GCSETSTEPMUL, previous_step_multiplier);
			return 4; // bounded atomic pre-pass made progress, commit still deferred
		}
		if (!Device.LuaGCAtomicPrepared)
		{
			Device.LuaGCAtomicPrepared = true;
			lua_gc(lua_state, LUA_GCSETSTEPMUL, previous_step_multiplier);
			return 5; // first quiescent probe; verify once more on the next post-frame pass
		}
		Device.LuaGCAtomicPrepared = false;
	}
	else if (Device.LuaGCNativeState != 2u)
	{
		Device.LuaGCAtomicPrepared = false;
	}

	if (Device.LuaGCNativeState == 2u)
		++Device.LuaGCAtomicCommits;
	const int result = luaJIT_gcstep(
		lua_state, work_bytes, allocation_headroom_kb << 10);
	if (result != 2)
		Device.LuaGCAtomicPrepared = false;
	lua_gc(lua_state, LUA_GCSETSTEPMUL, previous_step_multiplier);
	return result;
}

u32 CLevel::LuaGCMemory()
{
	CScriptLuaStateGuard lua_guard(ai().script_engine(), "deferred_lua_gc_memory");
	return static_cast<u32>(lua_gc(ai().script_engine().lua(), LUA_GCCOUNT, 0));
}

void CLevel::LuaGCStatus(SLuaGCStatus* status)
{
	if (!status)
		return;

	CScriptLuaStateGuard lua_guard(ai().script_engine(), "deferred_lua_gc_status");
	lua_State* lua_state = ai().script_engine().lua();
	luaJIT_GCStateInfo info = {};
	luaJIT_getgcstate(lua_state, &info);
	luaJIT_GCAtomicInfoV2 atomic_info = {};
	luaJIT_getgcatomicinfo_v2(lua_state, &atomic_info);
	luaJIT_GCPressureInfo pressure_info = {};
	luaJIT_getgcpressureinfo(lua_state, &pressure_info);

	const size_t max_u32 = static_cast<size_t>(u32(-1));
	status->state = static_cast<u32>(info.state);
	status->total_kb = static_cast<u32>(_min<size_t>(info.total >> 10, max_u32));
	status->debt_kb = static_cast<u32>(_min<size_t>(info.debt >> 10, max_u32));
	status->threshold_kb = static_cast<u32>(_min<size_t>(info.threshold >> 10, max_u32));
	const unsigned long long atomic_total_cycles = atomic_info.total_cycles;
	auto atomic_permille = [atomic_total_cycles](unsigned long long phase_cycles) -> u32
	{
		return atomic_total_cycles ? static_cast<u32>(_min<unsigned long long>(
			phase_cycles * 1000ull / atomic_total_cycles, 1000ull)) : 0u;
	};
	status->atomic_mark_permille = atomic_permille(atomic_info.mark_cycles);
	status->atomic_finalize_permille = atomic_permille(atomic_info.finalize_cycles);
	status->atomic_weak_permille = atomic_permille(atomic_info.weak_cycles);
	status->atomic_udata_visited = static_cast<u32>(_min<size_t>(atomic_info.udata_visited, max_u32));
	status->atomic_udata_finalizable = static_cast<u32>(_min<size_t>(atomic_info.udata_finalizable, max_u32));
	status->atomic_udata_pages = static_cast<u32>(_min<size_t>(atomic_info.udata_pages, max_u32));
	status->atomic_weak_tables = static_cast<u32>(_min<size_t>(atomic_info.weak_tables, max_u32));
	status->atomic_weak_slots = static_cast<u32>(_min<size_t>(atomic_info.weak_slots, max_u32));
	status->udata_alloc_serial = static_cast<u64>(pressure_info.udata_alloc_serial);

#ifdef _WIN64
	// LuaJIT's logical GC heap can be substantially smaller than the actual
	// low-address allocator occupancy because of allocator/segment fragmentation.
	// Sample the allocator at the serialized GC point so pressure decisions are
	// based on address-space usage that can actually cause an allocation failure.
	xr_luajit_pool_stats pool_stats = {};
	XR_GET_POOL_STATS(&pool_stats);
	status->pool_reserved_kb = static_cast<u32>(
		_min<size_t>(pool_stats.reserved_bytes >> 10, max_u32));
	status->pool_committed_kb = static_cast<u32>(
		_min<size_t>(pool_stats.committed_bytes >> 10, max_u32));
	status->pool_fallback_kb = static_cast<u32>(
		_min<size_t>(pool_stats.fallback_bytes >> 10, max_u32));
	status->pool_allocation_failures = static_cast<u32>(
		_min<size_t>(pool_stats.allocation_failures, max_u32));
#endif

	const bool allow_host_managed_deferral = lua_gc_allow_host_managed_deferral(
		status->total_kb, status->pool_reserved_kb, status->pool_committed_kb,
		status->pool_fallback_kb, status->pool_allocation_failures);
	luaJIT_setgchostmanaged(lua_state, allow_host_managed_deferral ? 1 : 0);
	status->cycle_active = info.active != 0;
}

void CLevel::LuaProfileFrameBegin()
{
	CScriptEngine& script_engine = ai().script_engine();
	const bool manual_profile = psLua_Profile != FALSE;
	const bool record_profile = script_engine.lua_recording();

	const u32 threshold_us = record_profile ? script_engine.lua_record_threshold_us() :
		static_cast<u32>(_max(psLua_ProfileThresholdUS, 0));

	script_engine.set_lua_hotspot_profiler(manual_profile || record_profile, threshold_us);
	script_engine.set_lua_allocation_tracking(manual_profile || record_profile || false);

	Device.LuaFrameAllocCalls = 0;
	Device.LuaFrameReallocCalls = 0;
	Device.LuaFrameFreeCalls = 0;
	Device.LuaFrameAllocatedBytes = 0;
	Device.LuaFrameFreedBytes = 0;

	if (!manual_profile)
		return;

	script_engine.lua_allocation_stats(lua_frame_allocation_begin);
}

void CLevel::LuaProfileFrameEnd()
{
	if (!psLua_Profile)
		return;

	SLuaAllocationStats frame_end;
	ai().script_engine().lua_allocation_stats(frame_end);

	Device.LuaFrameAllocCalls = allocation_counter_delta(frame_end.alloc_calls, lua_frame_allocation_begin.alloc_calls);
	Device.LuaFrameReallocCalls = allocation_counter_delta(frame_end.realloc_calls, lua_frame_allocation_begin.realloc_calls);
	Device.LuaFrameFreeCalls = allocation_counter_delta(frame_end.free_calls, lua_frame_allocation_begin.free_calls);
	Device.LuaFrameAllocatedBytes = allocation_counter_delta(
		frame_end.allocated_bytes, lua_frame_allocation_begin.allocated_bytes);
	Device.LuaFrameFreedBytes = allocation_counter_delta(
		frame_end.freed_bytes, lua_frame_allocation_begin.freed_bytes);
}

void CLevel::LuaGCDebug()
{
	static u32 last_report_frame = 0;
	static u64 frame_wait_sum_us = 0;
	static u64 gc_wait_sum_us = 0;
	static u64 gc_time_sum_us = 0;
	static u64 gc_call_sum = 0;
	static u32 frame_wait_max_us = 0;
	static u32 gc_wait_max_us = 0;
	static u32 gc_time_max_us = 0;
	static u32 gc_step_max_us = 0;
	static u32 gc_call_max = 0;
	static u32 budget_hit_count = 0;
	static u32 call_cap_hit_count = 0;
	static u64 budget_util_sum = 0;
	static u32 budget_util_max = 0;
	static u32 progress_max_kb = 0;
	static u32 cycle_age_max_frames = 0;
	static u32 emergency_count = 0;
	static u32 active_count = 0;
	static u32 native_active_count = 0;
	static u32 native_assist_count = 0;
	static u32 debt_max_kb = 0;
	static u32 sample_count = 0;

	frame_wait_sum_us += Device.FrameParallelWaitUS;
	gc_wait_sum_us += Device.LuaGCWaitUS;
	gc_time_sum_us += Device.LuaGCBudgetUsedUS;
	gc_call_sum += static_cast<u32>(_max(Device.LuaGCCount, 0));
	frame_wait_max_us = _max(frame_wait_max_us, Device.FrameParallelWaitUS);
	gc_wait_max_us = _max(gc_wait_max_us, Device.LuaGCWaitUS);
	gc_time_max_us = _max(gc_time_max_us, Device.LuaGCBudgetUsedUS);
	gc_step_max_us = _max(gc_step_max_us, Device.LuaGCStepMaxUS);
	gc_call_max = _max(gc_call_max, static_cast<u32>(_max(Device.LuaGCCount, 0)));
	budget_hit_count += Device.LuaGCBudgetHit ? 1u : 0u;
	call_cap_hit_count += Device.LuaGCCallCapHit ? 1u : 0u;
	budget_util_sum += Device.LuaGCBudgetUtilPercent;
	budget_util_max = _max(budget_util_max, Device.LuaGCBudgetUtilPercent);
	progress_max_kb = _max(progress_max_kb, Device.LuaGCProgressKB);
	cycle_age_max_frames = _max(cycle_age_max_frames, Device.LuaGCCycleAgeFrames);
	emergency_count += Device.LuaGCEmergencyStep ? 1u : 0u;
	active_count += Device.LuaGCCycleActive ? 1u : 0u;
	native_active_count += Device.LuaGCNativeCycleActive ? 1u : 0u;
	native_assist_count += Device.LuaGCAssistingNativeCycle ? 1u : 0u;
	debt_max_kb = _max(debt_max_kb, Device.LuaGCDebtKB);
	++sample_count;

	// Logging every 1 ms wait produced more than 1500 lines in one profiling run
	// and added its own formatting/file-lock pressure. Preserve every maximum in
	// the aggregation window, but emit at most once per 120 frames.
	if (Device.dwFrame - last_report_frame < 120)
		return;

	last_report_frame = Device.dwFrame;
	const u32 frame_wait_avg_us = sample_count ? static_cast<u32>(frame_wait_sum_us / sample_count) : 0;
	const u32 gc_wait_avg_us = sample_count ? static_cast<u32>(gc_wait_sum_us / sample_count) : 0;
	const u32 gc_time_avg_us = sample_count ? static_cast<u32>(gc_time_sum_us / sample_count) : 0;
	const u32 gc_call_avg = sample_count ? static_cast<u32>(gc_call_sum / sample_count) : 0;
	const u32 budget_util_avg = sample_count ? static_cast<u32>(budget_util_sum / sample_count) : 0;

	Msg("[Lua GC] mem=%u/%u KB committed=%u KB fallback=%u KB alloc_fail=%u growth=%u/%u KB scale=%u reclaim=%u KB/%u%% "
		"samples=%u calls(avg/max)=%u/%u step=%u KB stepmul=%d "
		"step_max=%u us time(avg/max)=%u/%u us budget=%u us wait(avg/max)=%u/%u us "
		"frame_wait(avg/max)=%u/%u us backoff=%u wait_streak=%u last_wait=%u us "
		"cycle=%s active=%u native_state=%u native_active=%u native_assist=%u "
		"debt=%u KB debt_max=%u KB threshold=%u KB budget_hits=%u call_cap_hits=%u "
		"util(avg/max)=%u/%u%% progress_max=%u KB cycle_age_max=%u frames emergency=%u",
		Device.LuaGCMemoryKB,
		Device.LuaGCPoolReservedKB,
		Device.LuaGCPoolCommittedKB,
		Device.LuaGCPoolFallbackKB,
		Device.LuaGCPoolAllocationFailures,
		Device.LuaGCMemoryGrowthKB,
		Device.LuaGCTriggerGrowthKB,
		Device.LuaGCGrowthScale,
		Device.LuaGCCycleReclaimedKB,
		Device.LuaGCReclaimPercent,
		sample_count,
		gc_call_avg,
		gc_call_max,
		Device.LuaGCStepKB,
		psLua_ParallelGCStepMul,
		gc_step_max_us,
		gc_time_avg_us,
		gc_time_max_us,
		Device.LuaGCBudgetUS,
		gc_wait_avg_us,
		gc_wait_max_us,
		frame_wait_avg_us,
		frame_wait_max_us,
		Device.LuaGCBackoffFrames,
		Device.LuaGCWaitStreak,
		Device.LuaGCLastWaitUS,
		Device.LuaGCCycleActive ? "active" : (Device.LuaGCDone ? "done" : "idle"),
		active_count,
		Device.LuaGCNativeState,
		native_active_count,
		native_assist_count,
		Device.LuaGCDebtKB,
		debt_max_kb,
		Device.LuaGCThresholdKB,
		budget_hit_count,
		call_cap_hit_count,
		budget_util_avg,
		budget_util_max,
		progress_max_kb,
		cycle_age_max_frames,
		emergency_count);

	if (psLua_Profile)
	{
		Msg("[Lua alloc/frame] alloc=%llu realloc=%llu free=%llu allocated=%.2f KB freed=%.2f KB",
			static_cast<unsigned long long>(Device.LuaFrameAllocCalls),
			static_cast<unsigned long long>(Device.LuaFrameReallocCalls),
			static_cast<unsigned long long>(Device.LuaFrameFreeCalls),
			static_cast<double>(Device.LuaFrameAllocatedBytes) / 1024.0,
			static_cast<double>(Device.LuaFrameFreedBytes) / 1024.0);
	}

	frame_wait_sum_us = 0;
	gc_wait_sum_us = 0;
	gc_time_sum_us = 0;
	gc_call_sum = 0;
	frame_wait_max_us = 0;
	gc_wait_max_us = 0;
	gc_time_max_us = 0;
	gc_step_max_us = 0;
	gc_call_max = 0;
	budget_hit_count = 0;
	call_cap_hit_count = 0;
	budget_util_sum = 0;
	budget_util_max = 0;
	progress_max_kb = 0;
	cycle_age_max_frames = 0;
	emergency_count = 0;
	active_count = 0;
	native_active_count = 0;
	native_assist_count = 0;
	debt_max_kb = 0;
	sample_count = 0;
}

#ifdef DEBUG_PRECISE_PATH
void test_precise_path();
#endif

#ifdef DEBUG
extern Flags32 dbg_net_Draw_Flags;
#endif

extern void draw_wnds_rects();
extern bool use_reshade;
extern void render_reshade_effects();

extern int ps_r4_hdr10_pda; // NOTE: this is a hack to avoid double HDR tonemapping the PDA

void CLevel::OnRender()
{
	// PDA
	if (game && CurrentGameUI() && &CurrentGameUI()->GetPdaMenu() != nullptr)
	{
		CUIPdaWnd* pda = &CurrentGameUI()->GetPdaMenu();
		if (psActorFlags.test(AF_3D_PDA) && pda->IsShown())
		{
			ps_r4_hdr10_pda = 1; // !!! HACK !!!

			pda->Draw();
			CUICursor* cursor = &UI().GetUICursor();

			if (cursor)
			{
				static bool need_reset;
				bool is_top = CurrentGameUI()->TopInputReceiver() == pda;

				if (pda->IsEnabled() && is_top && !Console->bVisible)
				{
					if (need_reset)
					{
						need_reset = false;
						pda->ResetCursor();
					}

					Frect &pda_border = pda->m_cursor_box;
					Fvector2 cursor_pos = cursor->GetCursorPosition();

					if (!pda_border.in(cursor_pos))
					{
						clamp(cursor_pos.x, pda_border.left, pda_border.right);
						clamp(cursor_pos.y, pda_border.top, pda_border.bottom);
						cursor->SetUICursorPosition(cursor_pos);
					}

					Fvector2 cursor_pos_dif;
					cursor_pos_dif.set(cursor_pos);
					cursor_pos_dif.sub(pda->last_cursor_pos);
					pda->last_cursor_pos.set(cursor_pos);
					pda->MouseMovement(cursor_pos_dif.x, cursor_pos_dif.y);
				}
				else
					need_reset = true;

				if (is_top)
					cursor->OnRender();
			}
			Render->RenderToTarget(Render->rtPDA);

			ps_r4_hdr10_pda = 0;
		}

		if (Actor() && Actor()->m_bDelayDrawPickupItems)
		{
			Actor()->m_bDelayDrawPickupItems = false;
			Actor()->DrawPickupItems();
		}
	}

	inherited::OnRender();
	if (!game)
		return;
	Game().OnRender();
	BulletManager().Render();

	if (Device.m_SecondViewport.IsSVPFrame())
		Render->RenderToTarget(Render->rtSVP);

	if (use_reshade)
		render_reshade_effects();

	HUD().RenderUI();

	ScriptDebugRender();

#ifdef DEBUG
    draw_wnds_rects();
    physics_world()->OnRender();
#endif
#ifdef DEBUG
    if (ai().get_level_graph())
        ai().level_graph().render();
#ifdef DEBUG_PRECISE_PATH
    test_precise_path();
#endif
    CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(Level().CurrentEntity());
    if (stalker)
        stalker->OnRender();
    if (bDebug)
    {
        for (u32 I = 0; I < Level().Objects.o_count(); I++)
        {
            CObject* _O = Level().Objects.o_get_by_iterator(I);
            CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(_O);
            if (stalker)
                stalker->OnRender();
            CCustomMonster* monster = smart_cast<CCustomMonster*>(_O);
            if (monster)
                monster->OnRender();
            CPhysicObject* physic_object = smart_cast<CPhysicObject*>(_O);
            if (physic_object)
                physic_object->OnRender();
            CSpaceRestrictor* space_restrictor = smart_cast<CSpaceRestrictor*>(_O);
            if (space_restrictor)
                space_restrictor->OnRender();
            CClimableObject* climable = smart_cast<CClimableObject*>(_O);
            if (climable)
                climable->OnRender();
            CTeamBaseZone* team_base_zone = smart_cast<CTeamBaseZone*>(_O);
            if (team_base_zone)
                team_base_zone->OnRender();
            if (GameID() != eGameIDSingle)
            {
                CInventoryItem* pIItem = smart_cast<CInventoryItem*>(_O);
                if (pIItem)
                    pIItem->OnRender();
            }
            if (dbg_net_Draw_Flags.test(dbg_draw_skeleton)) //draw skeleton
            {
                CGameObject* pGO = smart_cast<CGameObject*>	(_O);
                if (pGO && pGO != Level().CurrentViewEntity() && !pGO->H_Parent())
                {
                    if (pGO->Position().distance_to_sqr(Device.vCameraPosition) < 400.0f)
                    {
                        pGO->dbg_DrawSkeleton();
                    }
                }
            }
        }
        //  [7/5/2005]
        if (Server && Server->game) Server->game->OnRender();
        //  [7/5/2005]
        ObjectSpace.dbgRender();
        UI().Font().pFontStat->OutSet(170, 630);
        UI().Font().pFontStat->SetHeight(16.0f);
        UI().Font().pFontStat->SetColor(0xffff0000);
        if (Server)
            UI().Font().pFontStat->OutNext("Client Objects:      [%d]", Server->GetEntitiesNum());
        UI().Font().pFontStat->OutNext("Server Objects:      [%d]", Objects.o_count());
        UI().Font().pFontStat->OutNext("Interpolation Steps: [%d]", Level().GetInterpolationSteps());
        if (Server)
        {
            UI().Font().pFontStat->OutNext("Server updates size: [%d]", Server->GetLastUpdatesSize());
        }
        UI().Font().pFontStat->SetHeight(8.0f);
    }
#endif
	debug_renderer().render();
#ifdef DEBUG
    if (bDebug)
    {
        DBG().draw_object_info();
        DBG().draw_text();
        DBG().draw_level_info();
    }
    DBG().draw_debug_text();
    if (psAI_Flags.is(aiVision))
    {
        for (u32 I = 0; I < Level().Objects.o_count(); I++)
        {
            CObject* object = Objects.o_get_by_iterator(I);
            CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(object);
            if (!stalker)
                continue;
            stalker->dbg_draw_vision();
        }
    }

    if (psAI_Flags.test(aiDrawVisibilityRays))
    {
        for (u32 I = 0; I < Level().Objects.o_count(); I++)
        {
            CObject* object = Objects.o_get_by_iterator(I);
            CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(object);
            if (!stalker)
                continue;
            stalker->dbg_draw_visibility_rays();
        }
    }
#endif
}

void CLevel::ScriptDebugRender()
{
	if (!m_debug_render_queue.size())
		return;

	bool hasVisibleObj = false;
	auto it = m_debug_render_queue.begin();
	auto it_e = m_debug_render_queue.end();
	for (; it != it_e; ++it)
	{
		DBG_ScriptObject* obj = (*it).second;
		if (obj->m_visible) {
			hasVisibleObj = true;
			obj->Render();
		}
	}

	// demonized: fix of showing console window when there are no visible gizmos 
	if (hasVisibleObj)
		DRender->OnFrameEnd();
}

script_attachment* CLevel::add_attachment(LPCSTR name, script_attachment* att)
{
	R_ASSERT(att);
	remove_child(name, true);
	m_script_attachments.emplace(mk_pair(name, att));
	return att;
}

script_attachment* CLevel::get_attachment(LPCSTR name)
{
	if (m_script_attachments.size())
	{
		auto& att = m_script_attachments.find(name);
		if (att != m_script_attachments.end())
			return att->second;
	}

	return nullptr;
}

void CLevel::remove_child(LPCSTR name, bool destroy)
{
	script_attachment* attachment = get_attachment(name);
	if (!attachment)
		return;

	if (destroy)
		xr_delete(attachment);

	m_script_attachments.erase(name);
}

void CLevel::remove_attachment(script_attachment* child)
{
	if (!child) return;
	if (m_script_attachments.size())
	{
		script_attachment* attachment = get_attachment(child->GetName());
		if (!attachment || attachment != child)
			return;

		remove_child(child->GetName(), true);
	}
}

void CLevel::iterate_attachments(::luabind::functor<bool> functor)
{
	if (!m_script_attachments.size())
		return;

	for (auto& pair : m_script_attachments)
		if (functor(pair.first.c_str(), pair.second) == true)
			return;
}

void CLevel::OnEvent(EVENT E, u64 P1, u64 /**P2/**/)
{
	if (E == eEntitySpawn)
	{
		char Name[128];
		Name[0] = 0;
		sscanf(LPCSTR(P1), "%s", Name);
		Level().g_cl_Spawn(Name, 0xff, M_SPAWN_OBJECT_LOCAL, Fvector().set(0, 0, 0));
	}
	else if (E == eChangeRP && P1)
	{
	}
	else if (E == eDemoPlay && P1)
	{
		char* name = (char*)P1;
		string_path RealName;
		xr_strcpy(RealName, name);
		xr_strcat(RealName, ".xrdemo");
		Cameras().AddCamEffector(xr_new<CDemoPlay>(RealName, 1.3f, 0));
	}
	else if (E == eChangeTrack && P1)
	{
		// int id = atoi((char*)P1);
		// Environment->Music_Play(id);
	}
	else if (E == eEnvironment)
	{
		// int id=0; float s=1;
		// sscanf((char*)P1,"%d,%f",&id,&s);
		// Environment->set_EnvMode(id,s);
	}
}

void CLevel::AddObject_To_Objects4CrPr(CGameObject* pObj)
{
	if (!pObj)
		return;
	for (CGameObject* obj : pObjects4CrPr)
	{
		if (obj == pObj)
			return;
	}
	pObjects4CrPr.push_back(pObj);
}

void CLevel::AddActor_To_Actors4CrPr(CGameObject* pActor)
{
	if (!pActor)
		return;
	if (!smart_cast<CActor*>(pActor)) return;
	for (CGameObject* act : pActors4CrPr)
	{
		if (act == pActor)
			return;
	}
	pActors4CrPr.push_back(pActor);
}

void CLevel::RemoveObject_From_4CrPr(CGameObject* pObj)
{
	if (!pObj)
		return;
	auto objIt = std::find(pObjects4CrPr.begin(), pObjects4CrPr.end(), pObj);
	if (objIt != pObjects4CrPr.end())
	{
		pObjects4CrPr.erase(objIt);
	}
	auto aIt = std::find(pActors4CrPr.begin(), pActors4CrPr.end(), pObj);
	if (aIt != pActors4CrPr.end())
	{
		pActors4CrPr.erase(aIt);
	}
}

void CLevel::make_NetCorrectionPrediction()
{
	m_bNeed_CrPr = false;
	m_bIn_CrPr = true;
	u64 NumPhSteps = physics_world()->StepsNum();
	physics_world()->StepsNum() -= m_dwNumSteps;
	if (ph_console::g_bDebugDumpPhysicsStep && m_dwNumSteps > 10)
	{
		Msg("!!!TOO MANY PHYSICS STEPS FOR CORRECTION PREDICTION = %d !!!", m_dwNumSteps);
		m_dwNumSteps = 10;
	}
	physics_world()->Freeze();
	//setting UpdateData and determining number of PH steps from last received update
	for (CGameObject* obj : pObjects4CrPr)
	{
		if (!obj)
			continue;
		obj->PH_B_CrPr();
	}
	//first prediction from "delivered" to "real current" position
	//making enought PH steps to calculate current objects position based on their updated state
	for (u32 i = 0; i < m_dwNumSteps; i++)
	{
		physics_world()->Step();

		for (CGameObject* act : pActors4CrPr)
		{
			if (!act || act->CrPr_IsActivated())
				continue;
			act->PH_B_CrPr();
		}
	}
	for (CGameObject* obj : pObjects4CrPr)
	{
		if (!obj)
			continue;
		obj->PH_I_CrPr();
	}
	if (!InterpolationDisabled())
	{
		for (u32 i = 0; i < lvInterpSteps; i++) //second prediction "real current" to "future" position
		{
			physics_world()->Step();
		}
		for (CGameObject* obj : pObjects4CrPr)
		{
			if (!obj)
				continue;
			obj->PH_A_CrPr();
		}
	}
	physics_world()->UnFreeze();
	physics_world()->StepsNum() = NumPhSteps;
	m_dwNumSteps = 0;
	m_bIn_CrPr = false;
	pObjects4CrPr.clear();
	pActors4CrPr.clear();
}

u32 CLevel::GetInterpolationSteps()
{
	return lvInterpSteps;
}

void CLevel::UpdateDeltaUpd(u32 LastTime)
{
	u32 CurrentDelta = LastTime - m_dwLastNetUpdateTime;
	if (CurrentDelta < m_dwDeltaUpdate)
		CurrentDelta = iFloor(float(m_dwDeltaUpdate * 10 + CurrentDelta) / 11);
	m_dwLastNetUpdateTime = LastTime;
	m_dwDeltaUpdate = CurrentDelta;
	if (0 == g_cl_lvInterp)
		ReculcInterpolationSteps();
	else if (g_cl_lvInterp > 0)
	{
		lvInterpSteps = iCeil(g_cl_lvInterp / fixed_step);
	}
}

void CLevel::ReculcInterpolationSteps()
{
	lvInterpSteps = iFloor(float(m_dwDeltaUpdate) / (fixed_step * 1000));
	if (lvInterpSteps > 60)
		lvInterpSteps = 60;
	if (lvInterpSteps < 3)
		lvInterpSteps = 3;
}

bool CLevel::InterpolationDisabled()
{
	return g_cl_lvInterp < 0;
}

void CLevel::PhisStepsCallback(u32 Time0, u32 Time1)
{
	if (!Level().game)
		return;
	if (GameID() == eGameIDSingle)
		return;
	//#pragma todo("Oles to all: highly inefficient and slow!!!")
	//fixed (Andy)
	/*
	for (xr_vector<CObject*>::iterator O=Level().Objects.objects.begin(); O!=Level().Objects.objects.end(); ++O)
	{
	if( smart_cast<CActor*>((*O)){
	CActor* pActor = smart_cast<CActor*>(*O);
	if (!pActor || pActor->Remote()) continue;
	pActor->UpdatePosStack(Time0, Time1);
	}
	};
	*/
}

void CLevel::SetNumCrSteps(u32 NumSteps)
{
	m_bNeed_CrPr = true;
	if (m_dwNumSteps > NumSteps)
		return;
	m_dwNumSteps = NumSteps;
	if (m_dwNumSteps > 1000000)
	{
		VERIFY(0);
	}
}

ALife::_TIME_ID CLevel::GetStartGameTime()
{
	return (game->GetStartGameTime());
}

ALife::_TIME_ID CLevel::GetGameTime()
{
	return (game->GetGameTime());
}

ALife::_TIME_ID CLevel::GetEnvironmentGameTime()
{
	return (game->GetEnvironmentGameTime());
}

u8 CLevel::GetDayTime()
{
	u32 dummy32, hours;
	GetGameDateTime(dummy32, dummy32, dummy32, hours, dummy32, dummy32, dummy32);
	VERIFY(hours < 256);
	return u8(hours);
}

float CLevel::GetGameDayTimeSec()
{
	return (float(s64(GetGameTime() % (24 * 60 * 60 * 1000))) / 1000.f);
}

u32 CLevel::GetGameDayTimeMS()
{
	return (u32(s64(GetGameTime() % (24 * 60 * 60 * 1000))));
}

float CLevel::GetEnvironmentGameDayTimeSec()
{
	return (float(s64(GetEnvironmentGameTime() % (24 * 60 * 60 * 1000))) / 1000.f);
}

void CLevel::GetGameDateTime(u32& year, u32& month, u32& day, u32& hours, u32& mins, u32& secs, u32& milisecs)
{
	split_time(GetGameTime(), year, month, day, hours, mins, secs, milisecs);
}

float CLevel::GetGameTimeFactor()
{
	return (game ? game->GetGameTimeFactor() : 1.0f);
}

void CLevel::SetGameTimeFactor(const float fTimeFactor)
{
	game->SetGameTimeFactor(fTimeFactor);
}

void CLevel::SetGameTimeFactor(ALife::_TIME_ID GameTime, const float fTimeFactor)
{
	game->SetGameTimeFactor(GameTime, fTimeFactor);
}

void CLevel::SetEnvironmentGameTimeFactor(u64 const& GameTime, float const& fTimeFactor)
{
	if (!game)
		return;
	game->SetEnvironmentGameTimeFactor(GameTime, fTimeFactor);
}

bool CLevel::IsServer()
{
	if (!Server || IsDemoPlayStarted())
		return false;
	return true;
}

bool CLevel::IsClient()
{
	if (IsDemoPlayStarted())
		return true;
	if (Server)
		return false;
	return true;
}

void CLevel::OnAlifeSimulatorUnLoaded()
{
	MapManager().ResetStorage();
	GameTaskManager().ResetStorage();
	delete_data(m_debug_render_queue);
}

void CLevel::OnAlifeSimulatorLoaded()
{
	MapManager().ResetStorage();
	GameTaskManager().ResetStorage();
	delete_data(m_debug_render_queue);
}

void CLevel::OnSessionTerminate(LPCSTR reason)
{
	MainMenu()->OnSessionTerminate(reason);
}

u32 GameID()
{
	return Game().Type();
}

CZoneList* CLevel::create_hud_zones_list()
{
	hud_zones_list = xr_new<CZoneList>();
	hud_zones_list->clear();
	return hud_zones_list;
}

bool CZoneList::feel_touch_contact(CObject* O)
{
	TypesMapIt it = m_TypesMap.find(O->cNameSect());
	bool res = (it != m_TypesMap.end());
	CCustomZone* pZone = smart_cast<CCustomZone*>(O);
	if (pZone && !pZone->IsEnabled())
	{
		res = false;
	}
	return res;
}

CZoneList::CZoneList()
{}

CZoneList::~CZoneList()
{
	clear();
	destroy();
}
