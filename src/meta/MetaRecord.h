#pragma once
// Master camera metadata record: one value slot per registry field (Fields.def), each with provenance.
// Filled by a per-manufacturer adapter (Adapters.h); every UI view, output and log reads from it.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

enum class Field : uint16_t {
#define F(e, ...) e,
#include "meta/Fields.def"
#undef F
    Count
};
constexpr size_t kFieldCount = size_t(Field::Count);

// Focus distance stored for "infinity" (with note "infinity"); outputs substitute their configured infinity value.
constexpr double kInfinityFocusM = 1.0e6;

enum class FieldGroup { Identity, Lens, Exposure, Image, Orientation, Take };
enum class FieldType { Float, Int, String };

struct FieldInfo {
    Field field;
    const char* id;        // stable key (OSC suffix, JSON key)
    FieldGroup group;
    const char* label;
    const char* unit;
    FieldType type;
    const char* livegrade; // equivalent Pomfort Livegrade field, "" if none
    const char* otio;      // OpenTrackIO path, "" if not sent there
    const char* freed;     // FreeD slot, "" if not sent there
};

const std::vector<FieldInfo>& allFields();
const FieldInfo& fieldInfo(Field f);
const FieldInfo* fieldById(const std::string& id);
const char* fieldGroupName(FieldGroup g);

enum class CameraKind { None, Arri, Sony, Blackmagic };
const char* cameraKindName(CameraKind k);

enum class ValueSource { Camera, Derived, Manual };
enum class Confidence { Verified, Documented, Inferred };  // verified on real hardware / from a spec / reasoned
const char* valueSourceName(ValueSource s);
const char* confidenceName(Confidence c);

struct FieldValue {
    bool set = false;
    double num = 0;
    std::string str;
    ValueSource source = ValueSource::Camera;
    Confidence confidence = Confidence::Inferred;
    std::string origin;  // where it came from, e.g. "arri.0x045C", "sony.7F.8414", "bmd.1.14"
    std::string note;    // also used when not set, e.g. why a value can't be derived
};

class MetaRecord {
public:
    CameraKind kind = CameraKind::None;
    std::string adapter;  // e.g. "ARRI", "Sony RDD 18"

    void setNum(Field f, double v, Confidence c, std::string origin, ValueSource s = ValueSource::Camera);
    void setStr(Field f, std::string v, Confidence c, std::string origin, ValueSource s = ValueSource::Camera);
    void note(Field f, std::string text) { m_v[size_t(f)].note = std::move(text); }
    void clear(Field f) { m_v[size_t(f)] = {}; }

    const FieldValue& get(Field f) const { return m_v[size_t(f)]; }
    bool has(Field f) const { return m_v[size_t(f)].set; }
    double num(Field f, double fallback = 0) const { return has(f) ? m_v[size_t(f)].num : fallback; }
    const std::string& str(Field f) const { return m_v[size_t(f)].str; }
    std::string display(Field f) const;  // value with unit, or "n/a"

private:
    std::array<FieldValue, kFieldCount> m_v{};
};
