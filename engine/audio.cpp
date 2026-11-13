#include "audio.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gfx.h"
#include "pool.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace audio {
namespace {

const int RATE = 48000;
const int MAX_VOICES = 32;
const int CHUNK = 4096;

struct Sound {
    std::vector<float> pcm;   // unpacked clips: 1 or 2 channels at RATE
    int channels = 1;
    Sint64 frames = 0;
    int rate = RATE;
    // Streamed sounds keep the packed file and unpack a little at a time.
    std::vector<unsigned char> file;
    stb_vorbis* decoder = nullptr;
    std::vector<float> chunk;
    Sint64 chunkStart = 0, chunkFrames = 0;
    int fileChannels = 1;
    std::string key;
    Sint64 mtime = 0;
};

struct Voice {
    bool active = false;
    uint16_t gen = 1;
    Handle sound = 0;
    double pos = 0;
    float pitch = 1, gain = 1, pan = 0;
    bool loop = false, spatial = false;
    float at[3] = {0, 0, 0}, minRadius = 1, maxRadius = 50;
    float left = 0, right = 0;       // the gains in use, eased towards their targets
    bool started = false;
    float fade = 1, fadeStep = 0;    // for stopping without a click
    float lowpass = 1, lpLeft = 0, lpRight = 0;
    int group = 0;
    Uint64 born = 0;
};

SDL_AudioStream* stream = nullptr;
Pool<Sound> sounds;
Voice voiceList[MAX_VOICES];
float groupGain[GROUPS], groupTarget[GROUPS], groupStep[GROUPS];
bool groupPaused[GROUPS];
float ears[3] = {0, 0, 0}, earRight[3] = {1, 0, 0};
Uint64 counter = 0;
std::vector<float> mix;

float from_db(float db) { return db <= -80.0f ? 0.0f : powf(10.0f, db / 20.0f); }

Sint64 file_time(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) ? (Sint64)info.modify_time : 0;
}

struct Lock {
    Lock() { if (stream) SDL_LockAudioStream(stream); }
    ~Lock() { if (stream) SDL_UnlockAudioStream(stream); }
};

Voice* voice_for(Handle h) {
    uint32_t i = h & 0xFFFF;
    if (i == 0 || i > MAX_VOICES) return nullptr;
    Voice& v = voiceList[i - 1];
    return (v.active && v.gen == (h >> 16)) ? &v : nullptr;
}

// One frame of a sound as left and right. Streamed sounds unpack the next piece when the playhead reaches it.
void frame_at(Sound& s, Sint64 index, float out[2]) {
    out[0] = out[1] = 0;
    if (index < 0 || index >= s.frames) return;
    if (!s.decoder) {
        const float* p = &s.pcm[(size_t)index * (size_t)s.channels];
        out[0] = p[0];
        out[1] = s.channels > 1 ? p[1] : p[0];
        return;
    }
    if (index < s.chunkStart || index >= s.chunkStart + s.chunkFrames) {
        Sint64 keep = 0;
        if (index == s.chunkStart + s.chunkFrames && s.chunkFrames > 0) {
            // Carry the last frame over, so blending between two frames never has to look backwards.
            memmove(&s.chunk[0], &s.chunk[(size_t)(s.chunkFrames - 1) * (size_t)s.fileChannels], sizeof(float) * (size_t)s.fileChannels);
            keep = 1;
        } else {
            stb_vorbis_seek(s.decoder, (unsigned int)index);
        }
        int got = stb_vorbis_get_samples_float_interleaved(s.decoder, s.fileChannels, &s.chunk[(size_t)keep * (size_t)s.fileChannels], (CHUNK - 1) * s.fileChannels);
        s.chunkStart = index - keep;
        s.chunkFrames = keep + got;
        if (index >= s.chunkStart + s.chunkFrames) return;
    }
    const float* p = &s.chunk[(size_t)(index - s.chunkStart) * (size_t)s.fileChannels];
    out[0] = p[0];
    out[1] = s.fileChannels > 1 ? p[1] : p[0];
}

