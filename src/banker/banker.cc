#include "banker/banker.h"

namespace RTBKIT {

using nlohmann::json;

json AccountSummary::toJson() const
{
    return json{{"account", account.toJson()},
                {"budget", budget},
                {"spent", spent},
                {"inFlight", inFlight},
                {"available", available()}};
}

AccountSummary AccountSummary::fromJson(const json& j)
{
    try {
        AccountSummary s;
        s.account = AccountKey::fromJson(j.at("account"));
        s.budget = j.at("budget").get<Amount>();
        s.spent = j.at("spent").get<Amount>();
        s.inFlight = j.at("inFlight").get<Amount>();
        return s;
    }
    catch (const json::exception& e) {
        throw std::runtime_error(std::string("invalid account summary: ") + e.what());
    }
    catch (const std::invalid_argument& e) {
        throw std::runtime_error(std::string("invalid account summary: ") + e.what());
    }
}

json Banker::summaryJson() const
{
    json list = json::array();
    for (const AccountKey& account : accounts())
        list.push_back(getAccountSummary(account).toJson());
    return json{{"accounts", list}};
}

} // namespace RTBKIT
