#include "banker/redis_banker.h"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace RTBKIT {

namespace {

// KEYS: accounts set, items hash, then the account chain (leaf first)
// ARGV: leaf name, item field, amount, currency ("" if none), prefix
// Returns 1 if reserved, 0 if refused.
const char* AUTHORIZE_SCRIPT = R"lua(
if redis.call('SISMEMBER', KEYS[1], ARGV[1]) == 0 then return 0 end
if redis.call('EXISTS', ARGV[5] .. ':won:' .. ARGV[2]) == 1 then return 0 end
local amount = tonumber(ARGV[3])
local old = tonumber(redis.call('HGET', KEYS[2], ARGV[2]) or '0')
for i = 3, #KEYS do
  local v = redis.call('HMGET', KEYS[i], 'budget', 'budgetSet', 'spent', 'inFlight')
  if i == 3 or v[2] == '1' then
    local available = (tonumber(v[1]) or 0) - (tonumber(v[3]) or 0)
                      - ((tonumber(v[4]) or 0) - old)
    if available < amount then return 0 end
  end
end
local delta = amount - old
for i = 3, #KEYS do
  if delta ~= 0 then
    redis.call('HINCRBY', KEYS[i], 'inFlight', string.format('%.0f', delta))
  end
  if ARGV[4] ~= '' then redis.call('HSETNX', KEYS[i], 'cur', ARGV[4]) end
end
redis.call('HSET', KEYS[2], ARGV[2], ARGV[3])
return 1
)lua";

// KEYS: accounts set, items hash, then the account chain (leaf first)
// ARGV: item field, mode ("cancel" or "win"), paid, currency, prefix,
//       then the account names of the chain (same order as the keys)
// Returns the amount that was reserved, or -1 if the item was already won.
const char* SETTLE_SCRIPT = R"lua(
if redis.call('EXISTS', ARGV[5] .. ':won:' .. ARGV[1]) == 1 then return -1 end
local reservedText = redis.call('HGET', KEYS[2], ARGV[1])
local reserved = tonumber(reservedText or '0')
if ARGV[2] == 'cancel' and not reservedText then return 0 end
for i = 3, #KEYS do
  if reserved ~= 0 then
    redis.call('HINCRBY', KEYS[i], 'inFlight', string.format('%.0f', -reserved))
  end
  if ARGV[2] == 'win' then
    if tonumber(ARGV[3]) ~= 0 then
      redis.call('HINCRBY', KEYS[i], 'spent', ARGV[3])
    end
    redis.call('SADD', KEYS[1], ARGV[i + 3])
    if ARGV[4] ~= '' then redis.call('HSETNX', KEYS[i], 'cur', ARGV[4]) end
  end
end
redis.call('HDEL', KEYS[2], ARGV[1])
if ARGV[2] == 'win' then
  redis.call('SET', ARGV[5] .. ':won:' .. ARGV[1], '1', 'EX', 604800)
end
return reserved
)lua";

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

} // namespace

struct RedisBanker::Reply {
    explicit Reply(redisReply* r) : reply(r) {}
    Reply(Reply&& other) noexcept : reply(other.reply) { other.reply = nullptr; }
    Reply(const Reply&) = delete;
    Reply& operator=(const Reply&) = delete;
    ~Reply() { if (reply) freeReplyObject(reply); }

    redisReply* reply;

    int64_t integer() const
    {
        if (reply->type == REDIS_REPLY_INTEGER) return reply->integer;
        if (reply->type == REDIS_REPLY_STRING) return std::stoll(reply->str);
        throw std::runtime_error("redis: expected an integer reply");
    }
};

RedisBanker::RedisBanker(const RedisBankerConfig& config) : config_(config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    connect();
}

RedisBanker::~RedisBanker()
{
    if (context_) redisFree(context_);
}

void RedisBanker::connect() const
{
    if (context_) {
        redisFree(context_);
        context_ = nullptr;
    }
    redisContext* c = redisConnect(config_.host.c_str(), config_.port);
    if (!c) throw std::runtime_error("redis: out of memory");
    if (c->err) {
        std::string error = c->errstr;
        redisFree(c);
        throw std::runtime_error("redis: cannot connect to " + config_.host + ":"
                                 + std::to_string(config_.port) + ": " + error);
    }
    context_ = c;
}

RedisBanker::Reply RedisBanker::run(const std::vector<std::string>& args) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ || context_->err) connect();   // reconnect before sending only

    std::vector<const char*> argv;
    std::vector<size_t> lengths;
    for (const std::string& a : args) {
        argv.push_back(a.data());
        lengths.push_back(a.size());
    }
    void* raw = redisCommandArgv(context_, int(args.size()), argv.data(), lengths.data());
    if (!raw) throw std::runtime_error(std::string("redis: ") + context_->errstr);

    Reply reply(static_cast<redisReply*>(raw));
    if (reply.reply->type == REDIS_REPLY_ERROR)
        throw std::runtime_error(std::string("redis: ") + reply.reply->str);
    return reply;
}

void RedisBanker::createAccount(const AccountKey& account)
{
    std::vector<std::string> cmd = {"SADD", config_.prefix + ":accounts"};
    for (const AccountKey& key : chainOf(account)) cmd.push_back(key.toString());
    run(cmd);
}

