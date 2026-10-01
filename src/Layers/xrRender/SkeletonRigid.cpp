//---------------------------------------------------------------------------
#include 	"stdafx.h"
#pragma hdrstop

#include 	"SkeletonCustom.h"

#include "../../xrCore/profiler.h"

extern int psSkeletonUpdate;

#ifdef DEBUG
void check_kinematics(CKinematics* _k, LPCSTR s);
#endif

extern float IK_CALC_DIST;
extern float IK_ALWAYS_CALC_DIST;
extern float IK_CALC_SSA;
extern ENGINE_API BOOL g_bootComplete;
BOOL r_optimize_calculate_bones = TRUE;

class IRenderable;

namespace
{
constexpr u32 bones_cache_invalid_frame = u32(-1);

IC u64 make_bones_cache_state(u32 frame, u32 generation)
{
	return (u64(generation) << 32) | u64(frame);
}

IC u32 bones_cache_frame(u64 state)
{
	return u32(state);
}

IC u32 bones_cache_generation(u64 state)
{
	return u32(state >> 32);
}

}

bool CKinematics::CanCalculateBonesOnWorker() const
{
	if (!bones || !bone_instances)
	{
		return false;
	}
	if (Update_Callback)
	{
		return false;
	}

	// Per-bone callbacks routinely enter gameplay, IK and Lua-owned state. There
	// is no worker-safety contract for them, so such skeletons retain the exact
	// original lazy main-thread path.
	for (u32 bone = 0; bone < bones->size(); ++bone)
	{
		if (bone_instances[bone].callback())
		{
			return false;
		}
	}

	return true;
}

void CKinematics::CalculateBonesOnWorker(BOOL bForceExact)
{

	// A skeleton may be referenced by the main view and several independent light
	// frustums. Avoid rescanning every bone callback for duplicate worker candidates.
	const u32 frame = RDEVICE.dwFrame;
	u64 cache_state = UCalc_CacheState.load(std::memory_order_acquire);
	if (frame != bones_cache_invalid_frame && bones_cache_frame(cache_state) == frame)
	{

		return;
	}

	// Several independent light queries can discover the same actor. Keep the
	// callback/blend safety scan and pose update under the same per-instance lock;
	// otherwise one worker could mutate blends in UpdateTracks while another scans
	// them in CanCalculateBonesOnWorker. CRITICAL_SECTION is recursive, so the
	// internal calculation can retain its normal locking contract.
	xrCriticalSectionGuard guard(UCalc_Mutex);
	cache_state = UCalc_CacheState.load(std::memory_order_acquire);
	if (frame != bones_cache_invalid_frame && bones_cache_frame(cache_state) == frame)
	{

		return;
	}

	// Recheck at execution time: a derived object's UpdateCL may have installed a
	// callback after it first became a candidate earlier in the frame.
	if (!CanCalculateBonesOnWorker())
	{

		return;
	}

	CalculateBonesInternal(bForceExact);

}

u32 CKinematics::LastExactBonesRequestFrame() const
{
	return UCalc_ExactRequestFrame.load(std::memory_order_acquire);
}

void CKinematics::CalculateBones(BOOL bForceExact)
{
	if (bForceExact)
		UCalc_ExactRequestFrame.store(RDEVICE.dwFrame, std::memory_order_release);

	CalculateBonesInternal(bForceExact);
}

