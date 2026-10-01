#include "stdafx.h"
#pragma hdrstop

#include "soundrender_TargetA.h"
#include "soundrender_emitter.h"
#include "soundrender_source.h"

xr_vector<u8> g_target_temp_data;
xr_vector<u8> g_target_temp_data_16;

CSoundRender_TargetA::CSoundRender_TargetA(): CSoundRender_Target()
{
	cache_gain = 0.f;
	cache_pitch = 1.f;
	cache_reference_distance = 0.f;
	cache_max_distance = 0.f;
	cache_rolloff = 0.f;
	cache_position.set(0.f, 0.f, 0.f);
	cache_velocity.set(0.f, 0.f, 0.f);
	cache_relative = FALSE;
	cache_parameters_valid = FALSE;
	pSource = 0;
	Slot = u32(-1);
	startup_buffers_queued = 0;
}

CSoundRender_TargetA::~CSoundRender_TargetA()
{
}

void CSoundRender_TargetA::SetSlot(ALuint NewSlot)
{
	Slot = NewSlot;
}

BOOL CSoundRender_TargetA::_initialize()
{
	inherited::_initialize();
	invalidate_parameter_cache();
	// initialize buffer
	A_CHK(alGenBuffers (sdef_target_count, pBuffers));
	alGenSources(1, &pSource);
	ALenum error = alGetError();
	if (AL_NO_ERROR == error)
	{
		A_CHK(alSourcei (pSource, AL_LOOPING, AL_FALSE));
		A_CHK(alSourcef (pSource, AL_MIN_GAIN, 0.f));
		A_CHK(alSourcef (pSource, AL_MAX_GAIN, 1.f));
		A_CHK(alSourcef (pSource, AL_GAIN, cache_gain));
		A_CHK(alSourcef (pSource, AL_PITCH, cache_pitch));
		return TRUE;
	}
	else
	{
		Msg("! sound: OpenAL: Can't create source. Error: %s.", (LPCSTR)alGetString(error));
		return FALSE;
	}
}

void CSoundRender_TargetA::_destroy()
{
	// clean up target
	if (alIsSource(pSource))
		alDeleteSources(1, &pSource);
	A_CHK(alDeleteBuffers (sdef_target_count, pBuffers));
	inherited::_destroy();
}

void CSoundRender_TargetA::_restart()
{
	_destroy();
	_initialize();
}

void CSoundRender_TargetA::start(CSoundRender_Emitter* E)
{
	inherited::start(E);
	invalidate_parameter_cache();
	startup_buffers_queued = 0;

	// Calc storage
	buf_block = sdef_target_block * E->source()->m_wformat.nAvgBytesPerSec / 1000;
	prepare_block_storage();
}

void CSoundRender_TargetA::render()
{
	// A single short block is enough to begin streaming safely. Queue the two
	// full-size safety blocks incrementally from update(); OpenAL explicitly permits
	// adding buffers while a streaming source is playing. This removes the
	// previous 1.2 s decode/upload burst from every target start and resume.
	queue_initial_buffer();
	if (Slot != u32(-1) && !m_pEmitter->bIntro)
	{
		A_CHK(alSource3i(pSource, AL_AUXILIARY_SEND_FILTER, Slot, 0, AL_FILTER_NULL));
	}
	// demonized: explicitly disable effects by sending sounds to null slot, ie. not sending
	else
	{
		A_CHK(alSource3i(pSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, NULL));
	}
	A_CHK(alSourcePlay(pSource));

	inherited::render();
}

void CSoundRender_TargetA::stop()
{
	if (rendering)
	{
		A_CHK(alSourceStop(pSource));
		A_CHK(alSourcei (pSource, AL_BUFFER, NULL));
		A_CHK(alSourcei (pSource, AL_SOURCE_RELATIVE, TRUE));
	}
	startup_buffers_queued = 0;
	invalidate_parameter_cache();
	inherited::stop();
}

