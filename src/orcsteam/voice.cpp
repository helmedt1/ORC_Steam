// Voice chat: a real IXHV2Engine behind the game's own voice system (VoiceChatSystemPC).
// The game registers the talkers, pulls our "chat data" every update and sends it to the other players
// itself (message 0xAD over our P2P transport), then hands whatever arrives to SubmitIncomingChatData.
// The data is opaque to the game, so it carries Steam's compressed voice (ISteamUser GetVoice /
// DecompressVoice); decoded voices are played with XAudio2 (XAudio2_9 / XAudio2_8, loaded at run time,
// so the DLL still loads where they are missing). Steam and XAudio2 are only used on our voice thread;
// the game's calls just queue data for it.
//
// Spatial voice (1.4): every blob also carries where the talker's character stands and in which acoustic
// environment (spatial.cpp, from the game's Wwise). The listener places the voice from that: quieter with
// distance, panned to its side, with reverb for the environment, and past voice_far a "radio" (band-passed,
// centred, at a steady level) so teammates stay in touch anywhere on the map.
//
// Settings ([orcsteam] in orcsteam.ini): voice=0 turns voice off; voice_key=<key> is push-to-talk
// (default: open mic with Steam's voice detection); voice_volume=<0..400> is a percentage for all voices
// (on top of the automatic levelling);
// voice_spatial=0 plays every voice flat (as 1.3); voice_near=<m> full volume up to here (5);
// voice_far=<m> radio from here (40); voice_radio=0 lets far voices fade out instead; voice_reverb=0 no reverb.

#define WIN32_LEAN_AND_MEAN
#include "xlive_types.h"
#include <objbase.h>
#include <mmreg.h>
#include <math.h>
#include <xaudio2.h>
#include <xaudio2fx.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overrides.h"
#include "steam.h"

typedef LONG HRESULT_T;
static const HRESULT_T kInvalidArg = 0x80070057, kUnexpected = 0x8000FFFF, kPending = 0x8000000A;
static const DWORD kPriorityNever = 0xFFFFFFFF;  // XHV_PLAYBACK_PRIORITY_NEVER: the game muted this player

static const int kMaxTalkers = 32;
static const int kMaxSpeakers = 8;
static const DWORD kTickMs = 10;  // short ticks keep each Steam chunk small
static const DWORD kTalkingMs = 300;      // IsLocalTalking / IsRemoteTalking after the last voice data
static const DWORD kSpeakerIdleMs = 30000;
static const DWORD kStatsEveryMs = 60000;

// Our data inside the game's voice message: magic, version, then [u16 length][chunk]...
// The game asks with a 420-byte buffer and writes the message into a growing RakNet-style BitStream
// (0x961D20 reallocates past its 256 bytes on the stack), so a blob may use most of that buffer.
// 1.3-test1 capped it at 200 bytes and lost more than half of the voice: one Steam chunk per tick was
// often larger.
static const BYTE kBlobMagic = 0x56, kBlobVersion = 1;
static const DWORD kBlobMax = 400;
static const DWORD kChunkMax = kBlobMax - 4;
static const int kOutChunks = 32;
static const int kInBlobs = 64;
static const DWORD kInBlobMax = 420;  // the game's own send buffer

// The spatial chunk: "OSP1", position (3 floats, Wwise space), environment count, then per environment its
// Wwise ID and amount (0..255). 1.3 takes it for a Steam voice chunk, fails to decode it and moves on.
static const BYTE kSpatialTag[4] = {'O', 'S', 'P', '1'};
static const DWORD kSpatialChunkMin = 4 + 12 + 1;
static const DWORD kSpatialChunkMax = kSpatialChunkMin + kSpatialEnvs * 5;
static const DWORD kLocalSpatialEveryMs = 100;
static const DWORD kSpatialStaleMs = 3000;  // a talker's position this old: play flat
static const DWORD kSpatialApplyMs = 50;

// Placement.
static const float kMinDistanceVolume = 0.08f;  // nearer than voice_far a voice never gets quieter than this
static const float kRadioVolume = 0.6f;
static const float kRadioHysteresis = 0.85f;  // back from radio below voice_far * this
static const float kRadioFrequency = 1800.0f, kRadioOneOverQ = 1.0f;
static const float kPanFloor = 0.25f;  // the far ear still hears a little
static const float kWetMax = 0.45f;

// A Steam chunk too big for one blob (up to ~750 bytes at the start of a phrase) goes in pieces:
// "OFR1", piece id, index, count, then up to kFragData bytes; the listener joins them and decodes the whole.
static const BYTE kFragTag[4] = {'O', 'F', 'R', '1'};
static const DWORD kFragHeader = 7, kFragData = 300, kMaxFrags = 8;

// Steam's voice comes out quiet next to the game: each voice is levelled to about this RMS (of 32767),
// boosted at most kMaxGain times, and never pushed past kPeakLimit.
static const float kTargetRms = 3500.0f, kMaxGain = 6.0f, kPeakLimit = 30000.0f, kSpeechRms = 150.0f;
static const float kGainRise = 0.25f, kGainFall = 0.7f;  // slow to boost, quick to back off a loud voice

// Playback (as in ERVoiceChat): a short prebuffer at the start of a phrase, and no growing delay.
static const DWORD kFallbackRate = 24000;
static const DWORD kPrebufferMs = 60;
static const DWORD kMaxQueuedMs = 400;
static const int kBuffers = 16;
static const DWORD kBufferSamples = 6000;  // 250 ms at 24 kHz

struct Talker { XUID xuid; bool muted; DWORD lastHeard; };
struct OutChunk { WORD len; BYTE data[kChunkMax]; };
struct InBlob { XUID from; WORD len; BYTE data[kInBlobMax]; };  // len 0: the talker left, drop its speaker

static SRWLOCK g_lock = SRWLOCK_INIT;
static bool g_settingsRead, g_enabled, g_spatial, g_radio, g_reverbOn;
static int g_key;  // virtual key for push-to-talk, 0 = open mic
static float g_volume = 1.0f, g_near = 5.0f, g_far = 40.0f;
static volatile LONG g_localActive;
static volatile LONG g_lastLocalTalk;
static Talker g_talkers[kMaxTalkers];
static int g_talkerCount;
static OutChunk g_out[kOutChunks];
static int g_outHead, g_outCount;
static InBlob g_in[kInBlobs];
static int g_inHead, g_inCount;
static bool g_localValid;  // our character's place, refreshed by the voice thread, sent with every blob
static SpatialPoint g_local;
static volatile LONG g_sentBlobs, g_sentBytes, g_recvBlobs, g_droppedIn, g_badBlobs, g_decodeErrors, g_spatialIn;
static volatile LONG g_oversize, g_overflow, g_chatCalls, g_maxChunk;  // why outgoing voice was lost, and how often the game asks
static volatile LONG g_fragmented, g_joined, g_fragLost;

static HANDLE g_thread;
static volatile LONG g_stop;

static void Lock() { AcquireSRWLockExclusive(&g_lock); }
static void Unlock() { ReleaseSRWLockExclusive(&g_lock); }

// ---- settings ----

static int ParseKey(const char* name) {
    if (!name[0]) return 0;
    if (!name[1] && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) return name[0];
    if (!name[1] && name[0] >= 'a' && name[0] <= 'z') return name[0] - 'a' + 'A';
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12) return VK_F1 + n - 1;
    }
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) return (int)strtoul(name, nullptr, 16) & 0xFF;
    static const struct { const char* name; int vk; } kNames[] = {
        {"mouse3", VK_MBUTTON}, {"mouse4", VK_XBUTTON1}, {"mouse5", VK_XBUTTON2}, {"capslock", VK_CAPITAL},
        {"alt", VK_MENU},       {"ctrl", VK_CONTROL},    {"shift", VK_SHIFT},     {"tab", VK_TAB},
        {"space", VK_SPACE},
    };
    for (const auto& k : kNames)
        if (_stricmp(name, k.name) == 0) return k.vk;
    return -1;
}

static void ReadSettings() {
    if (g_settingsRead) return;
    g_settingsRead = true;
    g_enabled = OrcsteamIniInt("voice", 1) != 0;
    char key[32];
    OrcsteamIniString("voice_key", "", key, sizeof(key));
    g_key = ParseKey(key);
    if (g_key < 0) {
        OverrideLog("!! voice: unknown voice_key '%s', using open mic", key);
        g_key = 0;
    }
    int volume = OrcsteamIniInt("voice_volume", 100);
    g_volume = (volume < 0 ? 0 : volume > 400 ? 400 : volume) / 100.0f;
    g_spatial = OrcsteamIniInt("voice_spatial", 1) != 0;
    int nearM = OrcsteamIniInt("voice_near", 5), farM = OrcsteamIniInt("voice_far", 40);
    g_near = (float)(nearM < 1 ? 1 : nearM > 100 ? 100 : nearM);
    g_far = (float)(farM < nearM + 1 ? nearM + 1 : farM > 500 ? 500 : farM);
    g_radio = OrcsteamIniInt("voice_radio", 1) != 0;
    g_reverbOn = OrcsteamIniInt("voice_reverb", 1) != 0;
    OverrideLog("voice: %s, %s%s, volume %d%%", g_enabled ? "on" : "off (voice=0)", g_key ? "push-to-talk key " : "open mic",
                g_key ? key : "", (int)(g_volume * 100 + 0.5f));
    if (g_spatial)
        OverrideLog("voice: spatial, full volume up to %.0f m, %s from %.0f m, reverb %s", g_near,
                    g_radio ? "radio" : "fading out", g_far, g_reverbOn ? "on" : "off");
    else
        OverrideLog("voice: spatial off (voice_spatial=0), every voice flat");
}

// ---- talkers (game thread, under g_lock) ----

static Talker* FindTalker(XUID xuid) {
    for (int i = 0; i < g_talkerCount; i++)
        if (g_talkers[i].xuid == xuid) return &g_talkers[i];
    return nullptr;
}

static void QueueIncoming(XUID from, const BYTE* data, DWORD len) {  // caller holds g_lock
    if (g_inCount == kInBlobs) {
        InterlockedIncrement(&g_droppedIn);
        return;
    }
    InBlob& b = g_in[(g_inHead + g_inCount++) % kInBlobs];
    b.from = from;
    b.len = (WORD)len;
    if (len) memcpy(b.data, data, len);
}

static DWORD WriteSpatialChunk(BYTE* out, const SpatialPoint& p) {  // returns the chunk's payload size
    memcpy(out, kSpatialTag, 4);
    memcpy(out + 4, p.pos, 12);
    BYTE n = (BYTE)(p.envCount < kSpatialEnvs ? p.envCount : kSpatialEnvs);
    out[16] = n;
    for (int i = 0; i < n; i++) {
        memcpy(out + 17 + i * 5, &p.envId[i], 4);
        float v = p.envValue[i] < 0 ? 0 : p.envValue[i] > 1 ? 1 : p.envValue[i];
        out[21 + i * 5] = (BYTE)(v * 255 + 0.5f);
    }
    return kSpatialChunkMin + n * 5;
}

static bool ReadSpatialChunk(const BYTE* in, DWORD len, SpatialPoint* p) {
    if (len < kSpatialChunkMin || memcmp(in, kSpatialTag, 4)) return false;
    memcpy(p->pos, in + 4, 12);
    int n = in[16];
    if (n > kSpatialEnvs || len < kSpatialChunkMin + n * 5u) return false;
    p->envCount = n;
    for (int i = 0; i < n; i++) {
        memcpy(&p->envId[i], in + 17 + i * 5, 4);
        p->envValue[i] = in[21 + i * 5] / 255.0f;
    }
    for (int i = 0; i < 3; i++)
        if (!(fabsf(p->pos[i]) < 1e6f)) return false;
    return true;
}

