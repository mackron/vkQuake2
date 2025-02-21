/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// snd_loc.h -- private sound functions

/* Implemented in snd_miniaudio.c */
#include "../external/miniaudio/miniaudio.h"

// !!! if this is changed, the asm code must change !!!
typedef struct
{
	int			left;
	int			right;
} portable_samplepair_t;

typedef struct
{
	// For miniaudio mixer
	ma_resource_manager_data_source ds;

	// For DMA mixer
	int 		length;
	int 		loopstart;
	int 		speed;			// not needed, because converted on load?
	int 		width;
	int 		stereo;
	byte		data[1];		// variable sized
} sfxcache_t;

typedef struct sfx_s
{
	char 		name[MAX_QPATH];
	int			registration_sequence;
	sfxcache_t	*cache;
	char 		*truename;
} sfx_t;

// a playsound_t will be generated by each call to S_StartSound,
// when the mixer reaches playsound->begin, the playsound will
// be assigned to a channel
typedef struct playsound_s
{
	struct playsound_s	*prev, *next;
	sfx_t		*sfx;
	float		volume;
	float		attenuation;
	int			entnum;
	int			entchannel;
	qboolean	fixed_origin;	// use origin field instead of entnum's origin
	vec3_t		origin;
	unsigned	begin;			// begin on this sample
} playsound_t;

typedef struct
{
	int			channels;
	int			samples;				// mono samples in buffer
	int			submission_chunk;		// don't mix less than this #
	int			samplepos;				// in mono samples
	int			samplebits;
	int			speed;
	byte		*buffer;
} dma_t;

// !!! if this is changed, the asm code must change !!!
typedef struct
{
	sfx_t		*sfx;			// sfx number
	int			leftvol;		// 0-255 volume
	int			rightvol;		// 0-255 volume
	int			end;			// end time in global paintsamples
	int 		pos;			// sample position in sfx
	int			looping;		// where to loop, -1 = no looping OBSOLETE?
	int			entnum;			// to allow overriding a specific sound
	int			entchannel;		//
	vec3_t		origin;			// only use if fixed_origin is set
	vec_t		dist_mult;		// distance multiplier (attenuation/clipK)
	int			master_vol;		// 0-255 master volume
	qboolean	fixed_origin;	// use origin instead of fetching entnum's origin
	qboolean	autosound;		// from an entity->sound, cleared each frame
} channel_t;

typedef struct
{
	int			rate;
	int			width;
	int			channels;
	int			loopstart;
	int			samples;
	int			dataofs;		// chunk starts this many bytes from file start
} wavinfo_t;


/*
====================================================================

  SYSTEM SPECIFIC FUNCTIONS

====================================================================
*/

// initializes cycling through a DMA buffer and returns information on it
qboolean SNDDMA_Init(void);

// gets the current DMA position
int		SNDDMA_GetDMAPos(void);

// shutdown the DMA xfer.
void	SNDDMA_Shutdown(void);

void	SNDDMA_BeginPainting (void);

void	SNDDMA_Submit(void);

//====================================================================
#define SOUND_LOOPATTENUATE	0.003

extern int sound_started;

#define	MAX_CHANNELS			32
extern	channel_t   channels[MAX_CHANNELS];

extern	int		paintedtime;
extern	int		s_rawend;
extern	vec3_t	listener_origin;
extern	vec3_t	listener_forward;
extern	vec3_t	listener_right;
extern	vec3_t	listener_up;
extern	dma_t	dma;
extern	playsound_t	s_pendingplays;

#define	MAX_RAW_SAMPLES	8192
extern	portable_samplepair_t	s_rawsamples[MAX_RAW_SAMPLES];

extern cvar_t	*s_volume;
extern cvar_t	*s_nosound;
extern cvar_t	*s_loadas8bit;
extern cvar_t	*s_khz;
extern cvar_t	*s_show;
extern cvar_t	*s_mixahead;
extern cvar_t	*s_testsound;
extern cvar_t	*s_primary;

wavinfo_t GetWavinfo (char *name, byte *wav, int wavlength);

void S_InitScaletable (void);

sfxcache_t *S_LoadSound (sfx_t *s);

void S_IssuePlaysound (playsound_t *ps);

void S_PaintChannels(int endtime);

// picks a channel based on priorities, empty slots, number of channels
channel_t *S_PickChannel(int entnum, int entchannel);

// spatializes a channel
void S_Spatialize(channel_t *ch);

void S_TransformFilePath (char* dst, int dstCap, const char* name);
void S_FreePlaysound (playsound_t *ps);


/*
===============================================================================
BEGIN MINIAUDIO MIXING
===============================================================================
*/
#include "../external/miniaudio/miniaudio.h"

extern cvar_t *s_mixer;
extern cvar_t *s_latency;

typedef struct
{
	ma_sound sound;		/* The miniaudio sound. */
	int entnum;
	int entchannel;
	qboolean active;	/* When set to false, the sound is available for use. */
	qboolean autosound;
	qboolean fixed_origin;
	vec3_t origin;      /* Only used if fixed_origin is true. */
    sfx_t* sfx;         /* Only really required for autosounds. Used to check if an entity number has been recycled between subsequent frames. */
} sndma_sound_t;

extern int g_autosoundIndex[MAX_EDICTS];	/* Indexed with the entity number. When > 0, represents a 1-based index into g_persistentSounds. */
extern sndma_sound_t g_sounds[MAX_CHANNELS];


typedef enum
{
	sound_mixer_dma,		/* Original DMA mixer. */
	sound_mixer_miniaudio	/* miniaudio mixer via ma_engine. */
} sound_mixer_t;

/* Retrieves the mixer type based on the config variable "s_mixer". */
sound_mixer_t S_Mixer (void);


/* Initializes the miniaudio mixer. */
qboolean SNDMA_Init (void);

/* Shuts down the miniaudio mixer. */
void SNDMA_Shutdown (void);

/* Activates or deactivates the audio system. This is just a pause and resume, not an uninit/reinit. */
void SNDMA_Activate (qboolean active);

/* Called every frame to update the audio system. */
void SNDMA_Update (vec3_t origin, vec3_t forward, vec3_t right, vec3_t up);

/* Stops all sounds. */
void SNDMA_StopAllSounds (void);

/* Get's the current audio time. Used for scheduling sounds. */
int SNDMA_GetTime (void);

/* Begins miniaudio-specific sound registration. */
void SNDMA_BeginRegistration (void);

/* Ends miniaudio-specific sound registration. */
void SNDMA_EndRegistration (void);

/* Called when a sound needs to be loaded by the resource manager. */
sfxcache_t* SNDMA_LoadSound (sfx_t *sfx, const char* name);

/* For outputting raw audio data. This is used for cinematics and is called from S_RawSamples(). */
void SNDMA_RawSamples (int samples, int rate, int width, int channels, byte *data);

/*
===============================================================================
END MINIAUDIO MIXING
===============================================================================
*/