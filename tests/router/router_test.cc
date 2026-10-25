// Router skeleton checks. The router owner adds real tests in this folder.

#include "router/router.h"

#include <catch2/catch_test_macros.hpp>

using namespace RTBKIT;
using nlohmann::json;

TEST_CASE("router: config json")
{
    RouterConfig defaults;
    CHECK(defaults.agentEndpoint == "tcp://127.0.0.1:12340");

    RouterConfig config = RouterConfig::fromJson(json{{"agentEndpoint", "tcp://127.0.0.1:5555"}});
    CHECK(config.agentEndpoint == "tcp://127.0.0.1:5555");
    CHECK(RouterConfig::fromJson(config.toJson()).agentEndpoint == config.agentEndpoint);

    CHECK_THROWS_AS(RouterConfig::fromJson(json{{"agentEndpoint", 5}}), std::runtime_error);
}