void CKinematics::CalculateBonesInternal(BOOL bForceExact)
{
	PROF_EVENT("CKinematics::CalculateBones");

	const u32 frame = RDEVICE.dwFrame;
	const u64 cache_state = UCalc_CacheState.load(std::memory_order_acquire);

	// The renderer may request the same skeleton from the normal pass, shadow
	// passes, wallmarks and additional visibility traversals. The lower half of
	// the packed state gives this hot path a single atomic load and comparison.
	if (frame != bones_cache_invalid_frame && bones_cache_frame(cache_state) == frame)
	{

		return;
	}

	// Existing time-based reuse remains useful while game time is frozen or a
	// distant skeleton is intentionally updated less frequently.
	if (RDEVICE.dwTimeGlobal == UCalc_Time)
	{

		return; // early out for "fast" update
	}

	// demonized: reduce calculate bones updates when the object is far away and not in frustum
	// Available only if can get parent xform
	// Refactor later for per object basis
	float update_rate_k = 1.f;

#ifdef OPTIMIZE_CALCULATE_BONES
	if (g_bootComplete)
	{
		if (spatialParent)
		{
			auto& sphere = spatialParent->spatial.sphere;

			float dist = 0.f;
			float perceived_dist = Device.GetPerceivedDist(sphere.P, &dist);
			float dist_k = dist / perceived_dist;
			float ssa = Device.CalcSSADynamic(sphere.P, sphere.R);
			float ssa_k = IK_CALC_SSA / ssa;
			update_rate_k = _max(1.f, ssa_k);

			// Visibility check, perform always
			bool visibleCheck = (perceived_dist < IK_ALWAYS_CALC_DIST) || ::Render->ViewBase.testSphere_dirty(sphere.P, sphere.R);
			if (!visibleCheck)
			{
				bForceExact = FALSE;
				update_rate_k = _max(2.f, update_rate_k);
			}

			// screen space area check, perform when cvar is enabled and can be optimized
			if (r_optimize_calculate_bones && canBeOptimized() && (ssa < IK_CALC_SSA))
				bForceExact = FALSE;
		}
	}
#endif

	xrCriticalSectionGuard g(UCalc_Mutex);

	// Another thread can have passed the optimistic checks and completed this
	// skeleton while we waited for its per-instance lock.
	u64 locked_state = UCalc_CacheState.load(std::memory_order_acquire);
	if (frame != bones_cache_invalid_frame && bones_cache_frame(locked_state) == frame)
	{

		return;
	}

	if (RDEVICE.dwTimeGlobal == UCalc_Time)
	{
		// The time cache says the existing pose is valid. Publish it for this
		// frame only if no invalidation raced with the check.
		const u64 published_state = make_bones_cache_state(frame, bones_cache_generation(locked_state));
		if (UCalc_CacheState.compare_exchange_strong(locked_state, published_state,
			std::memory_order_acq_rel, std::memory_order_acquire))
		{

			return;
		}
		// A concurrent invalidation changed the generation. Continue and rebuild.

	}

	OnCalculateBones();

	if (!bForceExact && (RDEVICE.dwTimeGlobal < (UCalc_Time + UCalc_Interval * update_rate_k)))
	{

		return; // early out for "slow" update
	}

	if (Update_Visibility)
	{

		Visibility_Update();

	}

	_DBG_SINGLE_USE_MARKER;
	// here we have either:
	// 1: timeout elapsed
	// 2: exact computation required
	UCalc_Time = RDEVICE.dwTimeGlobal;

	// Capture the generation after track/update callbacks. If a callback
	// invalidates the pose while the recursive build is running, the mismatch at
	// the end deliberately prevents publication of a stale cache entry.
	const u64 calculation_state = UCalc_CacheState.load(std::memory_order_acquire);
	const u32 calculation_generation = bones_cache_generation(calculation_state);

#ifdef DEBUG
	CTimer animation_timer;
	if (g_bEnableStatGather)
		animation_timer.Start();
#endif

	Bone_Calculate(bones->at(iRoot), &Fidentity);

#ifdef DEBUG
	check_kinematics(this, dbg_name.c_str());
	if (g_bEnableStatGather)
		RDEVICE.Statistic->Animation.Add(animation_timer.GetElapsed_ticks());
#endif
	VERIFY(LL_GetBonesVisible()!=0);

	// Calculate BOXes/Spheres if needed
	UCalc_Visibox++;
	if (UCalc_Visibox >= psSkeletonUpdate)
	{

		// mark
		const u32 visibox_jitter_range = u32(_max(1, psSkeletonUpdate - 1));
		const u32 visibox_seed = u32((size_t(this) >> 4) ^ UCalc_Time);
		UCalc_Visibox = -int(visibox_seed % visibox_jitter_range);
		UCalc_ThisFrame = true;

		// the update itself
		Fbox Box;
		Box.invalidate();
		for (u32 b = 0; b < bones->size(); b++)
		{
			if (!LL_GetBoneVisible(u16(b))) continue;
			Fobb& obb = (*bones)[b]->obb;
			Fmatrix& Mbone = bone_instances[b].mTransform;
			Fmatrix Mbox;
			obb.xform_get(Mbox);
			Fmatrix X;
			X.mul_43(Mbone, Mbox);
			Fvector& S = obb.m_halfsize;

			Fvector E;
			E.x = _abs(X._11) * S.x + _abs(X._21) * S.y + _abs(X._31) * S.z;
			E.y = _abs(X._12) * S.x + _abs(X._22) * S.y + _abs(X._32) * S.z;
			E.z = _abs(X._13) * S.x + _abs(X._23) * S.y + _abs(X._33) * S.z;

			Fvector P;
			P.sub(X.c, E);
			Box.modify(P);
			P.add(X.c, E);
			Box.modify(P);
		}
		if (bones->size())
		{
			vis.box.min = (Box.min);
			vis.box.max = (Box.max);
			vis.box.getsphere(vis.sphere.P, vis.sphere.R);
		}
#ifdef DEBUG
		VERIFY3(_valid(vis.box.min)&&_valid(vis.box.max), "Invalid bones-xform in model", dbg_name.c_str());
		if(vis.sphere.R>1000.f)
		{
			for(u16 ii=0; ii<LL_BoneCount(); ++ii){
				Fmatrix tr;
				tr = LL_GetTransform(ii);
				Log("bone ",LL_BoneName_dbg(ii));
				Log("bone_matrix",tr);
			}
			Log("end-------");
		}
		VERIFY3(vis.sphere.R<1000.f, "Invalid bones-xform in model", dbg_name.c_str());
#endif

	}
	else
		UCalc_ThisFrame = false;

	if (Update_Callback)
	{

		Update_Callback(this);

	}

	u64 expected_state = calculation_state;
	const u64 published_state = make_bones_cache_state(frame, calculation_generation);
	UCalc_CacheState.compare_exchange_strong(expected_state, published_state,
		std::memory_order_release, std::memory_order_relaxed);

}

