// Port of rtbkit/core/router/filters/testing/creative_filters_test.cc to Catch2.

#include "utils.h"
#include "filters/creative_filters.h"

using namespace std;
using namespace RTBKIT;
using namespace RTBKIT::Test;

/******************************************************************************/
/* FORMAT FILTER                                                              */
/******************************************************************************/

TEST_CASE("creative filters: testFormatFilter")
{
    CreativeFormatFilter filter;
    CreativeMatrix creatives;

    AgentConfig c0;
    c0.creatives.push_back(Creative());
    c0.creatives.push_back(Creative(100, 100));
    c0.creatives.push_back(Creative(300, 300));

    AgentConfig c1;
    c1.creatives.push_back(Creative(100, 100));
    c1.creatives.push_back(Creative(200, 200));

    BidRequest r0;
    addImp(r0, AD_POS_ABOVE, { {100, 100} });
    addImp(r0, AD_POS_ABOVE, { {200, 200}, {100, 100} });
    addImp(r0, AD_POS_ABOVE, { {200, 200}, {300, 300} });
    addImp(r0, AD_POS_ABOVE, { {300, 300} });
    addImp(r0, AD_POS_ABOVE, { {400, 400} });

    BidRequest r1;
    addImp(r1, AD_POS_ABOVE, {}); //Empty format bid request

    title("format-1");
    addConfig(filter, 0, c0, creatives);
    addConfig(filter, 1, c1, creatives);

    check(filter, r0, creatives, 0, { {0, 1}, {0}        });
    check(filter, r0, creatives, 1, { {0, 1}, {0, 1}     });
    check(filter, r0, creatives, 2, { {0},    {1},   {0} });
    check(filter, r0, creatives, 3, { {0},    {},    {0} });
    check(filter, r0, creatives, 4, { {0}                });

    //Test empty format bid request
    //Empty format is a wild card, all filters should pass.
    check(filter, r1, creatives, 0, { {0, 1}, {0,1}, {0} });

    title("format-2");
    removeConfig(filter, 0, c0, creatives);

    check(filter, r0, creatives, 0, { {1},     });
    check(filter, r0, creatives, 1, { {1}, {1} });
    check(filter, r0, creatives, 2, { {},  {1} });
    check(filter, r0, creatives, 3, {          });
    check(filter, r0, creatives, 4, {          });
}


/******************************************************************************/
/* LANGUAGE FILTER                                                            */
/******************************************************************************/

TEST_CASE("creative filters: testLanguageFilter")
{
    CreativeLanguageFilter filter;
    CreativeMatrix creatives;

    auto addCr = [] (AgentConfig& cfg, const IncludeExclude<string>& ie) {
        cfg.creatives.emplace_back();
        cfg.creatives.back().languageFilter = std::move(ie);
    };

    AgentConfig c0;
    addCr(c0, ie<string>({"fr"}, {}));
    addCr(c0, ie<string>({}, {"en"}));

    AgentConfig c1;
    addCr(c1, ie<string>({"en"}, {}));
    addCr(c1, ie<string>({"en"}, {"fr"}));

    BidRequest r0;
    r0.language = "en";
    addImp(r0, AD_POS_ABOVE, { {100, 100} });
    addImp(r0, AD_POS_ABOVE, { {200, 200}, {100, 100} });

    BidRequest r1;
    r1.language = "fr";
    addImp(r1, AD_POS_ABOVE, { {100, 100} });

    BidRequest r2;
    r2.language = "es";
    addImp(r2, AD_POS_ABOVE, { {100, 100} });


    title("language-1");
    addConfig(filter, 0, c0, creatives);
    addConfig(filter, 1, c1, creatives);

    check(filter, r0, creatives, 0, { {1}, {1} });
    check(filter, r0, creatives, 1, { {1}, {1} });
    check(filter, r1, creatives, 0, { {0}, {0} });
    title("language-1-d");
    check(filter, r2, creatives, 0, { {},  {0} });


    title("language-2");
    removeConfig(filter, 0, c0, creatives);

    check(filter, r0, creatives, 0, { {1}, {1} });
    check(filter, r0, creatives, 1, { {1}, {1} });
    check(filter, r1, creatives, 0, {          });
    check(filter, r2, creatives, 0, {          });
}


