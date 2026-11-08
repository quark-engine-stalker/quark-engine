#pragma once

// BC7 block encoder for uncompressed textures, written against the D3D11 BC7 format description.
// Modes: 6 (one line through RGBA), 5 (RGB and alpha apart), 1 and 7 (two lines, for blocks with an edge in them).
// decode_block reads what encode_block writes; a test checks it against the GPU texel by texel.
// No engine dependencies: a standalone test builds this header as is.
#include <cstdint>
#include <cstring>

namespace bc7
{
using byte = std::uint8_t;

namespace detail
{
constexpr byte weights2[4] = {0, 21, 43, 64};
constexpr byte weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr byte weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// Bit i set: texel i belongs to the second subset
constexpr std::uint16_t partitions2[64] = {
	0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
	0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
	0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
	0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};

constexpr byte anchors2[64] = {
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
	15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};

inline int lerp(int a, int b, int weight) { return ((64 - weight) * a + weight * b + 32) >> 6; }

struct bit_writer
{
	std::uint64_t word[2] = {0, 0};
	unsigned pos = 0;
	void put(unsigned value, unsigned bits)
	{
		const unsigned index = pos >> 6, shift = pos & 63;
		word[index] |= std::uint64_t(value) << shift;
		if (shift + bits > 64)
			word[index + 1] |= std::uint64_t(value) >> (64 - shift);
		pos += bits;
	}
};

struct bit_reader
{
	std::uint64_t word[2];
	unsigned pos = 0;
	explicit bit_reader(const byte* block) { std::memcpy(word, block, 16); }
	unsigned get(unsigned bits)
	{
		const unsigned index = pos >> 6, shift = pos & 63;
		std::uint64_t value = word[index] >> shift;
		if (shift + bits > 64)
			value |= word[index + 1] << (64 - shift);
		pos += bits;
		return unsigned(value & ((std::uint64_t(1) << bits) - 1));
	}
};

// How an endpoint is stored
enum class precision { p7_own_bit, p5_own_bit, p6_shared_bit, p7, p8 };

inline int expand(precision kind, int code, int parity)
{
	switch (kind)
	{
	case precision::p7_own_bit: return (code << 1) | parity;
	case precision::p5_own_bit: { const int v = (code << 1) | parity; return (v << 2) | (v >> 4); }
	case precision::p6_shared_bit: { const int v = (code << 1) | parity; return (v << 1) | (v >> 6); }
	case precision::p7: return (code << 1) | (code >> 6);
	default: return code;
	}
}

inline int code_bits(precision kind)
{
	switch (kind)
	{
	case precision::p7_own_bit: case precision::p7: return 7;
	case precision::p5_own_bit: return 5;
	case precision::p6_shared_bit: return 6;
	default: return 8;
	}
}

// The code of one channel nearest to a target value, parity given
inline int nearest_code(precision kind, float target, int parity)
{
	const int bits = code_bits(kind), top = (1 << bits) - 1;
	const float scale = float(top) / 255.0f;
	int code = int(target * scale + 0.5f);
	code = code < 0 ? 0 : code > top ? top : code;
	int best = code, best_err = 0x7fffffff;
	for (int c = code > 0 ? code - 1 : code, end = code < top ? code + 1 : code; c <= end; ++c)
	{
		const float d = float(expand(kind, c, parity)) - target;
		const int err = int(d * d * 16.0f);
		if (err < best_err) { best_err = err; best = c; }
	}
	return best;
}

struct line_fit
{
	int code[2][4];    // stored codes
	int value[2][4];   // what the GPU reconstructs
	int parity[2];
	byte index[16];
	std::uint32_t error;
};

// Puts the two float endpoints on the grid of the stored precision
template <int Channels>
void quantize(precision kind, const float ends[2][4], line_fit& fit)
{
	const auto quantize_end = [&](int end, int parity, int* code, int* value)
	{
		float err = 0;
		for (int c = 0; c < Channels; ++c)
		{
			code[c] = nearest_code(kind, ends[end][c], parity);
			value[c] = expand(kind, code[c], parity);
			const float d = float(value[c]) - ends[end][c];
			err += d * d;
		}
		return err;
	};
	int code[2][2][4], value[2][2][4];
	float err[2][2];
	const bool parity_matters = kind == precision::p7_own_bit || kind == precision::p5_own_bit || kind == precision::p6_shared_bit;
	const int parities = parity_matters ? 2 : 1;
	for (int end = 0; end < 2; ++end)
		for (int p = 0; p < parities; ++p)
			err[end][p] = quantize_end(end, p, code[end][p], value[end][p]);
	int p0 = 0, p1 = 0;
	if (kind == precision::p6_shared_bit)
		p0 = p1 = err[0][1] + err[1][1] < err[0][0] + err[1][0] ? 1 : 0;
	else if (parities == 2)
	{
		p0 = err[0][1] < err[0][0] ? 1 : 0;
		p1 = err[1][1] < err[1][0] ? 1 : 0;
	}
	fit.parity[0] = p0;
	fit.parity[1] = p1;
	for (int c = 0; c < Channels; ++c)
	{
		fit.code[0][c] = code[0][p0][c]; fit.value[0][c] = value[0][p0][c];
		fit.code[1][c] = code[1][p1][c]; fit.value[1][c] = value[1][p1][c];
	}
}

// One line through the texels listed in members, Channels first channels of each texel.
// Principal axis for the first guess, then rounds of "best index for each texel, best endpoints for these indices".
template <int Channels>
void fit_line(const byte* texels, const byte* members, int count, precision kind, const byte* weights, int weight_count, int rounds, line_fit& out)
{
	float mean[4] = {0, 0, 0, 0};
	for (int i = 0; i < count; ++i)
		for (int c = 0; c < Channels; ++c)
			mean[c] += texels[members[i] * 4 + c];
	for (int c = 0; c < Channels; ++c)
		mean[c] /= float(count);

	float cov[4][4] = {};
	float low[4], high[4];
	for (int c = 0; c < Channels; ++c) { low[c] = 255.0f; high[c] = 0.0f; }
	for (int i = 0; i < count; ++i)
	{
		const byte* t = texels + members[i] * 4;
		float d[4];
		for (int c = 0; c < Channels; ++c)
		{
			d[c] = float(t[c]) - mean[c];
			if (t[c] < low[c]) low[c] = t[c];
			if (t[c] > high[c]) high[c] = t[c];
		}
		for (int a = 0; a < Channels; ++a)
			for (int b = a; b < Channels; ++b)
				cov[a][b] += d[a] * d[b];
	}
	for (int a = 0; a < Channels; ++a)
		for (int b = 0; b < a; ++b)
			cov[a][b] = cov[b][a];

	float axis[4];
	for (int c = 0; c < Channels; ++c)
		axis[c] = high[c] - low[c];
	for (int iteration = 0; iteration < 4; ++iteration)
	{
		float next[4] = {0, 0, 0, 0}, largest = 0;
		for (int a = 0; a < Channels; ++a)
		{
			for (int b = 0; b < Channels; ++b)
				next[a] += cov[a][b] * axis[b];
			const float magnitude = next[a] < 0 ? -next[a] : next[a];
			if (magnitude > largest) largest = magnitude;
		}
		if (largest <= 0.0001f)
			break;
		for (int c = 0; c < Channels; ++c)
			axis[c] = next[c] / largest;
	}

	float axis_len = 0;
	for (int c = 0; c < Channels; ++c)
		axis_len += axis[c] * axis[c];
	float t_min = 0, t_max = 0;
	if (axis_len > 0.0001f)
	{
		t_min = 1e30f; t_max = -1e30f;
		for (int i = 0; i < count; ++i)
		{
			float t = 0;
			for (int c = 0; c < Channels; ++c)
				t += (float(texels[members[i] * 4 + c]) - mean[c]) * axis[c];
			t /= axis_len;
			if (t < t_min) t_min = t;
			if (t > t_max) t_max = t;
		}
	}
	float ends[2][4];
	for (int c = 0; c < Channels; ++c)
	{
		const float a = mean[c] + axis[c] * t_min, b = mean[c] + axis[c] * t_max;
		ends[0][c] = a < 0 ? 0 : a > 255 ? 255 : a;
		ends[1][c] = b < 0 ? 0 : b > 255 ? 255 : b;
	}

	out.error = 0xffffffffu;
	for (int round = 0; round < rounds; ++round)
	{
		line_fit fit;
		quantize<Channels>(kind, ends, fit);

		int palette[16][4];
		for (int i = 0; i < weight_count; ++i)
			for (int c = 0; c < Channels; ++c)
				palette[i][c] = lerp(fit.value[0][c], fit.value[1][c], weights[i]);

		int direction[4], direction_len = 0;
		for (int c = 0; c < Channels; ++c)
		{
			direction[c] = fit.value[1][c] - fit.value[0][c];
			direction_len += direction[c] * direction[c];
		}
		fit.error = 0;
		for (int i = 0; i < count; ++i)
		{
			const byte* t = texels + members[i] * 4;
			int guess = 0;
			if (direction_len)
			{
				int dot = 0;
				for (int c = 0; c < Channels; ++c)
					dot += (int(t[c]) - fit.value[0][c]) * direction[c];
				guess = (dot * (weight_count - 1) * 2 + direction_len) / (direction_len * 2);
				guess = guess < 0 ? 0 : guess > weight_count - 1 ? weight_count - 1 : guess;
			}
			int best = guess, best_err = 0x7fffffff;
			for (int k = guess > 0 ? guess - 1 : guess, end = guess < weight_count - 1 ? guess + 1 : guess; k <= end; ++k)
			{
				int err = 0;
				for (int c = 0; c < Channels; ++c)
				{
					const int d = int(t[c]) - palette[k][c];
					err += d * d;
				}
				if (err < best_err) { best_err = err; best = k; }
			}
			fit.index[i] = byte(best);
			fit.error += std::uint32_t(best_err);
		}
		if (fit.error < out.error)
			out = fit;
		if (!fit.error || round + 1 == rounds)
			break;

		// Least squares for the endpoints with these indices
		float aa = 0, ab = 0, bb = 0, ax[4] = {0, 0, 0, 0}, bx[4] = {0, 0, 0, 0};
		for (int i = 0; i < count; ++i)
		{
			const float w = float(weights[fit.index[i]]) / 64.0f, a = 1.0f - w;
			aa += a * a; ab += a * w; bb += w * w;
			const byte* t = texels + members[i] * 4;
			for (int c = 0; c < Channels; ++c)
			{
				ax[c] += a * float(t[c]);
				bx[c] += w * float(t[c]);
			}
		}
		const float det = aa * bb - ab * ab;
		if (det < 0.0001f && det > -0.0001f)
			break;
		for (int c = 0; c < Channels; ++c)
		{
			const float e0 = (bb * ax[c] - ab * bx[c]) / det, e1 = (aa * bx[c] - ab * ax[c]) / det;
			ends[0][c] = e0 < 0 ? 0 : e0 > 255 ? 255 : e0;
			ends[1][c] = e1 < 0 ? 0 : e1 > 255 ? 255 : e1;
		}
	}
}

// The anchor texel of a subset stores one index bit less, its top bit must be 0
template <int Channels>
void fix_anchor(line_fit& fit, int anchor_member, int count, int weight_count)
{
	if (fit.index[anchor_member] < weight_count / 2)
		return;
	for (int c = 0; c < Channels; ++c)
	{
		const int code = fit.code[0][c], value = fit.value[0][c];
		fit.code[0][c] = fit.code[1][c]; fit.value[0][c] = fit.value[1][c];
		fit.code[1][c] = code; fit.value[1][c] = value;
	}
	const int parity = fit.parity[0];
	fit.parity[0] = fit.parity[1];
	fit.parity[1] = parity;
	for (int i = 0; i < count; ++i)
		fit.index[i] = byte(weight_count - 1 - fit.index[i]);
}

inline void store(const bit_writer& bits, byte* out) { std::memcpy(out, bits.word, 16); }

inline std::uint32_t encode_mode6(const byte* texels, byte* out)
{
	static constexpr byte all[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
	line_fit fit;
	fit_line<4>(texels, all, 16, precision::p7_own_bit, weights4, 16, 3, fit);
	fix_anchor<4>(fit, 0, 16, 16);
	bit_writer bits;
	bits.put(1u << 6, 7);
	for (int c = 0; c < 4; ++c)
	{
		bits.put(unsigned(fit.code[0][c]), 7);
		bits.put(unsigned(fit.code[1][c]), 7);
	}
	bits.put(unsigned(fit.parity[0]), 1);
	bits.put(unsigned(fit.parity[1]), 1);
	bits.put(fit.index[0], 3);
	for (int i = 1; i < 16; ++i)
		bits.put(fit.index[i], 4);
	store(bits, out);
	return fit.error;
}

inline std::uint32_t encode_mode5(const byte* texels, byte* out)
{
	static constexpr byte all[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
	line_fit color, alpha;
	fit_line<3>(texels, all, 16, precision::p7, weights2, 4, 3, color);
	byte alpha_texels[64];
	for (int i = 0; i < 16; ++i)
		alpha_texels[i * 4] = texels[i * 4 + 3];
	fit_line<1>(alpha_texels, all, 16, precision::p8, weights2, 4, 3, alpha);
	fix_anchor<3>(color, 0, 16, 4);
	fix_anchor<1>(alpha, 0, 16, 4);
	bit_writer bits;
	bits.put(1u << 5, 6);
	bits.put(0, 2); // no channel rotation
	for (int c = 0; c < 3; ++c)
	{
		bits.put(unsigned(color.code[0][c]), 7);
		bits.put(unsigned(color.code[1][c]), 7);
	}
	bits.put(unsigned(alpha.code[0][0]), 8);
	bits.put(unsigned(alpha.code[1][0]), 8);
	bits.put(color.index[0], 1);
	for (int i = 1; i < 16; ++i)
		bits.put(color.index[i], 2);
	bits.put(alpha.index[0], 1);
	for (int i = 1; i < 16; ++i)
		bits.put(alpha.index[i], 2);
	store(bits, out);
	return color.error + alpha.error;
}

// The partitions whose subsets hold the least spread, cheapest measure of "this split follows the edge"
inline int best_partitions(const byte* texels, int channels, int* best, int wanted)
{
	std::uint32_t scores[64];
	std::int32_t total_sum[4] = {0, 0, 0, 0};
	std::int64_t total_square = 0;
	for (int i = 0; i < 16; ++i)
		for (int c = 0; c < channels; ++c)
		{
			const int v = texels[i * 4 + c];
			total_sum[c] += v;
			total_square += v * v;
		}
	for (int p = 0; p < 64; ++p)
	{
		std::int32_t sum[4] = {0, 0, 0, 0};
		int count = 0;
		const unsigned mask = partitions2[p];
		for (int i = 0; i < 16; ++i)
			if (mask >> i & 1)
			{
				++count;
				for (int c = 0; c < channels; ++c)
					sum[c] += texels[i * 4 + c];
			}
		// spread = sum of squares - |sum|^2 / n per subset; the sum of squares is common to all partitions
		float kept = 0;
		for (int c = 0; c < channels; ++c)
		{
			const float a = float(sum[c]), b = float(total_sum[c] - sum[c]);
			kept += a * a / float(count) + b * b / float(16 - count);
		}
		const float spread = float(total_square) - kept;
		scores[p] = std::uint32_t(spread < 0 ? 0 : spread);
	}
	int found = 0;
	for (int k = 0; k < wanted; ++k)
	{
		int top = -1;
		for (int p = 0; p < 64; ++p)
		{
			bool used = false;
			for (int u = 0; u < found; ++u)
				used |= best[u] == p;
			if (!used && (top < 0 || scores[p] < scores[top]))
				top = p;
		}
		best[found++] = top;
	}
	return found;
}

// Mode 1 (no alpha, six bit endpoints, three bit indices) or mode 7 (alpha, five bit endpoints, two bit indices)
template <int Channels>
std::uint32_t encode_two_subsets(const byte* texels, int partition, byte* out)
{
	const unsigned mask = partitions2[partition];
	const int anchor = anchors2[partition];
	byte members[2][16];
	int counts[2] = {0, 0};
	for (int i = 0; i < 16; ++i)
	{
		const int subset = mask >> i & 1;
		members[subset][counts[subset]++] = byte(i);
	}
	const bool mode7 = Channels == 4;
	const precision kind = mode7 ? precision::p5_own_bit : precision::p6_shared_bit;
	const byte* weights = mode7 ? weights2 : weights3;
	const int weight_count = mode7 ? 4 : 8;
	line_fit fit[2];
	fit_line<Channels>(texels, members[0], counts[0], kind, weights, weight_count, 3, fit[0]);
	fit_line<Channels>(texels, members[1], counts[1], kind, weights, weight_count, 3, fit[1]);
	fix_anchor<Channels>(fit[0], 0, counts[0], weight_count);
	int anchor_member = 0;
	for (int i = 0; i < counts[1]; ++i)
		if (members[1][i] == anchor)
			anchor_member = i;
	fix_anchor<Channels>(fit[1], anchor_member, counts[1], weight_count);

	byte index[16];
	for (int subset = 0; subset < 2; ++subset)
		for (int i = 0; i < counts[subset]; ++i)
			index[members[subset][i]] = fit[subset].index[i];

	bit_writer bits;
	if (mode7)
		bits.put(1u << 7, 8);
	else
		bits.put(1u << 1, 2);
	bits.put(unsigned(partition), 6);
	const int code_size = mode7 ? 5 : 6;
	for (int c = 0; c < Channels; ++c)
		for (int subset = 0; subset < 2; ++subset)
		{
			bits.put(unsigned(fit[subset].code[0][c]), unsigned(code_size));
			bits.put(unsigned(fit[subset].code[1][c]), unsigned(code_size));
		}
	if (mode7)
	{
		bits.put(unsigned(fit[0].parity[0]), 1);
		bits.put(unsigned(fit[0].parity[1]), 1);
		bits.put(unsigned(fit[1].parity[0]), 1);
		bits.put(unsigned(fit[1].parity[1]), 1);
	}
	else
	{
		bits.put(unsigned(fit[0].parity[0]), 1);
		bits.put(unsigned(fit[1].parity[0]), 1);
	}
	const unsigned index_bits = mode7 ? 2 : 3;
	for (int i = 0; i < 16; ++i)
		bits.put(index[i], (i == 0 || i == anchor) ? index_bits - 1 : index_bits);
	store(bits, out);
	return fit[0].error + fit[1].error;
}
} // namespace detail

// texels: 16 RGBA8 in row order. Returns the squared error of the block.
inline std::uint32_t encode_block(const byte* texels, byte* out)
{
	using namespace detail;
	bool opaque = true, one_alpha = true;
	for (int i = 0; i < 16; ++i)
	{
		opaque &= texels[i * 4 + 3] == 255;
		one_alpha &= texels[i * 4 + 3] == texels[3];
	}
	byte candidate[16];
	std::uint32_t best = encode_mode6(texels, out);
	// A smooth block needs nothing else: about one level of error per channel
	constexpr std::uint32_t good_enough = 16u * 4u;
	if (best <= good_enough)
		return best;
	if (!one_alpha)
	{
		const std::uint32_t err = encode_mode5(texels, candidate);
		if (err < best) { best = err; std::memcpy(out, candidate, 16); }
		if (best <= good_enough)
			return best;
	}
	int partitions[3];
	const int count = best_partitions(texels, opaque ? 3 : 4, partitions, 3);
	for (int i = 0; i < count; ++i)
	{
		std::uint32_t err = opaque ? encode_two_subsets<3>(texels, partitions[i], candidate) : encode_two_subsets<4>(texels, partitions[i], candidate);
		// Mode 1 has no alpha channel: it decodes to 255, the same as an opaque block
		if (err < best) { best = err; std::memcpy(out, candidate, 16); }
	}
	return best;
}

// Modes 1, 5, 6 and 7. Returns false for any other mode.
inline bool decode_block(const byte* block, byte* texels)
{
	using namespace detail;
	bit_reader bits(block);
	unsigned mode = 0;
	while (mode < 8 && !bits.get(1))
		++mode;
	int ends[2][2][4];
	if (mode == 6)
	{
		int code[2][4];
		for (int c = 0; c < 4; ++c)
		{
			code[0][c] = int(bits.get(7));
			code[1][c] = int(bits.get(7));
		}
		const int p0 = int(bits.get(1)), p1 = int(bits.get(1));
		int values[2][4];
		for (int c = 0; c < 4; ++c)
		{
			values[0][c] = expand(precision::p7_own_bit, code[0][c], p0);
			values[1][c] = expand(precision::p7_own_bit, code[1][c], p1);
		}
		for (int i = 0; i < 16; ++i)
		{
			const unsigned index = bits.get(i == 0 ? 3 : 4);
			for (int c = 0; c < 4; ++c)
				texels[i * 4 + c] = byte(lerp(values[0][c], values[1][c], weights4[index]));
		}
		return true;
	}
	if (mode == 5)
	{
		const unsigned rotation = bits.get(2);
		for (int c = 0; c < 3; ++c)
		{
			ends[0][0][c] = expand(precision::p7, int(bits.get(7)), 0);
			ends[0][1][c] = expand(precision::p7, int(bits.get(7)), 0);
		}
		ends[0][0][3] = int(bits.get(8));
		ends[0][1][3] = int(bits.get(8));
		unsigned color_index[16];
		for (int i = 0; i < 16; ++i)
			color_index[i] = bits.get(i == 0 ? 1 : 2);
		for (int i = 0; i < 16; ++i)
		{
			const unsigned alpha_index = bits.get(i == 0 ? 1 : 2);
			for (int c = 0; c < 3; ++c)
				texels[i * 4 + c] = byte(lerp(ends[0][0][c], ends[0][1][c], weights2[color_index[i]]));
			texels[i * 4 + 3] = byte(lerp(ends[0][0][3], ends[0][1][3], weights2[alpha_index]));
			if (rotation)
			{
				const byte swap = texels[i * 4 + rotation - 1];
				texels[i * 4 + rotation - 1] = texels[i * 4 + 3];
				texels[i * 4 + 3] = swap;
			}
		}
		return true;
	}
	if (mode != 1 && mode != 7)
		return false;
	const bool mode7 = mode == 7;
	const int channels = mode7 ? 4 : 3;
	const unsigned partition = bits.get(6);
	const unsigned code_size = mode7 ? 5 : 6;
	int code[2][2][4];
	for (int c = 0; c < channels; ++c)
		for (int subset = 0; subset < 2; ++subset)
		{
			code[subset][0][c] = int(bits.get(code_size));
			code[subset][1][c] = int(bits.get(code_size));
		}
	int parity[2][2];
	if (mode7)
	{
		parity[0][0] = int(bits.get(1)); parity[0][1] = int(bits.get(1));
		parity[1][0] = int(bits.get(1)); parity[1][1] = int(bits.get(1));
	}
	else
	{
		parity[0][0] = parity[0][1] = int(bits.get(1));
		parity[1][0] = parity[1][1] = int(bits.get(1));
	}
	const precision kind = mode7 ? precision::p5_own_bit : precision::p6_shared_bit;
	for (int subset = 0; subset < 2; ++subset)
		for (int end = 0; end < 2; ++end)
			for (int c = 0; c < channels; ++c)
				ends[subset][end][c] = expand(kind, code[subset][end][c], parity[subset][end]);
	const unsigned mask = partitions2[partition];
	const int anchor = anchors2[partition];
	const unsigned index_bits = mode7 ? 2 : 3;
	const byte* weights = mode7 ? weights2 : weights3;
	for (int i = 0; i < 16; ++i)
	{
		const unsigned index = bits.get((i == 0 || i == anchor) ? index_bits - 1 : index_bits);
		const int subset = mask >> i & 1;
		for (int c = 0; c < channels; ++c)
			texels[i * 4 + c] = byte(lerp(ends[subset][0][c], ends[subset][1][c], weights[index]));
		if (!mode7)
			texels[i * 4 + 3] = 255;
	}
	return true;
}
} // namespace bc7