#ifdef DEBUG
void check_kinematics(CKinematics* _k, LPCSTR s)
{
	CKinematics* K = _k;
	Fmatrix&	MrootBone		= K->LL_GetBoneInstance(K->LL_GetBoneRoot()).mTransform;
	if(MrootBone.c.y >10000)
	{	
		Msg("all bones transform:--------[%s]",s);

		for(u16 ii=0; ii<K->LL_BoneCount();++ii){
			Fmatrix tr;

			tr = K->LL_GetTransform(ii);
			Log("bone ",K->LL_BoneName_dbg(ii));
			Log("bone_matrix",tr);
		}
		Log("end-------");
		VERIFY3(0,"check_kinematics failed for ", s);
	}
}
#endif

void CKinematics::BuildBoneMatrix(const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent,
                                  u8 channel_mask/* = (1<<0)*/)
{
	if (LL_GetBoneVisible(bd->GetSelfID()))
	{
		bi.mTransform.mul_43(*parent, bd->bind_transform);
		bi.mTransformHidden.set(bi.mTransform);
	}
	else
	{
		bi.mTransform.c = (*parent).c;
		bi.mTransformHidden.mul_43(*parent, bd->bind_transform);
	}
}

void CKinematics::CLBone(const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 channel_mask /*= (1<<0)*/)
{
	if (!bi.callback_overwrite())
		BuildBoneMatrix(bd, bi, parent, channel_mask);

	if (bi.callback())
		bi.callback()(&bi);

	bi.mRenderTransform.mul_43(bi.mTransform, bd->m2b_transform);
}

void CKinematics::Bone_GetAnimPos(Fmatrix& pos, u16 id, u8 mask_channel, bool ignore_callbacks)
{
	R_ASSERT(id<LL_BoneCount());
	// AI vision runs on the deferred scheduler while rendering may calculate the
	// same model. Protect blend reads and callback-backed chain evaluation with
	// the complete-pose lock, matching the current upstream MT safety contract.
	xrCriticalSectionGuard guard(UCalc_Mutex);
	CBoneInstance bi = LL_GetBoneInstance(id);
	BoneChain_Calculate(&LL_GetData(id), bi, mask_channel, ignore_callbacks);
#ifndef MASTER_GOLD
	R_ASSERT( _valid( bi.mTransform ) );
#endif
	pos.set(bi.mTransform);
}

void CKinematics::Bone_Calculate(CBoneData* bd, Fmatrix* parent)
{
	xrCriticalSectionGuard g(UCalc_Mutex2);
	Bone_CalculateImpl(bd, parent);
}

void CKinematics::Bone_CalculateImpl(CBoneData* bd, Fmatrix* parent)
{
	u16 SelfID = bd->GetSelfID();
	CBoneInstance& BONE_INST = LL_GetBoneInstance(SelfID);
	CLBone(bd, BONE_INST, parent, u8(-1));
	// Calculate children
	for (xr_vector<CBoneData*>::iterator C = bd->children.begin(); C != bd->children.end(); C++)
		Bone_CalculateImpl(*C, &BONE_INST.mTransform);
}

void CKinematics::BoneChain_Calculate(const CBoneData* bd, CBoneInstance& bi, u8 mask_channel, bool ignore_callbacks)
{
	u16 SelfID = bd->GetSelfID();
	//CBlendInstance& BLEND_INST	= LL_GetBlendInstance(SelfID);
	//CBlendInstance::BlendSVec &Blend = BLEND_INST.blend_vector();
	//ignore callbacks
	BoneCallback bc = bi.callback();
	BOOL ow = bi.callback_overwrite();
	if (ignore_callbacks)
	{
		bi.set_callback(bi.callback_type(), 0, bi.callback_param(), 0);
	}
	if (SelfID == LL_GetBoneRoot())
	{
		CLBone(bd, bi, &Fidentity, mask_channel);
		//restore callback	
		bi.set_callback(bi.callback_type(), bc, bi.callback_param(), ow);
		return;
	}
	u16 ParentID = bd->GetParentID();
	R_ASSERT(ParentID != BI_NONE);
	CBoneData* ParrentDT = &LL_GetData(ParentID);
	CBoneInstance parrent_bi = LL_GetBoneInstance(ParentID);
	BoneChain_Calculate(ParrentDT, parrent_bi, mask_channel, ignore_callbacks);
	CLBone(bd, bi, &parrent_bi.mTransform, mask_channel);
	//restore callback
	bi.set_callback(bi.callback_type(), bc, bi.callback_param(), ow);
}
