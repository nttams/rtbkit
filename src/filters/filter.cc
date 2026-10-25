#include "filters/filter.h"

namespace RTBKIT {

std::unordered_map<unsigned, BiddableSpots>
FilterState::biddableSpots()
{
    // Used to remove creatives for configs that have been filtered out.
    narrowAllCreatives(CreativeMatrix(configs_));

    std::unordered_map<unsigned, BiddableSpots> biddable;

    for (size_t impId = 0; impId < creatives_.size(); ++impId) {
        std::map<unsigned, std::vector<unsigned> > biddableCreatives;

        for (unsigned crId = 0; crId < creatives_[impId].size(); ++crId) {
            const ConfigSet& configs = creatives_[impId][crId];

            for (size_t config = configs.next();
                 config < configs.size();
                 config = configs.next(config + 1))
            {
                biddableCreatives[unsigned(config)].push_back(crId);
            }
        }

        for (auto& entry : biddableCreatives)
            biddable[entry.first].push_back({ unsigned(impId), std::move(entry.second) });
    }

    return biddable;
}

} // namespace RTBKIT
