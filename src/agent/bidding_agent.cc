#include "agent/bidding_agent.h"

#include "agent/zmq_util.h"

#include <thread>

namespace RTBKIT {

struct BiddingAgent::Impl {
    Impl(BiddingAgent& owner, AgentConfig cfg, std::string endpoint)
        : owner(owner), config(std::move(cfg)), endpoint(std::move(endpoint)),
          mailbox(context)
    {
    }

    BiddingAgent& owner;
    AgentConfig config;
    std::string endpoint;
    zmq::context_t context;
    zmqutil::Mailbox mailbox;
    std::unique_ptr<zmq::socket_t> socket;
    std::jthread thread;

    // Loop thread only.
    void send(const protocol::Message& message)
    {
        zmqutil::sendFrames(*socket, protocol::encode(message));
    }

    void sendConfig()
    {
        send(protocol::ConfigMsg{owner.name_, config});
    }

    void run(std::stop_token token)
    {
        zmq::pollitem_t items[] = {
            {socket->handle(), 0, ZMQ_POLLIN, 0},
            {mailbox.socket().handle(), 0, ZMQ_POLLIN, 0},
        };
        while (!token.stop_requested()) {
            zmq::poll(items, 2, std::chrono::milliseconds(50));
            if (items[1].revents & ZMQ_POLLIN) mailbox.drain();
            if (items[0].revents & ZMQ_POLLIN) {
                std::vector<std::string> frames;
                for (int i = 0; i < 256 && zmqutil::recvFrames(*socket, frames); ++i)
                    handle(frames);
            }
        }
    }

    template<typename F, typename... Args>
    static void call(const F& callback, Args&&... args)
    {
        if (callback) callback(std::forward<Args>(args)...);
    }

    void handle(const protocol::Frames& frames)
    {
        protocol::Message message;
        try {
            message = protocol::decode(frames);
        }
        catch (const protocol::ProtocolError& e) {
            call(owner.onError, std::string("bad message from router: ") + e.what(), Id());
            return;
        }

        using namespace protocol;
        if (auto* m = std::get_if<AuctionMsg>(&message)) {
            Bids bids;
            try {
                call(owner.onBidRequest, m->auctionId, m->request, m->spots, m->timeLeftMs);
                if (!owner.onBidRequest) send(BidMsg{m->auctionId, bids});
            }
            catch (const std::exception& e) {
                send(BidMsg{m->auctionId, bids});
                call(owner.onError, std::string("onBidRequest threw: ") + e.what(), m->auctionId);
            }
        }
        else if (auto* m = std::get_if<PingMsg>(&message)) send(PongMsg{m->token});
        else if (std::holds_alternative<NeedConfigMsg>(message)) sendConfig();
        else if (std::holds_alternative<GotConfigMsg>(message)) {
            {
                std::lock_guard lock(owner.mutex_);
                owner.gotConfig_ = true;
            }
            owner.configCv_.notify_all();
            call(owner.onGotConfig);
        }
        else if (auto* m = std::get_if<WinMsg>(&message)) call(owner.onWin, m->event);
        else if (auto* m = std::get_if<LossMsg>(&message)) call(owner.onLoss, m->event);
        else if (auto* m = std::get_if<CampaignEventMsg>(&message))
            call(owner.onCampaignEvent, m->event);
        else if (auto* m = std::get_if<NoBudgetMsg>(&message)) call(owner.onNoBudget, m->auctionId);
        else if (auto* m = std::get_if<TooLateMsg>(&message)) call(owner.onTooLate, m->auctionId);
        else if (auto* m = std::get_if<InvalidMsg>(&message))
            call(owner.onInvalid, m->auctionId, m->reason);
        else if (auto* m = std::get_if<ErrorMsg>(&message))
            call(owner.onError, m->reason, m->auctionId);
        // PONG, CONFIG, BID: nothing to do.
    }
};

BiddingAgent::BiddingAgent(std::string name, AgentConfig config, std::string routerEndpoint)
    : name_(std::move(name)),
      impl_(std::make_unique<Impl>(*this, std::move(config), std::move(routerEndpoint)))
{
}

BiddingAgent::~BiddingAgent()
{
    stop();
}

void BiddingAgent::start()
{
    if (impl_->thread.joinable()) return;
    impl_->socket = std::make_unique<zmq::socket_t>(impl_->context, zmq::socket_type::dealer);
    impl_->socket->set(zmq::sockopt::routing_id, name_);
    impl_->socket->set(zmq::sockopt::linger, 0);
    impl_->socket->connect(impl_->endpoint);
    impl_->sendConfig();   // zmq queues it until the router is reachable
    impl_->thread = std::jthread([impl = impl_.get()](std::stop_token token) {
        impl->run(token);
    });
}

void BiddingAgent::stop()
{
    if (impl_->thread.joinable()) {
        impl_->thread.request_stop();
        impl_->thread.join();
    }
    if (impl_->socket) {
        impl_->socket->close();
        impl_->socket.reset();
    }
}

void BiddingAgent::doBid(const Id& auctionId, const Bids& bids)
{
    impl_->mailbox.post([impl = impl_.get(), auctionId, bids] {
        impl->send(protocol::BidMsg{auctionId, bids});
    });
}

void BiddingAgent::sendConfig(const AgentConfig& config)
{
    impl_->mailbox.post([impl = impl_.get(), config] {
        impl->config = config;
        impl->sendConfig();
    });
}

bool BiddingAgent::waitForConfig(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return configCv_.wait_for(lock, timeout, [this] { return gotConfig_.load(); });
}

} // namespace RTBKIT
