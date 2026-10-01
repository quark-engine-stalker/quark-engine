//---------------------------------------------------------------------------
#include 	"stdafx.h"
#pragma hdrstop

#include 	"SkeletonAnimated.h"
#include <algorithm>

#include	"AnimationKeyCalculate.h"
#include	"SkeletonX.h"
#include	"../../xrEngine/fmesh.h"
#ifdef DEBUG
#include	"../../xrcore/dump_string.h"
#endif
extern int psSkeletonUpdate;
extern shared_str current_player_hud_sect;
using namespace animation;

namespace
{
thread_local const xr_vector<prepared_motion_source>* g_prepared_motion_sources = nullptr;
}

prepared_motion_source_scope::prepared_motion_source_scope(const xr_vector<prepared_motion_source>* sources)
	: previous(g_prepared_motion_sources)
{
	if (sources)
	{
		lookup = *sources;
		std::sort(lookup.begin(), lookup.end(), [](const prepared_motion_source& left,
			const prepared_motion_source& right)
		{
			return stricmp(left.name.c_str(), right.name.c_str()) < 0;
		});
	}
	g_prepared_motion_sources = &lookup;
}

prepared_motion_source_scope::~prepared_motion_source_scope()
{
	g_prepared_motion_sources = previous;
}

IReader* open_prepared_motion_source(LPCSTR name)
{
	if (!g_prepared_motion_sources || !name)
		return nullptr;

	const auto found = std::lower_bound(g_prepared_motion_sources->begin(),
		g_prepared_motion_sources->end(), name,
		[](const prepared_motion_source& source, LPCSTR value)
		{
			return stricmp(source.name.c_str(), value) < 0;
		});
	if (found == g_prepared_motion_sources->end() || stricmp(found->name.c_str(), name) != 0)
		return nullptr;

	return xr_new<IReader>(const_cast<u8*>(found->data), static_cast<int>(found->size));
}
//////////////////////////////////////////////////////////////////////////
// BoneInstance methods
void CBlendInstance::construct()
{
	ZeroMemory(this, sizeof(*this));
}

void CBlendInstance::blend_add(CBlend* H)
{
	if (Blend.size() == MAX_BLENDED)
	{
		if (H->fall_at_end)
			return;
		BlendSVecIt _d = Blend.begin();
		for (BlendSVecIt it = Blend.begin() + 1; it != Blend.end(); it++)
			if ((*it)->blendAmount < (*_d)->blendAmount) _d = it;
		Blend.erase(_d);
	}
	VERIFY(Blend.size()<MAX_BLENDED);
	Blend.push_back(H);
}

void CBlendInstance::blend_remove(CBlend* H)
{
	CBlend** I = std::find(Blend.begin(), Blend.end(), H);
	if (I != Blend.end()) Blend.erase(I);
}

// Motion control
void CKinematicsAnimated::Bone_Motion_Start(CBoneData* bd, CBlend* handle)
{
	LL_GetBlendInstance(bd->GetSelfID()).blend_add(handle);
	for (vecBonesIt I = bd->children.begin(); I != bd->children.end(); I++)
		Bone_Motion_Start(*I, handle);
}

void CKinematicsAnimated::Bone_Motion_Stop(CBoneData* bd, CBlend* handle)
{
	LL_GetBlendInstance(bd->GetSelfID()).blend_remove(handle);
	for (vecBonesIt I = bd->children.begin(); I != bd->children.end(); I++)
		Bone_Motion_Stop(*I, handle);
}

void CKinematicsAnimated::Bone_Motion_Start_IM(CBoneData* bd, CBlend* handle)
{
	LL_GetBlendInstance(bd->GetSelfID()).blend_add(handle);
}

void CKinematicsAnimated::Bone_Motion_Stop_IM(CBoneData* bd, CBlend* handle)
{
	LL_GetBlendInstance(bd->GetSelfID()).blend_remove(handle);
}

#if (defined DEBUG || defined _EDITOR)

std::pair<LPCSTR,LPCSTR> CKinematicsAnimated::LL_MotionDefName_dbg	(MotionID ID)
{
	shared_motions& s_mots	= m_Motions[ID.slot].motions;
	accel_map::iterator _I, _E=s_mots.motion_map()->end();
	for (_I	= s_mots.motion_map()->begin(); _I!=_E; ++_I)	if (_I->second==ID.idx) return std::make_pair(*_I->first,*s_mots.id());
	return std::make_pair((LPCSTR)0,(LPCSTR)0);
}

static LPCSTR name_bool( BOOL v )
{
	static  xr_token token_bool[] = { { "false", 0 }, { "true", 1 } };
	return get_token_name( token_bool, v );
}

static LPCSTR name_blend_type( CBlend::ECurvature blend )
{
	static xr_token token_blend[] = 
	{
		{"eFREE_SLOT"		, CBlend::eFREE_SLOT	},
		{"eAccrue"			, CBlend::eAccrue		},
		{"eFalloff"			, CBlend::eFalloff		},
		{"eFORCEDWORD"		, CBlend::eFORCEDWORD	}
	};
	return get_token_name( token_blend, blend );
}

static void dump_blend( CKinematicsAnimated* K, CBlend &B, u32 index )
{
	VERIFY( K );
	Msg( "----------------------------------------------------------" );
	Msg( "blend index: %d, poiter: %p ", index, &B );
	Msg( "time total: %f, speed: %f , power: %f ", B.timeTotal, B.speed, B.blendPower   );
	Msg( "ammount: %f, time current: %f, frame %d ", B.blendAmount, B.timeCurrent,B.dwFrame );
	Msg( "accrue: %f, fallof: %f ", B.blendAccrue, B.blendFalloff ); 

	Msg( "bonepart: %d, channel: %d, stop_at_end: %s, fall_at_end: %s "
		, B.bone_or_part, B.channel, name_bool( B.stop_at_end ), name_bool( B.fall_at_end ) );
	Msg( "state: %s, playing: %s, stop_at_end_callback: %s ", name_blend_type( B.blend_state() ), name_bool( B.playing ), name_bool( B.stop_at_end_callback ));
	Msg( "callback: %p callback param: %p", B.Callback, B.CallbackParam );

	if( B.blend_state() != CBlend::eFREE_SLOT )
	{
		Msg( "motion : name %s, set: %s ", K->LL_MotionDefName_dbg( B.motionID ).first, K->LL_MotionDefName_dbg( B.motionID ).second );
	}
	Msg( "----------------------------------------------------------" );
}

