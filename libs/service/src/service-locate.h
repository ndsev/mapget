#pragma once

#include "service-impl.h"

namespace mapget::detail
{

/** Request-owned locate candidate planning, tile loading, and result assembly. */
class LocateRequestExecution
{
public:
    /** Capture the service and portable identity request for synchronous resolution. */
    LocateRequestExecution(Service::Impl& service, LocateRequest request);

    /** Resolve all candidate groups and return stable-deduplicated results. */
    std::vector<LocateResponse> run();

private:
    /** Candidate selectors sharing one source-specific tile load. */
    struct CandidateGroup
    {
        std::string sourceId;
        MapTileKey tileKey;
        std::vector<LocateCandidate> candidates;
    };

    /** Collect cheap datasource-provided candidates into one load per tile. */
    std::vector<CandidateGroup> planCandidates() const;

    Service::Impl& service_;
    LocateRequest request_;
    std::mutex mutex_;
    std::condition_variable cv_;
    size_t pending_ = 0;
    std::vector<LocateResponse> results_;
    std::set<std::string> seen_;
};

}  // namespace mapget::detail
