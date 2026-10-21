#include "log/JsonLogger.h"
#include "anc/AncRegistry.h"
#include <cmath>
#include <cstdio>

namespace {
std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o.push_back(c);
        }
    }
    return o;
}
std::string num(double v) {
    if (std::isnan(v)) return "null";
    if (std::isinf(v)) return "\"inf\"";
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}
}  // namespace

bool JsonLogger::open(const std::string& path, bool includeRawAnc) {
    std::lock_guard<std::mutex> l(m_mtx);
    m_out.close();
    m_out.open(path, std::ios::out | std::ios::app);
    m_path = path;
    m_raw = includeRawAnc;
    m_lines = 0;
    return m_out.is_open();
}

void JsonLogger::close() {
    std::lock_guard<std::mutex> l(m_mtx);
    m_out.close();
}

bool JsonLogger::isOpen() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_out.is_open();
}

void JsonLogger::write(const FrameMetadata& md) {
    std::lock_guard<std::mutex> l(m_mtx);
    if (!m_out.is_open()) return;
    m_out << toJson(md, m_raw) << '\n';
    if (++m_lines % 50 == 0) m_out.flush();
}

std::string JsonLogger::toJson(const FrameMetadata& md, bool raw) {
    std::string j = "{";
    char b[64];
    std::snprintf(b, sizeof b, "\"frame\":%llu,\"host_time\":%.6f", (unsigned long long)md.frameIndex, md.hostTime);
    j += b;
    j += ",\"no_signal\":" + std::string(md.noSignal ? "true" : "false");
    j += ",\"tc\":" + (md.tc.valid ? "\"" + md.tc.str() + "\"" : std::string("null"));
    j += ",\"tc_advancing\":" + std::string(md.tcAdvancing ? "true" : "false");
    j += ",\"rec\":\"" + std::string(recStateName(md.rec)) + "\"";
    if (!md.recSource.empty()) j += ",\"rec_source\":\"" + esc(md.recSource) + "\"";

    if (md.hasArri) {
        const auto& a = md.arri;
        j += ",\"arri\":{";
        j += "\"camera\":\"" + esc(a.cameraModel.empty() ? a.cameraTypeName : a.cameraModel) + "\"";
        std::snprintf(b, sizeof b, ",\"serial\":%u", a.cameraSerial); j += b;
        if (a.cameraIndex) { j += ",\"index\":\""; j.push_back(a.cameraIndex); j += "\""; }
        j += ",\"reel\":\"" + esc(a.reel()) + "\"";
        j += ",\"clip\":\"" + esc(a.clip()) + "\"";
        j += ",\"master_tc\":\"" + a.masterTc + "\"";
        j += ",\"focus_mm\":" + (a.focusValid ? num(a.focusMm) : std::string("null"));
        j += ",\"focal_mm\":" + num(a.focalLengthMm);
        j += ",\"t_stop\":" + num(a.tStop);
        j += ",\"iris_state\":\"" + std::string(a.irisState) + "\"";
        std::snprintf(b, sizeof b, ",\"raw\":{\"focus_lds\":%u,\"focus_motor\":%u,", a.rawFocusLds, a.rawFocusMotor); j += b;
        std::snprintf(b, sizeof b, "\"focal_lds\":%u,\"focal_motor\":%u,", a.rawFocalLds, a.rawFocalMotor); j += b;
        std::snprintf(b, sizeof b, "\"iris_lds\":%u,\"iris_motor\":%u}", a.rawIrisLds, a.rawIrisMotor); j += b;
        std::snprintf(b, sizeof b, ",\"lds_lag_frames\":%u", a.ldsLagFrames); j += b;
        std::snprintf(b, sizeof b, ",\"entrance_pupil_um\":%d", a.entrancePupilUm); j += b;
        std::snprintf(b, sizeof b, ",\"efl_um\":%d", a.effectiveFocalLengthUm); j += b;
        j += ",\"lens\":\"" + esc(a.lensModel) + "\"";
        std::snprintf(b, sizeof b, ",\"ei\":%u,\"wb\":%u,\"tint\":%d", a.exposureIndex, a.whiteBalanceK, a.tint); j += b;
        j += ",\"shutter_deg\":" + num(a.shutterAngle);
        j += ",\"sensor_fps\":" + num(a.sensorFps);
        j += ",\"nd\":" + num(a.ndDensity);
        j += ",\"vari_frame_duplicate\":" + std::string(a.variFrameDuplicate ? "true" : "false");
        j += "}";
    }

    if (md.bmd.present) {
        j += ",\"blackmagic\":[";
        for (size_t i = 0; i < md.bmd.commands.size(); ++i) {
            const auto& c = md.bmd.commands[i];
            std::snprintf(b, sizeof b, "%s{\"cmd\":\"%u.%u\",", i ? "," : "", c.category, c.parameter);
            j += b;
            if (*c.name) j += "\"name\":\"" + esc(c.name) + "\",";
            j += "\"value\":\"" + esc(c.display) + "\",\"hex\":\"" + toHex(c.data.data(), c.data.size(), SIZE_MAX, 0) + "\"}";
        }
        j += "]";
    }
    if (!md.sonyClip.empty()) j += ",\"sony_clip\":\"" + esc(md.sonyClip) + "\"";
    if (!md.sonyUmid.empty()) j += ",\"sony_umid\":\"" + md.sonyUmid + "\"";
    if (md.rdd18.parsed()) {
        j += ",\"rdd18\":[";
        for (size_t si = 0; si < md.rdd18.sets.size(); ++si) {
            const auto& set = md.rdd18.sets[si];
            std::snprintf(b, sizeof b, "%s{\"set\":\"%02X\",\"complete\":%s,\"items\":[", si ? "," : "", set.setByte,
                          set.complete ? "true" : "false");
            j += b;
            for (size_t i = 0; i < set.items.size(); ++i) {
                const auto& it = set.items[i];
                std::snprintf(b, sizeof b, "%s{\"tag\":\"%04X\",", i ? "," : "", it.tag);
                j += b;
                if (*it.name) j += "\"name\":\"" + esc(it.name) + "\",";
                j += "\"value\":\"" + esc(it.display) + "\",\"hex\":\"" + toHex(it.value.data(), it.value.size(), SIZE_MAX, 0) + "\"}";
            }
            j += "]}";
        }
        j += "]";
    }

    j += ",\"anc\":[";
    for (size_t i = 0; i < md.packets.size(); ++i) {
        const auto& p = md.packets[i];
        std::snprintf(b, sizeof b, "%s{\"did\":\"%02X\",\"sdid\":\"%02X\",\"line\":%u,\"stream\":%u,\"len\":%zu",
                      i ? "," : "", p.did, p.sdid, p.line, p.stream, p.data.size());
        j += b;
        if (raw) j += ",\"hex\":\"" + toHex(p.data.data(), p.data.size(), SIZE_MAX, 0) + "\"";
        j += "}";
    }
    j += "]}";
    return j;
}
