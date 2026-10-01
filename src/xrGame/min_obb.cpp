#include "stdafx.h"
#include "magic_box3.h"
#include "magic_minimize_nd.h"
#include <immintrin.h>
#include <cfloat>
#include <cstddef>

class PointArray
{
public:
	PointArray(int iQuantity, const Fvector* akPoint)
		:
		m_akPoint(akPoint)
	{
		m_iQuantity = iQuantity;
	}

	int m_iQuantity;
	const Fvector* m_akPoint;
};

namespace
{
static_assert(offsetof(Fvector, x) == 0, "AVX2 point gather expects x first");
static_assert(offsetof(Fvector, y) == sizeof(float), "AVX2 point gather expects packed x/y");
static_assert(offsetof(Fvector, z) == sizeof(float) * 2, "AVX2 point gather expects packed x/y/z");
static_assert((sizeof(Fvector) % sizeof(float)) == 0, "Fvector stride must be float-addressable");

IC __m256 transform_component(__m256 x, __m256 y, __m256 z,
	const __m256 mx, const __m256 my, const __m256 mz, const __m256 translation)
{
	__m256 result = _mm256_mul_ps(x, mx);
	result = _mm256_add_ps(result, _mm256_mul_ps(y, my));
	result = _mm256_add_ps(result, _mm256_mul_ps(z, mz));
	return _mm256_add_ps(result, translation);
}

IC float horizontal_min8(const __m256 value)
{
	__m128 reduced = _mm_min_ps(_mm256_castps256_ps128(value), _mm256_extractf128_ps(value, 1));
	reduced = _mm_min_ps(reduced, _mm_shuffle_ps(reduced, reduced, _MM_SHUFFLE(2, 3, 0, 1)));
	reduced = _mm_min_ss(reduced, _mm_movehl_ps(reduced, reduced));
	return _mm_cvtss_f32(reduced);
}

IC float horizontal_max8(const __m256 value)
{
	__m128 reduced = _mm_max_ps(_mm256_castps256_ps128(value), _mm256_extractf128_ps(value, 1));
	reduced = _mm_max_ps(reduced, _mm_shuffle_ps(reduced, reduced, _MM_SHUFFLE(2, 3, 0, 1)));
	reduced = _mm_max_ss(reduced, _mm_movehl_ps(reduced, reduced));
	return _mm_cvtss_f32(reduced);
}

void transformed_bounds(const int quantity, const Fvector* points, const Fmatrix& matrix,
	Fvector& minimum, Fvector& maximum)
{
	VERIFY(quantity > 0);

	if (quantity < 16)
	{
		matrix.transform_tiny(minimum, points[0]);
		maximum = minimum;
		for (int i = 1; i < quantity; ++i)
		{
			Fvector transformed;
			matrix.transform_tiny(transformed, points[i]);
			minimum.x = _min(minimum.x, transformed.x);
			minimum.y = _min(minimum.y, transformed.y);
			minimum.z = _min(minimum.z, transformed.z);
			maximum.x = _max(maximum.x, transformed.x);
			maximum.y = _max(maximum.y, transformed.y);
			maximum.z = _max(maximum.z, transformed.z);
		}
		return;
	}

	constexpr int point_stride = sizeof(Fvector) / sizeof(float);
	const __m256i gather_indices = _mm256_setr_epi32(
		0 * point_stride, 1 * point_stride, 2 * point_stride, 3 * point_stride,
		4 * point_stride, 5 * point_stride, 6 * point_stride, 7 * point_stride);

	const __m256 m11 = _mm256_set1_ps(matrix._11);
	const __m256 m21 = _mm256_set1_ps(matrix._21);
	const __m256 m31 = _mm256_set1_ps(matrix._31);
	const __m256 m41 = _mm256_set1_ps(matrix._41);
	const __m256 m12 = _mm256_set1_ps(matrix._12);
	const __m256 m22 = _mm256_set1_ps(matrix._22);
	const __m256 m32 = _mm256_set1_ps(matrix._32);
	const __m256 m42 = _mm256_set1_ps(matrix._42);
	const __m256 m13 = _mm256_set1_ps(matrix._13);
	const __m256 m23 = _mm256_set1_ps(matrix._23);
	const __m256 m33 = _mm256_set1_ps(matrix._33);
	const __m256 m43 = _mm256_set1_ps(matrix._43);

	__m256 min_x = _mm256_set1_ps(FLT_MAX);
	__m256 min_y = min_x;
	__m256 min_z = min_x;
	__m256 max_x = _mm256_set1_ps(-FLT_MAX);
	__m256 max_y = max_x;
	__m256 max_z = max_x;

	int i = 0;
	for (; i + 8 <= quantity; i += 8)
	{
		const float* base = &points[i].x;
		const __m256 x = _mm256_i32gather_ps(base, gather_indices, sizeof(float));
		const __m256 y = _mm256_i32gather_ps(base + 1, gather_indices, sizeof(float));
		const __m256 z = _mm256_i32gather_ps(base + 2, gather_indices, sizeof(float));
		const __m256 tx = transform_component(x, y, z, m11, m21, m31, m41);
		const __m256 ty = transform_component(x, y, z, m12, m22, m32, m42);
		const __m256 tz = transform_component(x, y, z, m13, m23, m33, m43);
		min_x = _mm256_min_ps(min_x, tx);
		min_y = _mm256_min_ps(min_y, ty);
		min_z = _mm256_min_ps(min_z, tz);
		max_x = _mm256_max_ps(max_x, tx);
		max_y = _mm256_max_ps(max_y, ty);
		max_z = _mm256_max_ps(max_z, tz);
	}

	minimum.set(horizontal_min8(min_x), horizontal_min8(min_y), horizontal_min8(min_z));
	maximum.set(horizontal_max8(max_x), horizontal_max8(max_y), horizontal_max8(max_z));

	for (; i < quantity; ++i)
	{
		Fvector transformed;
		matrix.transform_tiny(transformed, points[i]);
		minimum.x = _min(minimum.x, transformed.x);
		minimum.y = _min(minimum.y, transformed.y);
		minimum.z = _min(minimum.z, transformed.z);
		maximum.x = _max(maximum.x, transformed.x);
		maximum.y = _max(maximum.y, transformed.y);
		maximum.z = _max(maximum.z, transformed.z);
	}
	_mm256_zeroupper();
}
}