void mix_voice(Voice& v, float* out, int frames) {
    Sound* s = sounds.get(v.sound);
    if (!s || s->frames <= 0) {
        v.active = false;
        return;
    }
    if (groupPaused[v.group]) return;

    // Where the sound sits between the ears, and how loud it is from here.
    float gain = v.gain * groupGain[v.group], pan = v.pan;
    if (v.spatial) {
        float to[3] = {v.at[0] - ears[0], v.at[1] - ears[1], v.at[2] - ears[2]};
        float d = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        float span = SDL_max(v.maxRadius - v.minRadius, 0.001f);
        if (d > v.minRadius) gain *= d >= v.maxRadius ? 0.0f : (v.minRadius / d) * (1.0f - (d - v.minRadius) / span);
        pan = d > 0.001f ? SDL_clamp((to[0] * earRight[0] + to[1] * earRight[1] + to[2] * earRight[2]) / d, -1.0f, 1.0f) : 0.0f;
    }
    float angle = (SDL_clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.78539816f;   // equal power: no dip in the middle
    float targetLeft = gain * cosf(angle) * 1.41421356f, targetRight = gain * sinf(angle) * 1.41421356f;
    if (!v.started) {
        v.left = targetLeft;
        v.right = targetRight;
        v.started = true;
    }
    float stepLeft = (targetLeft - v.left) / (float)frames, stepRight = (targetRight - v.right) / (float)frames;
    double step = (double)v.pitch * s->rate / RATE;

    for (int i = 0; i < frames; i++) {
        if (v.pos >= (double)s->frames) {
            if (!v.loop) {
                v.active = false;
                return;
            }
            v.pos = fmod(v.pos, (double)s->frames);
        }
        Sint64 index = (Sint64)v.pos;
        float a[2], b[2], k = (float)(v.pos - (double)index);
        frame_at(*s, index, a);
        frame_at(*s, index + 1 < s->frames ? index + 1 : (v.loop ? 0 : index), b);
        float l = a[0] + (b[0] - a[0]) * k, r = a[1] + (b[1] - a[1]) * k;
        v.lpLeft += (l - v.lpLeft) * v.lowpass;
        v.lpRight += (r - v.lpRight) * v.lowpass;
        v.left += stepLeft;
        v.right += stepRight;
        out[i * 2] += v.lpLeft * v.left * v.fade;
        out[i * 2 + 1] += v.lpRight * v.right * v.fade;
        v.pos += step;
        if (v.fadeStep > 0) {
            v.fade -= v.fadeStep;
            if (v.fade <= 0) {
                v.active = false;
                return;
            }
        }
    }
}

// SDL calls this on its own thread whenever it wants more sound.
void SDLCALL fill(void*, SDL_AudioStream* to, int needed, int) {
    int frames = needed / (int)(sizeof(float) * 2);
    if (frames <= 0) return;
    mix.assign((size_t)frames * 2, 0.0f);
    for (int g = 0; g < GROUPS; g++) {
        float move = groupStep[g] * (float)frames;
        if (fabsf(groupTarget[g] - groupGain[g]) <= move || groupStep[g] <= 0) groupGain[g] = groupTarget[g];
        else groupGain[g] += groupTarget[g] > groupGain[g] ? move : -move;
    }
    for (Voice& v : voiceList)
        if (v.active) mix_voice(v, mix.data(), frames);
    // Many loud sounds together would clip harshly. Round the peaks off instead.
    for (float& x : mix) {
        float size = fabsf(x);
        if (size > 0.8f) x = (x < 0 ? -1.0f : 1.0f) * (0.8f + 0.2f * tanhf((size - 0.8f) / 0.2f));
    }
    SDL_PutAudioStreamData(to, mix.data(), frames * (int)(sizeof(float) * 2));
}

void destroy(Sound& s) {
    if (s.decoder) stb_vorbis_close(s.decoder);
    s.decoder = nullptr;
}

bool ends_with(const std::string& s, const char* tail) {
    size_t n = strlen(tail);
    return s.size() >= n && SDL_strcasecmp(s.c_str() + s.size() - n, tail) == 0;
}

// Turns any unpacked sound into what the mixer wants: floats, at most two channels, at RATE.
bool convert(Sound& s, const SDL_AudioSpec& from, const Uint8* data, int bytes) {
    SDL_AudioSpec to = {SDL_AUDIO_F32, SDL_min(from.channels, 2), RATE};
    Uint8* out = nullptr;
    int outBytes = 0;
    if (!SDL_ConvertAudioSamples(&from, data, bytes, &to, &out, &outBytes)) return false;
    s.channels = to.channels;
    s.frames = outBytes / (Sint64)(sizeof(float) * (size_t)to.channels);
    s.pcm.assign((float*)out, (float*)out + (size_t)s.frames * (size_t)to.channels);
    SDL_free(out);
    return s.frames > 0;
}

}  // namespace

