#include "client.h"
#include "snd_loc.h"

#define OFFSET_PTR(p, offset)    (((byte*)(p)) + (offset))

cvar_t *s_mixer;
cvar_t *s_latency;

int g_autosoundIndex[MAX_EDICTS];
sndma_sound_t g_sounds[MAX_CHANNELS];

static void SNDMA_DeactivateSound (sndma_sound_t *snd);
static void SNDMA_FreeRawSamples (void);

sound_mixer_t S_Mixer (void)
{
	if (strcmp(Cvar_VariableString("s_mixer"), "dma") == 0) {
		return sound_mixer_dma;
	} else {
		return sound_mixer_miniaudio;
	}
}


static void* SNDMA_AllocationCallback_Malloc(size_t sz, void* pUserData)
{
    (void)pUserData;
    return Z_Malloc((int)sz);
}

static void* SNDMA_AllocationCallback_Realloc(void* p, size_t sz, void* pUserData)
{
    (void)pUserData;
    return Z_Realloc(p, (int)sz);
}

static void SNDMA_AllocationCallback_Free(void* p, void* pUserData)
{
    (void)pUserData;
    Z_Free(p);
}

static ma_allocation_callbacks g_audioAllocationCallbacks =
{
    NULL,   /* pUserData */
    SNDMA_AllocationCallback_Malloc,
    SNDMA_AllocationCallback_Realloc,
    SNDMA_AllocationCallback_Free
};


typedef struct
{
	void* pData;
	int sizeInBytes;
	int cursor;
} snd_vfs_file_t;

static ma_result SNDMA_VFS_Open(ma_vfs* pVFS, const char* pFilePath, ma_uint32 openMode, ma_vfs_file* pFile)
{
	void* pData;
	int sizeInBytes;
	snd_vfs_file_t* pVFSFile;
	
	sizeInBytes = FS_LoadFile(pFilePath, &pData);
	if (pData == NULL) {
		return MA_DOES_NOT_EXIST;	/* Could also be out of memory, but Quake 2 doesn't give us enough info. */
	}

	pVFSFile = Z_Malloc(sizeInBytes);
	if (pVFSFile == NULL) {
		Z_Free(pData);
		return MA_OUT_OF_MEMORY;
	}

	pVFSFile->pData       = pData;
	pVFSFile->sizeInBytes = sizeInBytes;
	pVFSFile->cursor      = 0;

	*pFile = (ma_vfs_file)pVFSFile;

	return MA_SUCCESS;
}

static ma_result SNDMA_VFS_Close(ma_vfs* pVFS, ma_vfs_file file)
{
	snd_vfs_file_t* pVFSFile = (snd_vfs_file_t*)file;
	assert(pVFSFile != NULL);

	Z_Free(pVFSFile->pData);
	Z_Free(pVFSFile);

	(void)pVFS;
	return MA_SUCCESS;
}

static ma_result SNDMA_VFS_Read(ma_vfs* pVFS, ma_vfs_file file, void* pDst, size_t sizeInBytes, size_t* pBytesRead)
{
	int bytesRemaining;
	snd_vfs_file_t* pVFSFile = (snd_vfs_file_t*)file;
	assert(pVFSFile != NULL);

	*pBytesRead = 0;	/* Safety. */

	bytesRemaining = pVFSFile->sizeInBytes - pVFSFile->cursor;
	if (sizeInBytes > bytesRemaining) {
		sizeInBytes = bytesRemaining;
	}

	memcpy(pDst, OFFSET_PTR(pVFSFile->pData, pVFSFile->cursor), sizeInBytes);
	pVFSFile->cursor += (int)sizeInBytes;

	*pBytesRead = sizeInBytes;

	if (pVFSFile->sizeInBytes == pVFSFile->cursor) {
		return MA_AT_END;
	}

	(void)pVFS;
	return MA_SUCCESS;
}