void	CKinematicsAnimated::LL_DumpBlends_dbg	( )
{
	Msg( "==================dump blends=================================================" );
	CBlend *I=blend_pool.begin(), *E=blend_pool.end();
	for (; I!=E; I++)
		dump_blend( this, *I, u32(I - blend_pool.begin()) );
}

#endif

u32 CKinematicsAnimated::LL_PartBlendsCount(u32 bone_part_id)
{
	return blend_cycle(bone_part_id).size();
}

CBlend* CKinematicsAnimated::LL_PartBlend(u32 bone_part_id, u32 n)
{
	if (LL_PartBlendsCount(bone_part_id) <= n)
		return 0;
	return blend_cycle(bone_part_id)[n];
}

bool CKinematicsAnimated::LL_IsBlendActive(const CBlend* blend) const
{
	if (!blend || blend_pool.empty())
		return false;

	// A blend handle is a pointer into this skeleton's fixed pool. Compare
	// addresses before dereferencing it: callers may still hold a handle from a
	// visual which has just been replaced.
	const size_t address = reinterpret_cast<size_t>(blend);
	const size_t begin = reinterpret_cast<size_t>(&blend_pool.front());
	const size_t end = begin + blend_pool.size() * sizeof(CBlend);
	if (address < begin || address >= end || ((address - begin) % sizeof(CBlend)) != 0)
		return false;

	return blend->blend_state() != CBlend::eFREE_SLOT;
}

void CKinematicsAnimated::LL_IterateBlends(IterateBlendsCallback& callback)
{
	CBlend *I = blend_pool.begin(), *E = blend_pool.end();
	for (; I != E; I++)
		if (I->blend_state() != CBlend::eFREE_SLOT) callback(*I);
}

/*
LPCSTR CKinematicsAnimated::LL_MotionDefName_dbg	(LPVOID ptr)
{
//.
	// cycles
	mdef::const_iterator I,E;
	I = motions.cycle()->begin(); 
	E = motions.cycle()->end(); 
	for ( ; I != E; ++I) if (&(*I).second == ptr) return *(*I).first;
	// fxs
	I = motions.fx()->begin(); 
	E = motions.fx()->end(); 
	for ( ; I != E; ++I) if (&(*I).second == ptr) return *(*I).first;
	return 0;
}
*/

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
MotionID CKinematicsAnimated::LL_MotionID(LPCSTR B)
{
	MotionID motion_ID;
	for (int k = int(m_Motions.size()) - 1; k >= 0; --k)
	{
		shared_motions* s_mots = &m_Motions[k].motions;
		auto I = s_mots->motion_hash()->find(LPSTR(B));
		if (I != s_mots->motion_hash()->end())
		{
			motion_ID.set(u16(k), I->second);
			break;
		}
	}
	return motion_ID;
}

u16 CKinematicsAnimated::LL_PartID(LPCSTR B)
{
	if (0 == m_Partition) return BI_NONE;
	for (u16 id = 0; id < MAX_PARTS; id++)
	{
		CPartDef* P = (*m_Partition)[id];
		if (nullptr == P) continue;
		if (0 == stricmp(B, *P->Name)) return id;
	}
	return BI_NONE;
}

// cycles
MotionID CKinematicsAnimated::ID_Cycle_Safe(LPCSTR N)
{
	MotionID motion_ID;
	for (int k = int(m_Motions.size()) - 1; k >= 0; --k)
	{
		shared_motions* s_mots = &m_Motions[k].motions;
		auto I = s_mots->cycle_hash()->find(LPSTR(N));
		if (I != s_mots->cycle_hash()->end())
		{
			motion_ID.set(u16(k), I->second);
			break;
		}
	}
	return motion_ID;
}

MotionID CKinematicsAnimated::ID_Cycle(shared_str N)
{
	MotionID motion_ID = ID_Cycle_Safe(N);
	R_ASSERT3(motion_ID.valid(), "! MODEL: can't find cycle: ", N.c_str());
	return motion_ID;
}

MotionID CKinematicsAnimated::ID_Cycle_Safe(shared_str N)
{
	MotionID motion_ID;
	for (int k = int(m_Motions.size()) - 1; k >= 0; --k)
	{
		shared_motions* s_mots = &m_Motions[k].motions;
		auto I = s_mots->cycle_hash()->find(N);
		if (I != s_mots->cycle_hash()->end())
		{
			motion_ID.set(u16(k), I->second);
			break;
		}
	}
	return motion_ID;
}

MotionID CKinematicsAnimated::ID_Cycle(LPCSTR N)
{
	MotionID motion_ID = ID_Cycle_Safe(N);
	R_ASSERT3(motion_ID.valid(), "! MODEL: can't find cycle: ", N);
	return motion_ID;
}

void CKinematicsAnimated::LL_FadeCycle(u16 part, float falloff, u8 mask_channel /*= (1<<0)*/)
{
	BlendSVec& Blend = blend_cycles[part];

	for (u32 I = 0; I < Blend.size(); I++)
	{
		CBlend& B = *Blend[I];
		if (!(mask_channel & (1 << B.channel)))
			continue;
		//B.blend				= CBlend::eFalloff;
		B.set_falloff_state();
		B.blendFalloff = falloff;
		//B.blendAccrue		= B.timeCurrent;
		if (B.stop_at_end) B.stop_at_end_callback = FALSE; // callback не должен приходить!
	}
}

