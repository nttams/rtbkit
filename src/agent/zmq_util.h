// Small ZeroMQ helpers shared by the router and the bidding agent client:
// multipart send / receive of string frames and a "mailbox" that lets other
// threads hand tasks to an event loop thread that owns the sockets.
#pragma once

#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace RTBKIT::zmqutil {

/** Sends all frames as one multipart message. */
inline void sendFrames(zmq::socket_t& socket, const std::vector<std::string>& frames)
{
    for (size_t i = 0; i < frames.size(); ++i) {
        auto flags = i + 1 < frames.size() ? zmq::send_flags::sndmore
                                           : zmq::send_flags::none;
        socket.send(zmq::buffer(frames[i]), flags);
    }
}

/** Receives one multipart message without blocking. False if none is ready. */
inline bool recvFrames(zmq::socket_t& socket, std::vector<std::string>& frames)
{
    frames.clear();
    zmq::message_t message;
    if (!socket.recv(message, zmq::recv_flags::dontwait)) return false;
    frames.emplace_back(message.to_string());
    while (message.more()) {
        if (!socket.recv(message, zmq::recv_flags::none)) break;
        frames.emplace_back(message.to_string());
    }
    return true;
}

/** Like recvFrames but waits up to `timeout` for a message. */
inline bool recvFramesWait(zmq::socket_t& socket, std::vector<std::string>& frames,
                           std::chrono::milliseconds timeout)
{
    zmq::pollitem_t item = {socket.handle(), 0, ZMQ_POLLIN, 0};
    zmq::poll(&item, 1, timeout);
    return recvFrames(socket, frames);
}

/** Thread safe task queue for an event loop. Any thread calls post(); the loop
    thread polls socket() for input and calls drain() when it is readable. */
class Mailbox {
public:
    explicit Mailbox(zmq::context_t& context)
        : pull_(context, zmq::socket_type::pull),
          push_(context, zmq::socket_type::push)
    {
        static std::atomic<unsigned> counter{0};
        const std::string endpoint = "inproc://rtb-mailbox-" + std::to_string(counter++);
        pull_.bind(endpoint);
        push_.connect(endpoint);
    }

    /** Thread safe. The task runs on the loop thread. */
    void post(std::function<void()> task)
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
        // A full pipe already guarantees a wake up, so a dropped signal is fine.
        (void) push_.send(zmq::buffer("x", 1), zmq::send_flags::dontwait);
    }

    zmq::socket_t& socket() { return pull_; }

    /** Loop thread only: runs every queued task. */
    void drain()
    {
        zmq::message_t signal;
        while (pull_.recv(signal, zmq::recv_flags::dontwait)) {}

        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard lock(mutex_);
            tasks.swap(tasks_);
        }
        for (auto& task : tasks) task();
    }

private:
    zmq::socket_t pull_;
    zmq::socket_t push_;
    std::mutex mutex_;
    std::vector<std::function<void()>> tasks_;
};

} // namespace RTBKIT::zmqutil
