// diag_dirty.cpp -- diagnostic: print each frame's dirty-area ratio (verify the differential screen-cast data source)
#include <cstdio>
#include <thread>
#include <chrono>
#include "capture/dxgi_capture.h"

int main() {
    DXGICapture cap;
    if (!cap.start(0, [](const CapturedFrame& f) {
        double ratio = f.dirtyArea / (double)((uint64_t)f.width * f.height);
        printf("dirty=%llu px, ratio=%.4f, rects=%zu, size=%dx%d\n",
               (unsigned long long)f.dirtyArea, ratio, f.dirtyRects.size(), f.width, f.height);
    })) {
        printf("capture start FAILED\n");
        return 1;
    }
    printf("capture OK, capture 12 seconds\n");
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(12)) {
        cap.pumpFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cap.stop();
    return 0;
}
