//---------------------------------------------------------------------------
#include "stdafx.h"
#pragma hdrstop

#include "SkeletonMotions.h"
//#include "SkeletonAnimated.h"
#include "Fmesh.h"
#include "motion.h"
#include "..\Include\xrRender\Kinematics.h"

motions_container* g_pMotionsContainer = 0;

namespace
{
shared_str make_motions_cache_key(const shared_str& source_key, const vecBones* bones)
{
	xr_string cache_key = source_key.c_str() ? source_key.c_str() : "";
	cache_key += "|bones:";
	if (!bones)
		return shared_str(cache_key.c_str());

	string32 length_prefix;
	for (const CBoneData* bone : *bones)
	{
		LPCSTR name = bone && bone->name.c_str() ? bone->name.c_str() : "";
		const u32 length = static_cast<u32>(xr_strlen(name));
		xr_sprintf(length_prefix, sizeof(length_prefix), "%u:", length);
		cache_key += length_prefix;
		cache_key.append(name, length);
		cache_key += ';';
	}
	return shared_str(cache_key.c_str());
}
}

u16 CPartition::part_id(const shared_str& name) const
{
	for (u16 i = 0; i < MAX_PARTS; ++i)
	{
		const CPartDef* pd = part(i);
		if (pd && pd->Name == name)
			return i;
	}
	Msg("!there is no part named [%s]", name.c_str());
	return u16(-1);
}

void CPartition::load(IKinematics* V, LPCSTR model_name)
{
	string_path fn, fn_full;
	xr_strcpy(fn, sizeof(fn), model_name);
	if (strext(fn))
		*strext(fn) = 0;
	xr_strcat(fn, sizeof(fn), ".ltx");

	FS.update_path(fn_full, "$game_meshes$", fn);

	CInifile ini(fn_full, TRUE, TRUE, FALSE);

	if (ini.sections().size() == 0) return;
	shared_str part_name = "partition_name";
	for (u32 i = 0; i < MAX_PARTS; ++i)
	{
		string64 buff;
		xr_sprintf(buff, sizeof(buff), "part_%d", i);

		CInifile::Sect S = ini.r_section(buff);
		CInifile::SectCIt it = S.Data.begin();
		CInifile::SectCIt it_e = S.Data.end();

		if (!S.Data.size()) continue;

		// A partition file may legally omit an earlier part. Never inspect the
		// reserved-but-not-constructed vector slots while filling that gap.
		while (P.size() <= i)
		{
			R_ASSERT2(create(), "Too many skeleton partitions");
		}
		CPartDef* const partition = P[i];
		partition->bones.clear_not_free();

		for (; it != it_e; ++it)
		{
			const CInifile::Item& I = *it;
			if (I.first == part_name)
			{
				partition->Name = I.second;
			}
			else
			{
				u32 bid = V->LL_BoneID(I.first.c_str());
				if (bid == BI_NONE || bid >= V->LL_BoneCount())
				{
					Msg("! MODEL: animation partition '%s' in '%s' references unknown bone '%s'",
						buff, model_name, I.first.c_str());
					continue;
				}
				partition->bones.push_back(bid);
			}
		}
	}
}

u16 find_bone_id(vecBones* bones, shared_str nm)
{
	for (u16 i = 0; i < (u16)bones->size(); i++)
		if (bones->at(i)->name == nm) return i;
	return BI_NONE;
}

