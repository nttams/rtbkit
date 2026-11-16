#include "banker/redis_banker.h"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>

#include <unistd.h>

namespace RTBKIT {

namespace {

// One sync request of one account; atomic in Redis.
// KEYS: accounts set, seq hash, then the account chain (leaf first)
// ARGV: slave field, seq, spent, fromAllocation, giveBack, want, currency,
//       then the account names of the chain (same order as the keys)
// spent:          paid since the last request (goes into spent of the chain)
// fromAllocation: the part of spent that was paid out of our allocation
// giveBack:       allocation we return unused
// want:           allocation we ask for
// Returns the allocation granted (0 .. want): limited by the leaf's budget
// (unset == 0) and by every ancestor with an explicit budget, counting spent
// and what all slaves hold. A request with the same seq as the last one of
// this slave is not applied again; the stored grant is returned.
const char* SYNC_SCRIPT = R"lua(
local stored = redis.call('HGET', KEYS[2], ARGV[1])
if stored then
  local s, g = string.match(stored, '^(%d+)|(%-?%d+)$')
  if s == ARGV[2] then return tonumber(g) end
end
local spent = tonumber(ARGV[3])
local release = tonumber(ARGV[4]) + tonumber(ARGV[5])
local want = tonumber(ARGV[6])
if redis.call('SISMEMBER', KEYS[1], ARGV[8]) == 0 then
  if spent == 0 then return 0 end
  for i = 8, #ARGV do redis.call('SADD', KEYS[1], ARGV[i]) end
end
for i = 3, #KEYS do
  if spent ~= 0 then redis.call('HINCRBY', KEYS[i], 'spent', ARGV[3]) end
  if release ~= 0 then
    redis.call('HINCRBY', KEYS[i], 'allocated', string.format('%.0f', -release))
  end
  if ARGV[7] ~= '' then redis.call('HSETNX', KEYS[i], 'cur', ARGV[7]) end
end
local grant = 0
if want > 0 then
  grant = want
  for i = 3, #KEYS do
    local v = redis.call('HMGET', KEYS[i], 'budget', 'budgetSet', 'spent', 'allocated')
    if i == 3 or v[2] == '1' then
      local available = (tonumber(v[1]) or 0) - (tonumber(v[3]) or 0) - (tonumber(v[4]) or 0)
      if available < grant then grant = available end
    end
  end
  if grant < 0 then grant = 0 end
  if grant > 0 then
    for i = 3, #KEYS do
      redis.call('HINCRBY', KEYS[i], 'allocated', string.format('%.0f', grant))
    end
  end
end
redis.call('HSET', KEYS[2], ARGV[1], ARGV[2] .. '|' .. string.format('%.0f', grant))
redis.call('EXPIRE', KEYS[2], 604800)
return grant
)lua";

// KEYS: accounts set; ARGV: prefix
// Returns name, budget, budgetSet, spent, cur for every account (flat).
const char* REFRESH_SCRIPT = R"lua(
local out = {}
for _, name in ipairs(redis.call('SMEMBERS', KEYS[1])) do
  local v = redis.call('HMGET', ARGV[1] .. ':acct:' .. name, 'budget', 'budgetSet', 'spent', 'cur')
  out[#out + 1] = name
  for i = 1, 4 do out[#out + 1] = v[i] or '' end
end
return out
)lua";

constexpr size_t MAX_LOCAL_ACCOUNTS = 100000;

std::vector<AccountKey> chainOf(const AccountKey& account)
{
    std::vector<AccountKey> result;
    for (size_t n = account.size(); n > 0; --n)
        result.push_back(AccountKey(std::vector<std::string>(
            account.begin(), account.begin() + long(n))));
    return result;
}

std::string currencyOf(const Amount& amount)
{
    return amount.currencyCode == CurrencyCode::None ? "" : toString(amount.currencyCode);
}

Amount makeAmount(int64_t value, const std::string& cur)
{
    if (value == 0 || cur.empty()) return Amount();
    return Amount(parseCurrencyCode(cur), value);
}

int64_t toInt(const std::string& text) { return text.empty() ? 0 : std::stoll(text); }

std::string itemKey(const AccountKey& account, const std::string& itemId)
{
    return account.toString() + '|' + itemId;
}

} // namespace

struct RedisBanker::Reply {
    explicit Reply(redisReply* r) : reply(r) {}
    Reply(Reply&& other) noexcept : reply(other.reply) { other.reply = nullptr; }
    Reply(const Reply&) = delete;
    Reply& operator=(const Reply&) = delete;
    ~Reply() { if (reply) freeReplyObject(reply); }

