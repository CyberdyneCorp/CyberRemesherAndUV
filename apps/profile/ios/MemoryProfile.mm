#import <UIKit/UIKit.h>
#import <mach/mach.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "cyber_capi.h"

static uint64_t residentBytes() {
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<uint64_t>(info.resident_size);
}

static size_t profileGridSize() {
    NSString* configuredSize = NSProcessInfo.processInfo.environment[@"CYBER_PROFILE_GRID"];
    NSInteger parsedSize = configuredSize.integerValue;
    return parsedSize >= 2 && parsedSize <= 1000 ? static_cast<size_t>(parsedSize) : 316;
}

static void updatePeak(std::atomic<uint64_t>& peak) {
    uint64_t observed = residentBytes();
    uint64_t current = peak.load(std::memory_order_relaxed);
    while (observed > current && !peak.compare_exchange_weak(
        current, observed, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

static void runProfile() {
    const size_t rows = profileGridSize();
    const size_t cols = rows;
    std::vector<float> points((rows + 1) * (cols + 1) * 3);
    for (size_t r = 0; r <= rows; ++r) {
        for (size_t c = 0; c <= cols; ++c) {
            const size_t i = 3 * (r * (cols + 1) + c);
            points[i] = static_cast<float>(c) / static_cast<float>(cols);
            points[i + 1] = static_cast<float>(r) / static_cast<float>(rows);
            points[i + 2] = 0.02f * sinf(static_cast<float>(r + c) * 0.1f);
        }
    }
    CyberMesh* input = cyber_mesh_create();
    size_t faces = 0;
    const CyberStatus grid = cyber_retopo_create_grid(input, points.data(), rows, cols, nullptr, &faces);
    CyberRemeshParams params{};
    cyber_default_params(&params);
    params.targetQuads = static_cast<uint32_t>(rows * cols);
    CyberMesh* output = nullptr;
    const uint64_t before = residentBytes();
    std::atomic<bool> sampling{true};
    std::atomic<uint64_t> peak{before};
    std::thread sampler([&sampling, &peak] {
        while (sampling.load(std::memory_order_relaxed)) {
            updatePeak(peak);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        updatePeak(peak);
    });
    const auto started = std::chrono::steady_clock::now();
    const CyberStatus status = grid == CYBER_OK
        ? cyber_remesh(input, &params, nullptr, nullptr, nullptr, &output)
        : grid;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    sampling.store(false, std::memory_order_relaxed);
    sampler.join();
    const uint64_t after = residentBytes();
    const uint64_t peakBytes = peak.load(std::memory_order_relaxed);
    const size_t outputFaces = output != nullptr ? cyber_mesh_face_count(output) : 0;
    NSLog(@"CYBER_PROFILE device=%@ grid=%zux%zu input_faces=%zu status=%d output_faces=%zu "
          @"rss_before=%llu rss_peak=%llu rss_after=%llu peak_delta=%lld elapsed_ms=%lld error=%s",
          UIDevice.currentDevice.model, rows, cols, faces, status, outputFaces, before,
          peakBytes, after, static_cast<long long>(peakBytes) - static_cast<long long>(before),
          static_cast<long long>(elapsed), cyber_last_error());
    cyber_mesh_free(output);
    cyber_mesh_free(input);
}

@interface ProfileDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation ProfileDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ runProfile(); });
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(ProfileDelegate.class)); }
}
