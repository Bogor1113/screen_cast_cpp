#pragma once
// ui/preview.h -- preview buffer and drawing (teacher GUI)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <mutex>
#include <vector>

struct PreviewBuffer {
    std::mutex mtx;
    std::vector<uint8_t> bgra;   // thumbnail BGRA (persistent: updated in place, region-by-region)
    int w = 0, h = 0;
    std::vector<RECT> dirty;     // changed regions (thumbnail coords)
    std::atomic<bool> showDirty{ false };
    std::atomic<uint64_t> seq{ 0 };  // frame sequence (increments each Preview_Update), GUI uses it to detect a new frame
    bool primed = false;         // thumbnail contains a complete picture (first update done)
    // capture-thread-only bookkeeping for the update throttle (see Preview_Update): the preview runs
    // on the capture thread, inside the capture -> encode -> send path, so it is deliberately capped
    // to ~25fps / region updates instead of rebuilding the whole thumbnail per frame.
    uint64_t lastMs = 0;
};

// called by the capture thread: scale-copy into the preview buffer
void Preview_Update(PreviewBuffer& pb, const uint8_t* bgra, int sw, int sh, int stride,
                    const RECT* dirty, int dirtyCount);
// called by the draw thread: draw within rc on hdc (keep aspect ratio, optionally draw changed regions)
void Preview_Draw(PreviewBuffer& pb, HDC hdc, const RECT& rc);