void RedisBanker::setBudget(const AccountKey& account, const Amount& budget)
{
    if (budget.isNegative()) throw std::invalid_argument("negative budget");
    createAccount(account);
    std::vector<std::string> cmd = {"HSET", config_.prefix + ":acct:" + account.toString(),
                                    "budget", std::to_string(budget.value),
                                    "budgetSet", "1"};
    if (!currencyOf(budget).empty()) {
        cmd.push_back("cur");
        cmd.push_back(currencyOf(budget));
    }
    run(cmd);
}

bool RedisBanker::authorizeBid(const AccountKey& account, const std::string& itemId,
                               const Amount& amount)
{
    if (amount.isNegative()) return false;
    std::vector<AccountKey> keys = chainOf(account);
    std::vector<std::string> cmd = {"EVAL", AUTHORIZE_SCRIPT,
                                    std::to_string(2 + keys.size()),
                                    config_.prefix + ":accounts", config_.prefix + ":items"};
    for (const AccountKey& key : keys)
        cmd.push_back(config_.prefix + ":acct:" + key.toString());
    cmd.push_back(account.toString());
    cmd.push_back(account.toString() + '|' + itemId);
    cmd.push_back(std::to_string(amount.value));
    cmd.push_back(currencyOf(amount));
    cmd.push_back(config_.prefix);
    return run(cmd).integer() == 1;
}

int64_t RedisBanker::settle(const AccountKey& account, const std::string& itemId,
                            const std::string& mode, const Amount& paid)
{
    std::vector<AccountKey> keys = chainOf(account);
    std::vector<std::string> cmd = {"EVAL", SETTLE_SCRIPT, std::to_string(2 + keys.size()),
                                    config_.prefix + ":accounts", config_.prefix + ":items"};
    for (const AccountKey& key : keys)
        cmd.push_back(config_.prefix + ":acct:" + key.toString());
    cmd.push_back(account.toString() + '|' + itemId);
    cmd.push_back(mode);
    cmd.push_back(std::to_string(paid.value));
    cmd.push_back(currencyOf(paid));
    cmd.push_back(config_.prefix);
    for (const AccountKey& key : keys) cmd.push_back(key.toString());
    return run(cmd).integer();
}

void RedisBanker::cancelBid(const AccountKey& account, const std::string& itemId)
{
    settle(account, itemId, "cancel", Amount());
}

void RedisBanker::winBid(const AccountKey& account, const std::string& itemId,
                         const Amount& paid)
{
    if (paid.isNegative()) throw std::invalid_argument("negative paid amount");
    int64_t reserved = settle(account, itemId, "win", paid);
    if (reserved > 0 && paid.value > reserved)
        std::cerr << "banker: item " << account << '|' << itemId << " paid " << paid
                  << " but only " << reserved << " was reserved\n";
}

void RedisBanker::forceWinBid(const AccountKey& account, const std::string& itemId,
                              const Amount& paid)
{
    winBid(account, itemId, paid);
}

AccountSummary RedisBanker::getAccountSummary(const AccountKey& account) const
{
    AccountSummary summary;
    summary.account = account;
    if (run({"SISMEMBER", config_.prefix + ":accounts", account.toString()}).integer() == 0)
        return summary;
    summary.budget = effectiveBudget(account, accounts());

    Reply r = run({"HMGET", config_.prefix + ":acct:" + account.toString(),
                   "spent", "inFlight", "cur"});
    auto field = [&](size_t i) {
        redisReply* e = r.reply->element[i];
        return e->type == REDIS_REPLY_STRING ? std::string(e->str, e->len) : std::string();
    };
    std::string cur = field(2);
    std::string spent = field(0), inFlight = field(1);
    summary.spent = makeAmount(spent.empty() ? 0 : std::stoll(spent), cur);
    summary.inFlight = makeAmount(inFlight.empty() ? 0 : std::stoll(inFlight), cur);
    return summary;
}

Amount RedisBanker::effectiveBudget(const AccountKey& account,
                                    const std::vector<AccountKey>& all) const
{
    Reply r = run({"HMGET", config_.prefix + ":acct:" + account.toString(),
                   "budget", "budgetSet", "cur"});
    auto field = [&](size_t i) {
        redisReply* e = r.reply->element[i];
        return e->type == REDIS_REPLY_STRING ? std::string(e->str, e->len) : std::string();
    };
    if (field(1) == "1") {
        std::string budget = field(0);
        return makeAmount(budget.empty() ? 0 : std::stoll(budget), field(2));
    }
    Amount sum;
    for (const AccountKey& key : all)
        if (key.size() == account.size() + 1 && key.hasPrefix(account))
            sum += effectiveBudget(key, all);
    return sum;
}

std::vector<AccountKey> RedisBanker::accounts() const
{
    Reply r = run({"SMEMBERS", config_.prefix + ":accounts"});
    std::vector<AccountKey> result;
    for (size_t i = 0; i < r.reply->elements; ++i)
        result.push_back(AccountKey(std::string(r.reply->element[i]->str,
                                                r.reply->element[i]->len)));
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace RTBKIT
