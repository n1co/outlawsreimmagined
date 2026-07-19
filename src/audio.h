/*
 * audio.h - Audio system (sound effects and music)
 *
 * Uses SDL2_mixer for audio playback.
 * WAV files are loaded from LAB archives.
 * OGG Vorbis music is streamed from the MUSIC/ directory.
 */
#pragma once

#include "engine.h"

#define AUDIO_MAX_SOUNDS 256
#define AUDIO_CHANNELS   32

typedef struct {
    uintptr_t chunk_ptr; /* Mix_Chunk* stored as integer to avoid including SDL headers */
    char name[64];
    bool loaded;
} SoundEffect;

typedef struct {
    SoundEffect sounds[AUDIO_MAX_SOUNDS];
    u32         sound_count;
    float       sfx_vol;    /* 0.0 to 1.0 */
    float       music_vol;
    bool        initialized;
} AudioSystem;

/* Initialize SDL2_mixer audio. */
bool audio_init(AudioSystem *audio);

/* Shutdown audio. */
void audio_shutdown(AudioSystem *audio);

/* Load a sound effect from memory (WAV data). Returns sound ID or 0 on error. */
u32 audio_load_wav(AudioSystem *audio, const char *name, const u8 *wav_data, u32 size);

/* Play a loaded sound once. Returns channel index, or -1 on error. */
int audio_play(AudioSystem *audio, u32 sound_id);

/* Play a loaded sound looping indefinitely. Returns channel index, or -1. */
int audio_play_looping(AudioSystem *audio, u32 sound_id);

/* Play music from a file path (OGG). Pass NULL to stop music. */
bool audio_play_music(AudioSystem *audio, const char *ogg_path);

/* Stop all sounds. */
void audio_stop_all(AudioSystem *audio);

/* Set volumes (0.0 to 1.0). */
void audio_set_sfx_volume(AudioSystem *audio, float vol);
void audio_set_music_volume(AudioSystem *audio, float vol);
