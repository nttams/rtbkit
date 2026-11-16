# Owner: exchange. Tests go in tests/exchange/*_test.cc (rtb_exchange_tests).
rtb_add_library(rtb_exchange
    SOURCES
        exchange_connector.cc
        openrtb_exchange_connector.cc
        mock_exchange_support.cc
    DEPS rtb_agent_config rtb_common rtb::asio llhttp::llhttp_shared Threads::Threads)

rtb_add_area_tests(exchange DEPS rtb_exchange)
