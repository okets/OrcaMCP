#ifndef slic3r_ObicoLink_hpp_
#define slic3r_ObicoLink_hpp_

#include <nlohmann/json_fwd.hpp>

namespace Slic3r {

class DynamicPrintConfig;

// A Flashforge physical printer may name a self-hosted Obico server (see
// docs/superpowers/specs/2026-09-15-obico-camera-source-design.md). Obico is where the printer's
// cameras and failure-detection state are read from once the flashforge-obico agent owns the
// printer's single-client camera stream. These two helpers are the only place the preset keys are
// interpreted, so the "both or nothing" rule and the redaction rule live in one spot.
constexpr const char* OBICO_URL_KEY   = "flashforge_obico_url";
constexpr const char* OBICO_TOKEN_KEY = "flashforge_obico_token";

/// What the console page needs to open Obico's token-authenticated websocket:
/// {"url": <base URL, trimmed, no trailing slash>, "token": <trimmed>}. Null unless both keys are
/// set; the page treats null as "no Obico" and draws the camera straight from the printer.
nlohmann::json obico_link_json(const DynamicPrintConfig& config);

/// {"configured": <bool>, "url": <base URL or null>} - safe to return from an MCP tool or write to
/// a log, because the token is deliberately not in it.
nlohmann::json obico_status_json(const DynamicPrintConfig& config);

} // namespace Slic3r

#endif // slic3r_ObicoLink_hpp_
