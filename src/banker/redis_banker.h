// Banker backed by Redis (hiredis), built like the original's SlaveBanker +
// MasterBanker. Only built with -DRTB_WITH_REDIS=ON.
//
// Hot path: authorizeBid, cancelBid, winBid and forceWinBid never touch the
// network. They work on a local, in-memory "shadow" view of each account that
// holds a budget allocation (a block of the account's budget taken from Redis).
// A background thread talks to Redis every syncIntervalMs (default 1000, the
// original's syncRate of 1 s):
//   * reports the spend made since the last round,
//   * gives back allocation that is not needed (more than the target block),
//   * asks for more when the local allocation is below the target.
// The target block is spendRate * interval (like the original's spendRate,
// default 0.1 USD per second), or twice the demand of the last round if that
// is bigger, so busy accounts get bigger blocks.
// All of this is one atomic Lua script per account, so several processes can
// share one Redis and the sum of their allocations plus the spend never
// exceeds the budget. Each request carries a sequence number, so a request
// whose reply was lost can be sent again without being counted twice.
//
// Rules, compared with in_memory_banker.h. Kept: the caller passes the price
// to charge (min(win, bid)) and it is charged as given, repeated wins are
// ignored, item ids are the caller's (length prefixed) strings, the named
// account is checked against its own budget (unset == 0). Loosened, as in the
// original:
//  * Allocation is only taken for active accounts: the ones a bid was tried on,
//    and the ones setBudget was called on that have no known child (a leaf).
//    So an account with a budget that only caps its children does not hold the
//    budget back; set the children before the parent (the factory does).
//  * An account can only bid when a sync round gave it allocation, so a new
//    budget (setBudget does a round at once) or a top-up is not seen by
//    other processes before their next round. Unknown accounts are refused,
//    and picked up by the next round if they exist in Redis.
//  * Ancestors with an explicit budget cap the whole subtree, but only when
//    allocation is handed out. Allocation already held stays usable if an
//    ancestor budget is lowered or set later.
//  * Reservations (in flight) are local to this process, so
//    getAccountSummary().inFlight only counts this process. budget and spent
//    are the values of the last sync round (spent includes this process's
//    unreported spend); they are exact for one process after sync().
//  * Repeated wins are detected in this process only (a bounded set of
//    recent item ids), not across processes.
//  * A win larger than the allocation still is charged in full; the account
//    just cannot authorize until the next round brings new allocation (the
//    shared total may go over the budget, as forceWinBid always could).
//
// Redis down: nothing blocks or throws on the hot path. Authorizations fail
// once the local allocation is used up; the sync thread logs (at most one line
// per 10 s) and retries every interval. Connect and read have a timeout
// (ioTimeoutMs). createAccount / setBudget are control calls: they do network
// I/O and throw std::runtime_error when Redis can not be reached.
// The constructor does not connect or throw.
//
// Shutdown (destructor or shutdown()): stops the thread, reports pending spend
// and gives all allocation back to Redis, so Redis ends with nothing held by
// this process. Call it after the last cancelBid / winBid; reservations that
// are still open are dropped.
//
// Redis layout (P = prefix):
//   P:accounts       set of account names
//   P:acct:<name>    hash: budget, budgetSet, spent, allocated, cur
//                    (spent / allocated include descendants; allocated is
//                    what all slaves hold and have not spent yet)
//   P:seq            hash "<slave id>|<account>" -> "<seq>|<grant>" (retry guard)
#pragma once

#include "banker/banker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

struct redisContext;

namespace RTBKIT {

struct RedisBankerConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string prefix = "rtb";
    int syncIntervalMs = 1000;        ///< time between sync rounds
    int64_t spendRate = 100000;       ///< micro-units per second per account (0.1 USD)
    int ioTimeoutMs = 500;            ///< connect and read timeout of the sync thread
    size_t maxSettledItems = 1'000'000;
};

