# Owner: router+agents. Add the bidding agent client library here.
rtb_add_library(rtb_agent_config
    SOURCES
        agent_config.cc
    DEPS rtb_common)

# Wire protocol between router and bidding agents (pure encode/decode).
rtb_add_library(rtb_agent_protocol
    SOURCES
        protocol.cc
    DEPS rtb_agent_config rtb_common)

rtb_add_area_tests(agent DEPS rtb_agent_protocol)
