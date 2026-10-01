/*
  This source code is a part of IKAN.
  Copyright (c) 2000 University of Pennsylvania
  Center for Human Modeling and Simulation
  All Rights Reserved.

  IN NO EVENT SHALL THE UNIVERSITY OF PENNSYLVANIA BE LIABLE TO ANY
  PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL
  DAMAGES, INCLUDING LOST PROFITS, ARISING OUT OF THE USE OF THIS
  SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF PENNSYLVANIA
  HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.

  Permission to use, copy, modify and distribute this software and its
  documentation for educational, research and non-profit purposes,
  without fee, and without a written agreement is hereby granted,
  provided that the above copyright notice and the following three
  paragraphs appear in all copies. For for-profit purposes, please
  contact University of Pennsylvania
 (http://hms.upenn.edu/software/ik/ik.html) for the software license
  agreement.


  THE UNIVERSITY OF PENNSYLVANIA SPECIFICALLY DISCLAIM ANY
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
  PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
  BASIS, AND THE UNIVERSITY OF PENNSYLVANIA HAS NO OBLIGATION
  TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
  MODIFICATIONS.

 */
#include "StdAfx.h"
#include "math3d.h"
#include <immintrin.h>

namespace
{
IC __m256 matrix_row_pair_coeff(const float low, const float high)
{
	return _mm256_setr_ps(low, low, low, low, high, high, high, high);
}

IC __m256 matrix_broadcast_row(const float* row)
{
	return _mm256_broadcast_ps(reinterpret_cast<const __m128*>(row));
}

IC __m256 matrix_mul_pair4(const float* low_row, const float* high_row, const Matrix right)
{
	__m256 result = _mm256_mul_ps(matrix_row_pair_coeff(low_row[0], high_row[0]), matrix_broadcast_row(right[0]));
	result = _mm256_add_ps(result, _mm256_mul_ps(matrix_row_pair_coeff(low_row[1], high_row[1]), matrix_broadcast_row(right[1])));
	result = _mm256_add_ps(result, _mm256_mul_ps(matrix_row_pair_coeff(low_row[2], high_row[2]), matrix_broadcast_row(right[2])));
	result = _mm256_add_ps(result, _mm256_mul_ps(matrix_row_pair_coeff(low_row[3], high_row[3]), matrix_broadcast_row(right[3])));
	return result;
}

IC __m256 matrix_mul_pair3(const float* low_row, const float* high_row, const Matrix right)
{
	__m256 result = _mm256_mul_ps(matrix_row_pair_coeff(low_row[0], high_row[0]), matrix_broadcast_row(right[0]));
	result = _mm256_add_ps(result, _mm256_mul_ps(matrix_row_pair_coeff(low_row[1], high_row[1]), matrix_broadcast_row(right[1])));
	result = _mm256_add_ps(result, _mm256_mul_ps(matrix_row_pair_coeff(low_row[2], high_row[2]), matrix_broadcast_row(right[2])));
	return result;
}

IC __m256 clear_homogeneous_w(__m256 value)
{
	const __m256 mask = _mm256_castsi256_ps(_mm256_setr_epi32(-1, -1, -1, 0, -1, -1, -1, 0));
	return _mm256_and_ps(value, mask);
}
}

#ifndef JACK

Matrix idmat =
{
	{1.0, 0.0, 0.0, 0.0},
	{0.0, 1.0, 0.0, 0.0},
	{0.0, 0.0, 1.0, 0.0},
	{0.0, 0.0, 0.0, 1.0}
};


void
matmult(Matrix A, Matrix B, Matrix C)
/*
 * Multiply two arbitrary 4x4 matrices.  Two destination rows are evaluated
 * together so every right-hand row load feeds both independent dot-product
 * chains.  The temporary keeps the documented A == B / A == C behaviour.
 */
{
	Matrix result;
	_mm256_storeu_ps(&result[0][0], matrix_mul_pair4(B[0], B[1], C));
	_mm256_storeu_ps(&result[2][0], matrix_mul_pair4(B[2], B[3], C));
	_mm256_zeroupper();
	CopyMemory(A, result, sizeof(result));
}

