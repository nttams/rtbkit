// In-process banker with hierarchical accounts. Thread safe (one mutex).
//
// Rules:
//  * authorizeBid is checked on the named account (its own budget, unset == 0)
//    and on every ancestor that has an explicit budget (a cap for the whole
//    subtree). Ancestors without a budget are not limited.
//  * spent / inFlight of an account include all its descendants.
//  * budget in a summary is the explicit budget, or, for an account without
//    one, the sum of its children's budgets.
//  * authorizing the same item again replaces the reservation if the new
//    amount fits (the old one counts as released); otherwise the old
//    reservation stays and false is returned.
//  * winBid / forceWinBid settle an item once. A repeat for the same item is
//    ignored, and so is a later authorizeBid for it. If paid is more than
//    reserved, paid is still charged and a line goes to stderr.
//  * forceWinBid on an item that is reserved releases the reservation too.
//  * cancelBid on an unknown or settled item, and authorizeBid on an unknown
//    account, do nothing; winBid / forceWinBid create the account.
//  * Negative budgets or paid amounts throw std::invalid_argument; a negative
//    authorize amount returns false.
#pragma once

#include "banker/banker.h"

#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace RTBKIT {

class InMemoryBanker : public Banker {
public:
    /** maxSettledItems: how many won item ids are remembered to ignore
        repeated settlements (oldest forgotten first). */
    explicit InMemoryBanker(size_t maxSettledItems = 1'000'000);

    void createAccount(const AccountKey& account) override;
    void setBudget(const AccountKey& account, const Amount& budget) override;
    bool authorizeBid(const AccountKey& account, const std::string& itemId,
                      const Amount& amount) override;
    void cancelBid(const AccountKey& account, const std::string& itemId) override;
    void winBid(const AccountKey& account, const std::string& itemId,
                const Amount& paid) override;
    void forceWinBid(const AccountKey& account, const std::string& itemId,
                     const Amount& paid) override;
    AccountSummary getAccountSummary(const AccountKey& account) const override;
    std::vector<AccountKey> accounts() const override;

private:
    struct Node {
        Amount budget;
        bool budgetSet = false;
        Amount spent;      // includes descendants
        Amount inFlight;   // includes descendants
    };

    // All private functions expect mutex_ to be held.
    static std::vector<AccountKey> chain(const AccountKey& account);
    void ensure(const std::vector<AccountKey>& chain);
    void adjust(const std::vector<AccountKey>& chain, const Amount& dInFlight,
                const Amount& dSpent);
    Amount effectiveBudget(const AccountKey& account) const;
    void settle(const AccountKey& account, const std::string& itemId,
                const Amount& paid);
    static std::string itemKey(const AccountKey& account, const std::string& itemId);

    size_t maxSettled_;
    mutable std::mutex mutex_;
    std::unordered_map<AccountKey, Node> nodes_;
    std::unordered_map<std::string, Amount> reservations_;
    std::unordered_set<std::string> settled_;
    std::deque<std::string> settledOrder_;
};

} // namespace RTBKIT
