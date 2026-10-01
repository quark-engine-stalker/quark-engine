#ifndef SoundRender_TargetAH
#define SoundRender_TargetAH
#pragma once

#include "soundrender_Target.h"
#include "soundrender_CoreA.h"

class CSoundRender_TargetA : public CSoundRender_Target
{
	typedef CSoundRender_Target inherited;

public:
	// OpenAL
	ALuint pSource;
	ALuint pBuffers[sdef_target_count];
	float cache_gain;
	float cache_pitch;
	float cache_reference_distance;
	float cache_max_distance;
	float cache_rolloff;
	Fvector cache_position;
	Fvector cache_velocity;
	BOOL cache_relative;
	BOOL cache_parameters_valid;
	ALuint Slot;

	ALuint buf_block;
private:
	u32 startup_buffers_queued;
	void fill_block(ALuint BufferID, ALuint block_size);
	void prepare_block_storage();
	void queue_initial_buffer();
	void invalidate_parameter_cache();
public:
	CSoundRender_TargetA();
	virtual ~CSoundRender_TargetA();

	void SetSlot(ALuint NewSlot);
	virtual BOOL _initialize();
	virtual void _destroy();
	virtual void _restart();

	virtual void start(CSoundRender_Emitter* E);
	virtual void render();
	virtual void rewind();
	virtual void stop();
	virtual void update();
	virtual void fill_parameters();
	void source_changed();
};
#endif