void hmatmult(Matrix A, Matrix B, Matrix C)
/*
 * Homogeneous transform multiplication.  The upper three rows omit the
 * known-zero fourth column, while the translation row adds C translation
 * directly.  A local result preserves aliasing support.
 */
{
	Matrix result;
	const __m256 upper = clear_homogeneous_w(matrix_mul_pair3(B[0], B[1], C));
	_mm256_storeu_ps(&result[0][0], upper);

	__m256 lower = clear_homogeneous_w(matrix_mul_pair3(B[2], B[3], C));
	const __m128 translation = _mm_setr_ps(C[3][0], C[3][1], C[3][2], 1.f);
	lower = _mm256_insertf128_ps(lower, _mm_add_ps(_mm256_extractf128_ps(lower, 1), translation), 1);
	_mm256_storeu_ps(&result[2][0], lower);

	_mm256_zeroupper();
	CopyMemory(A, result, sizeof(result));
}

void inverthomomatrix(Matrix N, Matrix M)
/* Invert a rotation + translation homogeneous transform. */
{
	const __m256 rows01 = _mm256_loadu_ps(&M[0][0]);
	const __m256 rows23 = _mm256_loadu_ps(&M[2][0]);
	__m128 r0 = _mm256_castps256_ps128(rows01);
	__m128 r1 = _mm256_extractf128_ps(rows01, 1);
	__m128 r2 = _mm256_castps256_ps128(rows23);
	__m128 r3 = _mm256_extractf128_ps(rows23, 1);
	_MM_TRANSPOSE4_PS(r0, r1, r2, r3);

	const float tx = -(M[3][0] * M[0][0] + M[3][1] * M[0][1] + M[3][2] * M[0][2]);
	const float ty = -(M[3][0] * M[1][0] + M[3][1] * M[1][1] + M[3][2] * M[1][2]);
	const float tz = -(M[3][0] * M[2][0] + M[3][1] * M[2][1] + M[3][2] * M[2][2]);
	const __m128 xyz_mask = _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0));
	_mm_storeu_ps(&N[0][0], _mm_and_ps(r0, xyz_mask));
	_mm_storeu_ps(&N[1][0], _mm_and_ps(r1, xyz_mask));
	_mm_storeu_ps(&N[2][0], _mm_and_ps(r2, xyz_mask));
	N[3][0] = tx;
	N[3][1] = ty;
	N[3][2] = tz;
	N[3][3] = 1.f;
	_mm256_zeroupper();
}

void vecmult0(float y[], float x[], Matrix M)
/* Direction transform: y = x * M, with y.w = 0. */
{
	const float x0 = x[0];
	const float x1 = x[1];
	const float x2 = x[2];
	__m128 result = _mm_mul_ps(_mm_set1_ps(x0), _mm_loadu_ps(M[0]));
	result = _mm_add_ps(result, _mm_mul_ps(_mm_set1_ps(x1), _mm_loadu_ps(M[1])));
	result = _mm_add_ps(result, _mm_mul_ps(_mm_set1_ps(x2), _mm_loadu_ps(M[2])));
	alignas(16) float transformed[4];
	_mm_store_ps(transformed, result);
	y[0] = transformed[0];
	y[1] = transformed[1];
	y[2] = transformed[2];
}

void vecmult(float y[], float x[], Matrix M)
/* Point transform: y = x * M, with y.w = 1. */
{
	const float x0 = x[0];
	const float x1 = x[1];
	const float x2 = x[2];
	__m128 result = _mm_loadu_ps(M[3]);
	result = _mm_add_ps(result, _mm_mul_ps(_mm_set1_ps(x0), _mm_loadu_ps(M[0])));
	result = _mm_add_ps(result, _mm_mul_ps(_mm_set1_ps(x1), _mm_loadu_ps(M[1])));
	result = _mm_add_ps(result, _mm_mul_ps(_mm_set1_ps(x2), _mm_loadu_ps(M[2])));
	alignas(16) float transformed[4];
	_mm_store_ps(transformed, result);
	y[0] = transformed[0];
	y[1] = transformed[1];
	y[2] = transformed[2];
}


