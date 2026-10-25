#include "banker/in_memory_banker.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace RTBKIT {

InMemoryBanker::InMemoryBanker(size_t maxSettledItems) : maxSettled_(maxSettledItems) {}

std::vector<AccountKey> InMemoryBanker::chain(const AccountKey& account)
{
    std::vector<AccountKey> result;
    for (size_t n = account.size(); n > 0; --n)
        result.push_back(AccountKey(std::vector<std::string>(
            account.begin(), account.begin() + long(n))));
    return result;
}

std::string InMemoryBanker::itemKey(const AccountKey& account, const std::string& itemId)
{
    return account.toString() + '|' + itemId;
}

void InMemoryBanker::ensure(const std::vector<AccountKey>& keys)
{
    for (const AccountKey& key : keys) nodes_.try_emplace(key);
}

// Computes every new value first, so a currency mismatch throws before
// anything changed.
void InMemoryBanker::adjust(const std::vector<AccountKey>& keys,
                            const Amount& dInFlight, const Amount& dSpent)
{
    std::vector<std::pair<Amount, Amount>> next;
    next.reserve(keys.size());
    for (const AccountKey& key : keys) {
        const Node& node = nodes_.at(key);
        next.emplace_back(node.inFlight + dInFlight, node.spent + dSpent);
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        Node& node = nodes_.at(keys[i]);
        node.inFlight = next[i].first;
        node.spent = next[i].second;
    }
}

Amount InMemoryBanker::effectiveBudget(const AccountKey& account) const
{
    const Node& node = nodes_.at(account);
    if (node.budgetSet) return node.budget;
    Amount sum;
    for (const auto& [key, child] : nodes_) {
        (void) child;
        if (key.size() == account.size() + 1 && key.hasPrefix(account))
            sum += effectiveBudget(key);
    }
    return sum;
}

void InMemoryBanker::createAccount(const AccountKey& account)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ensure(chain(account));
}

void InMemoryBanker::setBudget(const AccountKey& account, const Amount& budget)
{
    if (budget.isNegative()) throw std::invalid_argument("negative budget");
    std::lock_guard<std::mutex> lock(mutex_);
    ensure(chain(account));
    Node& node = nodes_.at(account);
    node.budget = budget;
    node.budgetSet = true;
}

bool InMemoryBanker::authorizeBid(const AccountKey& account,
                                  const std::string& itemId, const Amount& amount)
{
    if (amount.isNegative()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!nodes_.contains(account)) return false;

    std::string key = itemKey(account, itemId);
    if (settled_.contains(key)) return false;

    auto existing = reservations_.find(key);
    Amount old = existing == reservations_.end() ? Amount() : existing->second;

    std::vector<AccountKey> keys = chain(account);
    for (size_t i = 0; i < keys.size(); ++i) {
        const Node& node = nodes_.at(keys[i]);
        if (i != 0 && !node.budgetSet) continue;
        Amount available = node.budget - node.spent - (node.inFlight - old);
        if (available < amount) return false;
    }

    adjust(keys, amount - old, Amount());
    reservations_[key] = amount;
    return true;
}

void InMemoryBanker::cancelBid(const AccountKey& account, const std::string& itemId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = reservations_.find(itemKey(account, itemId));
    if (it == reservations_.end()) return;
    adjust(chain(account), -it->second, Amount());
    reservations_.erase(it);
}

void InMemoryBanker::settle(const AccountKey& account, const std::string& itemId,
                            const Amount& paid)
{
    if (paid.isNegative()) throw std::invalid_argument("negative paid amount");
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = itemKey(account, itemId);
    if (settled_.contains(key)) return;

    std::vector<AccountKey> keys = chain(account);
    ensure(keys);

    auto it = reservations_.find(key);
    Amount reserved = it == reservations_.end() ? Amount() : it->second;
    adjust(keys, -reserved, paid);
    if (it != reservations_.end()) reservations_.erase(it);

    if (!reserved.isZero() && paid > reserved)
        std::cerr << "banker: item " << key << " paid " << paid
                  << " but only " << reserved << " was reserved\n";

    settled_.insert(key);
    settledOrder_.push_back(key);
    while (settledOrder_.size() > maxSettled_) {
        settled_.erase(settledOrder_.front());
        settledOrder_.pop_front();
    }
}

void InMemoryBanker::winBid(const AccountKey& account, const std::string& itemId,
                            const Amount& paid)
{
    settle(account, itemId, paid);
}

void InMemoryBanker::forceWinBid(const AccountKey& account, const std::string& itemId,
                                 const Amount& paid)
{
    settle(account, itemId, paid);
}

AccountSummary InMemoryBanker::getAccountSummary(const AccountKey& account) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    AccountSummary summary;
    summary.account = account;
    auto it = nodes_.find(account);
    if (it == nodes_.end()) return summary;
    summary.budget = effectiveBudget(account);
    summary.spent = it->second.spent;
    summary.inFlight = it->second.inFlight;
    return summary;
}

std::vector<AccountKey> InMemoryBanker::accounts() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AccountKey> result;
    result.reserve(nodes_.size());
    for (const auto& [key, node] : nodes_) {
        (void) node;
        result.push_back(key);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace RTBKIT
