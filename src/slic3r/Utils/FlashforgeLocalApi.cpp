#include "FlashforgeLocalApi.hpp"

#include "Http.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <boost/format.hpp>

namespace Slic3r { namespace FlashforgeLocalApi {

std::string host_of(const std::string& address)
{
    const std::string trimmed = boost::algorithm::trim_copy(address);
    if (trimmed.empty())
        return {};
    // Upstream's one URL parser: it adds the scheme a bare "ip:port" lacks before curl reads it,
    // which is exactly the case the old scheme-less branch got wrong.
    return Http::get_host_from_url(trimmed);
}

std::string url_of(const std::string& address, const std::string& path)
{
    return (boost::format("http://%1%:%2%/%3%") % host_of(address) % kPort % path).str();
}

}} // namespace Slic3r::FlashforgeLocalApi
