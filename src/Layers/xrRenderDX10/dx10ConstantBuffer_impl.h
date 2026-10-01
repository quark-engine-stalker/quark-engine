#ifndef	dx10ConstantBuffer_impl_included
#define	dx10ConstantBuffer_impl_included
#pragma once

#include <immintrin.h>

namespace dx10_constant_buffer_detail
{
ICF bool equal_16_bytes(const BYTE* left, const BYTE* right)
{
	const __m128i difference = _mm_xor_si128(
		_mm_loadu_si128(reinterpret_cast<const __m128i*>(left)),
		_mm_loadu_si128(reinterpret_cast<const __m128i*>(right)));
	return _mm_testz_si128(difference, difference) != 0;
}

#if defined(__AVX2__) || defined(_M_AVX2)
ICF bool equal_32_bytes(const BYTE* left, const BYTE* right)
{
	const __m256i difference = _mm256_xor_si256(
		_mm256_loadu_si256(reinterpret_cast<const __m256i*>(left)),
		_mm256_loadu_si256(reinterpret_cast<const __m256i*>(right)));
	return _mm256_testz_si256(difference, difference) != 0;
}
#else
ICF bool equal_32_bytes(const BYTE* left, const BYTE* right)
{
	return equal_16_bytes(left, right) && equal_16_bytes(left + 16, right + 16);
}
#endif

template <typename T>
ICF T load_unaligned(const BYTE* source)
{
	T value;
	CopyMemory(&value, source, sizeof(value));
	return value;
}

ICF bool equal_data(const BYTE* left, const BYTE* right, u32 size)
{
	if (left == right)
		return true;

	// Shader constants overwhelmingly use these fixed widths. Keeping them
	// inline avoids a CRT memcmp call for every scalar, vector and matrix set.
	switch (size)
	{
	case 4:
		return load_unaligned<u32>(left) == load_unaligned<u32>(right);
	case 8:
		return load_unaligned<u64>(left) == load_unaligned<u64>(right);
	case 12:
		return load_unaligned<u64>(left) == load_unaligned<u64>(right) &&
			load_unaligned<u32>(left + 8) == load_unaligned<u32>(right + 8);
	case 16:
		return equal_16_bytes(left, right);
	case 32:
		return equal_32_bytes(left, right);
	case 48:
		return equal_32_bytes(left, right) && equal_16_bytes(left + 32, right + 32);
	case 64:
		return equal_32_bytes(left, right) && equal_32_bytes(left + 32, right + 32);
	default:
		return memcmp(left, right, size) == 0;
	}
}
} // namespace dx10_constant_buffer_detail

IC Fvector4* dx10ConstantBuffer::Access(u16 offset)
{
	//	Check buffer size in client code: don't know if actual data will cross
	//	buffer boundaries.
	VERIFY(offset<(int)m_uiBufferSize);
	BYTE* res = ((BYTE*)m_pBufferData) + offset;
	return (Fvector4*)res;
}

ICF bool dx10ConstantBuffer::UpdateData(u16 offset, const void* data, u32 dataSize)
{
	VERIFY(static_cast<u32>(offset) + dataSize <= m_uiBufferSize);
	BYTE* const destination = static_cast<BYTE*>(m_pBufferData) + offset;
	if (dx10_constant_buffer_detail::equal_data(
		destination, static_cast<const BYTE*>(data), dataSize))
		return false;

	CopyMemory(destination, data, dataSize);
	m_bChanged = true;
	return true;
}

IC bool dx10ConstantBuffer::SetData(R_constant_load& L, u32 relativeOffset, const void* data, u32 dataSize)
{
	const u32 offset = static_cast<u32>(L.index) + relativeOffset;
	if (!data || !dataSize || offset > u16(-1) || offset > m_uiBufferSize || dataSize > m_uiBufferSize - offset)
		return false;

	return UpdateData(static_cast<u16>(offset), data, dataSize);
}

