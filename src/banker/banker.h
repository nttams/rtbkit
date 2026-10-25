// Budget control, simplified from rtbkit/core/banker/banker.h.
//
// An account has a budget. Before a bid is sent to the exchange the router
// calls authorizeBid(): the amount is reserved ("in flight") under an item id
// (use "<auctionId>:<adSpotId>"). Later exactly one of these happens:
//   winBid      the reservation is released and `paid` is spent
//   cancelBid   the reservation is released, nothing is spent (loss, timeout)
//   forceWinBid a win with no reservation (late win notice): `paid` is spent
//               even if it goes over budget
//
// Amounts are per impression micro-units, like everywhere else.
// Implementations must be thread safe: the router and the post auction
// service call the same banker from different threads.
// Accounts form a tree ("campaign:strategy"). A bid is authorized on the
// account it names; getAccountSummary on a parent sums its children.
#pragma once

#include "common/account_key.h"
#include "common/currency.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace RTBKIT {

struct AccountSummary {
    AccountKey account;
    Amount budget;     ///< total money given to the account
    Amount spent;      ///< money paid for won impressions
    Amount inFlight;   ///< reserved for bids that are not settled yet

    /** budget - spent - inFlight; may be negative after forceWinBid. */
    Amount available() const { return budget - spent - inFlight; }

    /** {"account": [..], "budget": .., "spent": .., "inFlight": ..,
        "available": ..}; available is derived and ignored when parsing. */
    nlohmann::json toJson() const;
    /** Throws std::runtime_error. */
    static AccountSummary fromJson(const nlohmann::json& j);
};

class Banker {
public:
    virtual ~Banker() = default;

    /** Creates the account (and its parents) if missing; no-op if it exists. */
    virtual void createAccount(const AccountKey& account) = 0;

    /** Sets the total budget of the account, creating it if needed. Spent and
        in-flight amounts are kept. */
    virtual void setBudget(const AccountKey& account, const Amount& budget) = 0;

    /** Reserves `amount` under itemId. False (and nothing reserved) if the
        account is unknown or available() < amount. Authorizing the same item
        twice replaces the first reservation. */
    virtual bool authorizeBid(const AccountKey& account,
                              const std::string& itemId,
                              const Amount& amount) = 0;

    /** Releases the reservation of itemId. Unknown items are ignored. */
    virtual void cancelBid(const AccountKey& account,
                           const std::string& itemId) = 0;

    /** Releases the reservation of itemId and spends `paid`. If itemId was
        never authorized this behaves like forceWinBid. */
    virtual void winBid(const AccountKey& account, const std::string& itemId,
                        const Amount& paid) = 0;

    /** Spends `paid` without a reservation. */
    virtual void forceWinBid(const AccountKey& account,
                             const std::string& itemId,
                             const Amount& paid) = 0;

    /** Unknown accounts give an all zero summary. */
    virtual AccountSummary getAccountSummary(const AccountKey& account) const = 0;

    /** Every account that exists, parents included. */
    virtual std::vector<AccountKey> accounts() const = 0;

    /** {"accounts": [summary, ...]} for all accounts. */
    nlohmann::json summaryJson() const;
};

/** Authorizes everything and remembers nothing. For tests and for running
    without budgets. */
class NullBanker : public Banker {
public:
    void createAccount(const AccountKey&) override {}
    void setBudget(const AccountKey&, const Amount&) override {}
    bool authorizeBid(const AccountKey&, const std::string&, const Amount&) override
    {
        return true;
    }
    void cancelBid(const AccountKey&, const std::string&) override {}
    void winBid(const AccountKey&, const std::string&, const Amount&) override {}
    void forceWinBid(const AccountKey&, const std::string&, const Amount&) override {}
    AccountSummary getAccountSummary(const AccountKey& account) const override
    {
        AccountSummary summary;
        summary.account = account;
        return summary;
    }
    std::vector<AccountKey> accounts() const override { return {}; }
};

} // namespace RTBKIT