void CKinematicsAnimated::LL_CloseCycle(u16 part, u8 mask_channel /*= (1<<0)*/)
{
	if (BI_NONE == part) return;
	if (part >= MAX_PARTS) return;

	// destroy cycle(s)
	BlendSVecIt I = blend_cycles[part].begin(), E = blend_cycles[part].end();
	for (; I != E; I++)
	{
		CBlend& B = *(*I);
		if (!(mask_channel & (1 << B.channel)))
			continue;
		//B.blend = CBlend::eFREE_SLOT;
		B.set_free_state();

		CPartDef* P = (*m_Partition)[B.bone_or_part];
		if (nullptr == P) return;
		for (u32 i = 0; i < P->bones.size(); i++)
			Bone_Motion_Stop_IM((*bones)[P->bones[i]], *I);

		blend_cycles[part].erase(I); // ?
		E = blend_cycles[part].end();
		I--;
	}
	//blend_cycles[part].clear	(); // ?
}

float CKinematicsAnimated::get_animation_length(MotionID motion_ID)
{
	VERIFY(motion_ID.slot<m_Motions.size());

	SMotionsSlot& slot = m_Motions[motion_ID.slot];

	VERIFY(LL_GetBoneRoot() < slot.bone_motions.size());

	MotionVec* bone_motions = slot.bone_motions[LL_GetBoneRoot()];

	VERIFY(motion_ID.idx < bone_motions->size());

	CMotionDef* const m_def = slot.motions.motion_def(motion_ID.idx);

	float const anim_speed = m_def ? m_def->Speed() : 1.f;

	return bone_motions->at(motion_ID.idx).GetLength() / anim_speed;
}

void CKinematicsAnimated::IBlendSetup(CBlend& B, u16 part, u8 channel, MotionID motion_ID, BOOL bMixing,
                                      float blendAccrue, float blendFalloff, float Speed, BOOL noloop,
                                      PlayCallback Callback, LPVOID CallbackParam)
{
	VERIFY(B.channel<MAX_CHANNELS);
	// Setup blend params
	if (bMixing)
	{
		//B.blend		= CBlend::eAccrue;
		B.set_accrue_state();
		B.blendAmount = EPS_S;
	}
	else
	{
		//B.blend		= CBlend::eFixed;
		//B.blend		= CBlend::eAccrue;
		B.set_accrue_state();
		B.blendAmount = 1;
	}
	B.blendAccrue = blendAccrue;
	B.blendFalloff = 0; // blendFalloff used for previous cycles
	B.blendPower = 1;
	B.speed = Speed;
	B.motionID = motion_ID;
	B.timeCurrent = 0;
	B.timeTotal = m_Motions[B.motionID.slot].bone_motions[LL_GetBoneRoot()]->at(motion_ID.idx).GetLength();
	B.bone_or_part = part;
	B.stop_at_end = noloop;
	B.playing = TRUE;
	B.stop_at_end_callback = TRUE;
	B.Callback = Callback;
	B.CallbackParam = CallbackParam;

	B.channel = channel;
	B.fall_at_end = B.stop_at_end && (channel > 1);
}

void CKinematicsAnimated::IFXBlendSetup(CBlend& B, MotionID motion_ID, float blendAccrue, float blendFalloff,
                                        float Power, float Speed, u16 bone)
{
	//B.blend			= CBlend::eAccrue;
	B.set_accrue_state();
	B.blendAmount = EPS_S;
	B.blendAccrue = blendAccrue;
	B.blendFalloff = blendFalloff;
	B.blendPower = Power;
	B.speed = Speed;
	B.motionID = motion_ID;
	B.timeCurrent = 0;
	B.timeTotal = m_Motions[B.motionID.slot].bone_motions[bone]->at(motion_ID.idx).GetLength();
	B.bone_or_part = bone;

	B.playing = TRUE;
	B.stop_at_end_callback = TRUE;
	B.stop_at_end = FALSE;
	//
	B.Callback = 0;
	B.CallbackParam = 0;

	B.channel = 0;
	B.fall_at_end = FALSE;
}

CBlend* CKinematicsAnimated::LL_PlayCycle(u16 part, MotionID motion_ID, BOOL bMixing, float blendAccrue,
                                          float blendFalloff, float Speed, BOOL noloop, PlayCallback Callback,
                                          LPVOID CallbackParam, u8 channel/*=0*/)
{
	// validate and unroll
	if (!motion_ID.valid()) return 0;
	if (BI_NONE == part)
	{
		for (u16 i = 0; i < MAX_PARTS; i++)
			LL_PlayCycle(i, motion_ID, bMixing, blendAccrue, blendFalloff, Speed, noloop, Callback, CallbackParam,
			             channel);
		return 0;
	}
	if (part >= MAX_PARTS) return 0;
	if (0 == m_Partition->part(part)) return 0;

	//	shared_motions* s_mots	= &m_Motions[motion.slot];
	//	CMotionDef* m_def		= s_mots->motion_def(motion.idx);

	// Process old cycles and create _new_
	if (channel == 0)
	{
		_DBG_SINGLE_USE_MARKER;
		if (bMixing) LL_FadeCycle(part, blendFalloff, 1 << channel);
		else LL_CloseCycle(part, 1 << channel);
	}
	CPartDef* P = (*m_Partition)[part];
	CBlend* B = IBlend_Create();
	if (!B) return 0;

	_DBG_SINGLE_USE_MARKER;
	IBlendSetup(*B, part, channel, motion_ID, bMixing, blendAccrue, blendFalloff, Speed, noloop, Callback,
	            CallbackParam);
	for (u32 i = 0; i < P->bones.size(); i++)
	{
		if (P->bones[i] >= bones->size() || !(*bones)[P->bones[i]])
		{
			Msg("! MODEL: animation partition %u in '%s' references invalid bone %u",
				part, *getDebugName(), P->bones[i]);
			continue;
		}

		Bone_Motion_Start_IM((*bones)[P->bones[i]], B);
	}
	blend_cycles[part].push_back(B);
	return B;
}