static ma_result SNDMA_VFS_Seek(ma_vfs* pVFS, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
{
	snd_vfs_file_t* pVFSFile = (snd_vfs_file_t*)file;
	assert(pVFSFile != NULL);

	if (origin == ma_seek_origin_start) {
		if (offset < 0 || offset > pVFSFile->sizeInBytes) {
			return MA_INVALID_ARGS;	/* Seeking too far forward. */
		}

		pVFSFile->cursor = (int)offset;
	} else {
		ma_int64 target = pVFSFile->cursor + offset;
		if (target < 0 || target > pVFSFile->sizeInBytes) {
			return MA_INVALID_ARGS;
		}

		pVFSFile->cursor += (int)offset;
	}

	(void)pVFS;
	return MA_SUCCESS;
}

static ma_result SNDMA_VFS_Tell(ma_vfs* pVFS, ma_vfs_file file, ma_int64* pCursor)
{
	snd_vfs_file_t* pVFSFile = (snd_vfs_file_t*)file;
	assert(pVFSFile != NULL);

	*pCursor = pVFSFile->cursor;

	(void)pVFS;
	return MA_SUCCESS;
}

static ma_result SNDMA_VFS_Info(ma_vfs* pVFS, ma_vfs_file file, ma_file_info* pInfo)
{
	snd_vfs_file_t* pVFSFile = (snd_vfs_file_t*)file;
	assert(pVFSFile != NULL);

	pInfo->sizeInBytes = pVFSFile->sizeInBytes;

	(void)pVFS;
	return MA_SUCCESS;
}

static ma_vfs_callbacks g_audioEngineVFS = 
{
	SNDMA_VFS_Open,
    NULL,	/* SNDENG_VFS_OpenW */
    SNDMA_VFS_Close,
    SNDMA_VFS_Read,
    NULL,	/* SNDENG_VFS_Write. We don't do any writing here. */
    SNDMA_VFS_Seek,
    SNDMA_VFS_Tell,
    SNDMA_VFS_Info
};

static ma_resource_manager g_audioResourceManager;
static ma_engine g_audioEngine;

qboolean SNDMA_Init (void)
{
	ma_result result;
	ma_resource_manager_config resourceManagerConfig;
	ma_engine_config engineConfig;

	resourceManagerConfig = ma_resource_manager_config_init();
	resourceManagerConfig.pVFS = &g_audioEngineVFS;
	resourceManagerConfig.allocationCallbacks = g_audioAllocationCallbacks;

	/* The sample rate is configurable. */
	if (s_khz->value == 44) {
		resourceManagerConfig.decodedSampleRate = 44100;
	} else if (s_khz->value == 22) {
		resourceManagerConfig.decodedSampleRate = 22050;
	} else {
		resourceManagerConfig.decodedSampleRate = 11025;
	}

	/* The format to decode to is configurable. */
	if (s_loadas8bit->value) {
		resourceManagerConfig.decodedFormat = ma_format_u8;
	} else {
		resourceManagerConfig.decodedFormat = ma_format_s16;
	}

	Com_Printf("Initializing miniaudio: ");

	result = ma_resource_manager_init(&resourceManagerConfig, &g_audioResourceManager);
	if (result != MA_SUCCESS) {
		Com_Printf("failed to initialize resource manager\n");
		return false;
	}

	
	engineConfig = ma_engine_config_init();
	engineConfig.pResourceManager = &g_audioResourceManager;
    engineConfig.allocationCallbacks = g_audioAllocationCallbacks;
	engineConfig.periodSizeInMilliseconds = (ma_uint32)(s_latency->value * 1000);
	engineConfig.sampleRate = resourceManagerConfig.decodedSampleRate;
	
	result = ma_engine_init(&engineConfig, &g_audioEngine);
	if (result != MA_SUCCESS) {
		Com_Printf("failed to initialize engine\n");
		ma_resource_manager_uninit(&g_audioResourceManager);
		return false;
	}
	Com_Printf("ok\n");

    /* We need to set some parameters in the global "dma" object for some timing stuff. */
	dma.samplebits = ma_get_bytes_per_sample(resourceManagerConfig.decodedFormat)*8;
    dma.channels   = ma_engine_get_channels(&g_audioEngine);
    dma.speed      = ma_engine_get_sample_rate(&g_audioEngine);

	return true;
}

void SNDMA_Shutdown (void)
{
	int i;

	for (i = 0; i < MAX_CHANNELS; i += 1) {
		if (g_sounds[i].active) {
			ma_sound_uninit(&g_sounds[i].sound);
			g_sounds[i].active = false;
		}
	}

	SNDMA_FreeRawSamples();

	ma_engine_uninit(&g_audioEngine);
	ma_resource_manager_uninit(&g_audioResourceManager);
}

void SNDMA_Activate (qboolean active)
{
	if (!sound_started)
		return;

	if (active) {
		ma_engine_start(&g_audioEngine);
	} else {
		ma_engine_stop(&g_audioEngine);
	}
}


sndma_sound_t *SNDMA_PickTransientSound(int entnum, int entchannel)
{
    int iSound;
    int iPickedSound = -1;

    if (entchannel < 0) {
        Com_Error (ERR_DROP, "SNDMA_PickTransientSound: entchannel < 0");
    }

    for (iSound = 0; iSound < MAX_CHANNELS; iSound += 1) {
        /* Always override a sound that's associated with the same channel on the same entity. */
        if (g_sounds[iSound].active) {
            if (entchannel != 0 && g_sounds[iSound].entnum == entnum && g_sounds[iSound].entchannel == entchannel) {
                iPickedSound = iSound;
                break;
            }

            /* Never let a monster sound override a player sound. */
            if (g_sounds[iSound].entnum == cl.playernum+1 && entnum != cl.playernum+1 && g_sounds[iSound].active) {
                continue;
            }
        } else {
            /* If this sound is inactive, just pick this one. */
            if (!g_sounds[iSound].active && iPickedSound == -1) {
                iPickedSound = iSound;
            }
        }

        

        /*
        TODO: We need to keep track of the sound that will reach it's end next and then replace that with the new sound.
        */
    }

    if (iPickedSound == -1) {
        Com_Printf("No sound available.\n");
        return NULL;    /* Couldn't find a slot. */
    }

    /* If we're overwriting a sound, it needs to be deactivated. */
    if (g_sounds[iPickedSound].active) {
        SNDMA_DeactivateSound(&g_sounds[iPickedSound]);
    }

    return &g_sounds[iPickedSound];
}


typedef struct
{
	sfx_t		*sfx;
	float		volume;
	float		attenuation;
	int			entnum;
	int			entchannel;
	qboolean    loop;
	qboolean    autosound;
	qboolean	fixed_origin;	// use origin field instead of entnum's origin
	vec3_t		origin;
	unsigned	begin;			// begin on this sample
} sndma_activation_t;

static qboolean SNDMA_ActivateSound (sndma_sound_t *snd, sndma_activation_t *props)
{
	ma_result result;
    char path[MAX_QPATH];

    S_TransformFilePath(path, sizeof(path), props->sfx->name);

    result = ma_sound_init_from_file(&g_audioEngine, path, MA_SOUND_FLAG_DECODE /*| MA_SOUND_FLAG_NO_SPATIALIZATION*/, NULL, NULL, &snd->sound);  /* NO_SPATIALIZATION for testing. */
    if (result != MA_SUCCESS) {
        return false;
    }

    snd->active       = true;
    snd->entnum       = props->entnum;
    snd->entchannel   = props->entchannel;
	snd->autosound    = props->autosound;
    VectorCopy(props->origin, snd->origin);
    snd->fixed_origin = props->fixed_origin;
    snd->sfx          = props->sfx;

    /* The sound may have a delay. */
	if (props->begin > 0) {
		ma_sound_set_start_time_in_pcm_frames(&snd->sound, props->begin);
	}
    
    ma_sound_set_volume(&snd->sound, props->volume / 255.0f);
    ma_sound_set_min_gain(&snd->sound, 0);
    ma_sound_set_max_gain(&snd->sound, 1);

    /*
	Logic from S_IssueSound(), but with different attenuation constants due to differences between
	miniaudio's and Quake's attenuation models. Due to this, sound will not be 100% identical
	between the two mixers.
	*/
    if (props->attenuation == ATTN_STATIC) {
        ma_sound_set_rolloff(&snd->sound, props->attenuation * 0.005f);
    } else {
        ma_sound_set_rolloff(&snd->sound, props->attenuation * 0.001f);
    }

    if (props->loop) {
        ma_sound_set_looping(&snd->sound, props->loop);
    }

	/* We need to seek to where the sound would be sitting based on the global time. */
	if (props->autosound && props->loop) {
		ma_uint64 length;
		result = ma_sound_get_length_in_pcm_frames(&snd->sound, &length);
		if (result == MA_SUCCESS) {
			ma_sound_seek_to_pcm_frame(&snd->sound, ma_engine_get_time(&g_audioEngine) % length);
		}
	}

    /* Now that we've set up the sound we can start it. */
    ma_sound_start(&snd->sound);

    return true;
}

static qboolean SNDMA_ActivatePersistentSound (int entnum, sfx_t *sfx)
{
	sndma_activation_t props;

    props.sfx = sfx;
	props.volume = 255;
	props.attenuation = SOUND_LOOPATTENUATE / 0.0002f; /* What should this be set to? */
	props.entnum = entnum;
	props.entchannel = 0;
	props.autosound = true;
	props.fixed_origin = false;
    VectorClear(props.origin);
	props.begin = 0;			// begin on this sample
    props.loop = true;

    return SNDMA_ActivateSound(&g_sounds[g_autosoundIndex[entnum]-1], &props);
}

static void SNDMA_ActivatePersistentSounds (void)
{
	int i;
    int num;
    entity_state_t *ent;
    struct
    {
        int entnum;
        sfx_t* sfx;
    } sfxent[MAX_EDICTS];

	/*
	If we're paused we're going to stop all autosounds sounds. They'll be resumed when the game is
	unpaused.
	*/
	if (cl_paused->value) {
		for (i = 0; i < MAX_CHANNELS; i++) {
			if (g_sounds[i].autosound) {
				SNDMA_DeactivateSound(&g_sounds[i]);
			}
		}
	}

    /*
    Quake 2 is strange when it comes to what they call "autosounds", which is basically a looping
    sound that's tied to an entity. The system they've gone with works well with their DMA mixing
    engine, but it doesn't translate very well to miniaudio's more persistent object data design.

    What's basically happening is that every frame, Quake 2 provides a list of entities that have
    a looping sound that needs to play. The entity will have a "sound" variable is an index into
    the `cl.sound_precache` array which refers to a sfx_t object. Then a channel is allocated for
    the sound, with the playback position of the channel set (every frame) based on the total
    running time of the sound engine, which is modded with the length of the sound. This works
    really well for a DMA style mixer, but not with miniaudio which prefers persistent `ma_sound`
    objects.

    In order to avoid making changes to the server side, we'll need to come up with a way to make
    this work using the data we have available to us here. One theoretical thing to consider is
    entity recycling. I'm not sure if Quake 2 does thing, but in my head I can imagine it being a
    possibility - one entity is assigned a sound, it's killed, but then that entity number is
    reused, but with a different sound. We need to make sure we handle this scenario. From what I
    can tell, Quake 2 simply doesn't report entities that no longer have a sound associated with
    them.

    The solution I'm experimenting with is to maintain a list of entnum/sft_t pairs. Each frame
    we'll build a list of these pairs. Any of these pairs that do not exist in the list of playing
    sounds we'll deactivate.
    */

    /* Step 0: Clear our pairs array so we can check sfx for NULL. */
    memset(sfxent, 0, sizeof(sfxent));

    /* Step 1: Gather our entnum/sfx pairs. */
    for (i = 0; i < cl.frame.num_entities; i++) {
        sfx_t* sfx;
        num = (cl.frame.parse_entities + i)&(MAX_PARSE_ENTITIES-1);
		ent = &cl_parse_entities[num];

        if (cl_parse_entities[num].sound != 0) {
            sfx = cl.sound_precache[cl_parse_entities[num].sound];
            if (sfx != NULL) {
                sfxent[i].entnum = cl_parse_entities[num].number;
                sfxent[i].sfx    = sfx;
            }
        }
    }

    /* Step 2: Deactivate any pairs that are using a different sound (this is the recycling case). */
    for (i = 0; i < cl.frame.num_entities; i++) {
        sndma_sound_t* sound;
		int index;

		index = g_autosoundIndex[sfxent[i].entnum];
		if (index > 0) {
			sound = &g_sounds[index-1];
			if (sound->active && sound->entnum == sfxent[i].entnum && sound->sfx != sfxent[i].sfx) {
				//Com_Printf("Sound entity %d recycled: %s replaced with %s\n", sound->entnum, sound->sfx->name, sfxent[i].sfx->name);
				SNDMA_DeactivateSound(sound);
			}
		}
    }

    /* Step 3: Deactivate any pairs that are no longer active. */
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_sounds[i].active && g_sounds[i].autosound) {
            qboolean activeThisFrame = false;

			if (!cl_paused->value) {
				int j;
				for (j = 0; j < cl.frame.num_entities; j++) {
					if (g_sounds[i].autosound && g_sounds[i].entnum == sfxent[j].entnum && g_sounds[i].sfx == sfxent[j].sfx) {
						activeThisFrame = true;
						break;
					}
				}
			}

            if (!activeThisFrame) {
                //Com_Printf("Sound entity %d deactivated: %s\n", g_persistentSounds[i].entnum, g_persistentSounds[i].sfx->name);
				SNDMA_DeactivateSound(&g_sounds[i]);
            }
        }
    }

    /* Step 4: Activate new pairs, if we can find an available slot. */
	if (cl_paused->value) {
		return;
	}

    for (i = 0; i < cl.frame.num_entities; i++) {
        if (sfxent[i].sfx != NULL) {
			int index = g_autosoundIndex[sfxent[i].entnum];
			if (index == 0) {
				int j;
				for (j = 0; j < MAX_CHANNELS; j += 1) {
					if (!g_sounds[j].active) {
						/* Found a slot. */
						//Com_Printf("Sound entity %d activated: %s\n", sfxent[i].entnum, sfxent[i].sfx->name);
						g_autosoundIndex[sfxent[i].entnum] = j+1;	/* +1 to make it one based. */
						SNDMA_ActivatePersistentSound(sfxent[i].entnum, sfxent[i].sfx);
						break;
					}
				}
			}
        }
    }
}

