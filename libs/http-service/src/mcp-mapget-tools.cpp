#include "mcp-mapget-call.h"
#include "mcp-mapget-catalog.h"
#include "mcp-mapget-extraction.h"

#include "cli.h"

#include <openssl/rand.h>
#include <algorithm>

namespace mapget::detail
{
using Json = nlohmann::json;

McpMapgetTools::McpMapgetTools(
    Service& service,
    McpConfig config,
    std::function<Json()> diagnostics,
    LocationLookup const* location,
    std::function<Json()> readConfig,
    std::function<Json(Json const&, std::string const&)> writeConfig)
    : service_(service),
      config_(std::move(config)),
      help_(
          McpHelp::defaultDirectory(),
          [this]
          {
              auto paths = config_.helpDocs;
              if (!config_.webHelpDirectory.empty())
                  paths.push_back(config_.webHelpDirectory);
              return paths;
          }()),
      diagnostics_(std::move(diagnostics)),
      location_(location),
      readConfig_(std::move(readConfig)),
      writeConfig_(std::move(writeConfig))
{
    catalog_ = std::make_unique<Catalog>();
}

McpMapgetTools::~McpMapgetTools()
{
    stop();
}

std::string McpMapgetTools::retain(std::shared_ptr<Continuation> checkpoint)
{
    // Charge estimated pinned tile/query storage for every checkpoint, including
    // shared payloads. This accounting budget does not bound total allocations or
    // process RSS; active calls have separate admission limits.
    constexpr size_t maxBytes = 64 * 1024 * 1024;
    if (checkpoint->retainedBytes > maxBytes)
        return {};
    unsigned char random[24];
    if (RAND_bytes(random, sizeof(random)) != 1)
        return {};
    std::string token;
    for (auto byte : random) {
        token += "0123456789abcdef"[byte >> 4];
        token += "0123456789abcdef"[byte & 15];
    }
    auto revision = service_.sourceCatalogRevision();
    std::vector<std::shared_ptr<Continuation>> retired;
    std::lock_guard lock(mutex_);
    if (stopped_)
        return {};
    auto now = std::chrono::steady_clock::now();
    for (auto it = continuations_.begin(); it != continuations_.end();) {
        if (it->second->expires <= now || it->second->catalogRevision != revision) {
            retired.push_back(std::move(it->second));
            it = continuations_.erase(it);
        }
        else
            ++it;
    }
    for (;;) {
        size_t bytes = checkpoint->retainedBytes, owned = 0;
        for (auto const& [_, entry] : continuations_) {
            bytes += entry->retainedBytes;
            owned += entry->principal.sameUser(checkpoint->principal);
        }
        if (bytes <= maxBytes && owned < 4 && continuations_.size() < 16)
            break;
        auto oldest = continuations_.end();
        for (auto it = continuations_.begin(); it != continuations_.end(); ++it) {
            if (owned >= 4 && !it->second->principal.sameUser(checkpoint->principal))
                continue;
            if (oldest == continuations_.end() || it->second->expires < oldest->second->expires)
                oldest = it;
        }
        if (oldest == continuations_.end())
            return {};
        retired.push_back(std::move(oldest->second));
        continuations_.erase(oldest);
    }
    continuations_.emplace(token, std::move(checkpoint));
    return token;
}

bool McpMapgetTools::permitted(std::string_view name, Principal const& principal) const
{
    if (!principal.valid(std::chrono::system_clock::now()))
        return false;
    if (name == "mapget_get_config")
        return config_.configReadEnabled && principal.configRead && isGetConfigEndpointEnabled();
    if (name == "mapget_set_config")
        return config_.configWriteEnabled && config_.directConfigPersistence &&
            principal.configWrite && isPostConfigEndpointEnabled();
    if (name == "mapget_get_diagnostics")
        return principal.diagnostics;
    return principal.read;
}

Json McpMapgetTools::tools(Principal const& principal) const
{
    return catalog_->tools([&](std::string_view name) { return permitted(name, principal); });
}

bool McpMapgetTools::contains(std::string_view name) const
{
    return catalog_->contains(name);
}

bool McpMapgetTools::acceptsArguments(std::string_view name, Json const& value, Json* issues) const
{
    return catalog_->acceptsArguments(name, value, issues);
}

bool McpMapgetTools::acceptsResult(std::string_view name, Json const& value) const
{
    return catalog_->acceptsResult(name, value);
}

std::shared_ptr<std::atomic_bool>
McpMapgetTools::invoke(Principal principal, std::string name, Json arguments, Complete complete)
{
    auto reject = [&](std::string code, std::string message)
    {
        complete({{"error", {{"code", code}, {"message", message}}}});
        return std::shared_ptr<std::atomic_bool>{};
    };
    if (!contains(name) || !permitted(name, principal))
        return reject(
            "not_available",
            "Native tool permission or deployment capability unavailable.");
    if (name == "mapget_extract_features" && arguments.is_object() &&
        !arguments.contains("cursor") && !arguments.contains("partition") &&
        !arguments.contains("partitions") && !arguments.contains("featureIds"))
        return reject(
            "invalid_arguments",
            "Supply partition, nonempty partitions, or full primary/secondary featureIds. "
            "Extraction does not "
            "scan a map; use viewer_start_search for a viewport search.");
    if (!acceptsArguments(name, arguments))
        return reject("invalid_arguments", "Arguments do not match the tool schema.");
    // Reserve an invocation slot before scheduling: cancellation or a timed-out
    // caller does not release it while worker code is still executing.
    std::unique_lock lock(mutex_);
    auto owned = std::count_if(
        calls_.begin(),
        calls_.end(),
        [&](auto const& c) { return c->principal.sameUser(principal); });
    if (stopped_ || calls_.size() >= config_.limits.pendingCalls ||
        static_cast<size_t>(owned) >= config_.limits.callsPerPrincipal)
    {
        lock.unlock();
        return reject("busy", "Native tool admission limit reached.");
    }
    auto call = std::make_shared<Call>(
        *this,
        std::move(principal),
        std::move(name),
        std::move(arguments),
        std::move(complete));
    calls_.push_back(call);
    lock.unlock();
    if (!service_.scheduleTask([call](bool admitted) { call->run(admitted); }))
        call->run(false);
    return call->cancelled;
}

void McpMapgetTools::retire(Call* call)
{
    std::lock_guard lock(mutex_);
    std::erase_if(calls_, [call](auto const& c) { return c.get() == call; });
    idle_.notify_all();
}

void McpMapgetTools::cancel(std::shared_ptr<std::atomic_bool> const& token)
{
    std::shared_ptr<Call> call;
    {
        std::lock_guard lock(mutex_);
        auto found = std::find_if(
            calls_.begin(),
            calls_.end(),
            [&](auto const& c) { return c->cancelled == token; });
        if (found != calls_.end())
            call = *found;
    }
    if (call)
        call->cancel();
}

void McpMapgetTools::stop() noexcept
{
    std::unique_lock lock(mutex_);
    stopped_ = true;
    auto continuations = std::move(continuations_);
    // Cancellation can submit service work and complete callbacks; release the
    // admission lock before invoking it. Do not allocate a snapshot of calls
    // during destructor-driven shutdown: cancel marks each retained call before
    // submitting its abort, so already-cancelled calls can simply drain.
    lock.unlock();
    continuations.clear();
    lock.lock();
    for (;;) {
        auto pending = std::find_if(calls_.begin(), calls_.end(), [](auto const& call) {
            return !call->cancelled->load();
        });
        if (pending == calls_.end())
            break;
        auto call = *pending;
        lock.unlock();
        call->cancel();
        // Release query/tile storage outside the admission lock as well.
        call.reset();
        lock.lock();
    }
    idle_.wait(lock, [this] { return calls_.empty() && !helpRefreshPending_; });
}

void McpMapgetTools::refreshHelp()
{
    std::unique_lock lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    if (stopped_ || helpRefreshPending_ || now < nextHelpRefresh_)
        return;
    nextHelpRefresh_ = now + std::chrono::seconds(2);
    helpRefreshPending_ = true;
    // stop() drains this owner before the borrowed service or index can disappear.
    lock.unlock();
    if (service_.scheduleTask(
            [this](bool admitted)
            {
                try {
                    if (admitted) {
                        // Tile destruction runs on a worker and outside the admission mutex.
                        auto revision = service_.sourceCatalogRevision();
                        std::vector<std::shared_ptr<Continuation>> retired;
                        {
                            std::lock_guard cleanup(mutex_);
                            auto now = std::chrono::steady_clock::now();
                            for (auto it = continuations_.begin(); it != continuations_.end();) {
                                if (it->second->expires <= now ||
                                    it->second->catalogRevision != revision) {
                                    retired.push_back(std::move(it->second));
                                    it = continuations_.erase(it);
                                }
                                else
                                    ++it;
                            }
                        }
                        retired.clear();
                        help_.refresh();
                    }
                }
                catch (...) { /* Failed maintenance must still release the shutdown barrier. */
                }
                std::lock_guard done(mutex_);
                helpRefreshPending_ = false;
                idle_.notify_all();
            }))
        return;
    lock.lock();
    helpRefreshPending_ = false;
    idle_.notify_all();
}

}  // namespace mapget::detail
