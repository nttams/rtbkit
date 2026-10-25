rtb_add_library(rtb_common
    SOURCES
        account_key.cc
        auction.cc
        bid_request.cc
        bid_request_json.cc
        currency.cc
        events.cc
        openrtb_parser.cc
        segments.cc
    DEPS nlohmann_json::nlohmann_json)