void CSoundRender_TargetA::rewind()
{
	inherited::rewind();

	A_CHK(alSourceStop(pSource));
	A_CHK(alSourcei (pSource, AL_BUFFER, NULL));
	queue_initial_buffer();
	A_CHK(alSourcePlay (pSource));
}

void CSoundRender_TargetA::update()
{
	inherited::update();

	ALint processed;
	// Get status
	A_CHK(alGetSourcei(pSource, AL_BUFFERS_PROCESSED, &processed));

	if (processed > 0)
	{
		while (processed)
		{
			ALuint BufferID;
			A_CHK(alSourceUnqueueBuffers(pSource, 1, &BufferID));
			fill_block(BufferID, buf_block);
			A_CHK(alSourceQueueBuffers(pSource, 1, &BufferID));
			--processed;
		}
	}

	if (startup_buffers_queued < sdef_target_count && SoundRender->try_begin_target_prime())
	{
		const u64 prime_started = CPU::QPC();
		const ALuint buffer = pBuffers[startup_buffers_queued];
		fill_block(buffer, buf_block);
		A_CHK(alSourceQueueBuffers(pSource, 1, &buffer));
		++startup_buffers_queued;
		SoundRender->finish_target_prime(CPU::QPC() - prime_started);
	}

	// Check after both processed-buffer refills and startup priming. A stopped
	// source can be recovered with whatever valid queue depth is available.
	ALint state;
	A_CHK(alGetSourcei(pSource, AL_SOURCE_STATE, &state));
	if (state != AL_PLAYING)
	{
		//			Log		("Queuing underrun detected.");
		A_CHK(alSourcePlay(pSource));
	}
}

void CSoundRender_TargetA::fill_parameters()
{
	CSoundRender_Emitter* SE = m_pEmitter;
	VERIFY(SE);

	inherited::fill_parameters();

	// 3D params
	VERIFY2(m_pEmitter, SE->source()->file_name());
	const float reference_distance = m_pEmitter->p_source.min_distance;
	if (!cache_parameters_valid || cache_reference_distance != reference_distance)
	{
		cache_reference_distance = reference_distance;
		A_CHK(alSourcef(pSource, AL_REFERENCE_DISTANCE, reference_distance));
	}

	VERIFY2(m_pEmitter, SE->source()->file_name());
	const float max_distance = m_pEmitter->p_source.max_distance;
	if (!cache_parameters_valid || cache_max_distance != max_distance)
	{
		cache_max_distance = max_distance;
		A_CHK(alSourcef(pSource, AL_MAX_DISTANCE, max_distance));
	}

	VERIFY2(m_pEmitter, SE->source()->file_name ());
	const Fvector& position = m_pEmitter->p_source.position;
	if (!cache_parameters_valid || cache_position.x != position.x ||
		cache_position.y != position.y || cache_position.z != position.z)
	{
		cache_position = position;
		A_CHK(alSource3f(pSource, AL_POSITION, position.x, position.y, -position.z));
	}

	VERIFY2(m_pEmitter, SE->source()->file_name());
	const Fvector& velocity = m_pEmitter->p_source.velocity;
	if (!cache_parameters_valid || cache_velocity.x != velocity.x ||
		cache_velocity.y != velocity.y || cache_velocity.z != velocity.z)
	{
		cache_velocity = velocity;
		A_CHK(alSource3f(pSource, AL_VELOCITY, velocity.x, velocity.y, -velocity.z));
	}

	VERIFY2(m_pEmitter, SE->source()->file_name());
	const BOOL relative = m_pEmitter->b2D;
	if (!cache_parameters_valid || cache_relative != relative)
	{
		cache_relative = relative;
		A_CHK(alSourcei(pSource, AL_SOURCE_RELATIVE, relative));
	}

	if (!cache_parameters_valid || cache_rolloff != psSoundRolloff)
	{
		cache_rolloff = psSoundRolloff;
		A_CHK(alSourcef(pSource, AL_ROLLOFF_FACTOR, psSoundRolloff));
	}
	cache_parameters_valid = TRUE;

	VERIFY2(m_pEmitter, SE->source()->file_name());
	float _gain = m_pEmitter->smooth_volume;
	clamp(_gain, EPS_S, 1.f);
	if (!fsimilar(_gain, cache_gain, 0.01f))
	{
		cache_gain = _gain;
		A_CHK(alSourcef (pSource, AL_GAIN, _gain));
	}

	VERIFY2(m_pEmitter, SE->source()->file_name());
	float _pitch = m_pEmitter->p_source.freq;
	clamp(_pitch, EPS_L, 2.f);

	if (!fsimilar(cache_pitch, _pitch * psSpeedOfSound))
	{
		cache_pitch = _pitch * psSpeedOfSound;

		// Only update time to stop for non-looped sounds
		if (!m_pEmitter->iPaused && (m_pEmitter->m_current_state == CSoundRender_Emitter::stStarting || m_pEmitter->m_current_state == CSoundRender_Emitter::stPlaying || m_pEmitter->m_current_state == CSoundRender_Emitter::stSimulating))
			m_pEmitter->fTimeToStop = SoundRender->fTimer_Value + ((m_pEmitter->get_length_sec() - (SoundRender->fTimer_Value - m_pEmitter->fTimeStarted)) / cache_pitch);

		A_CHK(alSourcef(pSource, AL_PITCH, cache_pitch));
	}
	VERIFY2(m_pEmitter, SE->source()->file_name());
}