CBlend* CKinematicsAnimated::LL_PlayCycle(u16 part, MotionID motion_ID, BOOL bMixIn, PlayCallback Callback,
                                          LPVOID CallbackParam, u8 channel /*=0*/)
{
	VERIFY(motion_ID.valid());
	CMotionDef* m_def = m_Motions[motion_ID.slot].motions.motion_def(motion_ID.idx);
	VERIFY(m_def);
	if (!m_def) return NULL;
	return LL_PlayCycle(part, motion_ID, bMixIn,
	                    m_def->Accrue(), m_def->Falloff(), m_def->Speed(), m_def->StopAtEnd(),
	                    Callback, CallbackParam, channel);
}

CBlend* CKinematicsAnimated::PlayCycle(LPCSTR N, BOOL bMixIn, PlayCallback Callback, LPVOID CallbackParam,
                                       u8 channel /*= 0*/)
{
	MotionID motion_ID = ID_Cycle(N);
	if (motion_ID.valid()) return PlayCycle(motion_ID, bMixIn, Callback, CallbackParam, channel);
	else
	{
		Debug.fatal(DEBUG_INFO, "! MODEL: can't find cycle: %s", N);
		return 0;
	}
}

CBlend* CKinematicsAnimated::PlayCycle(MotionID motion_ID, BOOL bMixIn, PlayCallback Callback, LPVOID CallbackParam,
                                       u8 channel /*= 0*/)
{
	VERIFY(motion_ID.valid());
	CMotionDef* m_def = m_Motions[motion_ID.slot].motions.motion_def(motion_ID.idx);
	VERIFY(m_def);
	if (!m_def) return NULL;
	return LL_PlayCycle(m_def->bone_or_part, motion_ID, bMixIn,
	                    m_def->Accrue(), m_def->Falloff(), m_def->Speed(), m_def->StopAtEnd(),
	                    Callback, CallbackParam, channel);
}

CBlend* CKinematicsAnimated::PlayCycle(u16 partition, MotionID motion_ID, BOOL bMixIn, PlayCallback Callback,
                                       LPVOID CallbackParam, u8 channel, float speed)
{
	VERIFY(motion_ID.valid());
	CMotionDef* m_def = m_Motions[motion_ID.slot].motions.motion_def(motion_ID.idx);
	VERIFY(m_def);
	if (!m_def) return NULL;
	return LL_PlayCycle(partition, motion_ID, bMixIn,
	                    m_def->Accrue(), m_def->Falloff(), m_def->Speed() * speed, m_def->StopAtEnd(),
	                    Callback, CallbackParam, channel);
}

// fx'es
MotionID CKinematicsAnimated::ID_FX_Safe(LPCSTR N)
{
	MotionID motion_ID;
	for (int k = int(m_Motions.size()) - 1; k >= 0; --k)
	{
		shared_motions* s_mots = &m_Motions[k].motions;
		auto I = s_mots->fx_hash()->find(LPSTR(N));
		if (I != s_mots->fx_hash()->end())
		{
			motion_ID.set(u16(k), I->second);
			break;
		}
	}
	return motion_ID;
}

MotionID CKinematicsAnimated::ID_FX(LPCSTR N)
{
	MotionID motion_ID = ID_FX_Safe(N);
	R_ASSERT3(motion_ID.valid(), "! MODEL: can't find FX: ", N);
	return motion_ID;
}

CBlend* CKinematicsAnimated::PlayFX(MotionID motion_ID, float power_scale)
{
	VERIFY(motion_ID.valid());
	CMotionDef* m_def = m_Motions[motion_ID.slot].motions.motion_def(motion_ID.idx);
	VERIFY(m_def);
	if (!m_def) return NULL;
	return LL_PlayFX(m_def->bone_or_part, motion_ID,
	                 m_def->Accrue(), m_def->Falloff(),
	                 m_def->Speed(), m_def->Power() * power_scale);
}

CBlend* CKinematicsAnimated::PlayFX(LPCSTR N, float power_scale)
{
	MotionID motion_ID = ID_FX(N);
	return PlayFX(motion_ID, power_scale);
}

//u16 part,u8 channel, MotionID motion_ID, BOOL  bMixing, float blendAccrue, float blendFalloff, float Speed, BOOL noloop, PlayCallback callback(), LPVOID CallbackParam)

CBlend* CKinematicsAnimated::LL_PlayFX(u16 bone, MotionID motion_ID, float blendAccrue, float blendFalloff, float Speed,
                                       float Power)
{
	if (!motion_ID.valid()) return 0;
	if (blend_fx.size() >= MAX_BLENDED) return 0;
	if (BI_NONE == bone) bone = iRoot;

	CBlend* B = IBlend_Create();
	if (!B) return 0;

	_DBG_SINGLE_USE_MARKER;
	IFXBlendSetup(*B, motion_ID, blendAccrue, blendFalloff, Power, Speed, bone);
	Bone_Motion_Start((*bones)[bone], B);

	blend_fx.push_back(B);
	return B;
}

void CKinematicsAnimated::DestroyCycle(CBlend& B)
{
	if (GetBlendDestroyCallback())
		GetBlendDestroyCallback()->BlendDestroy(B);
	//B.blend 		= CBlend::eFREE_SLOT;
	B.set_free_state();
	const CPartDef* P = m_Partition->part(B.bone_or_part);
	if (nullptr == P) return;
	for (u32 i = 0; i < P->bones.size(); i++)
		Bone_Motion_Stop_IM((*bones)[P->bones[i]], &B);
}

//returns true if play time out