bool init() {
    // On the iPhone: obey the ring/silent switch and let the player's own music keep playing, as games should.
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "ambient");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("audio: %s", SDL_GetError());
        return false;
    }
    for (int g = 0; g < GROUPS; g++) {
        groupGain[g] = groupTarget[g] = 1;
        groupStep[g] = 0;
        groupPaused[g] = false;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, fill, nullptr);
    if (!stream) {
        SDL_Log("audio: no sound output (%s)", SDL_GetError());   // the game still runs, silently
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream);
    return true;
}

void shutdown() {
    if (stream) SDL_DestroyAudioStream(stream);
    stream = nullptr;
    sounds.each([](Handle, Sound& s) { destroy(s); });
    sounds = Pool<Sound>();
}

Handle load(const std::string& name, bool streamIt) {
    std::string path = gfx::game_path(name);
    if (path.empty()) return 0;
    Handle found = 0;
    sounds.each([&](Handle h, Sound& s) {
        if (s.key == path && (s.decoder != nullptr) == streamIt) found = h;
    });
    if (found) return found;

    Sound s;
    s.key = path;
    s.mtime = file_time(path);
    bool ok = false;
    if (ends_with(path, ".ogg")) {
        size_t size = 0;
        void* data = SDL_LoadFile(path.c_str(), &size);
        if (!data) return 0;
        if (streamIt) {
            s.file.assign((unsigned char*)data, (unsigned char*)data + size);
            int problem = 0;
            s.decoder = stb_vorbis_open_memory(s.file.data(), (int)s.file.size(), &problem, nullptr);
            if (s.decoder) {
                stb_vorbis_info info = stb_vorbis_get_info(s.decoder);
                s.rate = (int)info.sample_rate;
                s.fileChannels = info.channels;
                s.frames = (Sint64)stb_vorbis_stream_length_in_samples(s.decoder);
                s.chunk.assign((size_t)CHUNK * (size_t)s.fileChannels, 0.0f);
                ok = s.frames > 0;
            }
        } else {
            int channels = 0, rate = 0;
            short* samples = nullptr;
            int frames = stb_vorbis_decode_memory((const unsigned char*)data, (int)size, &channels, &rate, &samples);
            if (frames > 0) {
                SDL_AudioSpec from = {SDL_AUDIO_S16, channels, rate};
                ok = convert(s, from, (const Uint8*)samples, frames * channels * (int)sizeof(short));
            }
            free(samples);
        }
        SDL_free(data);
    } else {
        SDL_AudioSpec from;
        Uint8* data = nullptr;
        Uint32 bytes = 0;
        if (SDL_LoadWAV(path.c_str(), &from, &data, &bytes)) {
            ok = convert(s, from, data, (int)bytes);
            SDL_free(data);
        }
    }
    if (!ok) {
        destroy(s);
        return 0;
    }
    Lock lock;
    Handle h = sounds.add(std::move(s));
    // The decoder reads from the file bytes, which have just moved with the sound. Open it again where they now live.
    Sound* placed = sounds.get(h);
    if (placed->decoder) {
        stb_vorbis_close(placed->decoder);
        int problem = 0;
        placed->decoder = stb_vorbis_open_memory(placed->file.data(), (int)placed->file.size(), &problem, nullptr);
    }
    return h;
}

