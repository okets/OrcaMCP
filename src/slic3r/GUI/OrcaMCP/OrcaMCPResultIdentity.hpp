#pragma once
#include <string>

namespace Slic3r { namespace GUI { class PartPlate; namespace OrcaMCP {
struct GeometryIdentity;

// Process-session / stable plate key / plate index / native result ID; shared across both queries.
std::string native_result_identity(const GeometryIdentity& identity);
// Main thread: same result identity returned by native layer geometry and plate analysis.
std::string plate_native_result_identity(PartPlate& plate, int plate_index);
}}}
