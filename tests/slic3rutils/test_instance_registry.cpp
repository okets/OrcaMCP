#include "slic3r/GUI/OrcaMCP/OrcaMCPInstanceRegistry.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <regex>
#include <sstream>
#include <set>
#include <string>

#include "test_utils.hpp"

// Who each running OrcaMCP instance is (OrcaMCPInstanceRegistry.hpp): the entry it publishes for the
// bridge's list_instances, how stale entries go, and the check that refuses a call meant for another
// instance. Every test keeps its entries in a folder of its own, never in ~/.orcamcp.

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

InstanceIdentity identity(unsigned pid, Port port, const std::string& project = "bracket")
{
    InstanceIdentity identity;
    identity.instance_id = "id-" + std::to_string(pid);
    identity.pid         = pid;
    identity.port        = port;
    identity.version     = "2.5.0.6-dev";
    identity.executable  = "/Applications/OrcaMCP.app/Contents/MacOS/OrcaSlicer";
    identity.data_dir    = "/data/OrcaMCP";
    identity.started_at  = "2026-09-28T09:15:03.123Z";
    identity.project     = {project, "/prints/" + project + ".3mf", false};
    return identity;
}

json read_entry(const fs::path& path)
{
    boost::nowide::ifstream in(path.string());
    return json::parse(in);
}

void write_entry(const fs::path& dir, const InstanceIdentity& entry)
{
    boost::nowide::ofstream out((dir / (std::to_string(entry.pid) + ".json")).string());
    out << to_json(entry).dump();
}

// Every pid in `alive` runs; any other is gone.
InstanceRegistry::IsAlive running(std::set<unsigned> alive)
{
    return [alive](unsigned pid) { return alive.count(pid) != 0; };
}

// The JSON a response carries, after its headers.
json reply_body(Slic3r::GUI::HttpServer::Response& response)
{
    std::stringstream written;
    response.write_response(written);
    const std::string text = written.str();
    return json::parse(text.substr(text.find("\n\n") + 2));
}

std::set<std::string> entry_files(const fs::path& dir)
{
    std::set<std::string> names;
    for (fs::directory_iterator it(dir), end; it != end; ++it)
        names.insert(it->path().filename().string());
    return names;
}

} // namespace

TEST_CASE("an instance's entry holds its identity and its open project", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    CHECK(registry.publish(identity(4242, 13619), running({4242})).empty());

    const json entry = read_entry(registry.entry_path(4242));
    CHECK(entry.at("schema") == 1);
    CHECK(entry.at("instance_id") == "id-4242");
    CHECK(entry.at("pid") == 4242);
    CHECK(entry.at("port") == 13619);
    CHECK(entry.at("url") == "http://127.0.0.1:13619/mcp");
    CHECK(entry.at("version") == "2.5.0.6-dev");
    CHECK(entry.at("executable") == "/Applications/OrcaMCP.app/Contents/MacOS/OrcaSlicer");
    CHECK(entry.at("data_dir") == "/data/OrcaMCP");
    CHECK(entry.at("started_at") == "2026-09-28T09:15:03.123Z");
    CHECK(entry.at("project") == json{{"name", "bracket"}, {"path", "/prints/bracket.3mf"}, {"unsaved", false}});
    CHECK(identity_from_json(entry)->project == identity(4242, 13619).project);
}

TEST_CASE("an entry is written whole, with nothing left beside it", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618), running({4242})).empty());
    REQUIRE(registry.update_project({"bracket", "/prints/bracket.3mf", true}));
    CHECK(entry_files(dir.path()) == std::set<std::string>{"4242.json"}); // no .tmp left over
}

#ifndef _WIN32 // Windows keeps the user's profile to the user by its ACLs
TEST_CASE("the entries are the user's alone", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    const fs::path     folder = dir.path() / "instances"; // created by the registry, as ~/.orcamcp/instances is
    InstanceRegistry   registry(folder);
    REQUIRE(registry.publish(identity(4242, 13618), running({4242})).empty());
    CHECK((fs::status(folder).permissions() & fs::all_all) == fs::owner_all);
    CHECK((fs::status(registry.entry_path(4242)).permissions() & fs::all_all) == (fs::owner_read | fs::owner_write));
}
#endif

TEST_CASE("the project in the entry follows the project in the window, rewritten only when it changes", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618, "Untitled"), running({4242})).empty());

    CHECK_FALSE(registry.update_project({"Untitled", "/prints/Untitled.3mf", false}));
    CHECK(registry.update_project({"bracket", "/prints/bracket.3mf", true}));
    CHECK(read_entry(registry.entry_path(4242)).at("project") ==
          json{{"name", "bracket"}, {"path", "/prints/bracket.3mf"}, {"unsaved", true}});
    CHECK(registry.identity()->project.unsaved);
}

TEST_CASE("nothing is written before the instance is published", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    CHECK_FALSE(registry.update_project({"bracket", "", true}));
    CHECK_FALSE(registry.identity());
    CHECK(entry_files(dir.path()).empty());
}

TEST_CASE("a quitting instance's entry is removed", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618), running({4242})).empty());
    registry.withdraw();
    CHECK_FALSE(fs::exists(registry.entry_path(4242)));
    CHECK_FALSE(registry.identity());
    CHECK_FALSE(registry.update_project({"later", "", true})); // withdrawn: nothing comes back
    CHECK_FALSE(fs::exists(registry.entry_path(4242)));
}