void
axisangletomatrix(Matrix m, float axis[], float theta)
/*
 * Construct a matrix which is a rotation around 'axis' of 'theta' radians.
 * This routine is tuned for speed. It takes advantages of special cases
 * like the coordinate axes.
 */
{
	register float s, v, c;
	register float* p;
	register float a01, a02, a12, a0s, a1s, a2s, a01v, a02v, a12v;

	c = _cos(theta);
	s = _sin(theta);
	v = 1.0f - c;

	p = (float *)m;

	if (axis[0] == 0.0f && axis[1] == 0.0f)
	{
		if (axis[2] < 0)
		{
			s = -s;
		}
		/*
		 * z rotation
		 */
		*p++ = c;
		*p++ = s;
		*p++ = 0.0;
		*p++ = 0.0;

		*p++ = - s;
		*p++ = c;
		*p++ = 0.0;
		*p++ = 0.0;

		*p++ = 0.0;
		*p++ = 0.0;
		*p++ = 1.0;
	}
	else if (axis[0] == 0.0 && axis[2] == 0.0)
	{
		if (axis[1] < 0)
		{
			s = -s;
		}
		/*
		 * y rotation
		 */
		*p++ = c;
		*p++ = 0.0;
		*p++ = - s;
		*p++ = 0.0;

		*p++ = 0.0;
		*p++ = 1.0;
		*p++ = 0.0;
		*p++ = 0.0;

		*p++ = s;
		*p++ = 0.0;
		*p++ = c;
	}
	else if (axis[1] == 0.0 && axis[2] == 0.0)
	{
		if (axis[0] < 0)
		{
			s = -s;
		}
		/*
		 * x rotation
		 */
		*p++ = 1.0;
		*p++ = 0.0;
		*p++ = 0.0;
		*p++ = 0.0;

		*p++ = 0.0;
		*p++ = c;
		*p++ = s;
		*p++ = 0.0;

		*p++ = 0.0;
		*p++ = -s;
		*p++ = c;
	}
	else
	{
		a01 = axis[0] * axis[1];
		a02 = axis[0] * axis[2];
		a12 = axis[1] * axis[2];
		a0s = axis[0] * s;
		a1s = axis[1] * s;
		a2s = axis[2] * s;
		a01v = a01 * v;
		a02v = a02 * v;
		a12v = a12 * v;

		*p++ = axis[0] * axis[0] * v + c;
		*p++ = a01v + a2s;
		*p++ = a02v - a1s;
		*p++ = 0.0;

		*p++ = a01v - a2s;
		*p++ = axis[1] * axis[1] * v + c;
		*p++ = a12v + a0s;
		*p++ = 0.0;

		*p++ = a02v + a1s;
		*p++ = a12v - a0s;
		*p++ = axis[2] * axis[2] * v + c;
	}
	*p++ = 0.0;

	*p++ = 0.0;
	*p++ = 0.0;
	*p++ = 0.0;
	*p++ = 1.0;
}

#endif

//
// Construct a general rotation matrix given an angle and axis
//
void rotation_axis_to_matrix(float axis[3], float angle, Matrix R)
{
	float cos_a, sin_a;
	float s1, s2, s3;

	cos_a = _cos(angle);
	sin_a = _sin(angle);

	// Assume axis is normalized 

#if 0
	// float normal[3];
	// cpvector(normal,axis);
	// unitize(normal);

	// s1 = normal[0];
	// s2 = normal[1];
	// s3 = normal[2];
#else

	s1 = axis[0];
	s2 = axis[1];
	s3 = axis[2];

#endif
	float s1s1 = s1 * s1;
	float s1s2 = s1 * s2;
	float s1s3 = s1 * s3;
	float s2s2 = s2 * s2;
	float s2s3 = s2 * s3;
	float s3s3 = s3 * s3;
	float cos_as1s2 = cos_a * s1s2;
	float cos_as1s3 = cos_a * s1s3;
	float cos_as2s3 = cos_a * s2s3;
	float s1sin_a = s1 * sin_a;
	float s2sin_a = s2 * sin_a;
	float s3sin_a = s3 * sin_a;
	float* r = (float *)R;

	*r++ = s1s1 + cos_a * (1 - s1s1);
	*r++ = s1s2 - cos_as1s2 + s3sin_a;
	*r++ = s1s3 - cos_as1s3 - s2sin_a;
	*r++ = 0;

	*r++ = s1s2 - cos_as1s2 - s3sin_a;
	*r++ = s2s2 + cos_a * (1 - s2s2);
	*r++ = s2s3 - cos_as2s3 + s1sin_a;
	*r++ = 0;

	*r++ = s1s3 - cos_as1s3 + s2sin_a;
	*r++ = s2s3 - cos_as2s3 - s1sin_a;
	*r++ = s3s3 + cos_a * (1 - s3s3);
	*r++ = 0;

	*r++ = 0;
	*r++ = 0;
	*r++ = 0;
	*r = 1;
}

//
// p = Projection of u onto v
// 
void project(float p[3], const float u[3], const float v[3])
{
	float vnorm[3];

	cpvector(vnorm, v);
	unitize(vnorm);
	vecscalarmult(p, vnorm, DOT(u, vnorm));
}