void CKinematicsAnimated::LL_UpdateTracks(float dt, bool b_force, bool leave_blends)
{
	BlendSVecIt I, E;
	// Cycles
	for (u16 part = 0; part < MAX_PARTS; part++)
	{
		if (nullptr == m_Partition->part(part))
			continue;
		I = blend_cycles[part].begin();
		E = blend_cycles[part].end();
		for (; I != E; I++)
		{
			CBlend& B = *(*I);
			if (!b_force && B.dwFrame == RDEVICE.dwFrame)
				continue;
			B.dwFrame = RDEVICE.dwFrame;
			if (B.update(dt, B.Callback) && !leave_blends)
			{
				DestroyCycle(B);
				blend_cycles[part].erase(I);
				E = blend_cycles[part].end();
				I--;
			}
			//else{
			//	CMotionDef* m_def						= m_Motions[B.motionID.slot].motions.motion_def(B.motionID.idx);
			//	float timeCurrent						= B.timeCurrent;
			//	xr_vector<motion_marks>::iterator it	= m_def->marks.begin();
			//	xr_vector<motion_marks>::iterator it_e	= m_def->marks.end();
			//	for(;it!=it_e; ++it)
			//	{
			//		if( (*it).pick_mark(timeCurrent) )
			//	}
			//}
		}
	}

	LL_UpdateFxTracks(dt);
}

void CKinematicsAnimated::LL_UpdateFxTracks(float dt)
{
	// FX
	BlendSVecIt I, E;
	I = blend_fx.begin();
	E = blend_fx.end();
	for (; I != E; I++)
	{
		CBlend& B = *(*I);
		if (!B.stop_at_end_callback)
		{
			B.playing = FALSE;
			continue;
		}
		//B.timeCurrent += dt*B.speed;
		B.update_time(dt);
		switch (B.blend_state())
		{
		case CBlend::eFREE_SLOT:
			NODEFAULT;

		case CBlend::eAccrue:
			B.blendAmount += dt * B.blendAccrue * B.blendPower * B.speed;
			if (B.blendAmount >= B.blendPower)
			{
				// switch to fixed
				B.blendAmount = B.blendPower;
				//B.blend			= CBlend::eFalloff;//CBlend::eFixed;
				B.set_falloff_state();
			}
			break;
		case CBlend::eFalloff:
			B.blendAmount -= dt * B.blendFalloff * B.blendPower * B.speed;
			if (B.blendAmount <= 0)
			{
				// destroy fx
				//B.blend = CBlend::eFREE_SLOT;
				B.set_free_state();
				Bone_Motion_Stop((*bones)[B.bone_or_part], *I);
				blend_fx.erase(I);
				E = blend_fx.end();
				I--;
			}
			break;
		default: NODEFAULT;
		}
	}
}

void CKinematicsAnimated::UpdateTracks()
{
	_DBG_SINGLE_USE_MARKER;
	if (Update_LastTime == RDEVICE.dwTimeGlobal) return;
	u32 DT = RDEVICE.dwTimeGlobal - Update_LastTime;
	if (DT > 66) DT = 66;
	float dt = float(DT) / 1000.f;

	if (GetUpdateTracksCalback())
	{
		if ((*GetUpdateTracksCalback())(float(RDEVICE.dwTimeGlobal - Update_LastTime) / 1000.f, *this))
			Update_LastTime = RDEVICE.dwTimeGlobal;
		return;
	}
	Update_LastTime = RDEVICE.dwTimeGlobal;
	LL_UpdateTracks(dt, false, false);
}

void CKinematicsAnimated::Release()
{
	// xr_free bones
	//.	for (u32 i=0; i<bones->size(); i++)
	//.	{
	//.		CBoneDataAnimated* B	= (CBoneDataAnimated*)(*bones)[i];
	//.		B->Motions.clear		();
	//.	}

	// destroy shared data
	//.	xr_delete(partition);
	//.	xr_delete(motion_map);
	//.	xr_delete(m_cycle);
	//.	xr_delete(m_fx);

	inherited::Release();
}

CKinematicsAnimated::~CKinematicsAnimated()
{
	IBoneInstances_Destroy();
}

CKinematicsAnimated::CKinematicsAnimated():
	CKinematics(),
	IKinematicsAnimated(),
	blend_instances(NULL),
	m_Partition(NULL),
	m_blend_destroy_callback(0),
	m_update_tracks_callback(0),
	Update_LastTime(0)
{}

void CKinematicsAnimated::IBoneInstances_Create()
{
	inherited::IBoneInstances_Create();
	u32 size = bones->size();
	blend_instances = xr_alloc<CBlendInstance>(size);
	for (u32 i = 0; i < size; i++)
		blend_instances[i].construct();
}

void CKinematicsAnimated::IBoneInstances_Destroy()
{
	inherited::IBoneInstances_Destroy();
	if (blend_instances)
	{
		xr_free(blend_instances);
		blend_instances = NULL;
	}
}

#define PCOPY(a)	a = pFrom->a

void CKinematicsAnimated::Copy(dxRender_Visual* P)
{
	inherited::Copy(P);

	CKinematicsAnimated* pFrom = (CKinematicsAnimated*)P;
	PCOPY(m_Motions);
	PCOPY(m_Partition);

	IBlend_Startup();
}

void CKinematicsAnimated::Spawn()
{
	inherited::Spawn();

	IBlend_Startup();

	for (u32 i = 0; i < bones->size(); i++)
		blend_instances[i].construct();
	m_update_tracks_callback = 0;
	channels.init();
}

void CKinematicsAnimated::ChannelFactorsStartup()
{
	channels.init();
}

void CKinematicsAnimated::LL_SetChannelFactor(u16 channel, float factor)
{
	channels.set_factor(channel, factor);
}

