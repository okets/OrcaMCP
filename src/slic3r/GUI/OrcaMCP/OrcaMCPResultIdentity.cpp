#include "OrcaMCPResultIdentity.hpp"
#include "OrcaMCPLayerGeometry.hpp"
#include "OrcaMCPDigest.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {
std::string native_result_identity(const GeometryIdentity& identity)
{
    Digest hash;
    hash.text("orcamcp-result-v1"); hash.text(identity.session); hash.text(identity.plate);
    hash.number(identity.plate_index); hash.number(identity.native_result_id);
    return hash.finish();
}
}}}