//-----------------------------------------------------------------------
BOOL motions_value::load(LPCSTR N, IReader* data, vecBones* bones)
{
	m_id = N;

	bool bRes = true;
	// Load definitions
	U16Vec rm_bones;
	xr_vector<u8> mapped_model_bones(bones->size(), FALSE);
	u32 source_bone_count = 0;
	IReader* MP = data->open_chunk(OGF_S_SMPARAMS);

	if (MP)
	{
		u16 vers = MP->r_u16();
		string128 buf;
		R_ASSERT3(vers <= xrOGF_SMParamsVersion, "Invalid OGF/OMF version:", N);

		// partitions
		u16 part_count = MP->r_u16();
		if (part_count > MAX_PARTS)
		{
			MP->close();
			Msg("! OMF '%s': too many skeleton partitions (%u)", N, part_count);
			return false;
		}

		for (u16 part_i = 0; part_i < part_count; part_i++)
		{
			CPartDef* PART = m_partition[part_i];
			while (!PART) PART = m_partition.create();
			MP->r_stringZ(buf, sizeof(buf));
			PART->Name = _strlwr(buf);
			const u16 part_source_bone_count = MP->r_u16();
			PART->bones.clear_not_free();
			PART->bones.reserve(part_source_bone_count);

			for (u16 part_bone = 0; part_bone < part_source_bone_count; ++part_bone)
			{
				MP->r_stringZ(buf, sizeof(buf));
				const u32 motion_index = MP->r_u32();
				if (motion_index >= 64)
				{
					bRes = false;
					Msg("! OMF '%s': invalid bone index %u for '%s'", N, motion_index, buf);
					continue;
				}

				if (rm_bones.size() <= motion_index)
					rm_bones.resize(motion_index + 1, BI_NONE);
				source_bone_count = _max(source_bone_count, motion_index + 1);

				const u16 model_bone = find_bone_id(bones, buf);
				if (model_bone == BI_NONE)
					continue; // A motion-only bone is safe to ignore for a smaller compatible model.

				if (rm_bones[motion_index] != BI_NONE && rm_bones[motion_index] != model_bone)
				{
					bRes = false;
					Msg("! OMF '%s': duplicate bone index %u", N, motion_index);
					continue;
				}

				rm_bones[motion_index] = model_bone;
				mapped_model_bones[model_bone] = TRUE;
				PART->bones.push_back(model_bone);
			}
		}

		for (u32 bone_id = 0; bone_id < bones->size(); ++bone_id)
		{
			if (mapped_model_bones[bone_id])
				continue;
			bRes = false;
			Msg("! OMF '%s': no animation data for model bone '%s'", N,
				bones->at(bone_id)->name.c_str());
		}

		if (bRes)
		{
			// motion defs (cycle&fx)
			u16 mot_count = MP->r_u16();
			m_mdefs.resize(mot_count);
			m_motion_hash.reserve(mot_count);
			m_cycle_hash.reserve(mot_count);
			m_fx_hash.reserve(mot_count);

			for (u16 mot_i = 0; mot_i < mot_count; mot_i++)
			{
				MP->r_stringZ(buf, sizeof(buf));
				shared_str nm = _strlwr(buf);
				u32 dwFlags = MP->r_u32();
				CMotionDef& D = m_mdefs[mot_i];
				D.Load(MP, dwFlags, vers);
				if (dwFlags & esmFX)
				{
					// BI_NONE is the serialized sentinel for an FX rooted at the skeleton root.
					if (D.bone_or_part != BI_NONE &&
						(D.bone_or_part >= rm_bones.size() || rm_bones[D.bone_or_part] == BI_NONE))
					{
						bRes = false;
						Msg("! OMF '%s': FX motion '%s' targets a bone absent from the model", N, nm.c_str());
					}
					else if (D.bone_or_part != BI_NONE)
						D.bone_or_part = rm_bones[D.bone_or_part];
				}
				else if (D.bone_or_part != BI_NONE && !m_partition.part(D.bone_or_part))
				{
					bRes = false;
					Msg("! OMF '%s': cycle '%s' targets invalid partition %u", N, nm.c_str(), D.bone_or_part);
				}
				//. m_mdefs.push_back (D);

				if (dwFlags & esmFX)
				{
					m_fx.insert(mk_pair(nm, mot_i));
					m_fx_hash.emplace(nm, mot_i);
				}
				else
				{
					m_cycle.insert(mk_pair(nm, mot_i));
					m_cycle_hash.emplace(nm, mot_i);
				}

				m_motion_map.insert(mk_pair(nm, mot_i));
				m_motion_hash.emplace(nm, mot_i);
			}
		}
		MP->close();
	}
	else
	{
		Debug.fatal(DEBUG_INFO, "Old skinned model version unsupported! (%s)", N);
	}
	if (!bRes) return false;

	// Load animation
	IReader* MS = data->open_chunk(OGF_S_MOTIONS);
	if (!MS) return false;

	u32 dwCNT = 0;
	MS->r_chunk_safe(0, &dwCNT, sizeof(dwCNT));
	VERIFY(dwCNT < 0x3FFF); // MotionID 2 bit - slot, 14 bit - motion index

	// set per bone motion size
	for (u32 i = 0; i < bones->size(); i++)
		m_motions[bones->at(i)->name].resize(dwCNT);

	// load motions
	for (u16 m_idx = 0; m_idx < (u16)dwCNT; m_idx++)
	{
		string128 mname;
		R_ASSERT(MS->find_chunk(m_idx + 1));
		MS->r_stringZ(mname, sizeof(mname));
#ifdef _DEBUG
        // sanity check
        xr_strlwr (mname);
        accel_map::iterator I= m_motion_map.find(mname);
        VERIFY3 (I!=m_motion_map.end(),"Can't find motion:",mname);
        VERIFY3 (I->second==m_idx,"Invalid motion index:",mname);
#endif
		u32 dwLen = MS->r_u32();
		for (u32 i = 0; i < source_bone_count; i++)
		{
			const u16 bone_id = rm_bones[i];
			CMotion* motion = bone_id == BI_NONE ? nullptr : &m_motions[bones->at(bone_id)->name][m_idx];
			if (motion)
				motion->set_count(dwLen);
			const u8 motion_flags = MS->r_u8();
			if (motion)
				motion->set_flags(motion_flags);

			if (motion_flags & flRKeyAbsent)
			{
				CKeyQR* r = (CKeyQR*)MS->pointer();
				if (motion)
				{
					u32 crc_q = crc32(r, sizeof(CKeyQR));
					motion->_keysR.create(crc_q, 1, r);
				}
				MS->advance(1 * sizeof(CKeyQR));
			}
			else
			{
				u32 crc_q = MS->r_u32();
				if (motion)
					motion->_keysR.create(crc_q, dwLen, (CKeyQR*)MS->pointer());
				MS->advance(dwLen * sizeof(CKeyQR));
			}
			if (motion_flags & flTKeyPresent)
			{
				u32 crc_t = MS->r_u32();
				if (motion_flags & flTKey16IsBit)
				{
					if (motion)
						motion->_keysT16.create(crc_t, dwLen, (CKeyQT16*)MS->pointer());
					MS->advance(dwLen * sizeof(CKeyQT16));
				}
				else
				{
					if (motion)
						motion->_keysT8.create(crc_t, dwLen, (CKeyQT8*)MS->pointer());
					MS->advance(dwLen * sizeof(CKeyQT8));
				};

				if (motion)
				{
					MS->r_fvector3(motion->_sizeT);
					MS->r_fvector3(motion->_initT);
				}
				else
					MS->advance(sizeof(Fvector) * 2);
			}
			else
			{
				if (motion)
					MS->r_fvector3(motion->_initT);
				else
					MS->advance(sizeof(Fvector));
			}
		}
	}
	// Msg("Motions %d/%d %4d/%4d/%d, %s",p_cnt,m_cnt, m_load,m_total,m_r,N);
	MS->close();

	return bRes;
}