void CKinematicsAnimated::IBlend_Startup()
{
	_DBG_SINGLE_USE_MARKER;
	CBlend B;
	ZeroMemory(&B, sizeof(B));
	//B.blend				= CBlend::eFREE_SLOT;

	B.set_free_state();

#ifdef	DEBUG
	B.set_falloff_state();
#endif

	blend_pool.clear();
	for (u32 i = 0; i < MAX_BLENDED_POOL; i++)
	{
		blend_pool.push_back(B);
#ifdef	DEBUG
		blend_pool.back().set_free_state();
#endif
	}
	// cycles+fx clear
	for (u32 i = 0; i < MAX_PARTS; i++)
		blend_cycles[i].clear();
	blend_fx.clear();
	ChannelFactorsStartup();
}

CBlend* CKinematicsAnimated::IBlend_Create()
{
	UpdateTracks();
	_DBG_SINGLE_USE_MARKER;
	CBlend *I = blend_pool.begin(), *E = blend_pool.end();
	for (; I != E; I++)
		if (I->blend_state() == CBlend::eFREE_SLOT) return I;
//	FATAL("Too many blended motions requested");
	return 0;
}

void CKinematicsAnimated::Load(const char* N, IReader* data, u32 dwFlags)
{
	inherited::Load(N, data, dwFlags);

	// Globals
	blend_instances = NULL;
	m_Partition = NULL;
	Update_LastTime = 0;

	const auto loadOMF = [&](LPCSTR _path)
	{
		// Once an OMF is in the shared container no file lookup or prepared byte
		// copy is needed. This is the common path for additional NPC models using
		// the same (often very large) mod animation packs.
		const bool resident = g_pMotionsContainer->has(_path, bones);
		IReader* prepared_reader = nullptr;
		string_path fn;
		if (!resident)
		{
			prepared_reader = open_prepared_motion_source(_path);
			if (!prepared_reader && !FS.exist(fn, "$level$", _path))
			{
				if (!FS.exist(fn, "$game_meshes$", _path))
					Debug.fatal(DEBUG_INFO, "Can't find motion file '%s'\nsection '%s'\nmodel '%s'", _path, current_player_hud_sect.c_str(), N);
			}
		}

		// Check compatibility. Prepared OMF bytes are produced by the worker lane,
		// so render-thread finalization does not repeat archive lookup and disk IO.
		m_Motions.push_back(SMotionsSlot());
		bool create_res;
		if (!resident)
		{
			IReader* MS = prepared_reader ? prepared_reader : FS.r_open(fn);
			create_res = m_Motions.back().motions.create(_path, MS, bones);
			if (prepared_reader)
				xr_delete(MS);
			else
				FS.r_close(MS);
		}
		else
			create_res = m_Motions.back().motions.create(_path, nullptr, bones);

		if (!create_res)
		{
			m_Motions.pop_back();
			Msg("! error in model [%s]. Unable to load motion file '%s', section '%s'.", N, _path, current_player_hud_sect.c_str());
		}
	};

	// Load animation
	if (data->find_chunk(OGF_S_MOTION_REFS))
	{
		string_path items_nm;
		data->r_stringZ(items_nm, sizeof(items_nm));
		u32 set_cnt = _GetItemCount(items_nm);
		R_ASSERT2(set_cnt<MAX_ANIM_SLOT, make_string("section '%s'\nmodel '%s'", current_player_hud_sect.c_str(), N).c_str());
		m_Motions.reserve(set_cnt);
		string_path nm;
		for (u32 k = 0; k < set_cnt; ++k)
		{
			_GetItem(items_nm, k, nm);

			if (strstr(nm, "\\*.omf"))
			{
				FS_FileSet fset;
				FS.file_list(fset, "$game_meshes$", FS_ListFiles, nm);
				FS.file_list(fset, "$level$", FS_ListFiles, nm);

				m_Motions.reserve(m_Motions.size() + fset.size());

				for (FS_FileSet::iterator it = fset.begin(); it != fset.end(); it++)
					loadOMF((*it).name.c_str());

				continue;
			}

			xr_strcat(nm, ".omf");
			loadOMF(nm);

            //load extra stalker animations
            if (strstr(nm, "stalker_animation.omf"))
            {
                FS_FileSet extra_anims;
                FS.file_list(extra_anims, "$game_meshes$", FS_ListFiles, "actors\\modded_stalker_animations\\*.omf");

                if (!extra_anims.empty())
                {
                    m_Motions.reserve(m_Motions.size() + extra_anims.size());
                    for (auto it = extra_anims.begin(); it != extra_anims.end(); ++it)
                    {
                        loadOMF(it->name.c_str());
                    }
                }
            }
		}
	}
	else if (data->find_chunk(OGF_S_MOTION_REFS2))
	{
		u32 set_cnt = data->r_u32();
		m_Motions.reserve(set_cnt);
		string_path nm;
		for (u32 k = 0; k < set_cnt; ++k)
		{
			data->r_stringZ(nm, sizeof(nm));

			if (strstr(nm, "\\*.omf"))
			{
				FS_FileSet fset;
				FS.file_list(fset, "$game_meshes$", FS_ListFiles, nm);
				FS.file_list(fset, "$level$", FS_ListFiles, nm);

				m_Motions.reserve(m_Motions.size() + fset.size());

				for (FS_FileSet::iterator it = fset.begin(); it != fset.end(); it++)
					loadOMF((*it).name.c_str());

				continue;
			}

			xr_strcat(nm, ".omf");
			loadOMF(nm);

            //load extra stalker animations
            if (strstr(nm, "stalker_animation.omf"))
            {
                FS_FileSet extra_anims;
                FS.file_list(extra_anims, "$game_meshes$", FS_ListFiles, "actors\\modded_stalker_animations\\*.omf");

                if (!extra_anims.empty())
                {
                    m_Motions.reserve(m_Motions.size() + extra_anims.size());
                    for (auto it = extra_anims.begin(); it != extra_anims.end(); ++it)
                    {
                        loadOMF(it->name.c_str());
                    }
                }
            }
		}
	}
	else
	{
		string_path nm;
		strconcat(sizeof(nm), nm, N, ".ogf");
		m_Motions.push_back(SMotionsSlot());
		m_Motions.back().motions.create(nm, data, bones);
	}

	R_ASSERT2(m_Motions.size(), make_string("section '%s'\nmodel '%s'", current_player_hud_sect.c_str(), N).c_str());

	m_Partition = m_Motions[0].motions.partition();
	m_Partition->load(this, N);

	// initialize motions
	for (MotionsSlotVecIt m_it = m_Motions.begin(); m_it != m_Motions.end(); m_it++)
	{
		SMotionsSlot& MS = *m_it;
		MS.bone_motions.resize(bones->size());
		for (u32 i = 0; i < bones->size(); i++)
		{
			CBoneData* BD = (*bones)[i];
			MS.bone_motions[i] = MS.motions.bone_motions(BD->name);
		}
	}

	// Init blend pool
	IBlend_Startup();
}

