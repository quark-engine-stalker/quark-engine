// xrCDB.cpp : Defines the entry point for the DLL application.
//

#include "stdafx.h"
#pragma hdrstop

#include "xrCDB.h"
#include "../xrCore/job_system.h"

#ifdef USE_ARENA_ALLOCATOR
static const u32	s_arena_size = (128+16)*1024*1024;
static char			s_fake_array[s_arena_size];
//doug_lea_allocator	g_collision_allocator( s_fake_array, s_arena_size, "collision" );
#endif // #ifdef USE_ARENA_ALLOCATOR

namespace Opcode
{
#	include "OPC_TreeBuilders.h"
} // namespace Opcode

using namespace CDB;
using namespace Opcode;

//BOOL APIENTRY DllMain( HANDLE hModule, 
BOOL DllMainIgnore1(HANDLE hModule,
                    u32 ul_reason_for_call,
                    LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

namespace
{
struct BTHREAD_params
{
	MODEL* model;
	Fvector* vertices;
	int vertex_count;
	TRI* triangles;
	int triangle_count;
	build_callback* callback;
	void* callback_params;
	HANDLE input_consumed_event;
};

void signal_input_consumed(HANDLE event_handle, bool& input_consumed)
{
	if (!event_handle || input_consumed)
		return;

	const BOOL signal_result = SetEvent(event_handle);
	R_ASSERT2(signal_result != FALSE, "Failed to signal CDB input-consumed event");
	input_consumed = true;
}

constexpr u32 triangle_index_block_size = 8192;

struct triangle_index_context
{
	const TRI* triangles;
	u32* indices;
	u32 triangle_count;
	__declspec(align(64)) volatile LONG next;
};

void fill_triangle_indices_job(void* raw_context)
{
	triangle_index_context& context = *static_cast<triangle_index_context*>(raw_context);
	for (;;)
	{
		const LONG start_value = InterlockedExchangeAdd(&context.next, static_cast<LONG>(triangle_index_block_size));
		if (start_value < 0 || static_cast<u32>(start_value) >= context.triangle_count)
			return;

		const u32 start = static_cast<u32>(start_value);
		const u32 end = _min(start + triangle_index_block_size, context.triangle_count);
		for (u32 triangle_index = start; triangle_index < end; ++triangle_index)
		{
			const TRI& triangle = context.triangles[triangle_index];
			u32* destination = context.indices + triangle_index * 3;
			destination[0] = triangle.verts[0];
			destination[1] = triangle.verts[1];
			destination[2] = triangle.verts[2];
		}
	}
}
} // namespace

// Model building
MODEL::MODEL()
#ifdef PROFILE_CRITICAL_SECTIONS
	: cs(MUTEX_PROFILE_ID(MODEL)), status(S_INIT), input_consumed_event(nullptr)
#else
	: status(S_INIT), input_consumed_event(nullptr)
#endif // PROFILE_CRITICAL_SECTIONS
{
	tree = nullptr;
	tris = nullptr;
	tris_count = 0;
	verts = nullptr;
	verts_count = 0;
}

MODEL::~MODEL()
{
	xr_jobs::wait(build_group);

	if (input_consumed_event)
	{
		CloseHandle(input_consumed_event);
		input_consumed_event = nullptr;
	}

	xrCriticalSectionGuard guard(cs);
	set_status(S_INIT);

	CDELETE(tree);
	CFREE(tris);
	tris_count = 0;
	CFREE(verts);
	verts_count = 0;
}

void MODEL::syncronize() const
{
	u32 current_status = get_status();
	if ((current_status == S_INIT || current_status == S_BUILD) && !build_group.empty())
	{
		Log("! WARNING: synchronized CDB::query");
		xr_jobs::wait(build_group);
		current_status = get_status();
	}

	R_ASSERT2(current_status == S_READY, "CDB query attempted on an unbuilt or failed model");
}

void MODEL::build_job(void* params)
{
	BTHREAD_params* build_params = static_cast<BTHREAD_params*>(params);
	R_ASSERT(build_params);

	FPU::m64r();

	bool input_consumed = false;
	MODEL* model = build_params->model;

	model->set_status(S_BUILD);
	model->cs.Enter();
	const bool success = model->build_internal(
		build_params->vertices,
		build_params->vertex_count,
		build_params->triangles,
		build_params->triangle_count,
		build_params->callback,
		build_params->callback_params,
		build_params->input_consumed_event,
		input_consumed);

	model->set_status(success ? S_READY : S_FAILED);
	model->cs.Leave();

	signal_input_consumed(build_params->input_consumed_event, input_consumed);
	xr_delete(build_params);
}

void MODEL::build(Fvector* V, int Vcnt, TRI* T, int Tcnt, build_callback* bc, void* bcp)
{
	R_ASSERT(get_status() == S_INIT);
	R_ASSERT(V);
	R_ASSERT(T);
	R_ASSERT((Vcnt >= 4) && (Tcnt >= 2));

	_initialize_cpu_thread();

	auto build_synchronously = [&]()
	{
		xrCriticalSectionGuard guard(cs);
		set_status(S_BUILD);
		bool input_consumed = false;
		const bool success = build_internal(V, Vcnt, T, Tcnt, bc, bcp, nullptr, input_consumed);
		set_status(success ? S_READY : S_FAILED);
		R_ASSERT2(success, "CDB model build failed");
	};

#ifdef _EDITOR
	build_synchronously();
#else
	// The job-based CDB builder is safe to overlap with the remaining level load.
	// Keep an explicit opt-out for diagnosis or legacy hardware.
	if (Core.Params && strstr(Core.Params, "-no_mt_cdb"))
	{
		build_synchronously();
		return;
	}

	input_consumed_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!input_consumed_event)
	{
		Msg("! xrCDB: failed to create input-consumed event, using synchronous build");
		build_synchronously();
		return;
	}

	BTHREAD_params* build_params = xr_new<BTHREAD_params>();
	build_params->model = this;
	build_params->vertices = V;
	build_params->vertex_count = Vcnt;
	build_params->triangles = T;
	build_params->triangle_count = Tcnt;
	build_params->callback = bc;
	build_params->callback_params = bcp;
	build_params->input_consumed_event = input_consumed_event;

	if (!xr_jobs::submit(&MODEL::build_job, build_params, &build_group, xr_jobs::priority::normal))
	{
		xr_delete(build_params);
		CloseHandle(input_consumed_event);
		input_consumed_event = nullptr;
		Msg("! xrCDB: failed to queue build job, using synchronous build");
		build_synchronously();
		return;
	}

	const DWORD wait_result = WaitForSingleObject(input_consumed_event, INFINITE);
	if (wait_result != WAIT_OBJECT_0)
		xr_jobs::wait(build_group);

	CloseHandle(input_consumed_event);
	input_consumed_event = nullptr;
	R_ASSERT2(wait_result == WAIT_OBJECT_0, "Failed while waiting for CDB input copy");
#endif
}