static qboolean SNDMA_ActivateSoundFromPlaysound (sndma_sound_t *snd, playsound_t *ps)
{
	qboolean result;
	sndma_activation_t props;

	props.sfx          = ps->sfx; 
	props.volume       = ps->volume;
	props.attenuation  = ps->attenuation;
	props.entnum       = ps->entnum;
	props.entchannel   = ps->entchannel;
	props.loop         = false;
	props.autosound    = false;
	props.fixed_origin = ps->fixed_origin;
	VectorCopy(ps->origin, props.origin);
	props.begin        = ps->begin;

	result = SNDMA_ActivateSound(snd, &props);

	S_FreePlaysound(ps);    /* We're done with the playsound. */
    return result;
}

static void SNDMA_DeactivateSound (sndma_sound_t *snd)
{
	if (snd->autosound) {
		g_autosoundIndex[snd->entnum] = 0;
	}

	ma_sound_uninit(&snd->sound);
    memset(snd, 0, sizeof(*snd));
}

static void SNDMA_UpdateSound (sndma_sound_t *snd)
{
	/* If the sound is at the end we can deactivate the sound. */
	if (!ma_sound_at_end(&snd->sound)) {
		/* Sound is not at the end, so update the position. */
        vec3_t entorigin;

        /* From S_Spatialize() - anything coming from the view entity will always be full volume */
	    if (snd->entnum == cl.playernum+1) {
            ma_sound_set_spatialization_enabled(&snd->sound, MA_FALSE);
	    } else {
            if (snd->fixed_origin) {
                VectorCopy(snd->origin, entorigin);
            } else {
                CL_GetEntitySoundOrigin(snd->entnum, entorigin);
            }

            ma_sound_set_position(&snd->sound, entorigin[0], entorigin[2], -entorigin[1]);
        }
						

		/* We could also use the previous and current state of the entity to calculate velocity for doppler. */
		/* TODO: Implement me. */
	}
}

