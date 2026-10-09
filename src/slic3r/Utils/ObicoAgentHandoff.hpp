#ifndef slic3r_ObicoAgentHandoff_hpp_
#define slic3r_ObicoAgentHandoff_hpp_

#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "FlashforgeJobProgress.hpp"

namespace Slic3r { namespace ObicoAgentHandoff {

// The flashforge-obico agent watches a Flashforge for Obico and serves a phone console. It reads the
// printer's progress like the Device page did before the slice table (FlashforgeJobProgress): file
// bytes, so its layer and time left were wrong too. When OrcaMCP starts a print on a printer whose
// preset names an Obico server, it hands the agent the file's slice table (POST /api/slice-table).
//
// The agent's address is the one its cameras are re-served at: it advertises each one in Obico's
// webcam list (settings.webcams[].stream_url, http://<agent host>:<port>/cameras/<n>/stream), which
// Obico's websocket gives to anyone with the printer's token -- the token the preset already holds.

// The agent's base URL ("http://host:port") from an Obico printer document, its primary camera first.
// std::nullopt when no camera names an http address.
std::optional<std::string> agent_url(const nlohmann::json& obico_document);

// The body of POST /api/slice-table: {"file_name": <the name the printer reports>, "table": <to_json>}.
std::string slice_table_body(const std::string& file_name, const FlashforgeJobProgress::SliceTable& table);

// Obico's token-authenticated websocket path, the token percent-encoded as the Device page encodes it.
std::string obico_websocket_target(const std::string& token);

// Reads Obico's printer document over its websocket, by the preset's Obico URL and printer token,
// within a few seconds. Only an http:// Obico is read. False with `error` set otherwise.
bool read_obico_document(const std::string& obico_url, const std::string& token, nlohmann::json& document, std::string& error);

// Hands `table` to the agent of the printer whose Obico URL and token are given. Bounded to a few
// seconds per step. Returns what happened, for the log: the agent's answer, or why nothing was handed over.
std::string hand_over_slice_table(const std::string&                       obico_url,
                                  const std::string&                       token,
                                  const std::string&                       file_name,
                                  const FlashforgeJobProgress::SliceTable& table);

}} // namespace Slic3r::ObicoAgentHandoff

#endif
