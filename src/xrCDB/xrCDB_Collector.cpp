#include "stdafx.h"
//.#include "../xrCore/xrCore.h"
#pragma hdrstop

#include "xrCDB.h"

namespace CDB
{
	Collector::vertex_cell Collector::make_vertex_cell(const Fvector& vertex) const
	{
		vertex_cell cell;
		cell.x = iFloor(vertex.x * packed_inverse_epsilon);
		cell.y = iFloor(vertex.y * packed_inverse_epsilon);
		cell.z = iFloor(vertex.z * packed_inverse_epsilon);
		return cell;
	}

	void Collector::invalidate_vertex_cache()
	{
		packed_vertices.clear();
		packed_next.clear();
		packed_epsilon = 0.f;
		packed_inverse_epsilon = 0.f;
	}

	void Collector::ensure_vertex_cache(float eps)
	{
		R_ASSERT(eps > 0.f);
		if (packed_inverse_epsilon > 0.f && packed_epsilon == eps)
			return;

		packed_vertices.clear();
		packed_next.assign(verts.size(), u32(-1));
		packed_epsilon = eps;
		packed_inverse_epsilon = 1.f / eps;
		packed_vertices.reserve(verts.size());

		for (u32 index = 0; index < verts.size(); ++index)
		{
			const vertex_cell cell = make_vertex_cell(verts[index]);
			auto head = packed_vertices.find(cell);
			if (head == packed_vertices.end())
				packed_vertices.emplace(cell, index);
			else
			{
				packed_next[index] = head->second;
				head->second = index;
			}
		}
	}

	void Collector::reserve(size_t vertex_count, size_t face_count)
	{
		verts.reserve(vertex_count);
		faces.reserve(face_count);
		packed_vertices.reserve(vertex_count);
		packed_next.reserve(vertex_count);
		adjacency_edges.reserve(face_count * 3);
	}

	void Collector::clear_compact()
	{
		verts.clear_and_free();
		faces.clear_and_free();

		decltype(packed_vertices) empty_packed_vertices;
		packed_vertices.swap(empty_packed_vertices);

		packed_next.clear_and_free();
		adjacency_edges.clear_and_free();
		packed_epsilon = 0.f;
		packed_inverse_epsilon = 0.f;
	}

	u32 Collector::VPack(const Fvector& V, float eps)
	{
		ensure_vertex_cache(eps);
		const vertex_cell base_cell = make_vertex_cell(V);
		u32 best_index = u32(-1);

		// Similar vertices can only reside in the current quantized cell or one
		// of its 26 neighbours. Select the smallest source index to preserve the
		// old linear-search result exactly when several vertices match.
		for (s32 z = -1; z <= 1; ++z)
		{
			for (s32 y = -1; y <= 1; ++y)
			{
				for (s32 x = -1; x <= 1; ++x)
				{
					const vertex_cell candidate_cell{base_cell.x + x, base_cell.y + y, base_cell.z + z};
					const auto candidate = packed_vertices.find(candidate_cell);
					if (candidate == packed_vertices.end())
						continue;

					for (u32 index = candidate->second; index != u32(-1); index = packed_next[index])
					{
						if (index < best_index && verts[index].similar(V, eps))
							best_index = index;
					}
				}
			}
		}

		if (best_index != u32(-1))
			return best_index;

		const u32 index = static_cast<u32>(verts.size());
		verts.push_back(V);
		packed_next.push_back(u32(-1));

		auto head = packed_vertices.find(base_cell);
		if (head == packed_vertices.end())
			packed_vertices.emplace(base_cell, index);
		else
		{
			packed_next[index] = head->second;
			head->second = index;
		}
		return index;
	}

	void Collector::add_face_D(
		const Fvector& v0, const Fvector& v1, const Fvector& v2, // vertices
		u32 dummy // misc
	)
	{
		invalidate_vertex_cache();
		TRI T;
		T.verts[0] = verts.size();
		T.verts[1] = verts.size() + 1;
		T.verts[2] = verts.size() + 2;
		T.dummy = dummy;

		verts.push_back(v0);
		verts.push_back(v1);
		verts.push_back(v2);
		faces.push_back(T);
	}

	void Collector::add_face(const Fvector& v0, const Fvector& v1, const Fvector& v2, u16 material, u16 sector)
	{
		invalidate_vertex_cache();
		TRI T;
		T.verts[0] = verts.size();
		T.verts[1] = verts.size() + 1;
		T.verts[2] = verts.size() + 2;
		T.material = material;
		T.sector = sector;

		verts.push_back(v0);
		verts.push_back(v1);
		verts.push_back(v2);
		faces.push_back(T);
	}

	void Collector::add_face_packed(
		const Fvector& v0, const Fvector& v1, const Fvector& v2, // vertices
		u16 material, u16 sector, // misc
		float eps
	)
	{
		TRI T;
		T.verts[0] = VPack(v0, eps);
		T.verts[1] = VPack(v1, eps);
		T.verts[2] = VPack(v2, eps);
		T.material = material;
		T.sector = sector;
		faces.push_back(T);
	}