Handle play(Handle sound, float volumeDb, float pitch, bool loop, int group) {
    Lock lock;
    Sound* s = sounds.get(sound);
    if (!s || !stream) return 0;
    // A streamed sound has one playhead, so starting it again restarts it.
    if (s->decoder)
        for (Voice& v : voiceList)
            if (v.active && v.sound == sound) v.active = false;
    int slot = -1;
    for (int i = 0; i < MAX_VOICES && slot < 0; i++)
        if (!voiceList[i].active) slot = i;
    if (slot < 0) {
        // All in use: the oldest one-shot sound gives way.
        for (int i = 0; i < MAX_VOICES; i++)
            if (!voiceList[i].loop && (slot < 0 || voiceList[i].born < voiceList[slot].born)) slot = i;
        if (slot < 0) return 0;
    }
    uint16_t gen = (uint16_t)(voiceList[slot].gen + 1);
    Voice v;
    v.gen = gen ? gen : 1;
    v.active = true;
    v.sound = sound;
    v.gain = from_db(volumeDb);
    v.pitch = SDL_clamp(pitch, 0.05f, 8.0f);
    v.loop = loop;
    v.group = SDL_clamp(group, 0, GROUPS - 1);
    v.born = ++counter;
    voiceList[slot] = v;
    return ((uint32_t)v.gen << 16) | (uint32_t)(slot + 1);
}

void stop(Handle h, float fadeSeconds) {
    Lock lock;
    Voice* v = voice_for(h);
    if (!v) return;
    v->fadeStep = 1.0f / (SDL_max(fadeSeconds, 0.005f) * RATE);
}

bool playing(Handle h) {
    Lock lock;
    return voice_for(h) != nullptr;
}

void set_volume(Handle h, float volumeDb) {
    Lock lock;
    if (Voice* v = voice_for(h)) v->gain = from_db(volumeDb);
}

void set_pitch(Handle h, float pitch) {
    Lock lock;
    if (Voice* v = voice_for(h)) v->pitch = SDL_clamp(pitch, 0.05f, 8.0f);
}

void set_pan(Handle h, float pan) {
    Lock lock;
    if (Voice* v = voice_for(h)) {
        v->pan = pan;
        v->spatial = false;
    }
}

void set_lowpass(Handle h, float cutoffHz) {
    Lock lock;
    if (Voice* v = voice_for(h)) v->lowpass = (cutoffHz <= 0 || cutoffHz >= 20000) ? 1.0f : 1.0f - expf(-6.2831853f * cutoffHz / RATE);
}

void set_position(Handle h, const float pos[3], float minRadius, float maxRadius) {
    Lock lock;
    Voice* v = voice_for(h);
    if (!v) return;
    memcpy(v->at, pos, sizeof(v->at));
    v->minRadius = SDL_max(minRadius, 0.01f);
    v->maxRadius = SDL_max(maxRadius, v->minRadius + 0.01f);
    v->spatial = true;
}

void listener(const float pos[3], const float right[3]) {
    Lock lock;
    memcpy(ears, pos, sizeof(ears));
    memcpy(earRight, right, sizeof(earRight));
}

void group_volume(int group, float volumeDb, float fadeSeconds) {
    if (group < 0 || group >= GROUPS) return;
    Lock lock;
    groupTarget[group] = from_db(volumeDb);
    groupStep[group] = fadeSeconds > 0 ? fabsf(groupTarget[group] - groupGain[group]) / (fadeSeconds * RATE) : 0;
    if (fadeSeconds <= 0) groupGain[group] = groupTarget[group];
}

void group_pause(int group, bool paused) {
    if (group < 0 || group >= GROUPS) return;
    Lock lock;
    groupPaused[group] = paused;
}

void suspend(bool on) {
    if (!stream) return;
    if (on) SDL_PauseAudioStreamDevice(stream);
    else SDL_ResumeAudioStreamDevice(stream);
}

int voices() {
    Lock lock;
    int count = 0;
    for (const Voice& v : voiceList) count += v.active ? 1 : 0;
    return count;
}

void release(bool everything) {
    Lock lock;
    for (Voice& v : voiceList) v.active = false;
    for (int g = 0; g < GROUPS; g++) {
        groupGain[g] = groupTarget[g] = 1;
        groupStep[g] = 0;
        groupPaused[g] = false;
    }
    std::vector<Handle> gone;
    sounds.each([&](Handle h, Sound& s) {
        if (everything || file_time(s.key) != s.mtime) gone.push_back(h);
    });
    for (Handle h : gone) {
        destroy(*sounds.get(h));
        sounds.remove(h);
    }
}

}  // namespace audio