static void FromAxisAngle(Fmatrix& self, const Fvector& rkAxis, float fRadians)
{
	float fCos = _cos(fRadians);
	float fSin = _sin(fRadians);
	float fOneMinusCos = 1.0f - fCos;
	float fX2 = rkAxis.x * rkAxis.x;
	float fY2 = rkAxis.y * rkAxis.y;
	float fZ2 = rkAxis.z * rkAxis.z;
	float fXYM = rkAxis.x * rkAxis.y * fOneMinusCos;
	float fXZM = rkAxis.x * rkAxis.z * fOneMinusCos;
	float fYZM = rkAxis.y * rkAxis.z * fOneMinusCos;
	float fXSin = rkAxis.x * fSin;
	float fYSin = rkAxis.y * fSin;
	float fZSin = rkAxis.z * fSin;

	self.identity();
	self._11 = fX2 * fOneMinusCos + fCos;
	self._12 = fXYM - fZSin;
	self._13 = fXZM + fYSin;
	self._21 = fXYM + fZSin;
	self._22 = fY2 * fOneMinusCos + fCos;
	self._23 = fYZM - fXSin;
	self._31 = fXZM - fYSin;
	self._32 = fYZM + fXSin;
	self._33 = fZ2 * fOneMinusCos + fCos;
}

static Fvector GetColumn(Fmatrix& self, const u32& index)
{
	switch (index)
	{
	case 0: return (Fvector().set(self._11, self._21, self._31));
	case 1: return (Fvector().set(self._12, self._22, self._32));
	case 2: return (Fvector().set(self._13, self._23, self._33));
	default: NODEFAULT;
	}
#ifdef DEBUG
	return	(Fvector().set(flt_max,flt_max,flt_max));
#endif // DEBUG
}

