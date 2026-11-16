#include "banker/banker_factory.h"

#include "banker/in_memory_banker.h"
#ifdef RTB_WITH_REDIS
#include "banker/redis_banker.h"
#endif

#include <algorithm>
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

    // Deepest accounts first: the redis banker only gives allocation to a
    // budgeted account that has no children yet (see redis_banker.h).
    std::vector<std::pair<std::string, const json*>> entries;
    for (const auto& [name, value] : it->items()) entries.emplace_back(name, &value);
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return std::count(a.first.begin(), a.first.end(), ':')
             > std::count(b.first.begin(), b.first.end(), ':');
    });

    for (const auto& [name, valuePtr] : entries) {
        const json& value = *valuePtr;
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
        redis.syncIntervalMs = config.value("syncIntervalMs", redis.syncIntervalMs);
        redis.ioTimeoutMs = config.value("ioTimeoutMs", redis.ioTimeoutMs);
        redis.maxSettledItems = config.value("maxSettledItems", redis.maxSettledItems);
        if (config.contains("spendRate")) {
            Amount rate = parseBudget(config.at("spendRate"));
            if (rate.isNegative())
                throw std::runtime_error("banker config: spendRate must not be negative");
            redis.spendRate = rate.value;
        }
        if (redis.syncIntervalMs < 1 || redis.ioTimeoutMs < 1)
            throw std::runtime_error("banker config: syncIntervalMs and ioTimeoutMs must be positive");
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
