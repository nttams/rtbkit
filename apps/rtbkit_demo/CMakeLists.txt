rtb_add_app(rtbkit_demo
    SOURCES main.cc
    DEPS rtb_server rtb_bidding_agent)
target_compile_definitions(rtbkit_demo PRIVATE
    RTB_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config")