void CKinematicsAnimated::LL_BuldBoneMatrixDequatize(const CBoneData* bd, u8 channel_mask, SKeyTable& keys)
{
	u16 SelfID = bd->GetSelfID();
	CBlendInstance& BLEND_INST = LL_GetBlendInstance(SelfID);
	const CBlendInstance::BlendSVec& Blend = BLEND_INST.blend_vector();
	CKey BK[MAX_CHANNELS][MAX_BLENDED]; //base keys
	BlendSVecCIt BI;
	for (BI = Blend.begin(); BI != Blend.end(); BI++)
	{
		CBlend* B = *BI;
		int& b_count = keys.chanel_blend_conts[B->channel];
		CKey* D = &keys.keys[B->channel][b_count];
		if (!(channel_mask & (1 << B->channel)))
			continue;
		u8 channel = B->channel;
		//keys.blend_factors[channel][b_count]	=  B->blendAmount;
		keys.blends[channel][b_count] = B;
		CMotion& M = *LL_GetMotion(B->motionID, SelfID);
		Dequantize(*D, *B, M);

		// Base-pose keys are consumed only by additive channels.  The normal
		// interpolation channels (the overwhelmingly common NPC path) previously
		// dequantized and stored a second key that was never read.
		if (channels.rule(channel).extern_ == animation::add)
		{
			QR2Quat(M._keysR[0], BK[channel][b_count].Q);
			if (M.test_flag(flTKeyPresent))
			{
				if (M.test_flag(flTKey16IsBit))
					QT16_2T(M._keysT16[0], M, BK[channel][b_count].T);
				else
					QT8_2T(M._keysT8[0], M, BK[channel][b_count].T);
			}
			else
				BK[channel][b_count].T.set(M._initT);
		}
		++b_count;
	}
	for (u16 j = 0; MAX_CHANNELS > j; ++j)
		if (channels.rule(j).extern_ == animation::add)
			keys_substruct(keys.keys[j], BK[j], keys.chanel_blend_conts[j]);
}

void CKinematicsAnimated::LL_ApplyBoneKey(u16 bone_id, CBoneInstance& bi, const Fmatrix* parent, const CKey& key)
{
	Fmatrix RES;
	RES.mk_xform(key.Q, key.T);

	if (LL_GetBoneVisible(bone_id))
	{
		bi.mTransform.mul_43(*parent, RES);
		bi.mTransformHidden.set(bi.mTransform);
	}
	else
	{
		bi.mTransform.c = (*parent).c;
		bi.mTransformHidden.mul_43(*parent, RES);
	}

#ifdef DEBUG
#ifndef _EDITOR
		if(!check_scale(RES))
		{
			VERIFY(check_scale(bi.mTransform));
		}
		VERIFY( _valid( bi.mTransform ) );
		Fbox dbg_box;
		float box_size = 100000.f;
		dbg_box.set( -box_size, -box_size, -box_size, box_size, box_size, box_size );
		//VERIFY(dbg_box.contains(bi.mTransform.c));
		VERIFY2( dbg_box.contains(bi.mTransform.c), ( make_string( "model: %s has strange bone position, matrix : ", getDebugName().c_str() ) + get_string( bi.mTransform ) ).c_str() );

		//if(!is_similar(PrevTransform,RES,0.3f))
	//{
	//	Msg("bone %s",*bd->name)	;
	//}
	//BONE_INST.mPrevTransform.set(RES);
#endif
#endif
}

// calculate single bone with key blending
void CKinematicsAnimated::LL_BoneMatrixBuild(u16 bone_id, CBoneInstance& bi, const Fmatrix* parent, const SKeyTable& keys)
{
	// Blend them together
	CKey channel_keys[MAX_CHANNELS];
	animation::channel_def BC [MAX_CHANNELS];
	u16 ch_count = 0;

	for (u16 j = 0; MAX_CHANNELS > j; ++j)
	{
		if (j != 0 && keys.chanel_blend_conts[j] == 0)
			continue;
		//data for channel mix cycle based on ch_count
		channels.get_def(j, BC[ch_count]);
		process_single_channel(channel_keys[ch_count], BC[ch_count], keys.keys[j], keys.blends[j],
		                       keys.chanel_blend_conts[j]);
		++ch_count;
	}
	CKey result;
	//Mix channels
	MixChannels(result, channel_keys, BC, ch_count);
	LL_ApplyBoneKey(bone_id, bi, parent, result);
}

