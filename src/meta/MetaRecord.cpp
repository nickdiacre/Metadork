#include "meta/MetaRecord.h"
#include <cmath>
#include <cstdio>

const std::vector<FieldInfo>& allFields() {
    static const std::vector<FieldInfo> fields = {
#define F(e, id, g, label, unit, t, lg, otio, freed) {Field::e, id, FieldGroup::g, label, unit, FieldType::t, lg, otio, freed},
#include "meta/Fields.def"
#undef F
    };
    return fields;
}

const FieldInfo& fieldInfo(Field f) { return allFields()[size_t(f)]; }

const FieldInfo* fieldById(const std::string& id) {
    for (const auto& f : allFields())
        if (id == f.id) return &f;
    return nullptr;
}

const char* fieldGroupName(FieldGroup g) {
    switch (g) {
        case FieldGroup::Identity: return "Identity";
        case FieldGroup::Lens: return "Lens";
        case FieldGroup::Exposure: return "Exposure";
        case FieldGroup::Image: return "Image";
        case FieldGroup::Orientation: return "Orientation";
        default: return "Take / timing";
    }
}

const char* cameraKindName(CameraKind k) {
    switch (k) {
        case CameraKind::Arri: return "ARRI";
        case CameraKind::Sony: return "Sony";
        case CameraKind::Blackmagic: return "Blackmagic";
        case CameraKind::Red: return "RED";
        default: return "None";
    }
}

const char* valueSourceName(ValueSource s) {
    switch (s) {
        case ValueSource::Derived: return "derived";
        case ValueSource::Manual: return "manual";
        default: return "camera";
    }
}

const char* confidenceName(Confidence c) {
    switch (c) {
        case Confidence::Verified: return "verified";
        case Confidence::Documented: return "documented";
        default: return "inferred";
    }
}

void MetaRecord::setNum(Field f, double v, Confidence c, std::string origin, ValueSource s) {
    if (!std::isfinite(v)) return;
    auto& x = m_v[size_t(f)];
    x.set = true;
    x.num = v;
    x.str.clear();
    x.source = s;
    x.confidence = c;
    x.origin = std::move(origin);
}

void MetaRecord::setStr(Field f, std::string v, Confidence c, std::string origin, ValueSource s) {
    if (v.empty()) return;
    auto& x = m_v[size_t(f)];
    x.set = true;
    x.str = std::move(v);
    x.num = 0;
    x.source = s;
    x.confidence = c;
    x.origin = std::move(origin);
}

std::string MetaRecord::display(Field f) const {
    const auto& v = get(f);
    if (!v.set) return "n/a";
    const auto& info = fieldInfo(f);
    const std::string unit = *info.unit ? std::string(" ") + info.unit : "";
    char b[48];
    switch (info.type) {
        case FieldType::String: return v.str;
        case FieldType::Int: return std::to_string(std::lround(v.num)) + unit;
        default:
            std::snprintf(b, sizeof b, std::fabs(v.num) >= 100 ? "%.1f" : "%.3f", v.num);
            return b + unit;
    }
}