//
// p = Projection of u onto plane whose normal is n
//
void project_plane(float p[3], float u[3], float n[3])
{
	float un[3];

	project(un, u, n);
	vecsub(p, u, un);
}


//
// Return the angle between two vectors u,v about the axis n
//
float angle_between_vectors(float u[3], float v[3], float n[3])
{
#if 0
    float temp[3]; 
    float up[3];
    float vp[3];

    cpvector(up, u);
    cpvector(vp, v);
    unitize(up);
    unitize(vp);

    crossproduct(temp, up, vp);
    float mag = DOT(temp,n);

    // Vectors are parallel at 0 or 180 
    if (mag*mag < 1e-8)
    {
	if (DOT(up,vp) < 0)
	    return M_PI;
	else
	    return 0;
    }

    int sign = (mag > 0) ? 1 : -1;
    float t = DOT(up,vp); 
    if (t > 1.0)
	t = 1.0;
    else if (t < -1.0)
	t = -1.0;
    return sign*acos(t);
#else

	float up[3];
	float vp[3];
	float uv[3];

	project_plane(up, u, n);
	project_plane(vp, v, n);
	crossproduct(uv, up, vp);
	return atan2(DOT(n, uv), DOT(up, vp));

#endif
}


//
// Print 4x4 homogeneous matrix 
//
void print_matrix(Matrix M)
{
	for (int i = 0; i < 4; i++)
	{
		for (int j = 0; j < 4; j++)
			printf(" %lf ", M[i][j]);
		printf("\n");
	}
}


//
// Print vector
//
void print_vector(float v[3])
{
	printf(" %lf %lf %lf \n", *v, v[1], v[2]);
}


//
// Find a vector n normal to v
//

void find_normal_vector(float v[3], float n[3])
{
	int num_zero;
	float min, temp;
	int min_i;

	min_i = 0;
	min = _abs(v[0]);
	num_zero = (min < 1e-8f);

	temp = _abs(v[1]);
	if (temp < 1e-8f)
		num_zero++;
	if (temp < min)
	{
		min = temp;
		min_i = 1;
	}

	temp = _abs(v[2]);
	if (temp < 1e-8)
		num_zero++;
	if (temp < min)
	{
		min = temp;
		min_i = 2;
	}

	n[0] = n[1] = n[2] = 0.0;

	switch (num_zero)
	{
	case 3:
		// Vector is zero so there is no soln
		break;
	case 2:
		// Vector has only one nonzero component. Set any of the
		// other to 1.0 and 0 to the others
		n[min_i] = 1.0;
		break;

		// Vector has at least two nonzero components
	case 1:
	default:
		if (min_i == 0)
		{
			n[1] = -v[2];
			n[2] = v[1];
		}
		else if (min_i == 1)
		{
			n[0] = -v[2];
			n[2] = v[0];
		}
		else
		{
			n[0] = -v[1];
			n[1] = v[0];
		}
		unitize(n);
		break;
	}
}


//
// Multiplies only the rotational components of B*C 
// and stores the result into A
//
void rmatmult(Matrix A, Matrix B, Matrix C)
{
	Matrix result;
	_mm256_storeu_ps(&result[0][0], clear_homogeneous_w(matrix_mul_pair3(B[0], B[1], C)));

	__m256 lower = clear_homogeneous_w(matrix_mul_pair3(B[2], B[3], C));
	lower = _mm256_insertf128_ps(lower, _mm_setr_ps(0.f, 0.f, 0.f, 1.f), 1);
	_mm256_storeu_ps(&result[2][0], lower);

	_mm256_zeroupper();
	CopyMemory(A, result, sizeof(result));
}

void invertrmatrix(Matrix N, Matrix M)
/* Invert a pure rotation matrix. */
{
	const __m256 rows01 = _mm256_loadu_ps(&M[0][0]);
	const __m256 rows23 = _mm256_loadu_ps(&M[2][0]);
	__m128 r0 = _mm256_castps256_ps128(rows01);
	__m128 r1 = _mm256_extractf128_ps(rows01, 1);
	__m128 r2 = _mm256_castps256_ps128(rows23);
	__m128 r3 = _mm256_extractf128_ps(rows23, 1);
	_MM_TRANSPOSE4_PS(r0, r1, r2, r3);

	const __m128 xyz_mask = _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0));
	_mm_storeu_ps(&N[0][0], _mm_and_ps(r0, xyz_mask));
	_mm_storeu_ps(&N[1][0], _mm_and_ps(r1, xyz_mask));
	_mm_storeu_ps(&N[2][0], _mm_and_ps(r2, xyz_mask));
	_mm_storeu_ps(&N[3][0], _mm_setr_ps(0.f, 0.f, 0.f, 1.f));
	_mm256_zeroupper();
}