/******************************************************************************/
/* Segment FILTER                                                             */
/******************************************************************************/
TEST_CASE("creative filters: testSegmentsFilter")
{
    CreativeSegmentsFilter filter;
    CreativeMatrix creatives;

    typedef Creative::SegmentInfo SegInfo;

    auto addCrfilter = [] (AgentConfig& cfg,
                           const std::vector<std::string> & inc,
                           const std::vector<std::string> & exc,
                           bool excludeIfNotPresent = true) {
        SegInfo si;
        for ( auto & w : inc) si.include.add(w);
        for ( auto & w : exc) si.exclude.add(w);
        si.include.sort();
        si.exclude.sort();
        si.excludeIfNotPresent = excludeIfNotPresent;
        cfg.creatives.emplace_back();
        cfg.creatives.back().segments["words"] = si;
    };

    // Ad those filters to some creatives in some agentconfigs
    AgentConfig c0;
    addCrfilter(c0, {"hey"}, {});
    addCrfilter(c0, {}, {"oh"});
    addCrfilter(c0, {"oh"}, {});

    AgentConfig c1;
    addCrfilter(c1, {"go"}, {}, false);

    AgentConfig c2;
    addCrfilter(c2, {"oh"}, {});
    addCrfilter(c2, {}, {"no"}, false);
    addCrfilter(c2, {"go"}, {});

    // Create some BR with some segments to be filtered
    BidRequest br0;
    addImp(br0, AD_POS_ABOVE, { {100, 100} });
    addImp(br0, AD_POS_ABOVE, { {100, 200} });
    br0.segments.addStrings("words", {"hey"});

    BidRequest br1;
    addImp(br1, AD_POS_ABOVE, { {100, 100} });
    br1.segments.addStrings("words", {"oh", "go"});

    BidRequest br2;
    addImp(br2, AD_POS_ABOVE, { {100, 100} });
    br2.segments.addStrings("words", {"lets"});

    BidRequest br3;
    addImp(br3, AD_POS_ABOVE, { {100, 100} });

    title("segemnts-filter-include-exclude-1");
    addConfig(filter, 0, c0, creatives);

    check(filter, br0, creatives, 0, { {0}, {0}        });
    check(filter, br0, creatives, 1, { {0}, {0}        });
    check(filter, br1, creatives, 0, { { }, { }, {0}   });
    check(filter, br2, creatives, 0, { { }, {0},       });

    title("segemnts-filter-no-segment-1");
    check(filter, br3, creatives, 0, {                 });

    title("segemnts-filter-include-exclude-2");
    addConfig(filter, 1, c1, creatives);

    check(filter, br0, creatives, 0, { {0}, {0}         });
    check(filter, br0, creatives, 1, { {0}, {0}         });
    check(filter, br1, creatives, 0, { {1}, { }, {0}    });
    check(filter, br2, creatives, 0, { { }, {0},        });

    title("segemnts-filter-no-segment-2");
    check(filter, br3, creatives, 0, { {1}              });

    title("segemnts-filter-include-exclude-3");
    addConfig(filter, 2, c2, creatives);

    check(filter, br0, creatives, 0, { {0}, {0,2},      });
    check(filter, br0, creatives, 1, { {0}, {0,2},      });
    check(filter, br1, creatives, 0, { {1,2}, {2}, {0,2}});
    check(filter, br2, creatives, 0, { { }, {0,2},      });

    title("segemnts-filter-no-segment-3");
    check(filter, br3, creatives, 0, { {1}, {2}         });

}


/******************************************************************************/
/* PMP FILTER                                                              */
/******************************************************************************/

TEST_CASE("creative filters: testiPMPFilter")
{
    CreativePMPFilter filter;
    CreativeMatrix creatives;
    auto addDeal = [] (AgentConfig& cfg, const std::string& dealId) {
        for ( auto& c : cfg.creatives)
            c.dealId = dealId;
    };
  
    auto addPmpImp = [] (BidRequest& br, AdPosition pos, Pmp& pmp) {
        AdSpot imp;
        imp.position = pos;
        imp.pmp.emplace(pmp);
        br.imp.push_back(imp);
    };


// With DealId DEAL1
    AgentConfig c0;
    c0.creatives.push_back(Creative());
    c0.creatives.push_back(Creative(100, 100));
    c0.creatives.push_back(Creative(300, 300));
    addDeal(c0, "DEAL1");

// With DealId DEAL2
    AgentConfig c1;
    c1.creatives.push_back(Creative(100, 100));
    c1.creatives.push_back(Creative(200, 200));
    addDeal(c1, "DEAL2");

// With No DealId
    AgentConfig c2;
    c2.creatives.push_back(Creative());
    c2.creatives.push_back(Creative(200, 200));

    Deal d0;
    d0.id = Id("DEAL1");
    Deal d1;
    d1.id = Id("DEAL2");
    
    // No private Auction 
    Pmp p0;
    p0.privateAuction = 0;
    p0.deals.push_back(d0);
    
    // Single DealId in PMP
    Pmp p1;
    p1.privateAuction = 1;
    p1.deals.push_back(d0);

    // Both DealId in PMP
    Pmp p2;
    p2.privateAuction = 1;
    p2.deals.push_back(d0);
    p2.deals.push_back(d1);


    BidRequest r0;
    addImp(r0, AD_POS_ABOVE, { {100, 100} }); // No pmp
    addPmpImp(r0, AD_POS_ABOVE, p0); // pmp but No privateAuction
    addPmpImp(r0, AD_POS_ABOVE, p1); // Single Deal
    addPmpImp(r0, AD_POS_ABOVE, p2); // Two Deals

    title("PMP-1");
    addConfig(filter, 0, c0, creatives);
    addConfig(filter, 1, c1, creatives);
    addConfig(filter, 2, c2, creatives);

    check(filter, r0, creatives, 0, { {2},    {2}              });
    check(filter, r0, creatives, 1, { {2},    {2}              });
    check(filter, r0, creatives, 2, { {0},    {0},      {0}    });
    check(filter, r0, creatives, 3, { {0, 1}, {0, 1},   {0}    });
    

    title("PMP-2");
    removeConfig(filter, 0, c0, creatives);

    check(filter, r0, creatives, 0, { {2}, {2} });
    check(filter, r0, creatives, 1, { {2}, {2} });
    check(filter, r0, creatives, 2, { });
    check(filter, r0, creatives, 3, { {1}, {1} });
}