void SNDMA_Update (vec3_t origin, vec3_t forward, vec3_t right, vec3_t up)
{
	int i;
	ma_uint64 soundTime;
	playsound_t	*ps;

	/* Stop all sounds if the loading screen is up. */
	if (cls.disable_screen) {
		SNDMA_StopAllSounds();
		return;
	}

	/* Make sure we update the volume in case the user changed it in the settings. */
	ma_engine_set_volume(&g_audioEngine, s_volume->value);


	/* Update the engine's listener. Note that miniaudio and Quake2 have different coordinate systems, so needs a conversion. */
	ma_engine_listener_set_position( &g_audioEngine, 0, origin[0],  origin[2],  -origin[1] );
	ma_engine_listener_set_world_up( &g_audioEngine, 0, up[0],      up[2],      -up[1]     );
	ma_engine_listener_set_direction(&g_audioEngine, 0, forward[0], forward[2], -forward[1]);

    //Com_Printf("forward=%f %f %f\n", forward[0], forward[2], -forward[1]);

    /* Any completed sounds need to be deactivated to make room for new sounds. */
    for (i = 0; i < MAX_CHANNELS; i += 1) {
		if (g_sounds[i].active && ma_sound_at_end(&g_sounds[i].sound)) {
            SNDMA_DeactivateSound(&g_sounds[i]);
		}
	}

    soundTime = ma_engine_get_time(&g_audioEngine);

    /* Now we need to allocate a ma_sound object for any pending sounds in the playsound list. */
	ps = s_pendingplays.next;
    for (;;) {
        sndma_sound_t *snd;

		if (ps == &s_pendingplays) {
			break;	/* No more pending sounds. */
        }

		if (/*ps->sfx != NULL && */ps->begin <= soundTime) {
			/* We will choose a sound, and then set it's start time. */
			snd = SNDMA_PickTransientSound(ps->entnum, ps->entchannel);
			if (snd == NULL) {
				S_FreePlaysound(ps);
				ps = s_pendingplays.next;
				continue;
			}

			//Com_Printf("Activating Sound: %s\n", ps->sfx->name);
			SNDMA_ActivateSoundFromPlaysound(snd, ps);
			ps = s_pendingplays.next;
		} else {
			ps = ps->next;
		}
    }

    /* Activate persistent sounds (autosounds). */
    SNDMA_ActivatePersistentSounds();

    /* All active sounds need to have their positions updated. */
	for (i = 0; i < MAX_CHANNELS; i += 1) {
		if (g_sounds[i].active) {
            SNDMA_UpdateSound(&g_sounds[i]);
		}
	}

	/*
	Make sure the engine's timer is in sync with the server. Not doing this will result in delayed sounds
	not always getting fired at the right time.
	*/
	if (cl.frame.servertime * 0.001 * dma.speed > ma_engine_get_time(&g_audioEngine)) {
		ma_engine_set_time(&g_audioEngine, cl.frame.servertime * 0.001 * dma.speed);
	}
}

