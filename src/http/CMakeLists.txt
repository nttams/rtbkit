# Owner: http. Asynchronous HTTP server shared by the exchange connector, the
# ad server and the admin endpoint. Tests: tests/http/*_test.cc.
rtb_add_library(rtb_http
    SOURCES
        http_server.cc
    DEPS rtb::asio llhttp::llhttp_shared Threads::Threads)

rtb_add_area_tests(http DEPS rtb_http httplib::httplib)
