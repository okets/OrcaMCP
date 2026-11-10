#pragma once
#include <string>

namespace Slic3r { namespace GUI { namespace OrcaMCP {
struct GeometryIdentity;
// Process-session / stable plate key / plate index / native result ID; shared across both queries.
std::string native_result_identity(const GeometryIdentity& identity);
}}}