void SNDMA_StopAllSounds (void)
{
	int i;

	for (i = 0; i < MAX_CHANNELS; i += 1) {
		if (g_sounds[i].active) {
            SNDMA_DeactivateSound(&g_sounds[i]);
		}
	}

	SNDMA_FreeRawSamples();
}

int SNDMA_GetTime (void)
{
	return (int)ma_engine_get_time(&g_audioEngine);
}


static qboolean g_isAudioFenceInitialized = false;
static ma_fence g_audioFence;

void SNDMA_BeginRegistration (void)
{
	if (!g_isAudioFenceInitialized) {
		if (ma_fence_init(&g_audioFence) == MA_SUCCESS) {
			g_isAudioFenceInitialized = true;
		}
	}
}

void SNDMA_EndRegistration (void)
{
	if (g_isAudioFenceInitialized) {
		ma_fence_wait(&g_audioFence);
		ma_fence_uninit(&g_audioFence);
		g_isAudioFenceInitialized = false;
		
	}
}

sfxcache_t* SNDMA_LoadSound (sfx_t *sfx, const char* name)
{
	ma_result result;
	ma_pipeline_notifications notifications;
	qboolean waitForFullDecode = true;
	sfxcache_t *sc;

	sc = sfx->cache = Z_Malloc(sizeof(sfxcache_t));
	if (sc == NULL) {
		return NULL;
	}

	notifications = ma_pipeline_notifications_init();

	/* If we have a fence, use it. We'll use this to ensure  */
	if (g_isAudioFenceInitialized) {
		if (waitForFullDecode) {
			notifications.done.pFence = &g_audioFence;
		} else {
			notifications.init.pFence = &g_audioFence;
		}
	}

	result = ma_resource_manager_data_source_init(g_audioEngine.pResourceManager, name, MA_DATA_SOURCE_FLAG_DECODE | MA_DATA_SOURCE_FLAG_ASYNC, &notifications, &sc->ds);
	if (result != MA_SUCCESS) {
        Com_Printf("Failed to load audio data source (%d)\n", result);
		Z_Free(sc);
		return NULL;
	}

	return sc;
}