	void Collector::add_face_packed_D(
		const Fvector& v0, const Fvector& v1, const Fvector& v2, // vertices
		u32 dummy, float eps
	)
	{
		TRI T;
		T.verts[0] = VPack(v0, eps);
		T.verts[1] = VPack(v1, eps);
		T.verts[2] = VPack(v2, eps);
		T.dummy = dummy;
		faces.push_back(T);
	}

	void Collector::calc_adjacency(xr_vector<u32>& dest)
	{
		VERIFY(faces.size() < 65536);
		const u32 edge_count = static_cast<u32>(faces.size() * 3);
		if (!edge_count)
		{
			dest.clear_not_free();
			adjacency_edges.clear_not_free();
			return;
		}

		// Reuse the collector-owned edge array. Wallmark/HOM adjacency is rebuilt
		// frequently, so allocating and freeing this temporary block per query is
		// avoidable allocator traffic.
		adjacency_edges.clear_not_free();
		adjacency_edges.resize(edge_count);
		adjacency_edge* edges = &*adjacency_edges.begin();
		adjacency_edge* edge_it = edges;

		xr_vector<TRI>::const_iterator begin = faces.begin();
		for (xr_vector<TRI>::const_iterator face = begin, face_end = faces.end(); face != face_end; ++face)
		{
			const u32 face_id = static_cast<u32>(face - begin);
			for (u32 edge_id = 0; edge_id < 3; ++edge_id, ++edge_it)
			{
				VERIFY(edge_it < edges + edge_count);
				edge_it->face_id = face_id;
				edge_it->edge_id = edge_id;
				edge_it->vertex_id0 = static_cast<u16>(face->verts[edge_id]);
				edge_it->vertex_id1 = static_cast<u16>(face->verts[(edge_id + 1) % 3]);
				if (edge_it->vertex_id0 > edge_it->vertex_id1)
					std::swap(edge_it->vertex_id0, edge_it->vertex_id1);
			}
		}

		std::sort(adjacency_edges.begin(), adjacency_edges.end(),
			[](const adjacency_edge& left, const adjacency_edge& right)
			{
				if (left.vertex_id0 != right.vertex_id0)
					return left.vertex_id0 < right.vertex_id0;
				if (left.vertex_id1 != right.vertex_id1)
					return left.vertex_id1 < right.vertex_id1;
				return left.face_id < right.face_id;
			});

		dest.assign(edge_count, u32(-1));
		for (u32 index = 0; index + 1 < edge_count; ++index)
		{
			const adjacency_edge& left = adjacency_edges[index];
			const adjacency_edge& right = adjacency_edges[index + 1];
			if (left.vertex_id0 != right.vertex_id0 || left.vertex_id1 != right.vertex_id1)
				continue;

			dest[left.face_id * 3 + left.edge_id] = right.face_id;
			dest[right.face_id * 3 + right.edge_id] = left.face_id;
		}
	}

	IC BOOL similar(TRI& T1, TRI& T2)
	{
		if ((T1.verts[0] == T2.verts[0]) && (T1.verts[1] == T2.verts[1]) && (T1.verts[2] == T2.verts[2]) && (T1.dummy ==
			T2.dummy)) return TRUE;
		if ((T1.verts[0] == T2.verts[0]) && (T1.verts[2] == T2.verts[1]) && (T1.verts[1] == T2.verts[2]) && (T1.dummy ==
			T2.dummy)) return TRUE;
		if ((T1.verts[2] == T2.verts[0]) && (T1.verts[0] == T2.verts[1]) && (T1.verts[1] == T2.verts[2]) && (T1.dummy ==
			T2.dummy)) return TRUE;
		if ((T1.verts[2] == T2.verts[0]) && (T1.verts[1] == T2.verts[1]) && (T1.verts[0] == T2.verts[2]) && (T1.dummy ==
			T2.dummy)) return TRUE;
		if ((T1.verts[1] == T2.verts[0]) && (T1.verts[0] == T2.verts[1]) && (T1.verts[2] == T2.verts[2]) && (T1.dummy ==
			T2.dummy)) return TRUE;
		if ((T1.verts[1] == T2.verts[0]) && (T1.verts[2] == T2.verts[1]) && (T1.verts[0] == T2.verts[2]) && (T1.dummy ==
			T2.dummy)) return TRUE;
		return FALSE;
	}

	void Collector::remove_duplicate_T()
	{
		for (u32 f = 0; f < faces.size(); f++)
		{
			for (u32 t = f + 1; t < faces.size();)
			{
				if (t == f) continue;
				TRI& T1 = faces[f];
				TRI& T2 = faces[t];
				if (similar(T1, T2))
				{
					faces[t] = faces.back();
					faces.pop_back();
				}
				else
				{
					t++;
				}
			}
		}
	}

