#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/ObicoLink.hpp"

using json = nlohmann::json;
using namespace Slic3r;

namespace {

DynamicPrintConfig config_with(const std::string& url, const std::string& token)
{
    DynamicPrintConfig config;
    config.set_key_value(OBICO_URL_KEY, new ConfigOptionString(url));
    config.set_key_value(OBICO_TOKEN_KEY, new ConfigOptionString(token));
    return config;
}

} // namespace

TEST_CASE("the Obico preset keys exist as advanced strings", "[obico]")
{
    const ConfigOptionDef* url   = print_config_def.get(OBICO_URL_KEY);
    const ConfigOptionDef* token = print_config_def.get(OBICO_TOKEN_KEY);
    REQUIRE(url != nullptr);
    REQUIRE(token != nullptr);
    CHECK(url->type == coString);
    CHECK(token->type == coString);
    CHECK(url->mode == comAdvanced);
    CHECK(token->mode == comAdvanced);
}

TEST_CASE("obico_link_json needs both keys and normalises the URL", "[obico]")
{
    CHECK(obico_link_json(config_with("http://10.0.0.2:3334/", " tok ")) ==
          json{{"url", "http://10.0.0.2:3334"}, {"token", "tok"}});
    CHECK(obico_link_json(config_with("http://10.0.0.2:3334", "")).is_null());
    CHECK(obico_link_json(config_with("", "tok")).is_null());
    CHECK(obico_link_json(config_with("   ", "tok")).is_null());
    CHECK(obico_link_json(config_with("///", "tok")).is_null());
    CHECK(obico_link_json(DynamicPrintConfig()).is_null());
}

TEST_CASE("obico_status_json never carries the token", "[obico]")
{
    const json configured = obico_status_json(config_with("https://obico.example/", "secret"));
    CHECK(configured == json{{"configured", true}, {"url", "https://obico.example"}});
    CHECK(configured.dump().find("secret") == std::string::npos);
    CHECK(obico_status_json(config_with("", "")) == json{{"configured", false}, {"url", nullptr}});
    CHECK(obico_status_json(config_with("http://x", "")) == json{{"configured", false}, {"url", nullptr}});
    CHECK(obico_status_json(DynamicPrintConfig()) == json{{"configured", false}, {"url", nullptr}});
}