void CKinematicsAnimated::BuildBoneMatrix(const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent,
                                          u8 channel_mask /*= (1<<0)*/)
{
	// Normal NPC animation evaluates only interpolation channel 0. Avoid the
	// four-channel SKeyTable and additive base-pose scratch in this exact case;
	// mixed/additive/procedural callers retain the original path below.
	if (channel_mask == (1u << 0))
	{
		const u16 bone_id = bd->GetSelfID();
		CBlendInstance& blend_instance = LL_GetBlendInstance(bone_id);
		const CBlendInstance::BlendSVec& blends = blend_instance.blend_vector();
		CKey base_keys[MAX_BLENDED];
		CBlend* base_blends[MAX_BLENDED];
		u16 base_count = 0;

		for (CBlendInstance::BlendSVecCIt blend_it = blends.begin(); blend_it != blends.end(); ++blend_it)
		{
			CBlend* blend = *blend_it;
			if (blend->channel != 0)
				continue;

			VERIFY(base_count < MAX_BLENDED);
			base_blends[base_count] = blend;
			CMotion& motion = *LL_GetMotion(blend->motionID, bone_id);
			Dequantize(base_keys[base_count], *blend, motion);
			++base_count;
		}

		if (base_count)
		{
			CKey result;
			if (base_count == 1)
				result = base_keys[0];
			else
				MixInterlerp(result, base_keys, base_blends, base_count);
			LL_ApplyBoneKey(bone_id, bi, parent, result);
			return;
		}
	}

	//CKey				R						[MAX_CHANNELS][MAX_BLENDED];	//all keys 
	//float				BA						[MAX_CHANNELS][MAX_BLENDED];	//all factors
	//int				b_counts				[MAX_CHANNELS]	= {0,0,0,0}; //channel counts
	SKeyTable keys;
	LL_BuldBoneMatrixDequatize(bd, channel_mask, keys);

	LL_BoneMatrixBuild(bd->GetSelfID(), bi, parent, keys);

	/*
	if(bi.mTransform.c.y>10000)
	{
	Log("BLEND_INST",BLEND_INST.Blend.size());
	Log("Bone",LL_BoneName_dbg(SelfID));
	Msg("Result.Q %f,%f,%f,%f",Result.Q.x,Result.Q.y,Result.Q.z,Result.Q.w);
	Log("Result.T",Result.T);
	Log("lp parent",(u32)parent);
	Log("parent",*parent);
	Log("RES",RES);
	Log("mT",bi.mTransform);

	CBlend*			B		=	*BI;
	CMotion&		M		=	*LL_GetMotion(B->motionID,SelfID);
	float			time	=	B->timeCurrent*float(SAMPLE_FPS);
	u32				frame	=	iFloor(time);
	u32				count	=	M.get_count();
	float			delta	=	time-float(frame);

	Log("flTKeyPresent",M.test_flag(flTKeyPresent));
	Log("M._initT",M._initT);
	Log("M._sizeT",M._sizeT);

	// translate
	if (M.test_flag(flTKeyPresent))
	{
	CKeyQT*	K1t	= &M._keysT[(frame+0)%count];
	CKeyQT*	K2t	= &M._keysT[(frame+1)%count];

	Fvector T1,T2,Dt;
	T1.x		= float(K1t->x)*M._sizeT.x+M._initT.x;
	T1.y		= float(K1t->y)*M._sizeT.y+M._initT.y;
	T1.z		= float(K1t->z)*M._sizeT.z+M._initT.z;
	T2.x		= float(K2t->x)*M._sizeT.x+M._initT.x;
	T2.y		= float(K2t->y)*M._sizeT.y+M._initT.y;
	T2.z		= float(K2t->z)*M._sizeT.z+M._initT.z;

	Dt.lerp	(T1,T2,delta);

	Msg("K1t %d,%d,%d",K1t->x,K1t->y,K1t->z);
	Msg("K2t %d,%d,%d",K2t->x,K2t->y,K2t->z);

	Log("count",count);
	Log("frame",frame);
	Log("T1",T1);
	Log("T2",T2);
	Log("delta",delta);
	Log("Dt",Dt);

	}else
	{
	D->T.set	(M._initT);
	}
	VERIFY(0);
	}
	*/
}

void CKinematicsAnimated::OnCalculateBones()
{
	UpdateTracks();
}

bool CKinematicsAnimated::CanCalculateBonesOnWorker() const
{
	if (!inherited::CanCalculateBonesOnWorker())
		return false;
	if (m_blend_destroy_callback)
	{
		return false;
	}
	if (m_update_tracks_callback)
	{
		return false;
	}

	// A PlayCallback may enter an arbitrary game object or script when a motion
	// reaches its end. Keep every active callback-bearing blend on the legacy
	// main-thread path, even if it is unlikely to fire in this particular frame.
	for (u32 index = 0; index < blend_pool.size(); ++index)
	{
		const CBlend& blend = blend_pool[index];
		if (blend.blend_state() != CBlend::eFREE_SLOT && blend.Callback)
		{
			return false;
		}
	}

	return true;
}

IBlendDestroyCallback* CKinematicsAnimated::GetBlendDestroyCallback()
{
	return m_blend_destroy_callback;
}

void CKinematicsAnimated::SetUpdateTracksCalback(IUpdateTracksCallback* callback)
{
	m_update_tracks_callback = callback;
}

void CKinematicsAnimated::SetBlendDestroyCallback(IBlendDestroyCallback* cb)
{
	m_blend_destroy_callback = cb;
}

#ifdef _EDITOR
MotionID CKinematicsAnimated::ID_Motion(LPCSTR  N, u16 slot)
{
	MotionID 				motion_ID;
    if (slot<MAX_ANIM_SLOT){
        shared_motions* s_mots	= &m_Motions[slot].motions;
        // find in cycles
        auto I 	= s_mots->cycle_hash()->find(LPSTR(N));
        if (I!=s_mots->cycle_hash()->end())	motion_ID.set(slot,I->second);
        if (!motion_ID.valid()){
            // find in fx's
            auto I 	= s_mots->fx_hash()->find(LPSTR(N));
            if (I!=s_mots->fx_hash()->end())	motion_ID.set(slot,I->second);
        }
    }
    return motion_ID;
}
#endif
