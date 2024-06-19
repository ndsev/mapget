#pragma once

#include "httplib.h"
#include "mapget/detail/http-server.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stream.h"
#include "mapget/service/service.h"

#include <mutex>
#include <utility>

namespace mapget {

class HttpService : public HttpServer, public Service
{
public:
    explicit HttpService(Cache::Ptr cache = std::make_shared<MemCache>());
    ~HttpService() override;

protected:
    void setup(httplib::Server& server) override;

private:
    struct Impl;
    friend struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget
