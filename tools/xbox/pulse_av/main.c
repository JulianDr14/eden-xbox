// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pulse phase-3 payload: a homebrew that keeps the services a game leans on busy at every moment,
// so the frontend can capture it, destroy the process, restore it into a fresh System and see
// whether picture, sound and files carry on (boot.cfg "pulse_cycles=" with this as boot.nro).
//
//   video  vi stray layer + nvnflinger buffer queue + nvmap buffers (libnx Framebuffer), a frame
//          counter drawn into every frame
//   audio  audout with three queued buffers of a continuous tone, released in order
//   files  sdmc: a log written and read back on every beat through one open file
//
// No applet: __appInit opens sm and fs, main opens vi, nv and audout, so every session the checkpoint
// meets belongs to a service it is meant to serialize.
//
// Output, through svcOutputDebugString:
//   PULSE_HB <beat> frames=.. audio=.. samples=.. fs=.. late_ms=..
//   PULSE_FAIL <reason>     an invariant broke (never expected)

#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define HEAP_SIZE 0x2000000 // 32 MiB: the checkpoint stores every heap page
#define WIDTH 320
#define HEIGHT 240
#define AUDIO_BUFFERS 3
#define AUDIO_FRAMES 4800 // 100 ms at 48 kHz
#define STALL_BEATS 12    // 3 s without a frame or a buffer is a failure

static volatile u64 g_frames, g_audio, g_fs;
static volatile u64 g_late_ms;
static volatile bool g_failed;
static Framebuffer g_fb;
static NWindow g_window;
static ViDisplay g_display;
static ViLayer g_layer;
static FsFileSystem g_sd;
static FsFile g_log;
static AudioOutBuffer g_audio_buffers[AUDIO_BUFFERS];
static s16* g_audio_data[AUDIO_BUFFERS];

static void Report(const char* format, ...) {
    char line[192];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    svcOutputDebugString(line, length > 0 && length < (int)sizeof(line) ? (u64)length : strlen(line));
}

static void Fail(const char* reason, u64 a, u64 b) {
    if (!g_failed) Report("PULSE_FAIL %s %lu %lu", reason, a, b);
    g_failed = true;
}

// Every frame carries its number in its first pixels and a colour that follows it.
static void VideoWorker(void* arg) {
    (void)arg;
    for (u64 frame = 1;; ++frame) {
        u32 stride;
        u32* pixels = (u32*)framebufferBegin(&g_fb, &stride);
        if (!pixels) {
            Fail("framebuffer_begin", frame, 0);
            svcSleepThread(100000000);
            continue;
        }
        const u32 colour = 0xff000000u | (u32)((frame * 4) & 0xff) | (u32)(((frame >> 6) & 0xff) << 8) |
                           0x00400000u;
        for (u32 y = 0; y < HEIGHT; ++y) {
            u32* row = (u32*)((u8*)pixels + (size_t)y * stride);
            for (u32 x = 0; x < WIDTH; ++x) row[x] = colour;
        }
        pixels[0] = (u32)frame;
        pixels[1] = (u32)(frame >> 32);
        framebufferEnd(&g_fb);
        g_frames = frame;
    }
}

static void FillTone(s16* data, u64 buffer_index) {
    // A 440 Hz tone whose phase continues across buffers: a restore that dropped or repeated a
    // buffer would click, and the sequence number in the first sample pair would skip.
    for (u32 i = 0; i < AUDIO_FRAMES; ++i) {
        const u64 t = buffer_index * AUDIO_FRAMES + i;
        const s16 sample = (s16)(sinf((float)(t % 48000) * 2.0f * 3.14159265f * 440.0f / 48000.0f) * 6000.0f);
        data[i * 2] = sample;
        data[i * 2 + 1] = sample;
    }
    data[0] = (s16)(buffer_index & 0x7fff);
}

static void AudioWorker(void* arg) {
    (void)arg;
    u64 next_fill = AUDIO_BUFFERS;
    u64 expected = 0; // index of the buffer the service must release next
    for (;;) {
        AudioOutBuffer* released = NULL;
        u32 count = 0;
        const Result rc = audoutWaitPlayFinish(&released, &count, UINT64_MAX);
        if (R_FAILED(rc)) {
            Fail("audio_wait", rc, 0);
            svcSleepThread(100000000);
            continue;
        }
        if (!released || count == 0) continue;
        const u64 slot = (u64)(released - g_audio_buffers);
        if (slot != expected % AUDIO_BUFFERS) Fail("audio_order", slot, expected);
        ++expected;
        FillTone(g_audio_data[slot], next_fill++);
        if (R_FAILED(audoutAppendAudioOutBuffer(released))) Fail("audio_append", slot, 0);
        g_audio = expected;
    }
}

static void InitVideo(void) {
    Result rc = viInitialize(ViServiceType_Application);
    if (R_SUCCEEDED(rc)) rc = viOpenDefaultDisplay(&g_display);
    if (R_SUCCEEDED(rc)) rc = viCreateLayer(&g_display, &g_layer);
    if (R_SUCCEEDED(rc)) rc = viSetLayerScalingMode(&g_layer, ViScalingMode_FitToLayer);
    if (R_SUCCEEDED(rc)) rc = nwindowCreateFromLayer(&g_window, &g_layer);
    if (R_SUCCEEDED(rc)) rc = framebufferCreate(&g_fb, &g_window, WIDTH, HEIGHT, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_SUCCEEDED(rc)) rc = framebufferMakeLinear(&g_fb);
    if (R_FAILED(rc)) Fail("video_init", rc, 0);
}

static void InitAudio(void) {
    Result rc = audoutInitialize();
    if (R_SUCCEEDED(rc)) rc = audoutStartAudioOut();
    const size_t bytes = (AUDIO_FRAMES * 2 * sizeof(s16) + 0xfff) & ~(size_t)0xfff;
    for (u32 i = 0; R_SUCCEEDED(rc) && i < AUDIO_BUFFERS; ++i) {
        g_audio_data[i] = (s16*)memalign(0x1000, bytes);
        if (!g_audio_data[i]) {
            rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
            break;
        }
        FillTone(g_audio_data[i], i);
        g_audio_buffers[i].next = NULL;
        g_audio_buffers[i].buffer = g_audio_data[i];
        g_audio_buffers[i].buffer_size = bytes;
        g_audio_buffers[i].data_size = AUDIO_FRAMES * 2 * sizeof(s16);
        g_audio_buffers[i].data_offset = 0;
        rc = audoutAppendAudioOutBuffer(&g_audio_buffers[i]);
    }
    if (R_FAILED(rc)) Fail("audio_init", rc, 0);
}

static void InitFiles(void) {
    static const char path[] = "/pulse_av.log";
    Result rc = fsOpenSdCardFileSystem(&g_sd);
    if (R_SUCCEEDED(rc)) {
        fsFsDeleteFile(&g_sd, path); // a fresh log per boot; a restore reopens the same file
        rc = fsFsCreateFile(&g_sd, path, 0, 0);
    }
    if (R_SUCCEEDED(rc)) rc = fsFsOpenFile(&g_sd, path, FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append, &g_log);
    if (R_FAILED(rc)) Fail("fs_init", rc, 0);
}

// One record per beat, flushed, then the previous one read back through the same open file.
static void FileBeat(u64 beat) {
    const u64 record[2] = {beat, ~beat};
    Result rc = fsFileWrite(&g_log, (s64)((beat - 1) * sizeof(record)), record, sizeof(record), FsWriteOption_Flush);
    if (R_FAILED(rc)) {
        Fail("fs_write", rc, beat);
        return;
    }
    if (beat > 1) {
        u64 previous[2] = {0, 0};
        u64 read = 0;
        rc = fsFileRead(&g_log, (s64)((beat - 2) * sizeof(record)), previous, sizeof(previous), 0, &read);
        if (R_FAILED(rc) || read != sizeof(previous)) {
            Fail("fs_read", rc, read);
            return;
        }
        if (previous[0] != beat - 1 || previous[1] != ~(beat - 1)) Fail("fs_record", previous[0], beat - 1);
    }
    s64 size = 0;
    if (R_FAILED(fsFileGetSize(&g_log, &size)) || size != (s64)(beat * sizeof(record))) {
        Fail("fs_size", (u64)size, beat);
    }
    g_fs = beat;
}

void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    void* address = NULL;
    if (R_FAILED(svcSetHeapSize(&address, HEAP_SIZE))) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    fake_heap_start = (char*)address;
    fake_heap_end = (char*)address + HEAP_SIZE;
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_SUCCEEDED(rc)) rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
}

void __appExit(void) {
    fsExit();
    smExit();
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    InitFiles();
    InitVideo();
    InitAudio();

    static Thread threads[2];
    static const ThreadFunc entries[2] = {VideoWorker, AudioWorker};
    for (int i = 0; i < 2; ++i) {
        Result rc = threadCreate(&threads[i], entries[i], NULL, NULL, 0x10000, 0x2c, -2);
        if (R_SUCCEEDED(rc)) rc = threadStart(&threads[i]);
        if (R_FAILED(rc)) Fail("thread_start", i, rc);
    }

    u64 last_frames = 0, last_audio = 0, frame_stall = 0, audio_stall = 0;
    for (u64 beat = 1;; ++beat) {
        const u64 start = armTicksToNs(armGetSystemTick());
        svcSleepThread(250000000);
        const u64 elapsed = armTicksToNs(armGetSystemTick()) - start;
        if (elapsed + 2000000 < 250000000ULL) Fail("early_sleep_ms", elapsed / 1000000, 250);
        if (elapsed > 250000000ULL) g_late_ms = (elapsed - 250000000ULL) / 1000000;
        FileBeat(beat);
        const u64 frames = g_frames, audio = g_audio;
        frame_stall = frames == last_frames ? frame_stall + 1 : 0;
        audio_stall = audio == last_audio ? audio_stall + 1 : 0;
        if (frame_stall == STALL_BEATS) Fail("frames_stalled", frames, beat);
        if (audio_stall == STALL_BEATS) Fail("audio_stalled", audio, beat);
        last_frames = frames;
        last_audio = audio;
        u64 samples = 0;
        audoutGetAudioOutPlayedSampleCount(&samples);
        Report("PULSE_HB %lu frames=%lu audio=%lu samples=%lu fs=%lu late_ms=%lu%s", beat, frames, audio,
               samples, g_fs, g_late_ms, g_failed ? " FAILED" : "");
    }
    return 0;
}