MotionVec* motions_value::bone_motions(shared_str bone_name)
{
	BoneMotionMapIt I = m_motions.find(bone_name);
	// VERIFY (I != m_motions.end());
	if (I == m_motions.end())
		return (0);

	return (&(*I).second);
}

//-----------------------------------
motions_container::motions_container()
{
}

//extern shared_str s_bones_array_const;
motions_container::~motions_container()
{
	// clean (false);
	// clean (true);
	// dump ();
	VERIFY(container.empty());
	// Igor:
	//s_bones_array_const = 0;
}

bool motions_container::has(shared_str key)
{
	for (const auto& entry : container)
		if (entry.second && entry.second->m_id == key)
			return true;
	return false;
}

bool motions_container::has(shared_str key, const vecBones* bones)
{
	return container.find(make_motions_cache_key(key, bones)) != container.end();
}

motions_value* motions_container::dock(shared_str key, IReader* data, vecBones* bones)
{
	motions_value* result = 0;
	const shared_str cache_key = make_motions_cache_key(key, bones);
	SharedMotionsMapIt I = container.find(cache_key);
	if (I != container.end()) result = I->second;
	if (0 == result)
	{
		// loading motions
		VERIFY(data);
		result = xr_new<motions_value>();
		result->m_dwReference.store(0, std::memory_order_relaxed);
		BOOL bres = result->load(key.c_str(), data, bones);
		if (bres)
			container.insert(mk_pair(cache_key, result));
		else
			xr_delete(result);
	}
	return result;
}

void motions_container::clean(bool force_destroy)
{
	SharedMotionsMapIt it = container.begin();
	SharedMotionsMapIt _E = container.end();
	if (force_destroy)
	{
		for (; it != _E; it++)
		{
			motions_value* sv = it->second;
			xr_delete(sv);
		}
		container.clear();
	}
	else
	{
		for (; it != _E;)
		{
			motions_value* sv = it->second;
			if (0 == sv->m_dwReference.load(std::memory_order_acquire))
			{
				SharedMotionsMapIt i_current = it;
				SharedMotionsMapIt i_next = ++it;
				xr_delete(sv);
				container.erase(i_current);
				it = i_next;
			}
			else
			{
				it++;
			}
		}
	}
}

