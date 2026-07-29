#include "http-service-impl.h"

#include "tiles-ws-controller.h"

#include <drogon/HttpResponse.h>

#include <chrono>
#include <string>
#include <string_view>

namespace mapget
{
namespace
{

[[nodiscard]] bool parseBoolParameter(const drogon::HttpRequestPtr& req, std::string_view key, bool defaultValue = false)
{
    const std::string raw = req->getParameter(std::string(key));
    if (raw.empty())
        return defaultValue;

    if (raw == "1" || raw == "true" || raw == "TRUE" || raw == "yes" || raw == "on") {
        return true;
    }
    if (raw == "0" || raw == "false" || raw == "FALSE" || raw == "no" || raw == "off") {
        return false;
    }
    return defaultValue;
}

[[nodiscard]] std::string statusPageHtml()
{
    return R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>mapget status</title>
<style>
body {
    font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif;
    margin: 16px;
    color: #1f2937;
    background: #f8fafc;
}
h1, h2 { margin: 0 0 8px 0; }
h2 { margin-top: 18px; }
h3 { margin: 0 0 8px 0; font-size: 1rem; }
.panel {
    background: #ffffff;
    border: 1px solid #d1d5db;
    border-radius: 8px;
    padding: 12px;
    margin-top: 10px;
}
.controls {
    display: flex;
    flex-wrap: wrap;
    gap: 14px;
    align-items: center;
}
.muted { color: #4b5563; font-size: 0.9rem; }
.error {
    color: #b91c1c;
    font-weight: 600;
}
pre {
    margin: 0;
    overflow: auto;
    white-space: pre-wrap;
    word-break: break-word;
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, Liberation Mono, monospace;
    font-size: 12px;
    line-height: 1.35;
}
table {
    border-collapse: collapse;
    width: 100%;
    font-size: 13px;
}
th, td {
    border: 1px solid #d1d5db;
    padding: 6px 8px;
    text-align: left;
}
th { background: #f1f5f9; }
.number { text-align: right; font-variant-numeric: tabular-nums; }
#tileSizePanel { display: none; }
</style>
</head>
<body>
    <h1>Status Information</h1>
    <div class="panel controls">
        <label><input id="includeTileSizeDistribution" type="checkbox"> Include tile size distribution (heavy)</label>
        <label>Refresh interval (ms): <input id="refreshMs" type="number" min="200" step="100" value="1000" style="width:90px"></label>
        <button id="refreshNow" type="button">Refresh now</button>
        <span id="lastUpdated" class="muted">Never updated</span>
        <span id="error" class="error"></span>
    </div>

    <h2>Tiles WebSocket Metrics</h2>
    <div class="panel">
        <table id="wsMetricsTable">
            <thead><tr><th>Metric</th><th class="number">Value</th></tr></thead>
            <tbody></tbody>
        </table>
        <div class="muted" style="margin-top:8px">
            `pending-controller-*` covers tile frames currently queued for `/interactive/payload` pulls.
            `pending-pull-requests` counts currently blocked long-poll pull requests.
            `total-forwarded-*` counts tile frames/bytes already served through `/interactive/payload`.
        </div>
    </div>

    <div id="treeBreakdownPanel" style="display:none">
        <h2>Cached Feature Tree Breakdown</h2>
        <div class="panel">
            <table id="treeBreakdownSummary">
                <thead><tr><th>Metric</th><th class="number">Value</th></tr></thead>
                <tbody></tbody>
            </table>
        </div>
        <div class="panel">
            <h3>Feature Layer Bytes</h3>
            <table id="featureLayerBreakdown">
                <thead><tr><th>Type</th><th class="number">Bytes</th><th class="number">Readable</th><th class="number">Share</th></tr></thead>
                <tbody></tbody>
            </table>
        </div>
        <div class="panel">
            <h3>Model Pool Bytes</h3>
            <table id="modelPoolBreakdown">
                <thead><tr><th>Type</th><th class="number">Bytes</th><th class="number">Readable</th><th class="number">Share</th></tr></thead>
                <tbody></tbody>
            </table>
        </div>
        <div class="panel">
            <h3>Array Arena Singleton Usage</h3>
            <table id="arrayArenaSingletonsTable">
                <thead>
                    <tr>
                        <th>Arena</th>
                        <th class="number">Handles</th>
                        <th class="number">Occupied</th>
                        <th class="number">Empty</th>
                        <th class="number">Singleton Bytes</th>
                        <th class="number">Regular-Equivalent Bytes</th>
                        <th class="number">Saved Bytes</th>
                        <th class="number">Saved Share</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
        </div>
    </div>

    <h2>Service Statistics</h2>
    <div class="panel"><pre id="serviceStats"></pre></div>

    <h2>Cache Statistics</h2>
    <div class="panel"><pre id="cacheStats"></pre></div>

    <div id="tileSizePanel">
        <h2>Tile Size Distribution</h2>
        <div class="panel">
            <table id="tileSizeSummary">
                <thead><tr><th>Metric</th><th class="number">Value</th></tr></thead>
                <tbody></tbody>
            </table>
        </div>
        <div class="panel">
            <table id="tileSizeHistogram">
                <thead><tr><th>Bin</th><th class="number">Count</th><th class="number">Share</th></tr></thead>
                <tbody></tbody>
            </table>
        </div>
    </div>

<script>
const byId = (id) => document.getElementById(id);
const qs = (selector) => document.querySelector(selector);
const formatInt = (v) => Number(v || 0).toLocaleString();
const formatPct = (v) => `${(v * 100).toFixed(2)}%`;
const formatBytes = (bytes) => {
    const b = Number(bytes || 0);
    const units = ["B", "KiB", "MiB", "GiB"];
    let value = b;
    let idx = 0;
    while (value >= 1024 && idx < units.length - 1) {
        value /= 1024;
        idx++;
    }
    return `${value.toFixed(idx === 0 ? 0 : 2)} ${units[idx]}`;
};

const state = {
    timer: null,
    refreshInFlight: false,
    pendingForcedRefresh: false,
    lastServiceText: "",
    lastCacheText: "",
    lastBreakdownJson: "",
    lastArrayArenaSingletonJson: "",
    lastDistributionJson: "",
};
)HTML"
R"HTML(

const wsMetricDefinitions = [
    ["active-connections", "active-connections", (v) => formatInt(v)],
    ["active-sessions", "active-sessions", (v) => formatInt(v)],
    ["pending-controller-frames", "pending-controller-frames", (v) => formatInt(v)],
    ["pending-controller-bytes", "pending-controller-bytes", (v) => `${formatInt(v)} (${formatBytes(v)})`],
    ["pending-pull-requests", "pending-pull-requests", (v) => formatInt(v)],
    ["total-queued-frames", "total-queued-frames", (v) => formatInt(v)],
    ["total-queued-bytes", "total-queued-bytes", (v) => `${formatInt(v)} (${formatBytes(v)})`],
    ["total-forwarded-frames", "total-forwarded-frames", (v) => formatInt(v)],
    ["total-forwarded-bytes", "total-forwarded-bytes", (v) => `${formatInt(v)} (${formatBytes(v)})`],
    ["total-dropped-frames", "total-dropped-frames", (v) => formatInt(v)],
    ["total-dropped-bytes", "total-dropped-bytes", (v) => `${formatInt(v)} (${formatBytes(v)})`],
    ["total-pull-requests", "total-pull-requests", (v) => formatInt(v)],
    ["total-pull-timeouts", "total-pull-timeouts", (v) => formatInt(v)],
    ["total-pull-session-misses", "total-pull-session-misses", (v) => formatInt(v)],
    ["replaced-requests", "replaced-requests", (v) => formatInt(v)],
];

function setPreJsonIfChanged(id, value, cacheKey) {
    const text = JSON.stringify(value ?? {}, null, 2);
    if (state[cacheKey] === text) {
        return;
    }
    const el = byId(id);
    if (!el) {
        return;
    }
    el.textContent = text;
    state[cacheKey] = text;
}

function ensureWsMetricRows() {
    const table = byId("wsMetricsTable");
    const tbody = qs("#wsMetricsTable tbody");
    if (!table || !tbody) {
        return false;
    }
    if (table.dataset.rowsReady === "1") {
        return true;
    }
    tbody.innerHTML = "";
    for (const [label, key] of wsMetricDefinitions) {
        const tr = document.createElement("tr");
        const tdKey = document.createElement("td");
        tdKey.textContent = label;
        const tdValue = document.createElement("td");
        tdValue.className = "number";
        tdValue.id = `wsMetric-${key}`;
        tr.appendChild(tdKey);
        tr.appendChild(tdValue);
        tbody.appendChild(tr);
    }
    table.dataset.rowsReady = "1";
    return true;
}

function renderWsMetrics(metrics) {
    if (!ensureWsMetricRows()) {
        return;
    }
    for (const [_, key, formatter] of wsMetricDefinitions) {
        const el = byId(`wsMetric-${key}`);
        if (!el) {
            continue;
        }
        el.textContent = formatter(metrics[key] ?? 0);
    }
}

function renderByteBreakdownRows(selector, breakdown, totalBytes) {
    const tbody = qs(selector);
    if (!tbody) {
        return;
    }
    tbody.innerHTML = "";
    const entries = Object.entries(breakdown || {})
        .filter(([, value]) => Number.isFinite(Number(value)))
        .sort((a, b) => Number(b[1]) - Number(a[1]));
    for (const [key, valueRaw] of entries) {
        const value = Number(valueRaw || 0);
        const share = totalBytes > 0 ? value / totalBytes : 0;
        const tr = document.createElement("tr");
        tr.innerHTML =
            `<td>${key}</td><td class="number">${formatInt(value)}</td><td class="number">${formatBytes(value)}</td><td class="number">${formatPct(share)}</td>`;
        tbody.appendChild(tr);
    }
}

function renderTreeBreakdown(service) {
    const panel = byId("treeBreakdownPanel");
    if (!panel) {
        return;
    }
    const breakdown = service["cached-feature-tree-bytes"];
    if (!breakdown) {
        panel.style.display = "none";
        state.lastBreakdownJson = "";
        state.lastArrayArenaSingletonJson = "";
        return;
    }
    panel.style.display = "block";

    const breakdownJson = JSON.stringify(breakdown);
    if (state.lastBreakdownJson === breakdownJson) {
        return;
    }
    state.lastBreakdownJson = breakdownJson;

    const summaryBody = qs("#treeBreakdownSummary tbody");
    if (summaryBody) {
        summaryBody.innerHTML = "";
        const summaryRows = [
            ["Tile count", formatInt(breakdown["tile-count"])],
            ["Total tile bytes", `${formatInt(breakdown["total-tile-bytes"])} (${formatBytes(breakdown["total-tile-bytes"])})`],
            ["Parse errors", formatInt(breakdown["parse-errors"])],
        ];
        for (const [label, value] of summaryRows) {
            const tr = document.createElement("tr");
            tr.innerHTML = `<td>${label}</td><td class="number">${value}</td>`;
            summaryBody.appendChild(tr);
        }
    }

    const totalBytes = Number(breakdown["total-tile-bytes"] || 0);
    renderByteBreakdownRows("#featureLayerBreakdown tbody", breakdown["feature-layer"], totalBytes);
    renderByteBreakdownRows("#modelPoolBreakdown tbody", breakdown["model-pool"], totalBytes);
    renderArrayArenaSingletons(breakdown);
}
)HTML"
R"HTML(

function renderArrayArenaSingletons(breakdown) {
    const tbody = qs("#arrayArenaSingletonsTable tbody");
    if (!tbody) {
        return;
    }

    const singletonBreakdown = breakdown["array-arena-singletons"] || {};
    const singletonJson = JSON.stringify(singletonBreakdown);
    if (state.lastArrayArenaSingletonJson === singletonJson) {
        return;
    }
    state.lastArrayArenaSingletonJson = singletonJson;

    tbody.innerHTML = "";
    for (const [arenaName, statsRaw] of Object.entries(singletonBreakdown)) {
        const stats = statsRaw || {};
        const handles = Number(stats["handles"] || 0);
        const occupied = Number(stats["occupied"] || 0);
        const empty = Number(stats["empty"] || 0);
        const singletonBytes = Number(stats["singleton-storage-bytes"] || 0);
        const regularBytes = Number(stats["hypothetical-regular-bytes"] || 0);
        const savedBytes = Number(stats["estimated-saved-bytes"] || 0);
        const savedShare = regularBytes > 0 ? savedBytes / regularBytes : 0;

        const tr = document.createElement("tr");
        tr.innerHTML =
            `<td>${arenaName}</td>` +
            `<td class="number">${formatInt(handles)}</td>` +
            `<td class="number">${formatInt(occupied)}</td>` +
            `<td class="number">${formatInt(empty)}</td>` +
            `<td class="number">${formatInt(singletonBytes)} (${formatBytes(singletonBytes)})</td>` +
            `<td class="number">${formatInt(regularBytes)} (${formatBytes(regularBytes)})</td>` +
            `<td class="number">${formatInt(savedBytes)} (${formatBytes(savedBytes)})</td>` +
            `<td class="number">${formatPct(savedShare)}</td>`;
        tbody.appendChild(tr);
    }
}

function renderTileDistribution(service) {
    const distribution = service["cached-feature-tile-size-distribution"];
    const panel = byId("tileSizePanel");
    if (!panel) {
        return;
    }
    if (!distribution) {
        panel.style.display = "none";
        state.lastDistributionJson = "";
        return;
    }

    const distributionJson = JSON.stringify(distribution);
    panel.style.display = "block";
    if (state.lastDistributionJson === distributionJson) {
        return;
    }
    state.lastDistributionJson = distributionJson;

    const summaryBody = qs("#tileSizeSummary tbody");
    if (!summaryBody) {
        return;
    }
    summaryBody.innerHTML = "";
    const summaryRows = [
        ["Tile count", formatInt(distribution["tile-count"])],
        ["Total size", `${formatInt(distribution["total-tile-bytes"])} (${formatBytes(distribution["total-tile-bytes"])})`],
        ["Min", formatBytes(distribution["min-bytes"])],
        ["Mean", formatBytes(distribution["mean-bytes"])],
        ["Max", formatBytes(distribution["max-bytes"])],
    ];
    for (const [label, value] of summaryRows) {
        const tr = document.createElement("tr");
        tr.innerHTML = `<td>${label}</td><td class="number">${value}</td>`;
        summaryBody.appendChild(tr);
    }

    const histogramBody = qs("#tileSizeHistogram tbody");
    if (!histogramBody) {
        return;
    }
    histogramBody.innerHTML = "";
    const bins = distribution["histogram"] || [];
    const totalCount = Number(distribution["tile-count"] || 0);
    for (const bin of bins) {
        const count = Number(bin["count"] || 0);
        const share = totalCount > 0 ? count / totalCount : 0;
        const tr = document.createElement("tr");
        tr.innerHTML =
            `<td>${bin["label"]}</td><td class="number">${formatInt(count)}</td><td class="number">${formatPct(share)}</td>`;
        histogramBody.appendChild(tr);
    }
}
)HTML"
R"HTML(

async function refreshStatus(force = false) {
    if (state.refreshInFlight) {
        if (force) {
            state.pendingForcedRefresh = true;
        }
        return;
    }
    state.refreshInFlight = true;
    const errorEl = byId("error");
    if (errorEl) {
        errorEl.textContent = "";
    }
    try {
        const includeTileSizeDistribution = !!byId("includeTileSizeDistribution")?.checked;
        const includeCachedFeatureTreeBytes = includeTileSizeDistribution;
        const params = new URLSearchParams();
        if (includeTileSizeDistribution) {
            params.set("includeTileSizeDistribution", "1");
        }
        params.set("includeCachedFeatureTreeBytes", includeCachedFeatureTreeBytes ? "1" : "0");
        params.set("_", String(Date.now()));

        const response = await fetch(`/status-data?${params.toString()}`, {cache: "no-store"});
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const payload = await response.json();
        setPreJsonIfChanged("serviceStats", payload.service, "lastServiceText");
        setPreJsonIfChanged("cacheStats", payload.cache, "lastCacheText");
        renderWsMetrics(payload.tilesWebsocket || {});
        const service = payload.service || {};
        renderTreeBreakdown(service);
        renderTileDistribution(service);
        const lastUpdatedEl = byId("lastUpdated");
        if (lastUpdatedEl) {
            lastUpdatedEl.textContent = `Updated: ${new Date(payload.timestampMs || Date.now()).toLocaleTimeString()}`;
        }
    } catch (err) {
        if (errorEl) {
            errorEl.textContent = String(err);
        }
    } finally {
        state.refreshInFlight = false;
        if (state.pendingForcedRefresh) {
            state.pendingForcedRefresh = false;
            queueMicrotask(() => refreshStatus(false));
        }
    }
}

function resetTimer() {
    if (state.timer !== null) {
        clearInterval(state.timer);
    }
    const refreshMsInput = byId("refreshMs");
    const interval = Math.max(200, Number(refreshMsInput?.value || 1000));
    state.timer = setInterval(() => refreshStatus(false), interval);
}

byId("refreshMs")?.addEventListener("change", resetTimer);
byId("refreshNow")?.addEventListener("click", () => refreshStatus(true));
byId("includeTileSizeDistribution")?.addEventListener("change", () => refreshStatus(true));
resetTimer();
refreshStatus(true);
</script>
</body>
</html>
)HTML";
}

}  // namespace

void HttpService::Impl::handleStatusDataRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    const bool includeTileSizeDistribution =
        parseBoolParameter(req, "includeTileSizeDistribution", false);
    const bool includeCachedFeatureTreeBytes =
        parseBoolParameter(req, "includeCachedFeatureTreeBytes", false);

    const auto payload = nlohmann::json::object({
        {"timestampMs",
         std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count()},
        {"service", self_.getStatistics(includeCachedFeatureTreeBytes, includeTileSizeDistribution)},
        {"cache", self_.cache()->getStatistics()},
        {"tilesWebsocket", detail::tilesWebSocketMetricsSnapshot()},
    });

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(payload.dump());
    callback(resp);
}

void HttpService::Impl::handleStatusRequest(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCode(drogon::CT_TEXT_HTML);
    resp->setBody(statusPageHtml());
    callback(resp);
}

}  // namespace mapget