// ---- playback (voice thread only) ----

typedef HRESULT(__stdcall* XAudio2CreateFn)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
typedef HRESULT(__stdcall* CreateReverbFn)(IUnknown**);

struct Speaker {
    XUID xuid;
    IXAudio2SourceVoice* voice;
    DWORD submitted;  // buffers handed to the voice so far; slot = submitted % kBuffers
    DWORD sizes[kBuffers];
    short pcm[kBuffers][kBufferSamples];
    short pending[kBufferSamples];  // collected before a phrase starts
    DWORD pendingCount;
    DWORD lastPlayed;
    // where the talker said they are
    bool hasPlace;
    SpatialPoint place;
    DWORD placeTick;
    bool radio, filterSet;
    DWORD lastLog;
    float gain;  // levelling, see Level()
    // a big Steam chunk arriving in pieces
    BYTE frag[kMaxFrags * kFragData];
    DWORD fragLen;
    BYTE fragId, fragNext, fragCount;
};

static IXAudio2* g_xa;
static IXAudio2MasteringVoice* g_master;
static IXAudio2SubmixVoice* g_reverb;
static DWORD g_masterChannels = 2;
static Speaker g_speakers[kMaxSpeakers];
static DWORD g_rate = kFallbackRate;

// ---- reverb presets by environment name ----

struct Preset { const char* word; const char* name; XAUDIO2FX_REVERB_I3DL2_PARAMETERS params; float wet; };
// Checked in order: the first word found in the environment's name wins. Names come from GetIDFromString
// (spatial.cpp); an environment never hashed by name gets the generic preset.
static const Preset kPresets[] = {
    {"sewer", "sewer pipe", XAUDIO2FX_I3DL2_PRESET_SEWERPIPE, 1.0f},
    {"pipe", "sewer pipe", XAUDIO2FX_I3DL2_PRESET_SEWERPIPE, 1.0f},
    {"tunnel", "stone corridor", XAUDIO2FX_I3DL2_PRESET_STONECORRIDOR, 1.0f},
    {"corridor", "stone corridor", XAUDIO2FX_I3DL2_PRESET_STONECORRIDOR, 1.0f},
    {"hallway", "hallway", XAUDIO2FX_I3DL2_PRESET_HALLWAY, 0.9f},
    {"stair", "hallway", XAUDIO2FX_I3DL2_PRESET_HALLWAY, 0.9f},
    {"cave", "cave", XAUDIO2FX_I3DL2_PRESET_CAVE, 1.0f},
    {"garage", "parking lot", XAUDIO2FX_I3DL2_PRESET_PARKINGLOT, 0.9f},
    {"parking", "parking lot", XAUDIO2FX_I3DL2_PRESET_PARKINGLOT, 0.9f},
    {"hall", "large hall", XAUDIO2FX_I3DL2_PRESET_LARGEHALL, 0.9f},
    {"lobby", "large hall", XAUDIO2FX_I3DL2_PRESET_LARGEHALL, 0.9f},
    {"alley", "alley", XAUDIO2FX_I3DL2_PRESET_ALLEY, 0.7f},
    {"street", "city", XAUDIO2FX_I3DL2_PRESET_CITY, 0.5f},
    {"city", "city", XAUDIO2FX_I3DL2_PRESET_CITY, 0.5f},
    {"outdoor", "city", XAUDIO2FX_I3DL2_PRESET_CITY, 0.5f},
    {"outside", "city", XAUDIO2FX_I3DL2_PRESET_CITY, 0.5f},
    {"exterior", "city", XAUDIO2FX_I3DL2_PRESET_CITY, 0.5f},
    {"lab", "stone room", XAUDIO2FX_I3DL2_PRESET_STONEROOM, 0.8f},
    {"small", "small room", XAUDIO2FX_I3DL2_PRESET_SMALLROOM, 0.7f},
    {"large", "large room", XAUDIO2FX_I3DL2_PRESET_LARGEROOM, 0.9f},
    {"big", "large room", XAUDIO2FX_I3DL2_PRESET_LARGEROOM, 0.9f},
    {"room", "medium room", XAUDIO2FX_I3DL2_PRESET_MEDIUMROOM, 0.8f},
    {"office", "medium room", XAUDIO2FX_I3DL2_PRESET_MEDIUMROOM, 0.8f},
    {"interior", "medium room", XAUDIO2FX_I3DL2_PRESET_MEDIUMROOM, 0.8f},
    {"inside", "medium room", XAUDIO2FX_I3DL2_PRESET_MEDIUMROOM, 0.8f},
};
static const Preset kGenericPreset = {"", "generic", XAUDIO2FX_I3DL2_PRESET_GENERIC, 0.6f};

static const Preset& PresetFor(DWORD envId) {
    const char* name = SpatialName(envId);
    if (name) {
        char lower[40];
        size_t i = 0;
        for (; name[i] && i + 1 < sizeof(lower); i++) lower[i] = (char)tolower((unsigned char)name[i]);
        lower[i] = 0;
        for (const Preset& p : kPresets)
            if (strstr(lower, p.word)) return p;
    }
    return kGenericPreset;
}

static DWORD g_reverbEnv;  // the environment the reverb is set up for
static float g_reverbWet = kGenericPreset.wet;

static void SetReverbFor(DWORD envId) {
    if (!g_reverb || envId == g_reverbEnv) return;
    g_reverbEnv = envId;
    const Preset& p = PresetFor(envId);
    XAUDIO2FX_REVERB_PARAMETERS native;
    ReverbConvertI3DL2ToNative(&p.params, &native);
    g_reverb->SetEffectParameters(0, &native, sizeof(native));
    g_reverbWet = p.wet;
    if (!envId) return;
    // Once per environment: which name it had and which reverb it got (to tune the table later).
    static DWORD logged[64];
    static int loggedCount;
    for (int i = 0; i < loggedCount; i++)
        if (logged[i] == envId) return;
    if (loggedCount < 64) logged[loggedCount++] = envId;
    const char* name = SpatialName(envId);
    OverrideLog("voice: environment %08lX '%s' -> reverb '%s'", envId, name ? name : "?", p.name);
}