	CollectorPacked::CollectorPacked(const Fbox& bb, int apx_vertices, int apx_faces)
	{
		// Params
		VMscale.set(bb.max.x - bb.min.x, bb.max.y - bb.min.y, bb.max.z - bb.min.z);
		VMmin.set(bb.min);
		VMeps.set(VMscale.x / clpMX / 2, VMscale.y / clpMY / 2, VMscale.z / clpMZ / 2);
		VMeps.x = (VMeps.x < EPS_L) ? VMeps.x : EPS_L;
		VMeps.y = (VMeps.y < EPS_L) ? VMeps.y : EPS_L;
		VMeps.z = (VMeps.z < EPS_L) ? VMeps.z : EPS_L;

		// Preallocate memory
		verts.reserve(apx_vertices);
		faces.reserve(apx_faces);
		flags.reserve(apx_faces);
		int _size = (clpMX + 1) * (clpMY + 1) * (clpMZ + 1);
		int _average = (apx_vertices / _size) / 2;
		for (int ix = 0; ix < clpMX + 1; ix++)
			for (int iy = 0; iy < clpMY + 1; iy++)
				for (int iz = 0; iz < clpMZ + 1; iz++)
					VM[ix][iy][iz].reserve(_average);
	}

	void CollectorPacked::add_face(
		const Fvector& v0, const Fvector& v1, const Fvector& v2, // vertices
		u16 material, u16 sector, u32 _flags // misc
	)
	{
		TRI T;
		T.verts[0] = VPack(v0);
		T.verts[1] = VPack(v1);
		T.verts[2] = VPack(v2);
		T.material = material;
		T.sector = sector;
		flags.push_back(_flags);
		faces.push_back(T);
	}

	void CollectorPacked::add_face_D(
		const Fvector& v0, const Fvector& v1, const Fvector& v2, // vertices
		u32 dummy, u32 _flags // misc
	)
	{
		TRI T;
		T.verts[0] = VPack(v0);
		T.verts[1] = VPack(v1);
		T.verts[2] = VPack(v2);
		T.dummy = dummy;
		faces.push_back(T);
		flags.push_back(_flags);
	}

	u32 CollectorPacked::VPack(const Fvector& V)
	{
		u32 P = 0xffffffff;

		u32 ix, iy, iz;
		ix = iFloor(float(V.x - VMmin.x) / VMscale.x * clpMX);
		iy = iFloor(float(V.y - VMmin.y) / VMscale.y * clpMY);
		iz = iFloor(float(V.z - VMmin.z) / VMscale.z * clpMZ);

		//		R_ASSERT(ix<=clpMX && iy<=clpMY && iz<=clpMZ);
		clamp(ix, (u32)0, clpMX);
		clamp(iy, (u32)0, clpMY);
		clamp(iz, (u32)0, clpMZ);

		{
			DWORDList* vl;
			vl = &(VM[ix][iy][iz]);
			for (DWORDIt it = vl->begin(); it != vl->end(); it++)
				if (verts[*it].similar(V))
				{
					P = *it;
					break;
				}
		}
		if (0xffffffff == P)
		{
			P = verts.size();
			verts.push_back(V);

			VM[ix][iy][iz].push_back(P);

			u32 ixE, iyE, izE;
			ixE = iFloor(float(V.x + VMeps.x - VMmin.x) / VMscale.x * clpMX);
			iyE = iFloor(float(V.y + VMeps.y - VMmin.y) / VMscale.y * clpMY);
			izE = iFloor(float(V.z + VMeps.z - VMmin.z) / VMscale.z * clpMZ);

			//			R_ASSERT(ixE<=clpMX && iyE<=clpMY && izE<=clpMZ);
			clamp(ixE, (u32)0, clpMX);
			clamp(iyE, (u32)0, clpMY);
			clamp(izE, (u32)0, clpMZ);

			if (ixE != ix) VM[ixE][iy][iz].push_back(P);
			if (iyE != iy) VM[ix][iyE][iz].push_back(P);
			if (izE != iz) VM[ix][iy][izE].push_back(P);
			if ((ixE != ix) && (iyE != iy)) VM[ixE][iyE][iz].push_back(P);
			if ((ixE != ix) && (izE != iz)) VM[ixE][iy][izE].push_back(P);
			if ((iyE != iy) && (izE != iz)) VM[ix][iyE][izE].push_back(P);
			if ((ixE != ix) && (iyE != iy) && (izE != iz)) VM[ixE][iyE][izE].push_back(P);
		}
		return P;
	}

	void CollectorPacked::clear()
	{
		verts.clear_and_free();
		faces.clear_and_free();
		flags.clear_and_free();
		for (u32 _x = 0; _x <= clpMX; _x++)
			for (u32 _y = 0; _y <= clpMY; _y++)
				for (u32 _z = 0; _z <= clpMZ; _z++)
					VM[_x][_y][_z].clear_and_free();
	}
};
