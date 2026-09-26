#ifndef slic3r_Utils_FlashforgeLocalApi_hpp_
#define slic3r_Utils_FlashforgeLocalApi_hpp_

#include <string>

namespace Slic3r { namespace FlashforgeLocalApi {

// How the Flashforge local API is reached: which host, which URL. FlashforgeApi is what the API
// says; this is how a request gets there. Pure, so tests/slic3rutils/test_flashforge_local_api.cpp
// can pin every rule without a printer.

// The local API always listens here, whatever port the preset's print_host carries.
constexpr int kPort = 8898;

// The bare host the local API is reached at, from any address shape a preset or DeviceManager holds:
// "10.0.0.100", "10.0.0.100:8080", "http://10.0.0.100:8080/path", "printer.local/", "[fe80::1]:80".
// Any port is dropped, because the local API has its own. Surrounding whitespace is ignored; an
// empty address gives an empty host. The one parser both Flashforge and FlashforgePrinterAgent use,
// so the two cannot disagree about where the printer is.
std::string host_of(const std::string& address);

// "http://<host>:8898/<path>" for `address`, as host_of reads it.
std::string url_of(const std::string& address, const std::string& path);

}} // namespace Slic3r::FlashforgeLocalApi

#endif
