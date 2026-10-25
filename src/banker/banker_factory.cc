#include "banker/banker_factory.h"

#include "banker/in_memory_banker.h"
#ifdef RTB_WITH_REDIS
#include "banker/redis_banker.h"
#endif

#include <stdexcept>

namespace RTBKIT {

using nlohmann::json;

namespace {

Amount parseBudget(const json& j)
{
    if (j.is_number_integer()) return Amount(CurrencyCode::USD, j.get<int64_t>());
    return j.get<Amount>();
}

void configureAccounts(Banker& banker, const json& config)
{
    auto it = config.find("accounts");
    if (it == config.end()) return;
    if (!it->is_object())
        throw std::runtime_error("banker config: accounts must be an object");

    for (const auto& [name, value] : it->items()) {
        try {
            AccountKey account(name);
            auto budget = value.find("budget");
            if (!value.is_object() || budget == value.end()) {
                banker.createAccount(account);
                continue;
            }
            Amount amount = parseBudget(*budget);
            if (amount.isNegative()) throw std::invalid_argument("negative budget");
            banker.setBudget(account, amount);
        }
        catch (const std::exception& e) {
            throw std::runtime_error("banker config: account '" + name + "': " + e.what());
        }
    }
}

} // namespace

std::unique_ptr<Banker> makeBanker(const json& config)
{
    if (!config.is_null() && !config.is_object())
        throw std::runtime_error("banker config must be an object");

    std::string type = "memory";
    if (config.is_object() && config.contains("type")) {
        if (!config.at("type").is_string())
            throw std::runtime_error("banker config: type must be a string");
        type = config.at("type").get<std::string>();
    }

    if (type == "null") return std::make_unique<NullBanker>();

    std::unique_ptr<Banker> banker;
    if (type == "memory") {
        banker = std::make_unique<InMemoryBanker>();
    }
    else if (type == "redis") {
#ifdef RTB_WITH_REDIS
        RedisBankerConfig redis;
        redis.host = config.value("host", redis.host);
        redis.port = config.value("port", redis.port);
        redis.prefix = config.value("prefix", redis.prefix);
        banker = std::make_unique<RedisBanker>(redis);
#else
        throw std::runtime_error("banker config: this build has no redis support "
                                 "(configure with -DRTB_WITH_REDIS=ON)");
#endif
    }
    else {
        throw std::runtime_error("banker config: unknown type '" + type + "'");
    }

    if (config.is_object()) configureAccounts(*banker, config);
    return banker;
}

} // namespace RTBKIT