    redisReply* reply;
};

RedisBanker::RedisBanker(const RedisBankerConfig& config) : config_(config)
{
    if (config_.syncIntervalMs < 1) throw std::invalid_argument("syncIntervalMs must be positive");
    if (config_.spendRate < 0) throw std::invalid_argument("spendRate must not be negative");
    block_ = std::max<int64_t>(1, config_.spendRate * config_.syncIntervalMs / 1000);

    std::random_device rd;
    slaveId_ = std::to_string(::getpid()) + "-" + std::to_string(rd()) + std::to_string(rd());

    thread_ = std::thread([this] { run(); });
}

RedisBanker::~RedisBanker()
{
    shutdown();
    if (context_) redisFree(context_);
}

void RedisBanker::run()
{
    std::unique_lock<std::mutex> lock(stopMutex_);
    while (!stopping_) {
        if (stopCv_.wait_for(lock, std::chrono::milliseconds(config_.syncIntervalMs),
                             [this] { return stopping_; }))
            break;
        lock.unlock();
        try { sync(); }
        catch (const std::exception& e) { logProblem(e.what()); }
        lock.lock();
    }
}

void RedisBanker::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(stopMutex_);
        if (shutDown_) return;
        shutDown_ = true;
        stopping_ = true;
    }
    stopCv_.notify_all();
    if (thread_.joinable()) thread_.join();

    {
        std::lock_guard<std::mutex> lock(state_);
        closed_ = true;
    }
    std::lock_guard<std::mutex> io(io_);
    int sent = 0;
    // A grant that was still on its way is returned by the next round.
    for (int round = 0; round < 4; ++round) {
        sent = syncLocked(true);
        if (sent <= 0) break;
    }
    if (sent < 0) {
        int64_t lost = 0;
        {
            std::lock_guard<std::mutex> lock(state_);
            for (const auto& [account, l] : locals_)
                lost += l.unreported + (l.pending ? l.pending->spent : 0);
        }
        std::cerr << "banker: redis unreachable at shutdown, unreported spend of "
                  << lost << " is lost and the allocation stays held in Redis\n";
    }
}

void RedisBanker::logProblem(const std::string& what)
{
    auto now = std::chrono::steady_clock::now();
    if (lastLog_ != std::chrono::steady_clock::time_point{}
        && now - lastLog_ < std::chrono::seconds(10))
        return;
    lastLog_ = now;
    std::cerr << "banker: redis sync failed (will retry): " << what << '\n';
}

void RedisBanker::connectLocked()
{
    if (context_) {
        redisFree(context_);
        context_ = nullptr;
    }
    timeval tv;
    tv.tv_sec = config_.ioTimeoutMs / 1000;
    tv.tv_usec = (config_.ioTimeoutMs % 1000) * 1000;
    redisContext* c = redisConnectWithTimeout(config_.host.c_str(), config_.port, tv);
    if (!c) throw std::runtime_error("redis: out of memory");
    if (c->err) {
        std::string error = c->errstr;
        redisFree(c);
        throw std::runtime_error("redis: cannot connect to " + config_.host + ":"
                                 + std::to_string(config_.port) + ": " + error);
    }
    redisSetTimeout(c, tv);
    context_ = c;
}

