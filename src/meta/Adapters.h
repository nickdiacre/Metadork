#pragma once
// Per-manufacturer adapters: turn one frame's decoded ANC (FrameMetadata) into the master MetaRecord.
// To add a manufacturer (Canon, RED, Panasonic ...): write a decoder in src/anc/, add an adapter here
// (detect + fill), capture the camera with sdimeta_probe --dump into captures/, and add it to the coverage report.
#include "anc/MetadataProcessor.h"
#include "meta/MetaRecord.h"

struct CameraAdapter {
    const char* name;  // e.g. "ARRI"
    CameraKind kind;
    bool (*detect)(const FrameMetadata&);
    void (*fill)(const FrameMetadata&, MetaRecord&);
};

const std::vector<CameraAdapter>& cameraAdapters();

// Runs the first adapter whose detect() matches, then adds the camera-independent fields (SDI timecode,
// record state, clip name fallback).
MetaRecord buildRecord(const FrameMetadata& md);
