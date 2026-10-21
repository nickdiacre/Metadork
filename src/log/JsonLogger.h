#pragma once
#include "anc/MetadataProcessor.h"
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

// Writes one JSON object per frame (JSON Lines). Safe to call from the capture thread.
class JsonLogger {
public:
    bool open(const std::string& path, bool includeRawAnc);
    void close();
    bool isOpen() const;
    void write(const FrameMetadata& md);
    uint64_t linesWritten() const { return m_lines; }
    std::string path() const { return m_path; }

    static std::string toJson(const FrameMetadata& md, bool includeRawAnc);

private:
    mutable std::mutex m_mtx;
    std::ofstream m_out;
    std::string m_path;
    bool m_raw = false;
    uint64_t m_lines = 0;
};