// Sends all commands, then reads all replies. A broken connection is dropped
// (the next call reconnects) and std::runtime_error is thrown; replies that
// are Redis errors are returned for the caller to check.
std::vector<RedisBanker::Reply> RedisBanker::pipeline(
    const std::vector<std::vector<std::string>>& commands)
{
    if (!context_) connectLocked();

    auto fail = [this](const std::string& what) {
        std::string error = context_ ? context_->errstr : "";
        if (context_) redisFree(context_);
        context_ = nullptr;
        throw std::runtime_error("redis: " + what + (error.empty() ? "" : ": " + error));
    };

    for (const auto& args : commands) {
        std::vector<const char*> argv;
        std::vector<size_t> lengths;
        for (const std::string& a : args) {
            argv.push_back(a.data());
            lengths.push_back(a.size());
        }
        if (redisAppendCommandArgv(context_, int(args.size()), argv.data(), lengths.data())
            != REDIS_OK)
            fail("cannot send");
    }
    std::vector<Reply> replies;
    for (size_t i = 0; i < commands.size(); ++i) {
        void* raw = nullptr;
        if (redisGetReply(context_, &raw) != REDIS_OK) fail("no reply");
        replies.emplace_back(static_cast<redisReply*>(raw));
    }
    return replies;
}

RedisBanker::Local& RedisBanker::local(const AccountKey& account)
{
    return locals_.try_emplace(account).first->second;
}

void RedisBanker::touchAccount(const AccountKey& account)
{
    for (const AccountKey& key : chainOf(account)) remote_.try_emplace(key);
}

// One round for every local account, in one pipeline. Returns the number of
// account requests sent, or -1 if Redis could not be reached; requests without
// an answer stay pending and are sent again unchanged.
int RedisBanker::syncLocked(bool final)
{
    struct Sent { AccountKey account; };
    std::vector<Sent> sent;
    std::vector<std::vector<std::string>> commands;
    {
        std::lock_guard<std::mutex> lock(state_);
        for (auto& [account, l] : locals_) {
            if (!l.pending) {
                int64_t spent = l.unreported;
                int64_t fromAllocation = std::min(spent, std::max<int64_t>(l.redisHeld, 0));
                int64_t held = l.redisHeld - fromAllocation;
                int64_t free = held - l.reserved;
                // Open reservations can not be settled after the final round.
                int64_t target = (final || !l.active) ? 0 : std::max(block_, 2 * l.demand);
                int64_t giveBack = final ? held : std::max<int64_t>(0, free - target);
                int64_t want = final ? 0 : std::max<int64_t>(0, target - free);
                if (spent == 0 && giveBack == 0 && want == 0) continue;
                l.pending = Pending{++l.seq, spent, fromAllocation, giveBack, want};
                l.unreported -= spent;
                l.redisHeld -= fromAllocation + giveBack;
                l.demand = 0;
            }
            const Pending& p = *l.pending;
            std::vector<AccountKey> chain = chainOf(account);
            std::vector<std::string> cmd = {"EVAL", SYNC_SCRIPT, std::to_string(2 + chain.size()),
                                            key("accounts"), key("seq")};
            for (const AccountKey& k : chain) cmd.push_back(key("acct:" + k.toString()));
            cmd.push_back(slaveId_ + "|" + account.toString());
            cmd.push_back(std::to_string(p.seq));
            cmd.push_back(std::to_string(p.spent));
            cmd.push_back(std::to_string(p.fromAllocation));
            cmd.push_back(std::to_string(p.giveBack));
            cmd.push_back(std::to_string(p.want));
            cmd.push_back(l.cur);
            for (const AccountKey& k : chain) cmd.push_back(k.toString());
            commands.push_back(std::move(cmd));
            sent.push_back({account});
        }
    }
    commands.push_back({"EVAL", REFRESH_SCRIPT, "1", key("accounts"), config_.prefix});

    std::vector<Reply> replies;
    try {
        replies = pipeline(commands);
    }
    catch (const std::exception& e) {
        ++failedSyncs_;
        logProblem(e.what());
        return -1;
    }

    std::lock_guard<std::mutex> lock(state_);
    for (size_t i = 0; i < sent.size(); ++i) {
        const redisReply* r = replies[i].reply;
        if (r->type != REDIS_REPLY_INTEGER) {
            ++failedSyncs_;
            logProblem(r->type == REDIS_REPLY_ERROR ? r->str : "unexpected reply");
            continue;
        }
        Local& l = locals_.at(sent[i].account);
        l.redisHeld += r->integer;
        l.pending.reset();
    }

    const redisReply* all = replies.back().reply;
    if (all->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i + 4 < all->elements; i += 5) {
            auto text = [&](size_t j) {
                const redisReply* e = all->element[i + j];
                return e->type == REDIS_REPLY_STRING ? std::string(e->str, e->len)
                                                     : std::string();
            };
            try {
                Remote remote;
                remote.budget = toInt(text(1));
                remote.budgetSet = text(2) == "1";
                remote.spent = toInt(text(3));
                remote.cur = text(4);
                remote_[AccountKey(text(0))] = remote;
            }
            catch (const std::exception&) {
                // an account name or number written by someone else we can not read
            }
        }
    }
    else {
        ++failedSyncs_;
        logProblem("bad account listing");
    }
    return int(sent.size());
}

