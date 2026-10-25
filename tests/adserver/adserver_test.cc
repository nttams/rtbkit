// AdServer skeleton checks. The ad server owner adds real tests in this folder.

#include "adserver/adserver.h"

#include <catch2/catch_test_macros.hpp>

using namespace RTBKIT;

TEST_CASE("adserver: constructed on a post auction service")
{
    PostAuctionService postAuction(std::make_shared<NullBanker>());
    AdServer adServer(postAuction);
    SUCCEED();
}
