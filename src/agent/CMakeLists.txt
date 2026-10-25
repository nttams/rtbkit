# Owner: router+agents.
rtb_add_library(rtb_agent_config
    SOURCES
        agent_config.cc
    DEPS rtb_common)

# Wire protocol between router and bidding agents (pure encode/decode).
rtb_add_library(rtb_agent_protocol
    SOURCES
        protocol.cc
    DEPS rtb_agent_config rtb_common)

# Bidding agent client: DEALER socket + own thread (needs ZeroMQ).
rtb_add_library(rtb_bidding_agent
    SOURCES
        bidding_agent.cc
    DEPS rtb_agent_protocol rtb_agent_config rtb_common cppzmq Threads::Threads)

rtb_add_area_tests(agent DEPS rtb_bidding_agent)
