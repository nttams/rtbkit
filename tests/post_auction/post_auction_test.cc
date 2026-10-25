// PostAuctionService skeleton checks. The post auction owner adds real tests
// in this folder.

#include "post_auction/post_auction.h"

#include <catch2/catch_test_macros.hpp>

using namespace RTBKIT;

TEST_CASE("post auction: constructed on a banker")
{
    auto banker = std::make_shared<NullBanker>();
    PostAuctionService service(banker);
    CHECK(&service.banker() == banker.get());
}