//----------------------------------------------------------------------------
static float Volume(const float* afAngle, void* pvUserData)
{
	int iQuantity = ((PointArray*)pvUserData)->m_iQuantity;
	const Fvector* akPoint = ((PointArray*)pvUserData)->m_akPoint;

	float fCos0 = _cos(afAngle[0]);
	float fSin0 = _sin(afAngle[0]);
	float fCos1 = _cos(afAngle[1]);
	float fSin1 = _sin(afAngle[1]);
	Fvector kAxis = Fvector().set(fCos0 * fSin1, fSin0 * fSin1, fCos1);
	Fmatrix kRot;
	FromAxisAngle(kRot, kAxis, afAngle[2]);

	Fvector kMin;
	Fvector kMax;
	transformed_bounds(iQuantity, akPoint, kRot, kMin, kMax);

	float fVolume = (kMax.x - kMin.x) * (kMax.y - kMin.y) * (kMax.z - kMin.z);
	return fVolume;
}

//----------------------------------------------------------------------------
static void MinimalBoxForAngles(int iQuantity, const Fvector* akPoint,
                                float afAngle[3], MagicBox3& rkBox)
{
	float fCos0 = _cos(afAngle[0]);
	float fSin0 = _sin(afAngle[0]);
	float fCos1 = _cos(afAngle[1]);
	float fSin1 = _sin(afAngle[1]);
	Fvector kAxis = Fvector().set(fCos0 * fSin1, fSin0 * fSin1, fCos1);
	Fmatrix kRot;
	FromAxisAngle(kRot, kAxis, afAngle[2]);

	Fvector kMin;
	Fvector kMax;
	transformed_bounds(iQuantity, akPoint, kRot, kMin, kMax);

	Fvector kMid = Fvector().add(kMax, kMin).mul(0.5f);
	Fvector kRng = Fvector().sub(kMax, kMin).mul(0.5f);

	kRot.transform_tiny(rkBox.Center(), kMid);
	rkBox.Axis(0) = GetColumn(kRot, 0);
	rkBox.Axis(1) = GetColumn(kRot, 1);
	rkBox.Axis(2) = GetColumn(kRot, 2);
	rkBox.Extent(0) = kRng.x;
	rkBox.Extent(1) = kRng.y;
	rkBox.Extent(2) = kRng.z;
}

//----------------------------------------------------------------------------
MagicBox3 MagicMinBox(int iQuantity, const Fvector* akPoint)
{
	int iMaxLevel = 8;
	int iMaxBracket = 8;
	int iMaxIterations = 32;
	PointArray kPA(iQuantity, akPoint);
	MinimizeND<3> kMinimizer(Volume, iMaxLevel, iMaxBracket, iMaxIterations, &kPA);

	float afA0[3] =
	{
		0.0f,
		0.0f,
		0.0f
	};

	float afA1[3] =
	{
		PI,
		PI_DIV_2,
		PI
	};

	// compute some samples to narrow down the search region
	float fMinVolume = flt_max;
	float afAngle[3], afAInitial[3];
	const int iMax = 3;
	for (int i0 = 0; i0 <= iMax; i0++)
	{
		afAngle[0] = afA0[0] + i0 * (afA1[0] - afA0[0]) / iMax;
		for (int i1 = 0; i1 <= iMax; i1++)
		{
			afAngle[1] = afA0[1] + i1 * (afA1[1] - afA0[1]) / iMax;
			for (int i2 = 0; i2 <= iMax; i2++)
			{
				afAngle[2] = afA0[2] + i2 * (afA1[2] - afA0[2]) / iMax;
				float fVolume = Volume(afAngle, &kPA);
				if (fVolume < fMinVolume)
				{
					fMinVolume = fVolume;
					afAInitial[0] = afAngle[0];
					afAInitial[1] = afAngle[1];
					afAInitial[2] = afAngle[2];
				}
			}
		}
	}

	float afAMin[3], fVMin;
	kMinimizer.GetMinimum(afA0, afA1, afAInitial, afAMin, fVMin);

	MagicBox3 kBox;
	MinimalBoxForAngles(iQuantity, akPoint, afAMin, kBox);
	return kBox;
}

//----------------------------------------------------------------------------