bool RedisBanker::sync()
{
    if (closed_) return false;
    std::lock_guard<std::mutex> io(io_);
    return syncLocked(false) >= 0;
}

void RedisBanker::createAccount(const AccountKey& account)
{
    std::vector<std::string> cmd = {"SADD", key("accounts")};
    for (const AccountKey& k : chainOf(account)) cmd.push_back(k.toString());
    {
        std::lock_guard<std::mutex> io(io_);
        auto replies = pipeline({cmd});
        if (replies[0].reply->type == REDIS_REPLY_ERROR)
            throw std::runtime_error(std::string("redis: ") + replies[0].reply->str);
    }
    std::lock_guard<std::mutex> lock(state_);
    touchAccount(account);
    local(account);
}

void RedisBanker::setBudget(const AccountKey& account, const Amount& budget)
{
    if (budget.isNegative()) throw std::invalid_argument("negative budget");
    std::vector<std::string> add = {"SADD", key("accounts")};
    for (const AccountKey& k : chainOf(account)) add.push_back(k.toString());
    std::vector<std::string> set = {"HSET", key("acct:" + account.toString()),
                                    "budget", std::to_string(budget.value), "budgetSet", "1"};
    if (!currencyOf(budget).empty()) {
        set.push_back("cur");
        set.push_back(currencyOf(budget));
    }

    std::lock_guard<std::mutex> io(io_);
    for (const auto& reply : pipeline({add, set}))
        if (reply.reply->type == REDIS_REPLY_ERROR)
            throw std::runtime_error(std::string("redis: ") + reply.reply->str);
    {
        std::lock_guard<std::mutex> lock(state_);
        touchAccount(account);
        Remote& remote = remote_[account];
        remote.budget = budget.value;
        remote.budgetSet = true;
        if (!currencyOf(budget).empty() && remote.cur.empty()) remote.cur = currencyOf(budget);
        bool leaf = true;
        for (const auto& [other, r] : remote_) {
            (void) r;
            if (other.size() == account.size() + 1 && other.hasPrefix(account)) leaf = false;
        }
        Local& l = local(account);
        if (leaf) l.active = true;
    }
    syncLocked(false);   // take the allocation now; a failure is logged and retried
}

bool RedisBanker::authorizeBid(const AccountKey& account, const std::string& itemId,
                               const Amount& amount)
{
    if (amount.isNegative()) return false;
    std::lock_guard<std::mutex> lock(state_);
    if (closed_) return false;
    const std::string k = itemKey(account, itemId);
    if (settled_.count(k)) return false;

    auto it = locals_.find(account);
    if (it == locals_.end()) {
        // Unknown here: refuse, but remember it so the next round looks for
        // an allocation (the account may exist in Redis already).
        if (locals_.size() < MAX_LOCAL_ACCOUNTS) local(account).active = true;
        return false;
    }
    Local& l = it->second;
    l.active = true;
    auto reservation = reservations_.find(k);
    const int64_t old = reservation == reservations_.end() ? 0 : reservation->second;
    if (l.held() - (l.reserved - old) < amount.value) return false;

    if (l.cur.empty()) l.cur = currencyOf(amount);
    l.reserved += amount.value - old;
    if (amount.value > old) l.demand += amount.value - old;
    reservations_[k] = amount.value;
    return true;
}

