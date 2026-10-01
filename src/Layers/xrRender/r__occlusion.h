#pragma once

#include "../../xrCore/xrSyncronize.h"

const u32 occq_size = 2 * 768; //256	;	// queue for occlusion queries

// must conform to following order of allocation/free
// a(A), a(B), a(C), a(D), ....
// f(A), f(B), f(C), f(D), ....
// a(A), a(B), a(C), a(D), ....
//	this mean:
//		use as litle of queries as possible
//		first try to use queries allocated first
//	assumption:
//		used queries number is much smaller than total count

class R_occlusion
{
public:
	typedef u64 occq_id;
	typedef u64 occq_result;
	static const occq_id invalid_id = ~occq_id(0);

private:
	struct _Q
	{
		u32 order;
		ID3DQuery* Q;
		occq_id id;
	};

	// The low 16 bits identify a slot; the remaining bits identify its owner.
	// Keep the generation across device resets so an old light cannot consume a
	// new query merely because it received the same slot in the recreated pool.
	static const occq_id slot_mask = 0xFFFF;
	static_assert(occq_size <= slot_mask + 1, "Occlusion pool exceeds handle slot capacity");
	u64 next_generation;

	BOOL enabled; // 
	xr_vector<_Q> pool; // sorted (max ... min), insertions are usually at the end
	xr_vector<_Q> used; // id's are generated from this and it is cleared from back only
	xr_vector<_Q> pending; // completed by GPU, polled without blocking the render thread
	xr_vector<u32> fids; // free id's

	// Cancellation may originate from gameplay workers. Only this value queue
	// is shared; query ownership and every D3D11 call stay on the render thread.
	xrCriticalSection cancelled_lock;
	xr_vector<occq_id> cancelled;
	xr_vector<occq_id> cancelled_batch;

	_Q* find_query(occq_id ID);
	_Q detach_query(occq_id& ID);
	void drain_cancelled_queries();
	void recycle_query(const _Q& query);
	void recycle_pending_queries();
public:
	R_occlusion();
	~R_occlusion();

	void occq_create(u32 limit);
	void occq_destroy();
	u32 occq_begin(occq_id& ID); // returns 'order'
	void occq_end(occq_id& ID);
	occq_result occq_get(occq_id& ID);
	bool occq_try_get(occq_id& ID, occq_result& fragments);
	// The caller must protect its ID against simultaneous begin/get/cancel.
	void occq_cancel(occq_id& ID);
};
