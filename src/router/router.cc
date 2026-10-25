#include "router/router.h"

namespace RTBKIT {

using nlohmann::json;

RouterConfig RouterConfig::fromJson(const json& j)
{
    RouterConfig config;
    try {
        if (j.contains("agentEndpoint"))
            config.agentEndpoint = j.at("agentEndpoint").get<std::string>();
    }
    catch (const json::exception& e) {
        throw std::runtime_error(std::string("invalid router config: ") + e.what());
    }
    return config;
}

json RouterConfig::toJson() const
{
    return json{{"agentEndpoint", agentEndpoint}};
}

} // namespace RTBKIT