TEST_CASE("publishing clears the entries of crashed instances and keeps the running ones", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    write_entry(dir.path(), identity(1001, 13619, "crashed"));  // its process is gone
    write_entry(dir.path(), identity(1002, 13620, "running"));  // runs
    write_entry(dir.path(), identity(1003, 13618, "reused"));   // claims the port this instance now holds
    InstanceRegistry registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618), running({1002, 1003, 4242})).empty());

    CHECK(entry_files(dir.path()) == std::set<std::string>{"1002.json", "4242.json"});
    const auto others = registry.others(running({1002, 4242}));
    REQUIRE(others.size() == 1);
    CHECK(others[0].pid == 1002);
    CHECK(others[0].project.name == "running");
}

TEST_CASE("a file that is not an entry is neither read nor removed", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    {
        boost::nowide::ofstream out((dir.path() / "999.json").string());
        out << "{\"pid\": 999"; // half a file, from something else
    }
    InstanceRegistry registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618), running({4242})).empty());
    CHECK(registry.others(running({999, 4242})).empty());
    CHECK(fs::exists(dir.path() / "999.json"));
}

TEST_CASE("an entry without an instance id, pid or port is not an instance", "[InstanceRegistry]")
{
    const json whole = to_json(identity(4242, 13618));
    CHECK(identity_from_json(whole));
    for (const char* key : {"instance_id", "pid", "port"}) {
        json partial = whole;
        partial.erase(key);
        CHECK_FALSE(identity_from_json(partial));
    }
    CHECK_FALSE(identity_from_json(json::array()));
}

TEST_CASE("every instance id is new, and 32 hex digits", "[InstanceRegistry]")
{
    const std::string first = new_instance_id();
    CHECK(std::regex_match(first, std::regex("[0-9a-f]{32}")));
    CHECK(new_instance_id() != first);
}

TEST_CASE("start times are UTC with milliseconds, so launches a second apart order", "[InstanceRegistry]")
{
    const auto when = std::chrono::system_clock::time_point(std::chrono::milliseconds(1759050903123LL));
    CHECK(utc_timestamp(when) == "2025-09-28T09:15:03.123Z");
}

TEST_CASE("an instance answers at 127.0.0.1, the one address it listens on", "[InstanceRegistry]")
{
    CHECK(mcp_url(13619) == "http://127.0.0.1:13619/mcp");
}

TEST_CASE("a running process is alive and a finished one is not", "[InstanceRegistry]")
{
    CHECK(process_is_alive(Slic3r::get_current_pid()));
    CHECK_FALSE(process_is_alive(0));
}

TEST_CASE("a call meant for another instance is refused, and one meant for this or none runs", "[InstanceRegistry]")
{
    const InstanceIdentity self = identity(4242, 13619);
    auto stamped = [](const std::string& id) { return json{{"name", "new_project"}, {"_meta", {{instance_meta_key, id}}}}; };

    CHECK_FALSE(wrong_instance_refusal(stamped("id-4242"), self));
    CHECK_FALSE(wrong_instance_refusal(json{{"name", "new_project"}}, self)); // an older bridge, or curl
    CHECK_FALSE(wrong_instance_refusal(json{{"_meta", {{"orcamcp/wait_cap_s", 10}}}}, self));

    const auto refusal = wrong_instance_refusal(stamped("id-1001"), self);
    REQUIRE(refusal);
    CHECK(refusal->find("id-1001") != std::string::npos);
    CHECK(refusal->find("pid 4242 on port 13619") != std::string::npos);
    CHECK(refusal->find("not run") != std::string::npos);
    // An older OrcaMCP ignores the stamp, so the bridge names one "legacy": a new instance refuses it.
    CHECK(wrong_instance_refusal(stamped("legacy"), self));
    CHECK(wrong_instance_refusal(stamped("id-4242"), std::nullopt));
}

TEST_CASE("the server refuses a call meant for another instance with -32004 before running it", "[InstanceRegistry][orcamcp]")
{
    // No instance is published in the test process, so every stamped call is someone else's. The tool
    // does not exist: were the check missing, the call would be refused for that (-32602), not run.
    const json request = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
                          {"params", {{"name", "no_such_tool"}, {"arguments", json::object()}, {"_meta", {{instance_meta_key, "id-1001"}}}}}};
    const json reply = reply_body(*OrcaMCPServer::handle_request("POST", "/mcp", request.dump()));
    CHECK(reply.at("id") == 7);
    CHECK(reply.at("error").at("code") == WrongInstance::error_code);
    CHECK(reply.at("error").at("code") == -32004);
}

TEST_CASE("the user is told when this instance is not on the first port, and why", "[InstanceRegistry]")
{
    CHECK_FALSE(other_port_note(13618, {}));

    const auto held = other_port_note(13619, {identity(1002, 13618, "benchy")});
    REQUIRE(held);
    CHECK(held->find("port 13619") != std::string::npos);
    CHECK(held->find("pid 1002") != std::string::npos);
    CHECK(held->find("benchy") != std::string::npos);

    const auto foreign = other_port_note(13619, {});
    REQUIRE(foreign);
    CHECK(foreign->find("another program") != std::string::npos);

    CHECK(no_port_warning().find("13618 to 13627") != std::string::npos);
}

TEST_CASE("instances sharing a data folder are warned about", "[InstanceRegistry]")
{
    InstanceIdentity elsewhere = identity(1003, 13620);
    elsewhere.data_dir         = "/copies/OrcaMCP";
    CHECK_FALSE(shared_data_folder_warning("/data/OrcaMCP", {elsewhere}));

    const auto shared = shared_data_folder_warning("/data/OrcaMCP", {identity(1002, 13618), elsewhere});
    REQUIRE(shared);
    CHECK(shared->find("pid 1002") != std::string::npos);
    CHECK(shared->find("1003") == std::string::npos);
}