void CSoundRender_TargetA::invalidate_parameter_cache()
{
	cache_parameters_valid = FALSE;
}

void CSoundRender_TargetA::prepare_block_storage()
{
	R_ASSERT(m_pEmitter);
	xr_vector<u8>& storage = m_pEmitter->source()->m_wformat.nChannels == 1 ?
		g_target_temp_data : g_target_temp_data_16;
	if (storage.size() < buf_block)
		storage.resize(buf_block);
}

void CSoundRender_TargetA::queue_initial_buffer()
{
	R_ASSERT(sdef_target_count > 0);
	// The sound worker gets another update long before this block is consumed.
	// Keep the common zero-offset startup below the next cache-line boundary:
	// 150 ms needs one decoded line for mono and two for stereo, while retaining
	// roughly nine 60 Hz frames for the incremental 400 ms safety-block prime.
	const ALuint initial_block_size = _min<ALuint>(buf_block,
		sdef_target_initial_block * m_pEmitter->source()->m_wformat.nAvgBytesPerSec / 1000);
	fill_block(pBuffers[0], initial_block_size);
	A_CHK(alSourceQueueBuffers(pSource, 1, pBuffers));
	startup_buffers_queued = 1;
}

void CSoundRender_TargetA::fill_block(ALuint BufferID, const ALuint block_size)
{
	R_ASSERT(m_pEmitter);
	R_ASSERT(block_size > 0 && block_size <= buf_block);
	const ALuint sample_rate = m_pEmitter->source()->m_wformat.nSamplesPerSec;
	const ALuint format = (m_pEmitter->source()->m_wformat.nChannels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	if (format == AL_FORMAT_MONO16)
	{
		prepare_block_storage();
		m_pEmitter->fill_block(&g_target_temp_data.front(), block_size);
		A_CHK(alBufferData(BufferID, format, &g_target_temp_data.front(), block_size, sample_rate));
	}
	else
	{
		prepare_block_storage();
		m_pEmitter->fill_block(&g_target_temp_data_16.front(), block_size);
		A_CHK(alBufferData(BufferID, format, &g_target_temp_data_16.front(), block_size, sample_rate));
	}
}

void CSoundRender_TargetA::source_changed()
{
	dettach();
	attach();
	buf_block = sdef_target_block * m_pEmitter->source()->m_wformat.nAvgBytesPerSec / 1000;
	prepare_block_storage();
}