//
// Generate the derivative of a rotation matrix about a principal axis
//
void rotation_principal_axis_to_deriv_matrix(char axis, float angle, Matrix m)
{
	float cos_a, sin_a;

	ZeroMemory(m, sizeof(Matrix));
	cos_a = _cos(angle);
	sin_a = _sin(angle);

	switch (axis)
	{
	case 'x':
	case 'X':
		m[1][1] = -sin_a;
		m[2][1] = -cos_a;
		m[1][2] = cos_a;
		m[2][2] = -sin_a;
		break;

	case 'y':
	case 'Y':
		m[0][0] = -sin_a;
		m[2][0] = cos_a;
		m[0][2] = -cos_a;
		m[2][2] = -sin_a;
		break;

	default:
		m[0][0] = -sin_a;
		m[1][0] = -cos_a;
		m[0][1] = cos_a;
		m[1][1] = -sin_a;
		break;
	}
}

void rotation_principal_axis_to_matrix(char axis, float angle, Matrix m)
{
	float cos_a, sin_a;

	cpmatrix(m, idmat);
	cos_a = _cos(angle);
	sin_a = _sin(angle);

	switch (axis)
	{
	case 'x':
	case 'X':
		m[1][1] = cos_a;
		m[2][1] = -sin_a;
		m[1][2] = sin_a;
		m[2][2] = cos_a;
		break;

	case 'y':
	case 'Y':
		m[0][0] = cos_a;
		m[2][0] = sin_a;
		m[0][2] = -sin_a;
		m[2][2] = cos_a;
		break;

	default:
		m[0][0] = cos_a;
		m[1][0] = -sin_a;
		m[0][1] = sin_a;
		m[1][1] = cos_a;
		break;
	}
}

//
// To extract axis and angle from R use the formulas (murray, pg 414)
//
//	2 * cos(theta) - 1 = trace(R) 
// and 
//	axis = vector associated with skew symmetric matrix (R-R')/(2*sin(theta)) 
//
//
// By our convention always return 0 <= angle < M_PI
// 
void rotation_matrix_to_axis(const Matrix R, float axis[], float& angle)
{
	const float eps = 1e-7f;

	angle = acos((R[0][0] + R[1][1] + R[2][2] - 1) / 2.0f);


	// Close to identity. Arbitrarily set solution to z axis rotation of 0 
	if (_abs(angle) < eps || _abs(angle - M_PI) < eps)
	{
		angle = 0.0;
		axis[0] = axis[1] = 0.0;
		axis[2] = 1.0;
	}
	else
	{
		axis[0] = R[1][2] - R[2][1];
		axis[1] = R[2][0] - R[0][2];
		axis[2] = R[0][1] - R[1][0];
		unitize(axis);
	}
}

void
linterpmatrix(Matrix R, Matrix A, Matrix B, float t)
/*
 * Linearly interpolate matrices.  The rotation submatrix is interpolated
 * by finding the axis of rotation between the two matrix and interpolating
 * the angle of rotation. The translation part is just interpolated.
 *
 * t=0 gives A; t=1 gives B.
 *
 * R = (1-t) * A  + t * B
 */
{
	Matrix Ainv, T;
	float angle, axis[3];
	Quaternion q;

	inverthomomatrix(Ainv, A);
	matmult(T, B, Ainv);
	matrixtoq(q, T);
	qtoaxis(&angle, axis, q);

	angle *= t;

	axistoq(q, angle, axis);
	qtomatrix(T, q);
	matmult(R, T, A);
	vecinterp(R[3], A[3], B[3], t);
}


#define ATAN2(a,b) ((a==0.0)&&(b==0.0) ? 0.0 : atan2(a,b))

#define EPSILON 0.001f
#define W q[0]
#define X q[1]
#define Y q[2]
#define Z q[3]