void motions_container::dump()
{
	SharedMotionsMapIt it = container.begin();
	SharedMotionsMapIt _E = container.end();
	Log("--- motion container --- begin:");
	u32 sz = sizeof(*this);
	for (u32 k = 0; it != _E; k++, it++)
	{
		sz += it->second->mem_usage();
		Msg("#%3d: [%3d/%5d Kb] - %s", k,
			it->second->m_dwReference.load(std::memory_order_relaxed),
			it->second->mem_usage() / 1024, it->second->m_id.c_str());
	}
	Msg("--- items: %d, mem usage: %d Kb ", container.size(), sz / 1024);
	Log("--- motion container --- end.");
}

//////////////////////////////////////////////////////////////////////////
// High level control
void CMotionDef::Load(IReader* MP, u32 fl, u16 version)
{
	// params
	bone_or_part = MP->r_u16(); // bCycle?part_id:bone_id;
	motion = MP->r_u16(); // motion_id
	speed = Quantize(MP->r_float());
	power = Quantize(MP->r_float());
	accrue = Quantize(MP->r_float());
	falloff = Quantize(MP->r_float());
	flags = (u16)fl;
	if (!(flags & esmFX) && (falloff >= accrue)) falloff = u16(accrue - 1);

	if (version >= 4)
	{
		u32 cnt = MP->r_u32();
		if (cnt > 0)
		{
			marks.resize(cnt);

			for (u32 i = 0; i < cnt; ++i)
				marks[i].Load(MP);
		}
	}
}

bool CMotionDef::StopAtEnd()
{
	return !!(flags & esmStopAtEnd);
}

bool shared_motions::create(shared_str key, IReader* data, vecBones* bones)
{
	motions_value* v = g_pMotionsContainer->dock(key, data, bones);
	if (0 != v)
		v->m_dwReference.fetch_add(1, std::memory_order_relaxed);
	destroy();
	p_ = v;
	return (0 != v);
}

bool shared_motions::create(shared_motions const& rhs)
{
	motions_value* v = rhs.p_;
	if (0 != v)
		v->m_dwReference.fetch_add(1, std::memory_order_relaxed);
	destroy();
	p_ = v;
	return (0 != v);
}

const motion_marks::interval* motion_marks::pick_mark(const float& t) const
{
	C_ITERATOR it = intervals.begin();
	C_ITERATOR it_e = intervals.end();

	for (; it != it_e; ++it)
	{
		const interval& I = (*it);
		if (I.first <= t && I.second >= t)
			return &I;

		if (I.first > t)
			break;
	}
	return NULL;
}

bool motion_marks::is_mark_between(float const& t0, float const& t1) const
{
	VERIFY(t0 <= t1);

	C_ITERATOR i = intervals.begin();
	C_ITERATOR e = intervals.end();
	for (; i != e; ++i)
	{
		VERIFY((*i).first <= (*i).second);

		if ((*i).first == t0)
			return (true);

		if ((*i).first > t0)
		{
			if ((*i).second <= t1)
				return (true);

			if ((*i).first <= t1)
				return (true);

			return (false);
		}

		if ((*i).second < t0)
			continue;

		if ((*i).second == t0)
			return (true);

		return (true);
	}

	return (false);
}

float motion_marks::time_to_next_mark(float time) const
{
	C_ITERATOR i = intervals.begin();
	C_ITERATOR e = intervals.end();
	float result_dist = FLT_MAX;
	for (; i != e; ++i)
	{
		float dist = (*i).first - time;
		if (dist > 0.f && dist < result_dist)
			result_dist = dist;
	}
	return result_dist;
}

void ENGINE_API motion_marks::Load(IReader* R)
{
	xr_string tmp;
	R->r_string(tmp);
	name = tmp.c_str();
	u32 cnt = R->r_u32();
	intervals.resize(cnt);
	for (u32 i = 0; i < cnt; ++i)
	{
		interval& item = intervals[i];
		item.first = R->r_float();
		item.second = R->r_float();
	}
}
#ifdef _EDITOR
void motion_marks::Save(IWriter* W)
{
    W->w_string (name.c_str());
    u32 cnt = intervals.size();
    W->w_u32 (cnt);
    for(u32 i=0; i<cnt; ++i)
    {
        interval& item = intervals[i];
        W->w_float (item.first);
        W->w_float (item.second);
    }
}
#endif