IC bool dx10ConstantBuffer::set(R_constant* C, R_constant_load& L, const Fmatrix& A)
{
	VERIFY(RC_float == C->type);
	Fvector4 data[4];
	u32 lineCount = 0;
	switch (L.cls)
	{
	case RC_2x4:
		lineCount = 2;
		data[0].set(A._11, A._21, A._31, A._41);
		data[1].set(A._12, A._22, A._32, A._42);
		break;
	case RC_3x4:
		lineCount = 3;
		data[0].set(A._11, A._21, A._31, A._41);
		data[1].set(A._12, A._22, A._32, A._42);
		data[2].set(A._13, A._23, A._33, A._43);
		break;
	case RC_4x4:
		lineCount = 4;
		data[0].set(A._11, A._21, A._31, A._41);
		data[1].set(A._12, A._22, A._32, A._42);
		data[2].set(A._13, A._23, A._33, A._43);
		data[3].set(A._14, A._24, A._34, A._44);
		break;
	default:
#ifdef DEBUG
		Debug.fatal		(DEBUG_INFO,"Invalid constant run-time-type for '%s'",*C->name);
#else
		NODEFAULT;
#endif
	}
	return UpdateData(L.index, data, lineCount * lineSize);
}

IC bool dx10ConstantBuffer::set(R_constant* C, R_constant_load& L, const Fvector4& A)
{
	VERIFY(RC_float == C->type);
	VERIFY(RC_1x4 == L.cls || RC_1x3 == L.cls || RC_1x2 == L.cls);
	u32 componentCount = 0;
	switch (L.cls)
	{
	case RC_1x2:
		componentCount = 2;
		break;
	case RC_1x3:
		componentCount = 3;
		break;
	case RC_1x4:
		componentCount = 4;
		break;
	default:
		NODEFAULT;
	}
	return UpdateData(L.index, &A.x, componentCount * sizeof(float));
}

IC bool dx10ConstantBuffer::set(R_constant* C, R_constant_load& L, float A)
{
	VERIFY(RC_float == C->type);
	VERIFY(RC_1x1 == L.cls);
	return UpdateData(L.index, &A, sizeof(A));
}

IC bool dx10ConstantBuffer::set(R_constant* C, R_constant_load& L, int A)
{
	VERIFY(RC_int == C->type);
	VERIFY(RC_1x1 == L.cls);
	return UpdateData(L.index, &A, sizeof(A));
}

IC bool dx10ConstantBuffer::seta(R_constant* C, R_constant_load& L, u32 e, const Fmatrix& A)
{
	VERIFY(RC_float == C->type);
	u32 base;
	Fvector4 data[4];
	u32 lineCount = 0;
	switch (L.cls)
	{
	case RC_2x4:
		base = (u32)L.index + 2 * lineSize * e;
		lineCount = 2;
		data[0].set(A._11, A._21, A._31, A._41);
		data[1].set(A._12, A._22, A._32, A._42);
		break;
	case RC_3x4:
		base = (u32)L.index + 3 * lineSize * e;
		lineCount = 3;
		data[0].set(A._11, A._21, A._31, A._41);
		data[1].set(A._12, A._22, A._32, A._42);
		data[2].set(A._13, A._23, A._33, A._43);
		break;
	case RC_4x4:
		base = (u32)L.index + 4 * lineSize * e;
		lineCount = 4;
		data[0].set(A._11, A._21, A._31, A._41);
		data[1].set(A._12, A._22, A._32, A._42);
		data[2].set(A._13, A._23, A._33, A._43);
		data[3].set(A._14, A._24, A._34, A._44);
		break;
	default:
#ifdef DEBUG
		Debug.fatal		(DEBUG_INFO,"Invalid constant run-time-type for '%s'",*C->name);
#else
		NODEFAULT;
#endif
	}
	VERIFY(base <= u16(-1));
	return UpdateData(static_cast<u16>(base), data, lineCount * lineSize);
}

IC bool dx10ConstantBuffer::seta(R_constant* C, R_constant_load& L, u32 e, const Fvector4& A)
{
	VERIFY(RC_float == C->type);
	VERIFY(RC_1x4 == L.cls || RC_1x3 == L.cls || RC_1x2 == L.cls);

	u32 base = (u32)L.index + lineSize * e;
	VERIFY(base <= u16(-1));
	return UpdateData(static_cast<u16>(base), &A, lineSize);
}

IC void* dx10ConstantBuffer::AccessDirect(R_constant_load& L, u32 DataSize)
{
	//	Check buffer size in client code: don't know if actual data will cross
	//	buffer boundaries.
	VERIFY(L.index<(int)m_uiBufferSize);
	BYTE* res = ((BYTE*)m_pBufferData) + L.index;

	if ((u32)L.index + DataSize <= m_uiBufferSize)
	{
		m_bChanged = true;
		return res;
	}
	else return 0;
}

#endif	//	dx10ConstantBuffer_impl_included
