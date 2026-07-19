/*
 * audio.c - Audio system implementation using SDL2_mixer
 */
#include "audio.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

bool audio_init(AudioSystem *audio) {
    memset(audio, 0, sizeof(*audio));
    audio->sfx_vol   = 1.0f;
    audio->music_vol = 0.5f;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        OL_ERR("SDL_Init AUDIO: %s\n", SDL_GetError()); return false;
    }

    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        OL_ERR("Mix_OpenAudio: %s\n", Mix_GetError()); return false;
    }

    Mix_AllocateChannels(AUDIO_CHANNELS);
    Mix_Volume(-1, (int)(MIX_MAX_VOLUME * audio->sfx_vol));
    Mix_VolumeMusic((int)(MIX_MAX_VOLUME * audio->music_vol));

    audio->initialized = true;
    OL_LOG("Audio initialized (44100 Hz, stereo)\n");
    return true;
}

void audio_shutdown(AudioSystem *audio) {
    if (!audio->initialized) return;
    Mix_HaltMusic();
    Mix_HaltChannel(-1);

    /* Free chunks */
    for (u32 i = 0; i < audio->sound_count; i++) {
        Mix_Chunk *chunk = (Mix_Chunk *)audio->sounds[i].chunk_ptr;
        if (chunk) Mix_FreeChunk(chunk);
    }

    Mix_CloseAudio();
    audio->initialized = false;
}

u32 audio_load_wav(AudioSystem *audio, const char *name,
                   const u8 *wav_data, u32 size) {
    if (!audio->initialized) return 0;
    if (audio->sound_count >= AUDIO_MAX_SOUNDS) return 0;

    SDL_RWops *rw = SDL_RWFromConstMem(wav_data, (int)size);
    if (!rw) return 0;

    Mix_Chunk *chunk = Mix_LoadWAV_RW(rw, 1); /* 1 = free rw after */
    if (!chunk) {
        OL_WARN("Failed to load WAV '%s': %s\n", name, Mix_GetError());
        return 0;
    }

    u32 id = audio->sound_count++;
    audio->sounds[id].chunk_ptr = (uintptr_t)chunk;
    snprintf(audio->sounds[id].name, sizeof(audio->sounds[id].name), "%s", name);
    audio->sounds[id].loaded = true;
    return id + 1; /* 1-based */
}

int audio_play(AudioSystem *audio, u32 sound_id) {
    if (!audio->initialized || sound_id == 0 || sound_id > audio->sound_count) return -1;
    Mix_Chunk *chunk = (Mix_Chunk *)(uintptr_t)audio->sounds[sound_id - 1].chunk_ptr;
    if (!chunk) return -1;
    return Mix_PlayChannel(-1, chunk, 0);
}

int audio_play_looping(AudioSystem *audio, u32 sound_id) {
    if (!audio->initialized || sound_id == 0 || sound_id > audio->sound_count) return -1;
    Mix_Chunk *chunk = (Mix_Chunk *)(uintptr_t)audio->sounds[sound_id - 1].chunk_ptr;
    if (!chunk) return -1;
    return Mix_PlayChannel(-1, chunk, -1); /* -1 loops = infinite */
}

bool audio_play_music(AudioSystem *audio, const char *ogg_path) {
    if (!audio->initialized) return false;
    Mix_HaltMusic();
    if (!ogg_path) return true;

    Mix_Music *music = Mix_LoadMUS(ogg_path);
    if (!music) {
        OL_WARN("Failed to load music '%s': %s\n", ogg_path, Mix_GetError());
        return false;
    }
    Mix_PlayMusic(music, -1); /* loop forever */
    return true;
}

void audio_stop_all(AudioSystem *audio) {
    if (!audio->initialized) return;
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
}

void audio_set_sfx_volume(AudioSystem *audio, float vol) {
    audio->sfx_vol = OL_CLAMP(vol, 0.0f, 1.0f);
    Mix_Volume(-1, (int)(MIX_MAX_VOLUME * audio->sfx_vol));
}

void audio_set_music_volume(AudioSystem *audio, float vol) {
    audio->music_vol = OL_CLAMP(vol, 0.0f, 1.0f);
    Mix_VolumeMusic((int)(MIX_MAX_VOLUME * audio->music_vol));
}
