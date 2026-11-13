// Sound: short clips held in memory, long music streamed from its file, mixed here and handed to SDL.
#pragma once
#include <cstdint>
#include <string>

namespace audio {

typedef uint32_t Handle;
const int GROUPS = 8;   // 0 is "everything else"; a game names the rest (music, effects, voices...)

bool init();
void shutdown();

// .wav or .ogg from the game's folder. stream: play it straight from the file (music) instead of unpacking it all.
// Returns 0 if the file is missing or can't be read.
Handle load(const std::string& name, bool stream);
// Starts a sound and returns a handle for changing it while it plays. Volumes are in decibels: 0 is full, -6 about half.
Handle play(Handle sound, float volumeDb, float pitch, bool loop, int group);
void stop(Handle voice, float fadeSeconds);
bool playing(Handle voice);
void set_volume(Handle voice, float volumeDb);
void set_pitch(Handle voice, float pitch);
void set_pan(Handle voice, float pan);              // -1 left to 1 right
void set_lowpass(Handle voice, float cutoffHz);     // muffles the sound. 0 turns it off
// Places a sound in the world. It is full volume within minRadius and silent beyond maxRadius.
void set_position(Handle voice, const float pos[3], float minRadius, float maxRadius);
void listener(const float pos[3], const float right[3]);

void group_volume(int group, float volumeDb, float fadeSeconds);
void group_pause(int group, bool paused);
void suspend(bool on);   // the app went to the background or came back

int voices();            // how many sounds are playing
void release(bool everything);

}  // namespace audio
