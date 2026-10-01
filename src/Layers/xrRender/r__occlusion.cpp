#include "stdafx.h"
#include ".\r__occlusion.h"

#include "QueryHelper.h"

R_occlusion::R_occlusion(void)
{
	enabled = TRUE;
	next_generation = 1;
}

R_occlusion::~R_occlusion(void)
{
	occq_destroy();
}

void R_occlusion::occq_create(u32 limit)
{
	occq_destroy();
	VERIFY(limit <= slot_mask + 1);
	enabled = strstr(Core.Params, "-no_occq") ? FALSE : TRUE;
	pool.reserve(limit);
	used.reserve(limit);
	pending.reserve(limit);
	fids.reserve(limit);
	{
		xrCriticalSectionGuard guard(cancelled_lock);
		cancelled.reserve(limit);
	}
	cancelled_batch.reserve(limit);
	for (u32 it = 0; it < limit; it++)
	{
		_Q q = {};
		q.order = it;
		q.id = invalid_id;
		if (FAILED(CreateQuery(&q.Q, D3DQUERYTYPE_OCCLUSION)) || !q.Q) break;
		pool.push_back(q);
	}
	std::reverse(pool.begin(), pool.end());
}

void R_occlusion::occq_destroy()
{
	enabled = FALSE;
	{
		xrCriticalSectionGuard guard(cancelled_lock);
		cancelled.clear();
	}
	cancelled_batch.clear();
	while (!used.empty())
	{
		_RELEASE(used.back().Q);
		used.pop_back();
	}
	while (!pending.empty())
	{
		_RELEASE(pending.back().Q);
		pending.pop_back();
	}
	while (!pool.empty())
	{
		_RELEASE(pool.back().Q);
		pool.pop_back();
	}
	used.clear();
	pending.clear();
	pool.clear();
	fids.clear();
}

R_occlusion::_Q* R_occlusion::find_query(occq_id ID)
{
	const u32 slot = static_cast<u32>(ID & slot_mask);
	if (ID == invalid_id || slot >= used.size())
		return nullptr;
	_Q& query = used[slot];
	return query.Q && query.id == ID ? &query : nullptr;
}

R_occlusion::_Q R_occlusion::detach_query(occq_id& ID)
{
	const u32 slot = static_cast<u32>(ID & slot_mask);
	_Q query = used[slot];
	used[slot].Q = nullptr;
	used[slot].id = invalid_id;
	fids.push_back(slot);
	ID = invalid_id;
	return query;
}

void R_occlusion::drain_cancelled_queries()
{
	{
		xrCriticalSectionGuard guard(cancelled_lock);
		if (cancelled.empty())
			return;
		cancelled.swap(cancelled_batch);
	}

	for (occq_id ID : cancelled_batch)
	{
		// A result or reset may have retired this owner before its request arrived.
		// Duplicate/stale cancellations must not free the new owner of its slot.
		if (find_query(ID))
			pending.push_back(detach_query(ID));
	}
	cancelled_batch.clear();
}

void R_occlusion::recycle_query(const _Q& query)
{
	VERIFY(query.Q);
	if (pool.empty())
	{
		pool.push_back(query);
		return;
	}

	int it = static_cast<int>(pool.size()) - 1;
	while ((it >= 0) && (pool[it].order < query.order))
		--it;
	pool.insert(pool.begin() + it + 1, query);
}

void R_occlusion::recycle_pending_queries()
{
	for (u32 i = 0; i < pending.size();)
	{
		occq_result fragments = 0;
		const HRESULT hr = GetData(pending[i].Q, &fragments, sizeof(fragments));
		if (hr == S_FALSE)
		{
			++i;
			continue;
		}

		recycle_query(pending[i]);
		pending[i] = pending.back();
		pending.pop_back();
	}
}

u32 R_occlusion::occq_begin(occq_id& ID)
{
	if (!enabled)
	{
		ID = invalid_id;
		return 0;
	}

	drain_cancelled_queries();
	recycle_pending_queries();

	// Igor: if the GPU still owns every query, conservatively skip occlusion
	// culling for this object instead of stalling the render thread.
	if (pool.empty())
	{
		//		if ((Device.dwFrame % 40) == 0)
		//			Msg(" RENDER [Warning]: Too many occlusion queries were issued(>1536)!!!");
		ID = invalid_id;
		return 0;
	}

	RImplementation.stats.o_queries ++;
	_Q query = pool.back();
	u32 slot;
	if (!fids.empty())
	{
		slot = fids.back();
		fids.pop_back();
		used[slot] = query;
	}
	else
	{
		slot = static_cast<u32>(used.size());
		used.push_back(query);
	}
	ID = (next_generation++ << 16) | slot;
	used[slot].id = ID;
	pool.pop_back();
	CHK_DX(BeginQuery(query.Q));

	return query.order;
}

void R_occlusion::occq_end(occq_id& ID)
{
	if (!enabled) return;

	_Q* query = find_query(ID);
	if (!query)
	{
		ID = invalid_id;
		return;
	}

	CHK_DX(EndQuery(query->Q));
}

R_occlusion::occq_result R_occlusion::occq_get(occq_id& ID)
{
	drain_cancelled_queries();
	_Q* owned_query = find_query(ID);
	if (!enabled || !owned_query)
	{
		ID = invalid_id;
		return 0xFFFFFFFF;
	}

	occq_result fragments = 0;
	const HRESULT hr = GetData(owned_query->Q, &fragments, sizeof(fragments));
	_Q query = detach_query(ID);

	if (hr == S_FALSE)
	{
		// The result is not required for correctness. Treat the object as visible
		// for this frame and retire the query until the GPU completes it.
		pending.push_back(query);
		return 0xFFFFFFFF;
	}

	if (FAILED(hr))
		fragments = 0xFFFFFFFF;
	else if (fragments == 0)
		RImplementation.stats.o_culled++;

	recycle_query(query);
	return fragments;
}

// Non-blocking result path used by local-light visibility. Unlike occq_get(),
// an unfinished query stays owned by the light and can be polled next frame.
// This preserves the last confirmed visibility result instead of turning every
// GPU-late query into a temporarily visible shadow-casting light.
bool R_occlusion::occq_try_get(occq_id& ID, occq_result& fragments)
{
	fragments = 0xFFFFFFFF;
	drain_cancelled_queries();
	_Q* owned_query = find_query(ID);
	if (!enabled || !owned_query)
	{
		ID = invalid_id;
		return true;
	}

	const HRESULT hr = GetData(owned_query->Q, &fragments, sizeof(fragments));
	if (hr == S_FALSE)
		return false;

	_Q query = detach_query(ID);

	if (FAILED(hr))
		fragments = 0xFFFFFFFF;
	else if (fragments == 0)
		RImplementation.stats.o_culled++;

	recycle_query(query);
	return true;
}

void R_occlusion::occq_cancel(occq_id& ID)
{
	const occq_id owner = ID;
	ID = invalid_id;
	if (owner == invalid_id)
		return;

	// Do not touch used/pool/fids or the immediate context from a worker.
	// The render thread retires this exact owner and waits for GPU completion
	// through the existing non-blocking pending-query path.
	xrCriticalSectionGuard guard(cancelled_lock);
	cancelled.push_back(owner);
}