class RedisBanker : public Banker {
public:
    /** Starts the sync thread. Does not connect, never throws for Redis. */
    explicit RedisBanker(const RedisBankerConfig& config = {});
    ~RedisBanker() override;

    // Control calls: network I/O, throw std::runtime_error if Redis is down.
    void createAccount(const AccountKey& account) override;
    /** Writes the budget, then runs one sync round so the allocation is there. */
    void setBudget(const AccountKey& account, const Amount& budget) override;

    // Hot path: local only.
    bool authorizeBid(const AccountKey& account, const std::string& itemId,
                      const Amount& amount) override;
    void cancelBid(const AccountKey& account, const std::string& itemId) override;
    void winBid(const AccountKey& account, const std::string& itemId,
                const Amount& paid) override;
    void forceWinBid(const AccountKey& account, const std::string& itemId,
                     const Amount& paid) override;

    // Local views, no network.
    AccountSummary getAccountSummary(const AccountKey& account) const override;
    std::vector<AccountKey> accounts() const override;

    /** One blocking sync round now (what the thread does every interval).
        False if Redis could not be reached. */
    bool sync();

    /** Stops the thread, flushes spend and returns the allocation. Idempotent;
        the destructor calls it. After it, authorizations fail. */
    void shutdown();

    /** Allocation this process holds for the account and can still reserve or
        spend (negative after a win beyond it). */
    Amount heldAllocation(const AccountKey& account) const;

    /** Number of sync rounds that failed since the start. */
    uint64_t failedSyncs() const { return failedSyncs_; }

private:
    struct Reply;     // owns a redisReply
    struct Pending {  // request sent, answer not seen yet; re-sent unchanged
        uint64_t seq;
        int64_t spent, fromAllocation, giveBack, want;
    };
    struct Local {    // shadow account (the original's ShadowAccount)
        std::string cur;
        int64_t redisHeld = 0;   // allocation Redis counts for us (>= 0)
        int64_t unreported = 0;  // paid, not in a request yet
        int64_t reserved = 0;    // sum of reservations
        int64_t demand = 0;      // authorized since the last request
        uint64_t seq = 0;
        bool active = false;     // wants allocation (a bid was tried, or a leaf budget set)
        std::optional<Pending> pending;
        int64_t held() const { return redisHeld - unreported; }
    };
    struct Remote {   // last known Redis values
        int64_t budget = 0;
        bool budgetSet = false;
        int64_t spent = 0;
        std::string cur;
    };

    void run();                               // sync thread
    int syncLocked(bool final);               // io_ held; -1 failed, else requests sent
    std::vector<Reply> pipeline(const std::vector<std::vector<std::string>>& commands);
    void connectLocked();
    void logProblem(const std::string& what);
    Local& local(const AccountKey& account);  // state_ held; creates
    void touchAccount(const AccountKey& account);   // state_ held; adds chain to remote_
    Amount effectiveBudget(const AccountKey& account) const;   // state_ held
    std::string key(const std::string& suffix) const { return config_.prefix + ":" + suffix; }

    RedisBankerConfig config_;
    int64_t block_;
    std::string slaveId_;

    std::mutex io_;                    // the connection; taken before state_, never inside it
    redisContext* context_ = nullptr;
    std::chrono::steady_clock::time_point lastLog_{};

    mutable std::mutex state_;         // local view; never held across network I/O
    std::map<AccountKey, Local> locals_;
    std::map<AccountKey, Remote> remote_;
    std::unordered_map<std::string, int64_t> reservations_;
    std::unordered_set<std::string> settled_;
    std::deque<std::string> settledOrder_;

    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    bool stopping_ = false;
    bool shutDown_ = false;
    std::atomic<bool> closed_{false};  // set by shutdown(): no more authorizations
    std::thread thread_;
    std::atomic<uint64_t> failedSyncs_{0};
};

} // namespace RTBKIT
