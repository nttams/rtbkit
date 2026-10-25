// Banker backed by Redis (hiredis). Same rules as InMemoryBanker, see
// in_memory_banker.h. Only built with -DRTB_WITH_REDIS=ON.
//
// Layout (P = prefix):
//   P:accounts       set of account names
//   P:acct:<name>    hash: budget, budgetSet, spent, inFlight, cur
//                    (spent / inFlight include descendants)
//   P:items          hash: "<account>|<itemId>" -> reserved amount
//   P:won:<field>    marker, expires after 7 days; makes repeats idempotent
// authorize and settle are Lua scripts, so they are atomic across processes.
// Amounts are stored as integers; an account keeps the first currency it sees.
// One connection guarded by a mutex. Throws std::runtime_error when Redis
// is unreachable or answers with an error.
#pragma once

#include "banker/banker.h"

#include <mutex>

struct redisContext;

namespace RTBKIT {

struct RedisBankerConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string prefix = "rtb";
};

class RedisBanker : public Banker {
public:
    /** Connects now; throws std::runtime_error if that fails. */
    explicit RedisBanker(const RedisBankerConfig& config = {});
    ~RedisBanker() override;

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
    struct Reply;   // owns a redisReply
    Reply run(const std::vector<std::string>& args) const;
    void connect() const;
    int64_t settle(const AccountKey& account, const std::string& itemId,
                   const std::string& mode, const Amount& paid);
    Amount effectiveBudget(const AccountKey& account,
                           const std::vector<AccountKey>& all) const;

    RedisBankerConfig config_;
    mutable std::mutex mutex_;
    mutable redisContext* context_ = nullptr;
};

} // namespace RTBKIT
