/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "CamDev-HighFps"

#include "SamsungHighFpsVideo.h"

#include <log/log.h>
#include <system/camera_vendor_tags.h>
#include <system/graphics-base.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef CAMERA_HIGH_FPS_VIDEO_CLIENT
// samsung.android.control.cameraClient value injected for high fps sessions. The HAL treats
// 1 and 5 as the Samsung camera app and 2, 3, 4 and 6 as Samsung camera SDK clients; both
// lift the 30 fps limit. The SDK path changes the fewest other HAL behaviours.
#define CAMERA_HIGH_FPS_VIDEO_CLIENT 2
#endif

namespace android {
namespace hardware {
namespace camera {
namespace device {
namespace implementation {

using common::helper::CameraMetadata;

namespace {

// samsung.android.scaler.availableVideoConfigurations: (width, height, minFps, maxFps, x, y)
constexpr size_t kVideoConfigStride = 6;
// The HAL validates ranges with a maximum in [31, 119]; 120 fps and above is high speed video.
constexpr int32_t kMinHighFps = 31;
constexpr int32_t kMaxHighFps = 119;
constexpr int64_t kNsPerSec = 1000000000LL;

struct SamsungTags {
    bool valid = false;
    uint32_t availableVideoConfigurations = 0;
    uint32_t cameraClient = 0;
    uint32_t recordingMaxFps = 0;
    uint32_t recordingMinFps = 0;
    // recordingMinFps does not exist on every HAL (it is absent on the Exynos 990 one), so it is
    // only used when the HAL provides it.
    bool hasRecordingMinFps = false;
    uint32_t availableHighSpeedVideoConfigurations = 0;
    bool hasHighSpeedTable = false;
    uint32_t minZoomList = 0;
    bool hasMinZoomList = false;
    uint32_t scalerZoomRatio = 0;
    bool hasScalerZoomRatio = false;
};

std::once_flag sInitFlag;
SamsungTags sTags;

bool findTag(const vendor_tag_ops_t& ops, const std::vector<uint32_t>& tags, const char* section,
             const char* name, uint32_t* out) {
    for (uint32_t tag : tags) {
        const char* s = ops.get_section_name(&ops, tag);
        const char* n = ops.get_tag_name(&ops, tag);
        if (s != nullptr && n != nullptr && strcmp(s, section) == 0 && strcmp(n, name) == 0) {
            *out = tag;
            return true;
        }
    }
    return false;
}

bool hasPair(const camera_metadata_ro_entry& entry, int32_t a, int32_t b) {
    for (size_t i = 0; i + 1 < entry.count; i += 2) {
        if (entry.data.i32[i] == a && entry.data.i32[i + 1] == b) {
            return true;
        }
    }
    return false;
}

// Returns true if |a| has an entry, other than the AE target fps range, that is missing from
// or different in |b|.
bool otherParamsDiffer(const camera_metadata_t* a, const camera_metadata_t* b) {
    size_t count = a != nullptr ? get_camera_metadata_entry_count(a) : 0;
    for (size_t i = 0; i < count; i++) {
        camera_metadata_ro_entry_t ea, eb;
        if (get_camera_metadata_ro_entry(a, i, &ea) != 0) {
            return true;
        }
        if (ea.tag == ANDROID_CONTROL_AE_TARGET_FPS_RANGE) {
            continue;
        }
        if (b == nullptr || find_camera_metadata_ro_entry(b, ea.tag, &eb) != 0 ||
            ea.type != eb.type || ea.count != eb.count ||
            memcmp(ea.data.u8, eb.data.u8, camera_metadata_type_size[ea.type] * ea.count) != 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

void SamsungHighFpsVideo::init(const sp<common::helper::SamsungCameraModule>& module) {
    std::call_once(sInitFlag, [&module]() {
        if (module == nullptr || !module->isVendorTagDefined()) {
            return;
        }

        vendor_tag_ops_t ops{};
        module->getVendorTagOps(&ops);
        if (ops.get_tag_count == nullptr || ops.get_all_tags == nullptr ||
            ops.get_section_name == nullptr || ops.get_tag_name == nullptr) {
            return;
        }

        int count = ops.get_tag_count(&ops);
        if (count <= 0) {
            return;
        }
        std::vector<uint32_t> tags(count);
        ops.get_all_tags(&ops, tags.data());

        SamsungTags t;
        t.valid = findTag(ops, tags, "samsung.android.scaler", "availableVideoConfigurations",
                          &t.availableVideoConfigurations) &&
                  findTag(ops, tags, "samsung.android.control", "cameraClient", &t.cameraClient) &&
                  findTag(ops, tags, "samsung.android.control", "recordingMaxFps",
                          &t.recordingMaxFps);
        t.hasRecordingMinFps = findTag(ops, tags, "samsung.android.control", "recordingMinFps",
                                       &t.recordingMinFps);
        t.hasHighSpeedTable = findTag(ops, tags, "samsung.android.scaler", "availableHighSpeedVideoConfigurations", &t.availableHighSpeedVideoConfigurations);
        t.hasMinZoomList = findTag(ops, tags, "samsung.android.scaler", "availableMinDigitalZoomList", &t.minZoomList);
        t.hasScalerZoomRatio = findTag(ops, tags, "samsung.android.scaler", "zoomRatio", &t.scalerZoomRatio);
        if (!t.valid) {
            ALOGW("%s: Samsung video vendor tags not found, high fps video disabled", __FUNCTION__);
            return;
        }
        sTags = t;
    });
}

void SamsungHighFpsVideo::patchCharacteristics(CameraMetadata* chars) {
    if (chars == nullptr || !sTags.valid) {
        return;
    }
    // Read-only view: const find() does not lock the buffer. Entries are invalidated by
    // update(), so each one is looked up again right before it is used.
    const CameraMetadata& ro = *chars;

    camera_metadata_ro_entry videoConfigs = ro.find(sTags.availableVideoConfigurations);
    if (videoConfigs.count == 0 || videoConfigs.count % kVideoConfigStride != 0) {
        return;
    }

    // Collect the fixed high frame rates and the sizes supporting them.
    std::set<int32_t> highFps;
    std::vector<std::pair<std::pair<int32_t, int32_t>, int32_t>> sizes;  // ((w, h), fps)
    for (size_t i = 0; i < videoConfigs.count; i += kVideoConfigStride) {
        const int32_t* c = &videoConfigs.data.i32[i];
        if (c[2] == c[3] && c[3] >= kMinHighFps && c[3] <= kMaxHighFps) {
            highFps.insert(c[3]);
            sizes.push_back({{c[0], c[1]}, c[3]});
        }
    }
    if (highFps.empty()) {
        return;
    }

    // 1. Advertise the fixed high fps AE target ranges.
    camera_metadata_ro_entry ranges = ro.find(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
    std::vector<int32_t> newRanges(ranges.data.i32, ranges.data.i32 + ranges.count);
    for (int32_t fps : highFps) {
        if (!hasPair(ranges, fps, fps)) {
            newRanges.push_back(fps);
            newRanges.push_back(fps);
        }
    }
    if (newRanges.size() != ranges.count) {
        chars->update(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, newRanges.data(),
                      newRanges.size());
    }

    // 2. Lower the minimum frame durations of the video capable sizes accordingly.
    camera_metadata_ro_entry durations = ro.find(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS);
    if (durations.count > 0 && durations.count % 4 == 0) {
        std::vector<int64_t> newDurations(durations.data.i64, durations.data.i64 + durations.count);
        bool changed = false;
        for (size_t i = 0; i < newDurations.size(); i += 4) {
            int64_t format = newDurations[i];
            if (format != HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED &&
                format != HAL_PIXEL_FORMAT_YCBCR_420_888) {
                continue;
            }
            for (const auto& [size, fps] : sizes) {
                if (newDurations[i + 1] == size.first && newDurations[i + 2] == size.second) {
                    int64_t minDuration = kNsPerSec / fps;
                    if (newDurations[i + 3] > minDuration) {
                        newDurations[i + 3] = minDuration;
                        changed = true;
                    }
                }
            }
        }
        if (changed) {
            chars->update(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, newDurations.data(),
                          newDurations.size());
        }
    }

    // 3. Make the AE target fps range a session key, so it reaches configure_streams.
    camera_metadata_ro_entry sessionKeys = ro.find(ANDROID_REQUEST_AVAILABLE_SESSION_KEYS);
    std::vector<int32_t> newSessionKeys(sessionKeys.data.i32,
                                        sessionKeys.data.i32 + sessionKeys.count);
    if (std::find(newSessionKeys.begin(), newSessionKeys.end(),
                  ANDROID_CONTROL_AE_TARGET_FPS_RANGE) == newSessionKeys.end()) {
        newSessionKeys.push_back(ANDROID_CONTROL_AE_TARGET_FPS_RANGE);
        chars->update(ANDROID_REQUEST_AVAILABLE_SESSION_KEYS, newSessionKeys.data(),
                      newSessionKeys.size());

        camera_metadata_ro_entry charKeys = ro.find(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
        if (charKeys.count > 0) {
            std::vector<int32_t> newCharKeys(charKeys.data.i32, charKeys.data.i32 + charKeys.count);
            if (std::find(newCharKeys.begin(), newCharKeys.end(),
                          ANDROID_REQUEST_AVAILABLE_SESSION_KEYS) == newCharKeys.end()) {
                newCharKeys.push_back(ANDROID_REQUEST_AVAILABLE_SESSION_KEYS);
                chars->update(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, newCharKeys.data(),
                              newCharKeys.size());
            }
        }
    }
}

bool SamsungHighFpsVideo::getHighFpsRange(const camera_metadata_t* params, int32_t* minFps,
                                          int32_t* maxFps) {
    if (params == nullptr || !sTags.valid) {
        return false;
    }

    camera_metadata_ro_entry_t entry;
    if (find_camera_metadata_ro_entry(params, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, &entry) != 0 ||
        entry.count != 2) {
        return false;
    }

    int32_t lo = entry.data.i32[0];
    int32_t hi = entry.data.i32[1];
    if (hi < kMinHighFps || hi > kMaxHighFps || lo <= 0 || lo > hi) {
        return false;
    }

    if (minFps != nullptr) *minFps = lo;
    if (maxFps != nullptr) *maxFps = hi;
    return true;
}

bool SamsungHighFpsVideo::patchSessionParams(CameraMetadata* params) {
    if (params == nullptr || params->isEmpty()) {
        return false;
    }

    int32_t minFps, maxFps;
    {
        const camera_metadata_t* raw = params->getAndLock();
        bool isHighFps = getHighFpsRange(raw, &minFps, &maxFps);
        params->unlock(raw);
        if (!isHighFps) {
            return false;
        }
    }

    // Respect values explicitly set by the client.
    if (!params->exists(sTags.cameraClient)) {
        int32_t client = CAMERA_HIGH_FPS_VIDEO_CLIENT;
        params->update(sTags.cameraClient, &client, 1);
    }
    if (!params->exists(sTags.recordingMaxFps)) {
        params->update(sTags.recordingMaxFps, &maxFps, 1);
    }
    if (sTags.hasRecordingMinFps && !params->exists(sTags.recordingMinFps)) {
        params->update(sTags.recordingMinFps, &minFps, 1);
    }

    ALOGI("%s: high fps session [%d, %d], cameraClient %d", __FUNCTION__, minFps, maxFps,
          CAMERA_HIGH_FPS_VIDEO_CLIENT);
    return true;
}

void SamsungHighFpsVideo::addVideoSizes(CameraMetadata* chars) {
    if (chars == nullptr || !sTags.valid) {
        return;
    }
    const CameraMetadata& ro = *chars;
    camera_metadata_ro_entry videoConfigs = ro.find(sTags.availableVideoConfigurations);
    if (videoConfigs.count == 0 || videoConfigs.count % kVideoConfigStride != 0) {
        return;
    }

    // Lowest frame duration per size, for sizes that are at least 3840 pixels wide.
    std::map<std::pair<int32_t, int32_t>, int64_t> wanted;
    for (size_t i = 0; i < videoConfigs.count; i += kVideoConfigStride) {
        const int32_t* c = &videoConfigs.data.i32[i];
        if (c[0] < 3840 || c[3] <= 0 || c[3] > kMaxHighFps) {
            continue;
        }
        int64_t duration = kNsPerSec / c[3];
        auto key = std::make_pair(c[0], c[1]);
        auto it = wanted.find(key);
        if (it == wanted.end() || duration < it->second) {
            wanted[key] = duration;
        }
    }
    if (wanted.empty()) {
        return;
    }

    camera_metadata_ro_entry cfg = ro.find(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
    camera_metadata_ro_entry dur = ro.find(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS);
    camera_metadata_ro_entry stall = ro.find(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS);
    if (cfg.count == 0 || cfg.count % 4 != 0 || dur.count == 0 || dur.count % 4 != 0 ||
        stall.count == 0 || stall.count % 4 != 0) {
        return;
    }
    // Entries are invalidated by update(): work on copies.
    std::vector<int32_t> newCfg(cfg.data.i32, cfg.data.i32 + cfg.count);
    std::vector<int64_t> newDur(dur.data.i64, dur.data.i64 + dur.count);
    std::vector<int64_t> newStall(stall.data.i64, stall.data.i64 + stall.count);

    bool changed = false;
    for (const auto& [size, duration] : wanted) {
        // 8K only as encoder (IMPLEMENTATION_DEFINED) output; 4K also as YUV_420_888, like the main lens.
        std::vector<int32_t> formats = {HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED};
        if (size.first < 7680) {
            formats.push_back(HAL_PIXEL_FORMAT_YCBCR_420_888);
        }
        for (int32_t format : formats) {
            bool present = false;
            for (size_t i = 0; i < newCfg.size(); i += 4) {
                if (newCfg[i] == format && newCfg[i + 1] == size.first &&
                    newCfg[i + 2] == size.second &&
                    newCfg[i + 3] == ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) {
                    present = true;
                    break;
                }
            }
            if (present) {
                continue;
            }
            newCfg.insert(newCfg.end(), {format, size.first, size.second,
                                         ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT});
            newDur.insert(newDur.end(), {format, size.first, size.second, duration});
            newStall.insert(newStall.end(), {format, size.first, size.second, 0});
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    chars->update(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, newCfg.data(), newCfg.size());
    chars->update(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, newDur.data(), newDur.size());
    chars->update(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, newStall.data(), newStall.size());
}

namespace {
std::atomic<int32_t> sClampFps{0};
}  // namespace

void SamsungHighFpsVideo::setClampFps(int32_t fps) {
    int32_t old = sClampFps.exchange(fps);
    if (old != fps) {
    }
}

bool SamsungHighFpsVideo::needsFpsClamp(const camera_metadata_t* params) {
    int32_t fps = sClampFps.load();
    if (fps <= 0 || params == nullptr) {
        return false;
    }
    camera_metadata_ro_entry entry;
    if (find_camera_metadata_ro_entry(params, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, &entry) != 0 ||
        entry.count != 2) {
        return false;
    }
    return entry.data.i32[1] > fps || entry.data.i32[0] > fps;
}

bool SamsungHighFpsVideo::clampFpsRange(CameraMetadata* params) {
    int32_t fps = sClampFps.load();
    if (fps <= 0 || params == nullptr || params->isEmpty()) {
        return false;
    }
    const CameraMetadata& ro = *params;
    camera_metadata_ro_entry entry = ro.find(ANDROID_CONTROL_AE_TARGET_FPS_RANGE);
    if (entry.count != 2 || (entry.data.i32[1] <= fps && entry.data.i32[0] <= fps)) {
        return false;
    }
    int32_t range[2] = {fps, fps};
    params->update(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, range, 2);
    return true;
}

namespace {
std::atomic<bool> sZoomTranslate{false};
std::atomic<float> sNativeZoom{1.0f};
std::mutex sZoomMutex;
int32_t sBaseCrop[4] = {0, 0, 0, 0};  // the 1x crop region: the largest one seen in this session
float sLoggedRatio = 0.0f;
int32_t sActiveArray[4] = {0, 0, 0, 0};  // x, y, w, h of the opened camera
}  // namespace

void SamsungHighFpsVideo::setZoomTranslate(bool on) {
    std::lock_guard<std::mutex> lock(sZoomMutex);
    sZoomTranslate = on;
    memset(sBaseCrop, 0, sizeof(sBaseCrop));
    sLoggedRatio = 0.0f;
}

bool SamsungHighFpsVideo::needsZoomTranslate(const camera_metadata_t* settings) {
    if (!sZoomTranslate.load() || !sTags.hasScalerZoomRatio || settings == nullptr) {
        return false;
    }
    int32_t minFps, maxFps;
    return getHighFpsRange(settings, &minFps, &maxFps);
}

bool SamsungHighFpsVideo::translateZoom(CameraMetadata* settings) {
    if (settings == nullptr || settings->isEmpty()) {
        return false;
    }
    const CameraMetadata& ro = *settings;
    camera_metadata_ro_entry crop = ro.find(ANDROID_SCALER_CROP_REGION);
    int32_t active[4];
    {
        std::lock_guard<std::mutex> lock(sZoomMutex);
        memcpy(active, sActiveArray, sizeof(active));
    }
    if (crop.count != 4 || crop.data.i32[2] <= 0 || active[2] <= 0 || active[3] <= 0) {
        return false;
    }
    int32_t cur[4] = {crop.data.i32[0], crop.data.i32[1], crop.data.i32[2], crop.data.i32[3]};
    float ratio = (float)active[2] / (float)cur[2];
    camera_metadata_ro_entry zr = ro.find(ANDROID_CONTROL_ZOOM_RATIO);
    if (zr.count == 1 && zr.data.f[0] > ratio) {
        ratio = zr.data.f[0];  // the app zooms with the zoom ratio and left the crop region alone
    }
    if (ratio < 1.0f) {
        ratio = 1.0f;
    }
    float vendorRatio = ratio * sNativeZoom.load();
    {
        std::lock_guard<std::mutex> lock(sZoomMutex);
        if (ratio > sLoggedRatio + 0.5f || ratio < sLoggedRatio - 0.5f) {
            sLoggedRatio = ratio;
        }
    }
    settings->update(sTags.scalerZoomRatio, &vendorRatio, 1);
    // The crop region is left as the app set it: the HAL uses it when it ignores the vendor zoom ratio.
    return true;
}

void SamsungHighFpsVideo::setNativeZoom(const camera_metadata_t* chars) {
    float native = 1.0f;
    if (chars != nullptr && sTags.hasMinZoomList) {
        camera_metadata_ro_entry entry;
        // Index 5: the zoom range of the camera's own (tele / ultrawide) video zoom mode.
        if (find_camera_metadata_ro_entry(chars, sTags.minZoomList, &entry) == 0 &&
            entry.type == TYPE_FLOAT && entry.count > 5 && entry.data.f[5] > 0.0f) {
            native = entry.data.f[5];
        }
    }
    sNativeZoom = native;
    int32_t active[4] = {0, 0, 0, 0};
    if (chars != nullptr) {
        camera_metadata_ro_entry a;
        if (find_camera_metadata_ro_entry(chars, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &a) == 0 &&
            a.count == 4) {
            memcpy(active, a.data.i32, sizeof(active));
        }
    }
    {
        std::lock_guard<std::mutex> lock(sZoomMutex);
        memcpy(sActiveArray, active, sizeof(active));
    }
}

void SamsungHighFpsVideo::addHighSpeedConfigs(CameraMetadata* chars) {
    if (chars == nullptr || !sTags.valid || !sTags.hasHighSpeedTable) {
        return;
    }
    const CameraMetadata& ro = *chars;
    camera_metadata_ro_entry pub = ro.find(ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS);
    if (pub.count > 0) {
        return;
    }
    // (width, height, minFps, maxFps, unused)
    constexpr size_t kHsStride = 5;
    camera_metadata_ro_entry hs = ro.find(sTags.availableHighSpeedVideoConfigurations);
    if (hs.count == 0 || hs.count % kHsStride != 0) {
        return;
    }
    std::vector<int32_t> out;
    for (size_t i = 0; i < hs.count; i += kHsStride) {
        const int32_t* c = &hs.data.i32[i];
        if (c[0] <= 0 || c[1] <= 0 || c[2] != c[3] || c[3] < 120) {
            continue;
        }
        // Same batch sizes as the main camera: 2 for 120 fps, 4 for 240 fps.
        int32_t batch = c[3] / 60;
        out.insert(out.end(), {c[0], c[1], 30, c[3], batch});
        out.insert(out.end(), {c[0], c[1], c[3], c[3], batch});
    }
    if (out.empty()) {
        return;
    }
    camera_metadata_ro_entry caps = ro.find(ANDROID_REQUEST_AVAILABLE_CAPABILITIES);
    std::vector<uint8_t> newCaps;
    if (caps.count > 0) {
        newCaps.assign(caps.data.u8, caps.data.u8 + caps.count);
    }
    if (std::find(newCaps.begin(), newCaps.end(),
                  (uint8_t)ANDROID_REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO) ==
        newCaps.end()) {
        newCaps.push_back(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO);
    }
    // Entries are invalidated by update(): the data above was copied first.
    chars->update(ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS, out.data(), out.size());
    chars->update(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, newCaps.data(), newCaps.size());

    camera_metadata_ro_entry charKeys = ro.find(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
    if (charKeys.count > 0) {
        std::vector<int32_t> keys(charKeys.data.i32, charKeys.data.i32 + charKeys.count);
        if (std::find(keys.begin(), keys.end(),
                      (int32_t)ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS) ==
            keys.end()) {
            keys.push_back(ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS);
            chars->update(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, keys.data(), keys.size());
        }
    }
}

bool SamsungHighFpsVideo::isReconfigurationRequired(const camera_metadata_t* oldParams,
                                                    const camera_metadata_t* newParams) {
    if (otherParamsDiffer(oldParams, newParams) || otherParamsDiffer(newParams, oldParams)) {
        return true;
    }

    int32_t oldMin = 0, oldMax = 0, newMin = 0, newMax = 0;
    bool oldHigh = getHighFpsRange(oldParams, &oldMin, &oldMax);
    bool newHigh = getHighFpsRange(newParams, &newMin, &newMax);
    if (oldHigh != newHigh) {
        return true;
    }
    return oldHigh && (oldMin != newMin || oldMax != newMax);
}

}  // namespace implementation
}  // namespace device
}  // namespace camera
}  // namespace hardware
}  // namespace android