void
qtomatrix(Matrix m, Quaternion q)
/*
 * Convert quaterion to rotation sub-matrix of 'm'.
 * The left column of 'm' gets zeroed, and m[3][3]=1.0, but the 
 * translation part is left unmodified.
 * 
 * m = q
 */
{
	float x2 = X * X;
	float y2 = Y * Y;
	float z2 = Z * Z;

	m[0][0] = 1 - 2 * (y2 + z2);
	m[0][1] = 2 * (X * Y + W * Z);
	m[0][2] = 2 * (X * Z - W * Y);
	m[0][3] = 0.0;

	m[1][0] = 2 * (X * Y - W * Z);
	m[1][1] = 1 - 2 * (x2 + z2);
	m[1][2] = 2 * (Y * Z + W * X);
	m[1][3] = 0.0;

	m[2][0] = 2 * (X * Z + W * Y);
	m[2][1] = 2 * (Y * Z - W * X);
	m[2][2] = 1 - 2 * (x2 + y2);
	m[2][3] = 0.0;

	m[3][3] = 1.0;
}

void
matrixtoq(Quaternion q, Matrix m)
/*
 * Convert the rotation sub-matrix of `m' to a quaternion.
 *
 * q = m
 */
{
	float f;

	f = (1.0f + m[0][0] + m[1][1] + m[2][2]) / 4.0f;
	if (f > EPSILON)
	{
		W = _sqrt(f);
		X = (m[1][2] - m[2][1]) / (4 * W);
		Y = (m[2][0] - m[0][2]) / (4 * W);
		Z = (m[0][1] - m[1][0]) / (4 * W);
	}
	else
	{
		W = 0.0;
		f = - (m[1][1] + m[2][2]) / 2.0f;
		if (f > EPSILON)
		{
			X = _sqrt(f);
			Y = m[0][1] / (2 * X);
			Z = m[0][2] / (2 * X);
		}
		else
		{
			X = 0.0;
			f = (1 - m[2][2]) / 2.0f;
			if (f > EPSILON)
			{
				Y = _sqrt(f);
				Z = m[1][2] / (2 * Y);
			}
			else
			{
				Y = 0.0;
				Z = 1.0;
			}
		}
	}
	unitize4(q);
}

void
axistoq(Quaternion q, float angle, float axis[])
{
	float f;

	f = (float)_sin(angle / 2);
	q[0] = (float)_cos(angle / 2);
	q[1] = axis[0] * f;
	q[2] = axis[1] * f;
	q[3] = axis[2] * f;
}

void
qtoaxis(float* angle, float axis[], Quaternion q)
{
	float f;

	*angle = 2 * ((float)acos(q[0]));
	f = (float)_sin(*angle / 2);
	if (f > 0)
	{
		axis[0] = q[1] / f;
		axis[1] = q[2] / f;
		axis[2] = q[3] / f;
	}
	else
	{
		axis[0] = axis[1] = axis[2] = 0.0;
	}
}

void
vecinterp(float x[], float u[], float v[], float t)
/*
 * x = u + t * (v - u).
 */
{
	x[0] = u[0] + t * (v[0] - u[0]);
	x[1] = u[1] + t * (v[1] - u[1]);
	x[2] = u[2] + t * (v[2] - u[2]);
}

float
unitize4(float u[4])
/*
 * Same as unitize, except that arguments are 4-vectors.
 * Make u into a unit 4-vector.
 */
{
	float f;

	f = (float)_sqrt(DOT4(u, u));
	if (f > 0)
	{
		f = 1.0f / f;
		u[0] *= f;
		u[1] *= f;
		u[2] *= f;
		u[3] *= f;
	}
	return (f);
}


// length of a vector
//
float norm(float v[3])
{
	return _sqrt(DOT(v, v));
}

//
// translation component of a matrix
//

void get_translation(const Matrix M, float p[3])
{
	p[0] = M[3][0];
	p[1] = M[3][1];
	p[2] = M[3][2];
}

float get_translation(const Matrix M)
{
	float p[3];
	get_translation(M, p);
	return norm(p);
}

void set_translation(Matrix M, const float p[3])
{
	M[3][0] = p[0];
	M[3][1] = p[1];
	M[3][2] = p[2];
}

void get_translation(const Matrix M, float& x, float& y, float& z)
{
	x = M[3][0];
	y = M[3][1];
	z = M[3][2];
}

void set_translation(Matrix M, float x, float y, float z)
{
	M[3][0] = x;
	M[3][1] = y;
	M[3][2] = z;
}

void set_row(Matrix M, int row, const float v[3])
{
	M[row][0] = v[0];
	M[row][1] = v[1];
	M[row][2] = v[2];
}

void get_row(Matrix M, int row, float v[3])
{
	v[0] = M[row][0];
	v[1] = M[row][1];
	v[2] = M[row][2];
}

float vecdist(const float t[], const float t2[])
{
	float t3[3];

	vecsub(t3, (float*)t, (float*)t2);
	return _sqrt(DOT(t3, t3));
}
