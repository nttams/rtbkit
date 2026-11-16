// A private redis-server for tests: free port, temp dir, no persistence.
// start() returns nullptr when no redis-server binary is found or it does not
// come up, so a test can SKIP. Set RTB_REDIS_SERVER to use another binary.
#pragma once
#ifdef RTB_WITH_REDIS

#include <hiredis/hiredis.h>

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace banker_test {

class RedisServer {
public:
    static std::unique_ptr<RedisServer> start()
    {
        std::string binary = findBinary();
        if (binary.empty()) return nullptr;
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::unique_ptr<RedisServer> server(new RedisServer());
            if (server->launch(binary)) return server;
        }
        return nullptr;
    }

    ~RedisServer()
    {
        halt();
        std::error_code ignored;
        if (!dir_.empty()) std::filesystem::remove_all(dir_, ignored);
    }

    int port() const { return port_; }

    /** Freezes the server: connections are accepted, nothing is answered. */
    void pause() { if (pid_ > 0) ::kill(pid_, SIGSTOP); }
    void resume() { if (pid_ > 0) ::kill(pid_, SIGCONT); }

    /** Kills the server; its port refuses connections afterwards. */
    void halt()
    {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGCONT);
        ::kill(pid_, SIGTERM);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }

    /** Runs a command on a fresh connection; the reply as text ("" for nil). */
    std::string command(const std::vector<std::string>& args) const
    {
        redisContext* c = redisConnect("127.0.0.1", port_);
        if (!c) return "";
        std::string out;
        if (!c->err) {
            std::vector<const char*> argv;
            std::vector<size_t> lengths;
            for (const std::string& a : args) {
                argv.push_back(a.data());
                lengths.push_back(a.size());
            }
            auto* r = static_cast<redisReply*>(
                redisCommandArgv(c, int(args.size()), argv.data(), lengths.data()));
            if (r) {
                if (r->type == REDIS_REPLY_INTEGER) out = std::to_string(r->integer);
                else if (r->type == REDIS_REPLY_STRING || r->type == REDIS_REPLY_STATUS)
                    out = std::string(r->str, r->len);
                freeReplyObject(r);
            }
        }
        redisFree(c);
        return out;
    }

    /** Integer field of a hash (0 if missing). */
    long long field(const std::string& key, const std::string& name) const
    {
        std::string v = command({"HGET", key, name});
        return v.empty() ? 0 : std::stoll(v);
    }

private:
    RedisServer() = default;

    static std::string findBinary()
    {
        std::vector<std::string> candidates;
        if (const char* env = std::getenv("RTB_REDIS_SERVER")) candidates.push_back(env);
        candidates.push_back("/opt/homebrew/bin/redis-server");
        candidates.push_back("/usr/local/bin/redis-server");
        candidates.push_back("/usr/bin/redis-server");
        if (const char* path = std::getenv("PATH")) {
            std::string all = path;
            size_t start = 0;
            while (start <= all.size()) {
                size_t end = all.find(':', start);
                if (end == std::string::npos) end = all.size();
                candidates.push_back(all.substr(start, end - start) + "/redis-server");
                start = end + 1;
            }
        }
        for (const std::string& c : candidates)
            if (::access(c.c_str(), X_OK) == 0) return c;
        return "";
    }

    static int freePort()
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return 0;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        int port = 0;
        socklen_t len = sizeof(addr);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0
            && ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
            port = ntohs(addr.sin_port);
        ::close(fd);
        return port;
    }

    bool launch(const std::string& binary)
    {
        port_ = freePort();
        if (port_ == 0) return false;
        std::string pattern = (std::filesystem::temp_directory_path() / "rtb-redis-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) return false;
        dir_ = pattern;

        std::string portText = std::to_string(port_);
        pid_ = ::fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            if (!std::freopen("/dev/null", "w", stdout)) _exit(127);
            if (!std::freopen("/dev/null", "w", stderr)) _exit(127);
            ::execl(binary.c_str(), binary.c_str(), "--port", portText.c_str(),
                    "--bind", "127.0.0.1", "--dir", dir_.c_str(), "--save", "",
                    "--appendonly", "no", static_cast<char*>(nullptr));
            _exit(127);
        }
        for (int i = 0; i < 100; ++i) {
            if (command({"PING"}) == "PONG") return true;
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) { pid_ = -1; return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    pid_t pid_ = -1;
    int port_ = 0;
    std::string dir_;
};

} // namespace banker_test

#endif // RTB_WITH_REDIS