void RedisBanker::cancelBid(const AccountKey& account, const std::string& itemId)
{
    std::lock_guard<std::mutex> lock(state_);
    auto reservation = reservations_.find(itemKey(account, itemId));
    if (reservation == reservations_.end()) return;
    auto it = locals_.find(account);
    if (it != locals_.end()) it->second.reserved -= reservation->second;
    reservations_.erase(reservation);
}

void RedisBanker::winBid(const AccountKey& account, const std::string& itemId,
                         const Amount& paid)
{
    if (paid.isNegative()) throw std::invalid_argument("negative paid amount");
    std::lock_guard<std::mutex> lock(state_);
    const std::string k = itemKey(account, itemId);
    if (settled_.count(k)) return;

    touchAccount(account);
    Local& l = local(account);
    int64_t reserved = 0;
    auto reservation = reservations_.find(k);
    if (reservation != reservations_.end()) {
        reserved = reservation->second;
        l.reserved -= reserved;
        reservations_.erase(reservation);
    }
    else {
        l.demand += paid.value;   // no earlier authorize counted it
    }
    if (l.cur.empty()) l.cur = currencyOf(paid);
    l.unreported += paid.value;

    settled_.insert(k);
    settledOrder_.push_back(k);
    while (settledOrder_.size() > config_.maxSettledItems) {
        settled_.erase(settledOrder_.front());
        settledOrder_.pop_front();
    }
    if (reserved > 0 && paid.value > reserved)
        std::cerr << "banker: item " << k << " paid " << paid << " but only " << reserved
                  << " was reserved\n";
}

void RedisBanker::forceWinBid(const AccountKey& account, const std::string& itemId,
                              const Amount& paid)
{
    winBid(account, itemId, paid);
}

Amount RedisBanker::effectiveBudget(const AccountKey& account) const
{
    auto it = remote_.find(account);
    if (it != remote_.end() && it->second.budgetSet)
        return makeAmount(it->second.budget, it->second.cur);
    Amount sum;
    for (const auto& [key, remote] : remote_) {
        (void) remote;
        if (key.size() == account.size() + 1 && key.hasPrefix(account))
            sum += effectiveBudget(key);
    }
    return sum;
}

AccountSummary RedisBanker::getAccountSummary(const AccountKey& account) const
{
    AccountSummary summary;
    summary.account = account;
    std::lock_guard<std::mutex> lock(state_);
    auto it = remote_.find(account);
    if (it == remote_.end()) return summary;

    summary.budget = effectiveBudget(account);
    int64_t spent = it->second.spent, inFlight = 0;
    std::string cur = it->second.cur;
    for (const auto& [key, l] : locals_) {
        if (!key.hasPrefix(account)) continue;
        spent += l.unreported + (l.pending ? l.pending->spent : 0);
        inFlight += l.reserved;
        if (cur.empty()) cur = l.cur;
    }
    summary.spent = makeAmount(spent, cur);
    summary.inFlight = makeAmount(inFlight, cur);
    return summary;
}

std::vector<AccountKey> RedisBanker::accounts() const
{
    std::lock_guard<std::mutex> lock(state_);
    std::vector<AccountKey> result;
    for (const auto& [key, remote] : remote_) {
        (void) remote;
        result.push_back(key);
    }
    std::sort(result.begin(), result.end());
    return result;
}

Amount RedisBanker::heldAllocation(const AccountKey& account) const
{
    std::lock_guard<std::mutex> lock(state_);
    auto it = locals_.find(account);
    if (it == locals_.end()) return Amount();
    return makeAmount(it->second.held(), it->second.cur.empty() ? "USD" : it->second.cur);
}

} // namespace RTBKIT
