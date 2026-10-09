#include <catch2/catch_all.hpp>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/ObicoAgentHandoff.hpp"

using namespace Slic3r;
using namespace Slic3r::ObicoAgentHandoff;
using nlohmann::json;

namespace {

// The webcam list the flashforge-obico agent sends Obico (its obico/status.py webcam_entry): each
// camera re-served at http://<PUBLIC_HOST>:<RESERVE_PORT>/cameras/<n>/stream.
json document_with(json webcams) { return json{{"settings", {{"webcams", std::move(webcams)}}}, {"status", json::object()}}; }

json webcam(const std::string& name, bool primary, const std::string& stream_url)
{
    return json{{"name", name}, {"is_primary_camera", primary}, {"stream_url", stream_url}, {"snapshot_url", stream_url}};
}

} // namespace

TEST_CASE("The agent is found at the address of its primary camera", "[ObicoAgentHandoff]")
{
    CHECK(agent_url(document_with({webcam("Side", false, "http://10.0.0.3:9000/cameras/1/stream"),
                                   webcam("Printer", true, "http://10.0.0.2:8081/cameras/0/stream")})) ==
          std::optional<std::string>("http://10.0.0.2:8081"));
    // No primary: the first camera with an http address.
    CHECK(agent_url(document_with({webcam("A", false, "rtsp://cam/stream"), webcam("B", false, "http://agent.lan/cameras/1/stream")})) ==
          std::optional<std::string>("http://agent.lan:80"));
    CHECK(agent_url(document_with({webcam("Printer", true, "http://[fd00::2]:8081/cameras/0/stream")})) ==
          std::optional<std::string>("http://[fd00::2]:8081"));
}

TEST_CASE("A document with no camera address of the agent names no agent", "[ObicoAgentHandoff]")
{
    CHECK_FALSE(agent_url(json::object()).has_value());
    CHECK_FALSE(agent_url(document_with(json::array())).has_value());
    CHECK_FALSE(agent_url(document_with({webcam("Printer", true, "")})).has_value());
    CHECK_FALSE(agent_url(document_with({webcam("Printer", true, "https://agent/cameras/0/stream")})).has_value());
    CHECK_FALSE(agent_url(document_with({json{{"name", "no url"}, {"is_primary_camera", true}}})).has_value());
}

TEST_CASE("The agent is posted the file's name and its slice table", "[ObicoAgentHandoff]")
{
    FlashforgeJobProgress::SliceTable table;
    table.file_bytes        = 1000;
    table.layer_start_bytes = {100, 200, 600};
    table.layer_start_s     = {10, 70, 80};
    table.total_s           = 100;
    const json body = json::parse(slice_table_body("job.gcode.3mf", table));
    CHECK(body["file_name"] == "job.gcode.3mf");
    CHECK(body["table"] == json::parse(FlashforgeJobProgress::to_json(table)));
}

TEST_CASE("Obico's websocket path carries the token encoded as the Device page encodes it", "[ObicoAgentHandoff]")
{
    CHECK(obico_websocket_target("abc123") == "/ws/token/web/abc123/");
    CHECK(obico_websocket_target("a b/c") == "/ws/token/web/a%20b%2Fc/");
}

TEST_CASE("Only an http Obico in the printer settings is read", "[ObicoAgentHandoff]")
{
    json        document;
    std::string error;
    CHECK_FALSE(read_obico_document("", "token", document, error));
    CHECK(error == "no Obico server in the printer settings");
    CHECK_FALSE(read_obico_document("https://obico.example", "token", document, error));
    CHECK(error == "the Obico URL is not an http:// address");
}