static void AudioInit() {
    static const char* kDlls[] = {"XAudio2_9.dll", "XAudio2_8.dll"};
    for (const char* dll : kDlls) {
        HMODULE m = LoadLibraryA(dll);
        XAudio2CreateFn create = m ? (XAudio2CreateFn)GetProcAddress(m, "XAudio2Create") : nullptr;
        if (!create) continue;
        if (FAILED(create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
            g_xa = nullptr;
            continue;
        }
        if (FAILED(g_xa->CreateMasteringVoice(&g_master))) {
            g_xa->Release();
            g_xa = nullptr;
            OverrideLog("!! voice: no audio output device (%s)", dll);
            return;
        }
        g_master->SetVolume(g_volume);
        XAUDIO2_VOICE_DETAILS details = {};
        g_master->GetVoiceDetails(&details);
        g_masterChannels = details.InputChannels ? details.InputChannels : 2;

        // One shared reverb (a stereo submix with the XAudio2 reverb effect); each voice sends to it.
        CreateReverbFn createReverb = (CreateReverbFn)GetProcAddress(m, "CreateAudioReverb");
        IUnknown* apo = nullptr;
        if (g_spatial && g_reverbOn && createReverb && SUCCEEDED(createReverb(&apo))) {
            XAUDIO2_EFFECT_DESCRIPTOR effect = {apo, TRUE, 2};
            XAUDIO2_EFFECT_CHAIN chain = {1, &effect};
            if (FAILED(g_xa->CreateSubmixVoice(&g_reverb, 2, details.InputSampleRate, 0, 0, nullptr, &chain))) g_reverb = nullptr;
            apo->Release();
            if (g_reverb) {
                g_reverbEnv = 1;  // force the first SetReverbFor
                SetReverbFor(0);
            }
        }
        OverrideLog("voice: playback through %s, %lu channels%s", dll, g_masterChannels,
                    g_reverb ? ", reverb ready" : (g_spatial && g_reverbOn ? ", no reverb" : ""));
        return;
    }
    OverrideLog("!! voice: XAudio2 not available, other players will not be heard");
}

static void DestroySpeaker(Speaker& s) {
    if (s.voice) s.voice->DestroyVoice();  // waits for processing to end, so the buffers are free after it
    s.voice = nullptr;
    s.xuid = 0;
    s.submitted = 0;
    s.pendingCount = 0;
    s.lastPlayed = 0;
    s.hasPlace = false;
    s.radio = s.filterSet = false;
}

static Speaker* SpeakerFor(XUID xuid, bool create) {
    Speaker* free = nullptr;
    for (Speaker& s : g_speakers) {
        if (s.xuid == xuid) return &s;
        if (!s.xuid && !free) free = &s;
    }
    if (!create || !free || !g_xa) return nullptr;
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = g_rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = g_rate * 2;
    XAUDIO2_SEND_DESCRIPTOR sends[2] = {{0, g_master}, {0, g_reverb}};
    XAUDIO2_VOICE_SENDS sendList = {g_reverb ? 2u : 1u, sends};
    if (FAILED(g_xa->CreateSourceVoice(&free->voice, &wf, g_spatial ? XAUDIO2_VOICE_USEFILTER : 0,
                                       XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, &sendList))) {
        free->voice = nullptr;
        return nullptr;
    }
    free->voice->Start();
    free->xuid = xuid;
    free->submitted = 0;
    free->pendingCount = 0;
    free->lastPlayed = GetTickCount();
    free->hasPlace = free->radio = free->filterSet = false;
    free->lastLog = 0;
    free->gain = 2.0f;  // a sensible start until the first phrase is measured
    free->fragCount = 0;
    OverrideLog("voice: playing account %lu", (DWORD)xuid);
    return free;
}

static void SubmitPcm(Speaker& s, const short* pcm, DWORD samples, DWORD queued) {
    while (samples && queued < (DWORD)kBuffers) {
        DWORD n = samples < kBufferSamples ? samples : kBufferSamples;
        int slot = s.submitted % kBuffers;
        memcpy(s.pcm[slot], pcm, n * sizeof(short));
        s.sizes[slot] = n;
        XAUDIO2_BUFFER xb = {};
        xb.AudioBytes = n * sizeof(short);
        xb.pAudioData = (const BYTE*)s.pcm[slot];
        if (FAILED(s.voice->SubmitSourceBuffer(&xb))) return;
        s.submitted++;
        queued++;
        pcm += n;
        samples -= n;
    }
}

static void Play(Speaker& s, const short* pcm, DWORD samples) {
    XAUDIO2_VOICE_STATE st = {};
    s.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    DWORD queued = st.BuffersQueued, queuedSamples = 0;
    for (DWORD i = 0; i < queued && i < (DWORD)kBuffers; i++) queuedSamples += s.sizes[(s.submitted - 1 - i) % kBuffers];
    if (queuedSamples * 1000 / g_rate > kMaxQueuedMs) return;  // far behind: drop instead of building up delay
    s.lastPlayed = GetTickCount();
    if (queued == 0) {
        // Silence before: a new phrase. Collect kPrebufferMs first, then hand it over in one piece.
        DWORD room = kBufferSamples - s.pendingCount, n = samples < room ? samples : room;
        memcpy(s.pending + s.pendingCount, pcm, n * sizeof(short));
        s.pendingCount += n;
        if (s.pendingCount * 1000 / g_rate < kPrebufferMs && n == samples) return;
        SubmitPcm(s, s.pending, s.pendingCount, 0);
        s.pendingCount = 0;
        if (n < samples) SubmitPcm(s, pcm + n, samples - n, 1);
        return;
    }
    SubmitPcm(s, pcm, samples, queued);
}

// Brings a quiet Steam voice up to a steady level: the gain follows the speech RMS of each decoded block
// (silence leaves it alone), then the block is scaled with saturation and a peak limit.
static void Level(Speaker& s, short* pcm, DWORD samples) {
    if (!samples) return;
    double sum = 0;
    int peak = 0;
    for (DWORD i = 0; i < samples; i++) {
        int v = pcm[i];
        sum += (double)v * v;
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    float rms = (float)sqrt(sum / samples);
    if (rms > kSpeechRms) {
        float target = kTargetRms / rms;
        if (target > kMaxGain) target = kMaxGain;
        if (target < 1.0f) target = 1.0f;
        s.gain += (target - s.gain) * (target < s.gain ? kGainFall : kGainRise);
    }
    float gain = s.gain;
    if (peak && peak * gain > kPeakLimit) gain = kPeakLimit / peak;
    if (gain <= 1.0f) return;
    for (DWORD i = 0; i < samples; i++) {
        float v = pcm[i] * gain;
        pcm[i] = (short)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
}

static void DecodeAndPlay(const SteamApi& steam, Speaker& s, const BYTE* data, DWORD len) {
    static short pcm[32768];
    DWORD written = 0;
    if (steam.user->DecompressVoice(data, len, pcm, sizeof(pcm), &written, g_rate) == kVoiceOK && written) {
        Level(s, pcm, written / sizeof(short));
        Play(s, pcm, written / sizeof(short));
    } else {
        InterlockedIncrement(&g_decodeErrors);
    }
}

// One piece of a big chunk; the whole is decoded once its last piece is in (a missing piece drops it).
static void JoinPiece(const SteamApi& steam, Speaker& s, const BYTE* in, DWORD len) {
    BYTE id = in[4], index = in[5], count = in[6];
    DWORD part = len - kFragHeader;
    if (!count || count > kMaxFrags || index >= count || part > kFragData) {
        InterlockedIncrement(&g_badBlobs);
        return;
    }
    if (index == 0) {
        if (s.fragCount && s.fragNext < s.fragCount) InterlockedIncrement(&g_fragLost);  // the previous one never completed
        s.fragId = id;
        s.fragCount = count;
        s.fragNext = 0;
        s.fragLen = 0;
    }
    if (!s.fragCount || id != s.fragId || index != s.fragNext) {
        if (s.fragCount) InterlockedIncrement(&g_fragLost);
        s.fragCount = 0;
        return;
    }
    memcpy(s.frag + s.fragLen, in + kFragHeader, part);
    s.fragLen += part;
    s.fragNext++;
    if (s.fragNext == s.fragCount) {
        DecodeAndPlay(steam, s, s.frag, s.fragLen);
        InterlockedIncrement(&g_joined);
        s.fragCount = 0;
    }
}

static void HandleIncoming(const SteamApi& steam, const InBlob& b) {
    if (!b.len) {  // the talker left
        if (Speaker* s = SpeakerFor(b.from, false)) DestroySpeaker(*s);
        return;
    }
    Speaker* s = SpeakerFor(b.from, true);
    if (!s) return;
    for (DWORD pos = 2; pos + 2 <= b.len;) {
        DWORD len = b.data[pos] | (b.data[pos + 1] << 8);
        pos += 2;
        if (!len || pos + len > b.len) {
            InterlockedIncrement(&g_badBlobs);
            return;
        }
        SpatialPoint place;
        if (ReadSpatialChunk(b.data + pos, len, &place)) {
            s->place = place;
            s->hasPlace = true;
            s->placeTick = GetTickCount();
            InterlockedIncrement(&g_spatialIn);
        } else if (len > kFragHeader && !memcmp(b.data + pos, kFragTag, 4)) {
            JoinPiece(steam, *s, b.data + pos, len);
        } else {
            DecodeAndPlay(steam, *s, b.data + pos, len);
        }
        pos += len;
    }
}

// ---- placement (voice thread) ----

static float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void SetFilter(Speaker& s, bool radio) {
    if (!g_spatial || (s.filterSet && s.radio == radio)) return;  // voices have a filter only with voice_spatial
    XAUDIO2_FILTER_PARAMETERS f;
    if (radio) {
        f.Type = BandPassFilter;
        f.Frequency = 2.0f * sinf(3.14159265f * kRadioFrequency / g_rate);  // XAudio2CutoffFrequencyToRadians
        f.OneOverQ = kRadioOneOverQ;
    } else {
        f.Type = LowPassFilter;
        f.Frequency = XAUDIO2_MAX_FILTER_FREQUENCY;  // off
        f.OneOverQ = 1.0f;
    }
    s.voice->SetFilterParameters(&f);
    s.radio = radio;
    s.filterSet = true;
}

// left/right gains for the master, the rest silent
static void SetPan(Speaker& s, float left, float right) {
    float matrix[8] = {};
    if (g_masterChannels == 1) {
        matrix[0] = left > right ? left : right;
    } else {
        matrix[0] = left;
        matrix[1] = right;
    }
    DWORD channels = g_masterChannels < 8 ? g_masterChannels : 8;
    s.voice->SetOutputMatrix(g_master, 1, channels, matrix);
}

static void SetWet(Speaker& s, float wet) {
    if (!g_reverb) return;
    float matrix[2] = {wet, wet};
    s.voice->SetOutputMatrix(g_reverb, 1, 2, matrix);
}

static void PlaceSpeaker(Speaker& s, const SpatialFrame* listener) {
    DWORD now = GetTickCount();
    if (!g_spatial || !listener || !s.hasPlace || now - s.placeTick > kSpatialStaleMs) {
        // As in 1.3: the talker (1.3, dead, in a menu) or we have no place.
        SetFilter(s, false);
        SetPan(s, 1.0f, 1.0f);
        SetWet(s, 0.0f);
        s.voice->SetVolume(1.0f);
        return;
    }
    float dir[3] = {s.place.pos[0] - listener->pos[0], s.place.pos[1] - listener->pos[1], s.place.pos[2] - listener->pos[2]};
    float d = sqrtf(Dot(dir, dir));
    bool radio = g_radio && (s.radio ? d > g_far * kRadioHysteresis : d > g_far);

    float volume, pan = 0.0f, wet = 0.0f;
    if (radio) {
        volume = kRadioVolume;  // centred and steady: a radio in your ear
    } else {
        volume = d <= g_near ? 1.0f : g_near / d;
        if (volume < kMinDistanceVolume) volume = kMinDistanceVolume;
        if (!g_radio && d > g_far) {
            float fade = 1.0f - (d - g_far) / (g_far * 0.25f);
            volume *= fade > 0 ? fade : 0;
        }
        if (d > 0.01f) {
            // Wwise is left-handed (X right, Y up, Z front): right = top x front.
            const float* t = listener->top;
            const float* f = listener->front;
            float right[3] = {t[1] * f[2] - t[2] * f[1], t[2] * f[0] - t[0] * f[2], t[0] * f[1] - t[1] * f[0]};
            pan = Dot(dir, right) / d;
            if (pan > 1) pan = 1;
            if (pan < -1) pan = -1;
            if (d < 1.5f) pan *= d / 1.5f;  // right next to us: barely to one side
        }
        // The talker's environment: more of it the farther they are.
        float env = 0.0f;
        for (int i = 0; i < s.place.envCount; i++)
            if (s.place.envValue[i] > env) env = s.place.envValue[i];
        float distanceShare = 0.5f + d / g_far;
        wet = env * kWetMax * g_reverbWet * (distanceShare < 1.0f ? distanceShare : 1.0f);
    }
    // Equal-power pan, normalised so the centre is 1/1 (as a flat voice), the far ear never fully silent.
    float angle = (pan + 1.0f) * 0.785398f;
    float left = 1.41421f * cosf(angle), right = 1.41421f * sinf(angle);
    left = left > 1 ? 1 : left < kPanFloor ? kPanFloor : left;
    right = right > 1 ? 1 : right < kPanFloor ? kPanFloor : right;

    SetFilter(s, radio);
    SetPan(s, left, right);
    SetWet(s, wet);
    s.voice->SetVolume(volume);

    if (OrcsteamDiag() && now - s.lastLog >= 2000 && now - s.lastPlayed < 1000) {
        s.lastLog = now;
        OverrideLog("voice: account %lu at %.1f m, %s, volume %.2f, pan %+.2f, reverb %.2f, levelling x%.1f", (DWORD)s.xuid,
                    d, radio ? "radio" : "direct", volume, pan, wet, s.gain);
    }
}

// ---- voice thread ----

static bool OurWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void LogStats() {
    static LONG last[3];
    LONG now[3] = {g_sentBlobs, g_recvBlobs, g_decodeErrors + g_badBlobs + g_droppedIn + g_oversize + g_overflow};
    if (!memcmp(now, last, sizeof(now))) return;
    memcpy(last, now, sizeof(now));
    OverrideLog("voice: sent %ld (%ld B, game asked %ld times, %ld big chunks in pieces), received %ld (%ld with a place, "
                "%ld big chunks joined, %ld incomplete); lost out: %ld too big (max chunk %ld B), %ld queue full; "
                "in: %ld queue full, %ld bad, %ld decode errors",
                g_sentBlobs, g_sentBytes, g_chatCalls, g_fragmented, g_recvBlobs, g_spatialIn, g_joined, g_fragLost,
                g_oversize, g_maxChunk, g_overflow, g_droppedIn, g_badBlobs, g_decodeErrors);
}

static DWORD WINAPI VoiceThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool recording = false, rateKnown = false, audioReady = false;
    LONG recordingLogs = 0;
    DWORD lastStats = GetTickCount(), lastLocal = 0, lastPlace = 0;
    bool localLogged = false;
    static BYTE chunk[8192];
    static InBlob work[kInBlobs];
    while (!g_stop) {
        Sleep(kTickMs);
        const SteamApi* steam = Steam();
        if (!steam) continue;
        if (!rateKnown) {
            DWORD rate = steam->user->GetVoiceOptimalSampleRate();
            if (rate >= 8000 && rate <= 48000) g_rate = rate;
            rateKnown = true;
        }
        if (!audioReady) {  // after the rate: source voices use it
            AudioInit();
            audioReady = true;
        }
        DWORD now = GetTickCount();

        // Where we are, for our outgoing blobs.
        if (g_spatial && now - lastLocal >= kLocalSpatialEveryMs) {
            lastLocal = now;
            SpatialPoint me;
            bool valid = SpatialLocalCharacter(&me);
            Lock();
            g_localValid = valid;
            if (valid) g_local = me;
            Unlock();
            if (valid && !localLogged) {
                localLogged = true;
                OverrideLog("voice: our character found at %.1f %.1f %.1f, sent with our voice", me.pos[0], me.pos[1], me.pos[2]);
            }
        }

        // Capture: only while the game has a local talker in a session (and, with push-to-talk, the key is down).
        bool want = g_localActive && (!g_key || ((GetAsyncKeyState(g_key) & 0x8000) && OurWindowFocused()));
        if (want != recording) {
            if (want) steam->user->StartVoiceRecording();
            else steam->user->StopVoiceRecording();  // Steam still hands out the tail of the phrase afterwards
            recording = want;
            if (InterlockedIncrement(&recordingLogs) <= (OrcsteamDiag() ? 1000 : 4))
                OverrideLog("voice: recording %s", want ? "started" : "stopped");
        }
        DWORD compressed = 0, raw = 0, written = 0;
        if (steam->user->GetAvailableVoice(&compressed, &raw, 0) == kVoiceOK && compressed &&
            steam->user->GetVoice(true, chunk, sizeof(chunk), &written, false, nullptr, 0, nullptr, 0) == kVoiceOK &&
            written) {
            if ((LONG)written > g_maxChunk) InterlockedExchange(&g_maxChunk, (LONG)written);
            static BYTE fragId;
            DWORD single = kChunkMax - (2 + kSpatialChunkMax);  // what fits next to our place in one blob
            DWORD pieces = written <= single ? 1 : (written + kFragData - 1) / kFragData;
            if (pieces > kMaxFrags) {
                InterlockedIncrement(&g_oversize);  // over 2.4 KB at once: Steam stalled, drop it
            } else {
                Lock();
                fragId++;
                for (DWORD i = 0; i < pieces; i++) {
                    if (g_outCount == kOutChunks) {  // the game is not taking data fast enough: drop the oldest
                        g_outHead = (g_outHead + 1) % kOutChunks;
                        g_outCount--;
                        InterlockedIncrement(&g_overflow);
                    }
                    OutChunk& c = g_out[(g_outHead + g_outCount++) % kOutChunks];
                    if (pieces == 1) {
                        c.len = (WORD)written;
                        memcpy(c.data, chunk, written);
                    } else {  // a piece: header, then its part of the chunk
                        DWORD at = i * kFragData, part = written - at < kFragData ? written - at : kFragData;
                        memcpy(c.data, kFragTag, 4);
                        c.data[4] = fragId;
                        c.data[5] = (BYTE)i;
                        c.data[6] = (BYTE)pieces;
                        memcpy(c.data + kFragHeader, chunk + at, part);
                        c.len = (WORD)(kFragHeader + part);
                    }
                }
                Unlock();
                if (pieces > 1) InterlockedIncrement(&g_fragmented);
                InterlockedExchange(&g_lastLocalTalk, (LONG)GetTickCount());
            }
        }

        // Playback of what the game handed over.
        Lock();
        int n = g_inCount;
        for (int i = 0; i < n; i++) work[i] = g_in[(g_inHead + i) % kInBlobs];
        g_inHead = (g_inHead + n) % kInBlobs;
        g_inCount = 0;
        Unlock();
        for (int i = 0; i < n; i++) HandleIncoming(*steam, work[i]);

        // Placement of every voice, and the reverb for where we are.
        if (now - lastPlace >= kSpatialApplyMs) {
            lastPlace = now;
            SpatialFrame listener;
            bool haveListener = g_spatial && SpatialListener(&listener);
            if (g_reverb && haveListener) {
                Lock();
                DWORD env = 0;
                float best = 0.0f;
                for (int i = 0; g_localValid && i < g_local.envCount; i++)
                    if (g_local.envValue[i] > best) {
                        best = g_local.envValue[i];
                        env = g_local.envId[i];
                    }
                Unlock();
                if (env) SetReverbFor(env);
            }
            for (Speaker& s : g_speakers)
                if (s.xuid && s.voice) PlaceSpeaker(s, haveListener ? &listener : nullptr);
        }

        for (Speaker& s : g_speakers)
            if (s.xuid && now - s.lastPlayed > kSpeakerIdleMs) DestroySpeaker(s);
        if (now - lastStats >= kStatsEveryMs) {
            LogStats();
            lastStats = now;
        }
    }
    if (recording)
        if (const SteamApi* steam = Steam()) steam->user->StopVoiceRecording();
    for (Speaker& s : g_speakers)
        if (s.xuid) DestroySpeaker(s);
    if (g_reverb) g_reverb->DestroyVoice();
    if (g_master) g_master->DestroyVoice();
    if (g_xa) g_xa->Release();
    g_reverb = nullptr;
    g_master = nullptr;
    g_xa = nullptr;
    LogStats();
    CoUninitialize();
    return 0;
}

static void StartThread() {
    if (g_thread || !g_enabled) return;
    InterlockedExchange(&g_stop, 0);
    g_thread = CreateThread(nullptr, 0, VoiceThread, nullptr, 0, nullptr);
}

void VoiceShutdown() {
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}

// ---- the engine the game talks to ----

class XHVEngine {
public:
    XHVEngine() : refs(1) { InitializeCriticalSection(&cs); }
    virtual ULONG __stdcall AddRef() { return InterlockedIncrement(&refs); }
    virtual ULONG __stdcall Release() {
        LONG r = InterlockedDecrement(&refs);
        if (!r) {
            VoiceShutdown();
            ::Lock();
            g_talkerCount = 0;
            g_outCount = g_inCount = 0;
            ::Unlock();
            InterlockedExchange(&g_localActive, 0);
            DeleteCriticalSection(&cs);
            delete this;
        }
        return r;
    }
    virtual HRESULT_T __stdcall Lock(DWORD lockType) {
        switch (lockType) {
        case 0: EnterCriticalSection(&cs); return 0;
        case 1: return TryEnterCriticalSection(&cs) ? 0 : kUnexpected;
        case 2: LeaveCriticalSection(&cs); return 0;
        default: return kInvalidArg;
        }
    }
    virtual HRESULT_T __stdcall StartLocalProcessingModes(DWORD user, const DWORD* modes, DWORD count) { return user ? kInvalidArg : 0; }
    virtual HRESULT_T __stdcall StopLocalProcessingModes(DWORD user, const DWORD* modes, DWORD count) { return user ? kInvalidArg : 0; }
    virtual HRESULT_T __stdcall StartRemoteProcessingModes(XUID xuid, const DWORD* modes, DWORD count) { return Has(xuid) ? 0 : kUnexpected; }
    virtual HRESULT_T __stdcall StopRemoteProcessingModes(XUID xuid, const DWORD* modes, DWORD count) { return Has(xuid) ? 0 : kUnexpected; }
    virtual HRESULT_T __stdcall SetMaxDecodePackets(DWORD count) { return 0; }
    virtual HRESULT_T __stdcall RegisterLocalTalker(DWORD user) {
        if (user) return kInvalidArg;
        if (!InterlockedExchange(&g_localActive, 1)) OverrideLog("voice: local talker registered");
        return 0;
    }
    virtual HRESULT_T __stdcall UnregisterLocalTalker(DWORD user) {
        if (user) return kInvalidArg;
        if (InterlockedExchange(&g_localActive, 0)) OverrideLog("voice: local talker unregistered");
        return 0;
    }
    virtual HRESULT_T __stdcall RegisterRemoteTalker(XUID xuid, void* a, void* b, void* c) {
        ::Lock();
        if (!FindTalker(xuid) && g_talkerCount < kMaxTalkers) g_talkers[g_talkerCount++] = {xuid, false, 0};
        ::Unlock();
        return 0;
    }
    virtual HRESULT_T __stdcall UnregisterRemoteTalker(XUID xuid) {
        ::Lock();
        for (int i = 0; i < g_talkerCount; i++)
            if (g_talkers[i].xuid == xuid) {
                g_talkers[i] = g_talkers[--g_talkerCount];
                QueueIncoming(xuid, nullptr, 0);
                break;
            }
        ::Unlock();
        return 0;
    }
    virtual HRESULT_T __stdcall GetRemoteTalkers(DWORD* count, XUID* xuids) {
        if (!count || !xuids) return kInvalidArg;
        ::Lock();
        *count = g_talkerCount;
        for (int i = 0; i < g_talkerCount; i++) xuids[i] = g_talkers[i].xuid;
        ::Unlock();
        return 0;
    }
    virtual BOOL __stdcall IsHeadsetPresent(DWORD user) { return user == 0 && g_enabled; }
    virtual BOOL __stdcall IsLocalTalking(DWORD user) {
        return user == 0 && g_enabled && GetTickCount() - (DWORD)g_lastLocalTalk < kTalkingMs;
    }
    virtual BOOL __stdcall IsRemoteTalking(XUID xuid) {
        ::Lock();
        Talker* t = FindTalker(xuid);
        BOOL talking = t && t->lastHeard && GetTickCount() - t->lastHeard < kTalkingMs;
        ::Unlock();
        return talking;
    }
    virtual DWORD __stdcall GetDataReadyFlags() { return g_outCount ? 1 : 0; }
    virtual HRESULT_T __stdcall GetLocalChatData(DWORD user, BYTE* data, DWORD* size, DWORD* packets) {
        if (!size || !packets) return kInvalidArg;
        if (data) InterlockedIncrement(&g_chatCalls);
        DWORD cap = *size < kBlobMax ? *size : kBlobMax, n = 0;
        ::Lock();
        DWORD placeLen = g_localValid ? 2 + kSpatialChunkMin + g_local.envCount * 5 : 0;
        if (data && user == 0 && g_outCount && cap >= 4u + placeLen + g_out[g_outHead].len) {
            data[n++] = kBlobMagic;
            data[n++] = kBlobVersion;
            if (g_localValid) {  // where we are, first
                DWORD len = WriteSpatialChunk(data + n + 2, g_local);
                data[n++] = (BYTE)len;
                data[n++] = (BYTE)(len >> 8);
                n += len;
            }
            while (g_outCount && n + 2 + g_out[g_outHead].len <= cap) {
                const OutChunk& c = g_out[g_outHead];
                data[n++] = (BYTE)c.len;
                data[n++] = (BYTE)(c.len >> 8);
                memcpy(data + n, c.data, c.len);
                n += c.len;
                g_outHead = (g_outHead + 1) % kOutChunks;
                g_outCount--;
            }
        }
        ::Unlock();
        *size = n;
        *packets = n ? 1 : 0;
        if (!n) return kPending;
        InterlockedIncrement(&g_sentBlobs);
        InterlockedExchangeAdd(&g_sentBytes, (LONG)n);
        return 0;
    }
    virtual HRESULT_T __stdcall SetPlaybackPriority(XUID xuid, DWORD user, DWORD priority) {
        ::Lock();
        Talker* t = FindTalker(xuid);
        bool muted = priority == kPriorityNever;
        if (t && t->muted != muted) {
            t->muted = muted;
            OverrideLog("voice: account %lu %s", (DWORD)xuid, muted ? "muted" : "unmuted");
        }
        ::Unlock();
        return 0;
    }
    virtual HRESULT_T __stdcall SubmitIncomingChatData(XUID xuid, const BYTE* data, DWORD* size) {
        if (!data || !size) return kInvalidArg;
        if (!g_enabled) return 0;
        if (*size < 4 || *size > kInBlobMax || data[0] != kBlobMagic || data[1] != kBlobVersion) {
            InterlockedIncrement(&g_badBlobs);
            return 0;
        }
        ::Lock();
        Talker* t = FindTalker(xuid);  // not registered (yet): still played, only a mute applies to a known talker
        if (!t || !t->muted) {
            if (t) t->lastHeard = GetTickCount();
            QueueIncoming(xuid, data, *size);
            InterlockedIncrement(&g_recvBlobs);
        }
        ::Unlock();
        return 0;
    }

private:
    static bool Has(XUID xuid) {
        ::Lock();
        bool has = FindTalker(xuid) != nullptr;
        ::Unlock();
        return has;
    }
    volatile LONG refs;
    CRITICAL_SECTION cs;
};

static HRESULT_T WINAPI My_XHVCreateEngine(const void* params, HANDLE* worker, XHVEngine** engine) {
    if (!engine) return kInvalidArg;
    ReadSettings();
    *engine = new XHVEngine();
    StartThread();
    return 0;
}

const Override kVoiceOverrides[] = {
    {5008, (void*)My_XHVCreateEngine},
};
const int kVoiceOverrideCount = sizeof(kVoiceOverrides) / sizeof(kVoiceOverrides[0]);