/*
Raw samples are processed using a custom data source which is backed by a ring buffer. Data is
written to the ring buffer from S_RawSamples() and read from 
*/
typedef struct
{
    ma_data_source_base ds;
    ma_pcm_rb rb;
} raw_samples_ds_t;

ma_result raw_samples_ds_read(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
{
    raw_samples_ds_t* rs = (raw_samples_ds_t*)pDataSource;
    ma_result result = MA_SUCCESS;
    ma_uint64 totalFramesRead = 0;

    /* Keep going until we have consumed every frame or we run out of room in the ring buffer. */
    while (totalFramesRead < frameCount) {
        ma_uint32 framesRemaining = (ma_uint32)(frameCount - totalFramesRead);
        ma_uint32 framesToRead = framesRemaining;
        void* pData;

        result = ma_pcm_rb_acquire_read(&rs->rb, &framesToRead, &pData);
        if (result != MA_SUCCESS) {
            break;
        }

        ma_copy_pcm_frames(ma_offset_pcm_frames_ptr(pFramesOut, totalFramesRead, rs->rb.format, rs->rb.channels), pData, framesToRead, rs->rb.format, rs->rb.channels);
        totalFramesRead += framesToRead;

        //Com_Printf("frameCount = %d; framesToRead = %d\n", (int)frameCount, (int)framesToRead);

        result = ma_pcm_rb_commit_read(&rs->rb, framesToRead, pData);
        if (result != MA_SUCCESS) {
            break;
        }
    }
    
    *pFramesRead = totalFramesRead;
    //Com_Printf("frameCount = %d; totalFramesRead = %d\n", (int)frameCount, (int)totalFramesRead);

    return result;
}

ma_result raw_samples_ds_seek(ma_data_source* pDataSource, ma_uint64 frameIndex)
{
    return MA_INVALID_OPERATION;    /* Cannot seek in a ring buffer. */
}

ma_result raw_samples_get_data_format(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate)
{
    raw_samples_ds_t* rs = (raw_samples_ds_t*)pDataSource;

    *pFormat   = rs->rb.format;
    *pChannels = rs->rb.channels;
    *pSampleRate = 0;   /* There's no notion of a sample rate in a ring buffer. */

    return MA_SUCCESS;
}

static ma_data_source_vtable g_rawSamplesVTable =
{
    raw_samples_ds_read,
    NULL,   /* onSeek() - Seeking doesn't make sense here. */
    NULL,   /* onMap() */
    NULL,   /* onUnmap() */
    raw_samples_get_data_format,
    NULL,   /* onGetCursor() - There's no notion of a cursor in a ring buffer. */
    NULL    /* onGetLength() - There's no notion of a length in a ring buffer. */
};

/*
The sound used for playing back data written to by S_RawSamples. this uses a custom data source
which is a ring buffer. When the read pointer reaches the write pointer, the data sources reports
MA_AT_END which will stop the sound.
*/
static void* g_rawSamplesBuffer;
static raw_samples_ds_t g_rawSamplesDS;
static ma_sound g_rawSamplesSound;
static qboolean g_isRawSamplesInitialized = false;
static ma_data_converter g_rawSamplesConverter;

void SNDMA_InitRawSamples()
{
    ma_result result;
    ma_format format;
    ma_uint32 channels;
    int bufferSizeInFrames;
    ma_data_source_config dataSourceConfig;

    if (g_isRawSamplesInitialized) {
        return; /* Already initialized. Don't do anything. */
    }

    dataSourceConfig = ma_data_source_config_init();
    dataSourceConfig.vtable = &g_rawSamplesVTable;

    result = ma_data_source_init(&dataSourceConfig, &g_rawSamplesDS.ds);
    if (result != MA_SUCCESS) {
        Com_Printf("Failed to initialize base data source for raw samples.\n");
        return;
    }

    format   = ma_format_s16;
    channels = ma_engine_get_channels(&g_audioEngine);
    bufferSizeInFrames = 8192;

    g_rawSamplesBuffer = Z_Malloc(bufferSizeInFrames * ma_get_bytes_per_frame(format, channels));
    if (g_rawSamplesBuffer == NULL) {
        Com_Printf("Failed to allocate memory for raw samples buffer.\n");
        return;
    }

    result = ma_pcm_rb_init(format, channels, bufferSizeInFrames, g_rawSamplesBuffer, NULL, &g_rawSamplesDS.rb);
    if (result != MA_SUCCESS) {
        Com_Printf("Failed to initialized ring buffer for raw samples.\n");
        Z_Free(g_rawSamplesBuffer);
        ma_data_source_uninit(&g_rawSamplesDS.ds);
        return;
    }

    /* Seek the ring buffer's write pointer forward just a little bit to give us some breathing room for reading. */
    ma_pcm_rb_seek_write(&g_rawSamplesDS.rb, g_audioEngine.pDevice->playback.internalPeriodSizeInFrames);

    result = ma_sound_init_from_data_source(&g_audioEngine, &g_rawSamplesDS, 0, NULL, &g_rawSamplesSound);
    if (result != MA_SUCCESS) {
        Com_Printf("Failed to initialized sound for raw samples.\n");
        ma_pcm_rb_uninit(&g_rawSamplesDS.rb);
        Z_Free(g_rawSamplesBuffer);
        ma_data_source_uninit(&g_rawSamplesDS.ds);
        return;
    }

    /* Done. */
    g_isRawSamplesInitialized = true;
}

void SNDMA_FreeRawSamples()
{
    if (g_isRawSamplesInitialized) {
        ma_sound_uninit(&g_rawSamplesSound);
        ma_pcm_rb_uninit(&g_rawSamplesDS.rb);
        Z_Free(g_rawSamplesBuffer);
        ma_data_source_uninit(&g_rawSamplesDS.ds);
        g_isRawSamplesInitialized = false;
    }
}

void SNDMA_RawSamples (int samples, int rate, int width, int channels, byte *data)
{
	ma_result result;
    ma_format srcFormat;
    ma_uint32 totalFramesWritten = 0;
    ma_uint32 totalFramesRead = 0;

	/* Don't do anything if the loading screen is up or else we'll get glitching. */
	if (cls.disable_screen) {
		SNDMA_FreeRawSamples();
		return;
	}

    /* Initialize first. */
    SNDMA_InitRawSamples();

    if (width == 1) {
        srcFormat = ma_format_u8;
    } else {
        srcFormat = ma_format_s16;
    }

    /*
    Keep going until we've written the entire input data. This is annoying because we need to do
    resampling. From what I can tell, Quake 2 only uses this function for cinematics. I'm going
    to use a persistent data converter for this. When the input data changes, we'll just reinit
    the data converter.
    */
    if (g_rawSamplesConverter.config.formatIn != srcFormat || g_rawSamplesConverter.config.channelsIn != channels || g_rawSamplesConverter.config.sampleRateIn != rate) {
        /* Reinitialization of the data converter is necessary. */
        if (g_rawSamplesConverter.config.formatOut != ma_format_unknown) {
            ma_data_converter_uninit(&g_rawSamplesConverter);
        }

        ma_data_converter_config config;
        config = ma_data_converter_config_init(srcFormat, g_rawSamplesDS.rb.format, channels, g_rawSamplesDS.rb.channels, rate, ma_engine_get_sample_rate(&g_audioEngine));
        config.resampling.algorithm = ma_resample_algorithm_linear;
        config.resampling.linear.lpfOrder = 0;  /* No need to any filtering here - everything in Quake 2 is low quality anyway. */

        result = ma_data_converter_init(&config, &g_rawSamplesConverter);
        if (result != MA_SUCCESS) {
            return; /* Failed to initialize the data converter. */
        }
    }

    while (totalFramesRead < (ma_uint32)samples) {  /* Just keep reading until all input frames have been consumed. */
        ma_uint64 framesToRead = (ma_uint32)samples - totalFramesRead;
        ma_uint32 framesToWrite = 4096;
        ma_uint64 framesRead;
        ma_uint64 framesWritten;
        void* pOutputData;

        result = ma_pcm_rb_acquire_write(&g_rawSamplesDS.rb, &framesToWrite, &pOutputData);
        if (result != MA_SUCCESS) {
            break;
        }

        framesRead    = framesToRead;
        framesWritten = framesToWrite;

        ma_data_converter_process_pcm_frames(&g_rawSamplesConverter, ma_offset_pcm_frames_ptr(data, totalFramesRead, srcFormat, channels), &framesRead, pOutputData, &framesWritten);
            
        totalFramesWritten += framesWritten;
        totalFramesRead    += framesRead;

        result = ma_pcm_rb_commit_write(&g_rawSamplesDS.rb, framesWritten, pOutputData);
        if (result != MA_SUCCESS) {
            break;
        }
    }

    /* Now that the data has been written we can start the sound if necessary. */
    if (totalFramesWritten > 0) {
        if (ma_sound_is_playing(&g_rawSamplesSound) == MA_FALSE || ma_sound_at_end(&g_rawSamplesSound)) {
            //Com_Printf("Starting raw samples sound... ");
            result = ma_sound_start(&g_rawSamplesSound);
            if (result != MA_SUCCESS) {
                //Com_Printf("failed\n");
            } else {
                //Com_Printf("ok\n");
            }
        }
    }
}