bool MODEL::build_internal(Fvector* V, int Vcnt, TRI* T, int Tcnt, build_callback* bc, void* bcp,
	HANDLE input_consumed_event, bool& input_consumed)
{
	// Own copies are required before asynchronous build() may return to the caller.
	verts_count = Vcnt;
#ifdef USE_ARENA_ALLOCATOR
	verts = CALLOC(Fvector, verts_count);
#else
	verts = xr_alloc_uninitialized<Fvector>(verts_count);
#endif
	if (!verts)
	{
		verts_count = 0;
		return false;
	}
	CopyMemory(verts, V, verts_count * sizeof(Fvector));

	tris_count = Tcnt;
#ifdef USE_ARENA_ALLOCATOR
	tris = CALLOC(TRI, tris_count);
#else
	tris = xr_alloc_uninitialized<TRI>(tris_count);
#endif
	if (!tris)
	{
		CFREE(verts);
		verts_count = 0;
		tris_count = 0;
		return false;
	}
	CopyMemory(tris, T, tris_count * sizeof(TRI));

	// The source reader may be released as soon as both arrays are copied. Material
	// remapping and OPCODE construction operate exclusively on MODEL-owned memory and
	// can overlap geometry, visuals and texture loading. Early queries synchronize on
	// build_group through MODEL::syncronize().
	set_status(S_BUILD);
	signal_input_consumed(input_consumed_event, input_consumed);

	if (bc)
		bc(verts, Vcnt, tris, Tcnt, bcp);

#ifdef USE_ARENA_ALLOCATOR
	u32* temp_tris = CALLOC(u32, tris_count * 3);
#else
	u32* temp_tris = xr_alloc_uninitialized<u32>(tris_count * 3);
#endif
	if (!temp_tris)
	{
		CFREE(verts);
		CFREE(tris);
		verts_count = 0;
		tris_count = 0;
		return false;
	}

	triangle_index_context index_context
	{
		tris,
		temp_tris,
		static_cast<u32>(tris_count),
		0
	};

	const u32 index_block_count = (index_context.triangle_count + triangle_index_block_size - 1) / triangle_index_block_size;
	const bool parallel_indices = index_block_count > 1 && xr_jobs::worker_count() > 0 &&
		!(Core.Params && strstr(Core.Params, "-no_mt_cdb_indices"));
	const u32 index_lane_count = parallel_indices ?
		_min(index_block_count, xr_jobs::available_thread_count()) : 1;

	xr_jobs::task_group index_group;
	xr_jobs::submit_many(&fill_triangle_indices_job, &index_context,
		index_lane_count - 1, &index_group, xr_jobs::priority::normal);

	fill_triangle_indices_job(&index_context);
	if (index_lane_count > 1)
		xr_jobs::wait(index_group);

	OPCODECREATE create_params;
	create_params.NbTris = tris_count;
	create_params.NbVerts = verts_count;
	create_params.Tris = reinterpret_cast<unsigned*>(temp_tris);
	create_params.Verts = reinterpret_cast<Point*>(verts);
	create_params.Rules = SPLIT_COMPLETE | SPLIT_SPLATTERPOINTS | SPLIT_GEOMCENTER;
	create_params.NoLeaf = true;
	create_params.Quantized = false;

	tree = CNEW(OPCODE_Model)();
	if (!tree || !tree->Build(create_params))
	{
		CDELETE(tree);
		CFREE(verts);
		CFREE(tris);
		CFREE(temp_tris);
		verts_count = 0;
		tris_count = 0;
		return false;
	}

	CFREE(temp_tris);
	return true;
}

u32 MODEL::memory()
{
	if (get_status() != S_READY)
	{
		if (get_status() == S_BUILD)
			syncronize();

		if (get_status() != S_READY)
			return 0;
	}

	R_ASSERT(tree);
	const u32 vertices_memory = verts_count * sizeof(Fvector);
	const u32 triangles_memory = tris_count * sizeof(TRI);
	return tree->GetUsedBytes() + vertices_memory + triangles_memory + sizeof(*this) + sizeof(*tree);
}

// This is the constructor of a class that has been exported.
// see xrCDB.h for the class definition
COLLIDER::COLLIDER()
{
	ray_mode = 0;
	box_mode = 0;
	frustum_mode = 0;
	rd.reserve(8);
}

COLLIDER::~COLLIDER()
{
	r_free();
}

RESULT& COLLIDER::r_add()
{
	rd.push_back(RESULT());
	return rd.back();
}

void COLLIDER::r_free()
{
	rd.clear_and_free();
}
