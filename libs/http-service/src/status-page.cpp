#include "status-page.h"

namespace mapget::detail
{

std::string_view statusPageHtml()
{
    static constexpr std::string_view page = R"STATUS(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>mapget service status</title>
<style>
:root {
    color-scheme: dark;
    --ink: #eaf2ef;
    --muted: #9cafaa;
    --line: #2a3d39;
    --line-strong: #3d5751;
    --surface: rgba(16, 26, 24, 0.94);
    --surface-strong: #152320;
    --canvas: #09110f;
    --brand: #65d9bf;
    --brand-strong: #15947f;
    --brand-soft: #163b34;
    --ok: #70dda0;
    --ok-soft: #153a29;
    --warn: #f3bd67;
    --warn-soft: #422f15;
    --bad: #ff8c8f;
    --bad-soft: #442124;
    --shadow: 0 18px 46px rgba(0, 0, 0, 0.28);
}

* { box-sizing: border-box; }
[hidden] { display: none !important; }

body {
    margin: 0;
    min-width: 320px;
    color: var(--ink);
    background:
        radial-gradient(circle at 12% -10%, rgba(29, 148, 126, 0.23), transparent 36rem),
        radial-gradient(circle at 92% 8%, rgba(97, 119, 83, 0.12), transparent 28rem),
        linear-gradient(145deg, #0d1715 0%, var(--canvas) 56%, #0a1412 100%);
    font-family: "Avenir Next", "Segoe UI Variable", "Noto Sans", sans-serif;
    line-height: 1.45;
}

button, select { font: inherit; }
button { cursor: pointer; }

.shell {
    width: min(1540px, calc(100% - 32px));
    margin: 0 auto;
    padding: 24px 0 48px;
}

.masthead {
    display: flex;
    justify-content: space-between;
    gap: 24px;
    align-items: flex-end;
    padding: 22px 24px;
    border: 1px solid var(--line-strong);
    border-radius: 18px;
    background: var(--surface);
    box-shadow: var(--shadow);
    backdrop-filter: blur(12px);
}

.brand { display: flex; align-items: center; gap: 14px; min-width: 0; }
.brand-mark {
    display: grid;
    place-items: center;
    width: 46px;
    height: 46px;
    flex: 0 0 auto;
    border-radius: 13px;
    color: #071612;
    background: linear-gradient(145deg, #77e2c9, #20a88f);
    box-shadow: 0 8px 22px rgba(24, 161, 137, 0.24);
    font-weight: 800;
    letter-spacing: -0.06em;
}

.eyebrow {
    margin: 0 0 2px;
    color: var(--brand);
    font-size: 0.72rem;
    font-weight: 800;
    letter-spacing: 0.16em;
    text-transform: uppercase;
}

h1 { margin: 0; font-size: clamp(1.5rem, 3vw, 2.2rem); letter-spacing: -0.035em; }
h2 { margin: 0; font-size: 1.25rem; letter-spacing: -0.02em; }
h3 { margin: 0; font-size: 1rem; }
.host { color: var(--muted); font-family: "Cascadia Code", "SFMono-Regular", monospace; font-size: 0.8rem; }

.masthead-tools {
    display: flex;
    flex-wrap: wrap;
    justify-content: flex-end;
    align-items: center;
    gap: 10px;
}

.field {
    display: flex;
    align-items: center;
    gap: 8px;
    color: var(--muted);
    font-size: 0.82rem;
    font-weight: 700;
}

select, .button {
    min-height: 36px;
    border: 1px solid var(--line-strong);
    border-radius: 10px;
    background: var(--surface-strong);
    color: var(--ink);
    padding: 7px 11px;
}

.button { font-weight: 750; transition: transform 120ms ease, border-color 120ms ease, background 120ms ease; }
.button:hover { border-color: var(--brand); transform: translateY(-1px); }
.button:disabled { cursor: wait; opacity: 0.62; transform: none; }
.button-primary { border-color: var(--brand-strong); background: var(--brand-strong); color: #f4fffb; }
.button-primary:hover { background: #1cab93; }
.button-quiet { background: transparent; }

.status-badge {
    display: inline-flex;
    align-items: center;
    gap: 7px;
    min-height: 32px;
    border-radius: 999px;
    padding: 5px 11px;
    background: #1b2926;
    color: var(--muted);
    font-size: 0.8rem;
    font-weight: 800;
}

.status-dot { width: 8px; height: 8px; border-radius: 50%; background: currentColor; }
.status-healthy { color: var(--ok); background: var(--ok-soft); }
.status-starting { color: var(--warn); background: var(--warn-soft); }
.status-degraded { color: var(--bad); background: var(--bad-soft); }
.status-waiting .status-dot { animation: pulse 1.3s ease-in-out infinite; }

@keyframes pulse { 50% { opacity: 0.3; transform: scale(0.7); } }

.refresh-meta { min-width: 145px; color: var(--muted); font-size: 0.78rem; text-align: right; }
.error-banner {
    margin-top: 12px;
    border: 1px solid #744044;
    border-radius: 12px;
    padding: 10px 13px;
    color: var(--bad);
    background: var(--bad-soft);
    font-weight: 700;
}

.tabs {
    position: sticky;
    top: 0;
    z-index: 5;
    display: flex;
    gap: 6px;
    margin: 18px 0;
    padding: 6px;
    overflow-x: auto;
    border: 1px solid var(--line);
    border-radius: 14px;
    background: rgba(12, 21, 19, 0.94);
    box-shadow: 0 10px 28px rgba(0, 0, 0, 0.2);
    backdrop-filter: blur(12px);
}

.tab {
    flex: 0 0 auto;
    border: 0;
    border-radius: 9px;
    padding: 9px 15px;
    color: var(--muted);
    background: transparent;
    font-weight: 800;
}
.tab:hover { color: var(--ink); background: #1a2a27; }
.tab[aria-selected="true"] { color: #061511; background: var(--brand); }

.tab-panel { display: grid; gap: 16px; animation: reveal 180ms ease-out; }
#cacheReportResults { display: grid; gap: 14px; }
@keyframes reveal { from { opacity: 0; transform: translateY(4px); } }

.section-head {
    display: flex;
    justify-content: space-between;
    align-items: flex-end;
    gap: 16px;
    margin: 0;
}
.section-head p { margin: 3px 0 0; color: var(--muted); font-size: 0.88rem; }

.metric-grid {
    display: grid;
    grid-template-columns: repeat(6, minmax(150px, 1fr));
    gap: 12px;
    margin-bottom: 0;
}

.metric-card {
    min-height: 118px;
    border: 1px solid var(--line);
    border-radius: 15px;
    padding: 15px;
    background: var(--surface);
    box-shadow: 0 9px 26px rgba(0, 0, 0, 0.2);
}
.metric-label { display: block; color: var(--muted); font-size: 0.72rem; font-weight: 800; letter-spacing: 0.08em; text-transform: uppercase; }
.metric-value { display: block; margin-top: 12px; font-size: 1.35rem; font-weight: 850; letter-spacing: -0.035em; font-variant-numeric: tabular-nums; }
.metric-note { display: block; margin-top: 4px; color: var(--muted); font-size: 0.78rem; }

.grid-two { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 14px; align-items: start; }
.stack { display: grid; gap: 14px; }
.panel {
    min-width: 0;
    border: 1px solid var(--line);
    border-radius: 15px;
    padding: 16px;
    background: var(--surface);
    box-shadow: 0 9px 26px rgba(0, 0, 0, 0.18);
}
.panel-head { display: flex; justify-content: space-between; align-items: center; gap: 12px; margin-bottom: 12px; }
.panel-subtitle { color: var(--muted); font-size: 0.8rem; }
.table-wrap { width: 100%; overflow-x: auto; border-radius: 10px; }

table { width: 100%; border-collapse: separate; border-spacing: 0; font-size: 0.81rem; }
th, td { border-bottom: 1px solid var(--line); padding: 8px 10px; text-align: left; vertical-align: middle; }
th {
    color: #b5c9c3;
    background: #1c2b28;
    font-size: 0.7rem;
    letter-spacing: 0.055em;
    text-transform: uppercase;
    white-space: nowrap;
}
tbody tr:last-child td { border-bottom: 0; }
tbody tr:hover td { background: rgba(101, 217, 191, 0.07); }
.number { text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }
.muted { color: var(--muted); }
.explanation { margin: 12px 0 0; color: var(--muted); font-size: 0.8rem; }

.metric-name { display: inline-flex; align-items: center; gap: 7px; }
.info-bubble {
    display: inline-grid;
    place-items: center;
    width: 17px;
    height: 17px;
    flex: 0 0 auto;
    border: 1px solid var(--line-strong);
    border-radius: 50%;
    padding: 0;
    color: var(--brand);
    background: #10201d;
    font: 800 0.68rem/1 "Cascadia Code", "SFMono-Regular", monospace;
    cursor: help;
}
.info-bubble:hover, .info-bubble:focus-visible {
    border-color: var(--brand);
    outline: none;
    background: var(--brand-soft);
}

.state-pill {
    display: inline-flex;
    border-radius: 999px;
    padding: 3px 8px;
    color: var(--muted);
    background: #1b2926;
    font-size: 0.72rem;
    font-weight: 800;
    text-transform: capitalize;
}
.state-ready { color: var(--ok); background: var(--ok-soft); }
.state-initializing { color: var(--warn); background: var(--warn-soft); }
.state-failed { color: var(--bad); background: var(--bad-soft); }

.report-callout {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 18px;
    border: 1px solid #38665d;
    border-radius: 15px;
    padding: 18px;
    background: linear-gradient(120deg, #132a25, #18352e);
}
.report-callout p { margin: 4px 0 0; color: #a9c0ba; font-size: 0.86rem; }
.report-actions { display: flex; flex-wrap: wrap; gap: 8px; justify-content: flex-end; }
.report-status { margin-top: 10px; min-height: 22px; color: var(--muted); font-size: 0.82rem; }
.report-status.busy::before {
    content: "";
    display: inline-block;
    width: 10px;
    height: 10px;
    margin-right: 7px;
    border: 2px solid #477d72;
    border-top-color: var(--brand);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
}
@keyframes spin { to { transform: rotate(360deg); } }
.report-stale { color: var(--warn); font-weight: 750; }
.empty-state { padding: 34px 18px; text-align: center; color: var(--muted); }

details { border-top: 1px solid var(--line); padding-top: 10px; }
details + details { margin-top: 10px; }
summary { cursor: pointer; color: var(--brand); font-weight: 800; }
pre {
    max-height: 64vh;
    margin: 10px 0 0;
    overflow: auto;
    border-radius: 10px;
    padding: 12px;
    color: #d7eee8;
    background: #07100e;
    white-space: pre-wrap;
    word-break: break-word;
    font: 0.76rem/1.48 "Cascadia Code", "SFMono-Regular", Consolas, monospace;
}
.raw-actions { display: flex; gap: 8px; margin: 10px 0; }

@media (max-width: 1180px) {
    .metric-grid { grid-template-columns: repeat(3, minmax(150px, 1fr)); }
}
@media (max-width: 780px) {
    .shell { width: min(100% - 20px, 1540px); padding-top: 10px; }
    .masthead { align-items: flex-start; padding: 17px; }
    .masthead, .report-callout { flex-direction: column; }
    .masthead-tools, .report-actions { justify-content: flex-start; }
    .refresh-meta { text-align: left; }
    .grid-two { grid-template-columns: 1fr; }
    .metric-grid { grid-template-columns: repeat(2, minmax(135px, 1fr)); }
    .panel { padding: 13px; }
}
@media (max-width: 470px) {
    .metric-grid { grid-template-columns: 1fr; }
    .metric-card { min-height: 98px; }
    .field { width: 100%; justify-content: space-between; }
}
</style>
)STATUS"
R"STATUS(</head>
<body>
<div class="shell">
    <header class="masthead">
        <div class="brand">
            <div class="brand-mark" aria-hidden="true">M</div>
            <div>
                <p class="eyebrow">mapget operations</p>
                <h1>Service Status</h1>
                <div id="host" class="host"></div>
            </div>
        </div>
        <div class="masthead-tools">
            <span id="overallBadge" class="status-badge status-waiting"><span class="status-dot"></span><span id="overallBadgeText">Connecting</span></span>
            <label class="field">Auto refresh
                <select id="refreshInterval">
                    <option value="0">Off</option>
                    <option value="1000">1 second</option>
                    <option value="5000" selected>5 seconds</option>
                    <option value="15000">15 seconds</option>
                </select>
            </label>
            <button id="refreshNow" class="button" type="button">Refresh</button>
            <span id="lastUpdated" class="refresh-meta" aria-live="polite">Waiting for first snapshot</span>
        </div>
    </header>
    <div id="errorBanner" class="error-banner" role="alert" hidden></div>

    <nav class="tabs" role="tablist" aria-label="Status sections">
        <button class="tab" role="tab" id="tab-overview" aria-controls="panel-overview" aria-selected="true" data-tab="overview">Overview</button>
        <button class="tab" role="tab" id="tab-memory" aria-controls="panel-memory" aria-selected="false" data-tab="memory" tabindex="-1">Memory</button>
        <button class="tab" role="tab" id="tab-traffic" aria-controls="panel-traffic" aria-selected="false" data-tab="traffic" tabindex="-1">Traffic</button>
        <button class="tab" role="tab" id="tab-cache" aria-controls="panel-cache" aria-selected="false" data-tab="cache" tabindex="-1">Cache Report</button>
        <button class="tab" role="tab" id="tab-diagnostics" aria-controls="panel-diagnostics" aria-selected="false" data-tab="diagnostics" tabindex="-1">Diagnostics</button>
    </nav>

    <main>
        <section id="panel-overview" class="tab-panel" role="tabpanel" aria-labelledby="tab-overview" data-panel="overview">
            <div class="metric-grid">
                <article class="metric-card"><span class="metric-label">Datasources</span><strong id="metricDatasources" class="metric-value">-</strong><span id="metricDatasourcesNote" class="metric-note">Waiting</span></article>
                <article class="metric-card"><span class="metric-label">Process RSS</span><strong id="metricRss" class="metric-value">-</strong><span id="metricRssNote" class="metric-note">Current resident memory</span></article>
                <article class="metric-card"><span class="metric-label">Cache</span><strong id="metricCache" class="metric-value">-</strong><span id="metricCacheNote" class="metric-note">Retained tile data</span></article>
                <article class="metric-card"><span class="metric-label">Active work</span><strong id="metricWork" class="metric-value">-</strong><span id="metricWorkNote" class="metric-note">Requests and filters</span></article>
                <article class="metric-card"><span class="metric-label">Interactive</span><strong id="metricInteractive" class="metric-value">-</strong><span id="metricInteractiveNote" class="metric-note">Connected sessions</span></article>
                <article class="metric-card"><span class="metric-label">Allocator free</span><strong id="metricAllocator" class="metric-value">-</strong><span id="metricAllocatorNote" class="metric-note">Reusable arena memory</span></article>
            </div>

            <div class="grid-two">
                <section class="panel">
                    <div class="panel-head"><div><h2>Datasource Readiness</h2><div class="panel-subtitle">Configured sources in service order</div></div></div>
                    <div class="table-wrap"><table id="overviewDatasourceTable"><thead><tr><th>Source</th><th>Map</th><th>Status</th><th class="number">Retained</th></tr></thead><tbody></tbody></table></div>
                </section>
                <div class="stack">
                    <section class="panel">
                        <div class="panel-head"><h2>Current Work</h2></div>
                        <div class="table-wrap"><table id="currentWorkTable"><thead><tr><th>Workload</th><th class="number">Current</th></tr></thead><tbody></tbody></table></div>
                    </section>
                    <section class="panel">
                        <div class="panel-head"><h2>Pressure Signals</h2></div>
                        <div class="table-wrap"><table id="pressureTable"><thead><tr><th>Signal</th><th class="number">Current</th></tr></thead><tbody></tbody></table></div>
                    </section>
                </div>
            </div>
        </section>

        <section id="panel-memory" class="tab-panel" role="tabpanel" aria-labelledby="tab-memory" data-panel="memory" hidden>
            <div class="section-head"><div><h2>Memory Accountability</h2><p>Process residency, allocator state, and instrumented ownership shown as distinct measurement domains.</p></div></div>
            <section class="panel">
                <div class="table-wrap"><table id="memoryOverviewTable"><thead><tr><th>Owner / control total</th><th class="number">Current</th><th class="number">Peak</th></tr></thead><tbody></tbody></table></div>
                <p class="explanation">These rows intentionally overlap and must not all be added together. Use each information icon for its measurement scope. Mapget and cache values are capacity-based lower bounds; datasource values are cooperative exclusive estimates.</p>
            </section>
            <div class="grid-two">
                <section class="panel">
                    <div class="panel-head"><h2>Allocator Maintenance</h2></div>
                    <div class="table-wrap"><table id="allocatorTrimTable"><thead><tr><th>Metric</th><th class="number">Value</th></tr></thead><tbody></tbody></table></div>
                </section>
                <section class="panel">
                    <div class="panel-head"><h2>Datasource-Owned State</h2></div>
                    <div class="table-wrap"><table id="datasourceMemoryTable"><thead><tr><th>Source</th><th>Map</th><th>Status</th><th>Measurement</th><th class="number">Retained</th></tr></thead><tbody></tbody></table></div>
                </section>
            </div>
            <section class="panel">
                <div class="panel-head"><h2>Active Filter Ownership</h2></div>
                <div class="table-wrap"><table id="activeFilterMemoryTable"><thead><tr><th>Filter</th><th>Map / Layer</th><th class="number">Tiles</th><th class="number">Current</th><th class="number">Source Models</th><th class="number">Output Models</th><th class="number">Relation Targets</th><th class="number">Evaluation</th><th class="number">Orchestration</th></tr></thead><tbody></tbody></table></div>
            </section>
            <section class="panel"><details><summary>Raw memory snapshot</summary><pre id="memoryStats"></pre></details></section>
        </section>

        <section id="panel-traffic" class="tab-panel" role="tabpanel" aria-labelledby="tab-traffic" data-panel="traffic" hidden>
            <div class="section-head"><div><h2>Traffic</h2><p>Live queue pressure and lifetime transport counters.</p></div></div>
            <div class="grid-two">
                <section class="panel">
                    <div class="panel-head"><div><h2>Interactive Transport</h2><div class="panel-subtitle">WebSocket control and payload pulls</div></div></div>
                    <div class="table-wrap"><table id="interactiveMetricsTable"><thead><tr><th>Metric</th><th class="number">Value</th></tr></thead><tbody></tbody></table></div>
                </section>
                <section class="panel">
                    <div class="panel-head"><div><h2>REST Streams</h2><div class="panel-subtitle">Current streamed response ownership</div></div></div>
                    <div class="table-wrap"><table id="restMetricsTable"><thead><tr><th>Metric</th><th class="number">Value</th></tr></thead><tbody></tbody></table></div>
                </section>
            </div>
        </section>

        <section id="panel-cache" class="tab-panel" role="tabpanel" aria-labelledby="tab-cache" data-panel="cache" hidden>
            <div class="section-head"><div><h2>Cache</h2><p>Live counters plus an explicitly generated point-in-time analysis.</p></div></div>
            <div class="metric-grid">
                <article class="metric-card"><span class="metric-label">Entries</span><strong id="cacheEntries" class="metric-value">-</strong><span class="metric-note">In-memory tile records</span></article>
                <article class="metric-card"><span class="metric-label">Tile storage</span><strong id="cacheTileBytes" class="metric-value">-</strong><span class="metric-note">Allocated blob capacity</span></article>
                <article class="metric-card"><span class="metric-label">Hit ratio</span><strong id="cacheHitRatio" class="metric-value">-</strong><span class="metric-note">Lifetime lookups</span></article>
                <article class="metric-card"><span class="metric-label">Loaded pools</span><strong id="cacheStringPools" class="metric-value">-</strong><span class="metric-note">Datasource string pools</span></article>
                <article class="metric-card"><span class="metric-label">Hits</span><strong id="cacheHits" class="metric-value">-</strong><span class="metric-note">Lifetime</span></article>
                <article class="metric-card"><span class="metric-label">Misses</span><strong id="cacheMisses" class="metric-value">-</strong><span class="metric-note">Lifetime</span></article>
            </div>

            <section class="report-callout">
                <div><h2>Detailed Cache Report</h2><p>Parses cached feature tiles once to measure model storage and build a tile-size histogram. Normal status refreshes never run this analysis.</p></div>
                <div class="report-actions">
                    <button id="generateCacheReport" class="button button-primary" type="button">Generate report</button>
                    <button id="downloadCacheReport" class="button" type="button" disabled>Download JSON</button>
                </div>
            </section>
            <div id="cacheReportStatus" class="report-status" role="status" aria-live="polite">No report generated in this browser session.</div>

            <div id="cacheReportEmpty" class="panel empty-state">Generate a report to inspect cached feature-model storage and tile sizes.</div>
            <div id="cacheReportResults" hidden>
                <section class="panel">
                    <div class="panel-head"><div><h2>Report Snapshot</h2><div id="cacheReportMeta" class="panel-subtitle"></div></div><span id="cacheReportStale" class="report-stale"></span></div>
                    <div class="table-wrap"><table id="treeBreakdownSummary"><thead><tr><th>Metric</th><th class="number">Value</th></tr></thead><tbody></tbody></table></div>
                </section>
                <div class="grid-two">
                    <section class="panel"><div class="panel-head"><h2>Feature Layer Bytes</h2></div><div class="table-wrap"><table id="featureLayerBreakdown"><thead><tr><th>Type</th><th class="number">Bytes</th><th class="number">Readable</th><th class="number">Share</th></tr></thead><tbody></tbody></table></div></section>
                    <section class="panel"><div class="panel-head"><h2>Model Pool Bytes</h2></div><div class="table-wrap"><table id="modelPoolBreakdown"><thead><tr><th>Type</th><th class="number">Bytes</th><th class="number">Readable</th><th class="number">Share</th></tr></thead><tbody></tbody></table></div></section>
                </div>
                <section class="panel"><div class="panel-head"><h2>Array Arena Singleton Usage</h2></div><div class="table-wrap"><table id="arrayArenaSingletonsTable"><thead><tr><th>Arena</th><th class="number">Handles</th><th class="number">Occupied</th><th class="number">Empty</th><th class="number">Singleton</th><th class="number">Regular equivalent</th><th class="number">Saved</th><th class="number">Saved share</th></tr></thead><tbody></tbody></table></div></section>
                <div class="grid-two">
                    <section class="panel"><div class="panel-head"><h2>Tile Size Summary</h2></div><div class="table-wrap"><table id="tileSizeSummary"><thead><tr><th>Metric</th><th class="number">Value</th></tr></thead><tbody></tbody></table></div></section>
                    <section class="panel"><div class="panel-head"><h2>Tile Size Distribution</h2></div><div class="table-wrap"><table id="tileSizeHistogram"><thead><tr><th>Bin</th><th class="number">Count</th><th class="number">Share</th></tr></thead><tbody></tbody></table></div></section>
                </div>
                <section class="panel"><details><summary>Raw cache report</summary><pre id="cacheReportRaw"></pre></details></section>
            </div>
        </section>

        <section id="panel-diagnostics" class="tab-panel" role="tabpanel" aria-labelledby="tab-diagnostics" data-panel="diagnostics" hidden>
            <div class="section-head"><div><h2>Diagnostics</h2><p>Raw snapshots for support and machine-assisted investigation.</p></div><div class="raw-actions"><button id="copyStatusPayload" class="button button-quiet" type="button">Copy snapshot</button><button id="downloadStatusPayload" class="button" type="button">Download JSON</button></div></div>
            <section class="panel">
                <details><summary>Service statistics</summary><pre id="serviceStats"></pre></details>
                <details><summary>Cache statistics</summary><pre id="cacheStats"></pre></details>
                <details><summary>Complete status payload</summary><pre id="completeStats"></pre></details>
            </section>
        </section>
    </main>
</div>

)STATUS"
R"STATUS(<script>
/** Resolve one dashboard element by ID. */
const byId = (id) => document.getElementById(id);
/** Resolve one dashboard element by selector. */
const qs = (selector) => document.querySelector(selector);
/** Format an integer using the browser locale. */
const formatInt = (value) => Number(value || 0).toLocaleString();
/** Format a ratio as a percentage. */
const formatPct = (value) => `${(Number(value || 0) * 100).toFixed(1)}%`;
/** Format a measured duration in milliseconds. */
const formatDuration = (milliseconds) => `${Number(milliseconds || 0).toLocaleString(undefined, {maximumFractionDigits: 1})} ms`;
/** Format a byte count with a binary unit. */
const formatBytes = (bytes) => {
    const units = ["B", "KiB", "MiB", "GiB", "TiB"];
    let value = Number(bytes || 0);
    let unit = 0;
    while (Math.abs(value) >= 1024 && unit < units.length - 1) {
        value /= 1024;
        unit++;
    }
    return `${value.toFixed(unit === 0 ? 0 : 2)} ${units[unit]}`;
};

const state = {
    timer: null,
    refreshInFlight: false,
    pendingForcedRefresh: false,
    cacheReportInFlight: false,
    payload: null,
    cacheReport: null,
    textCache: new Map(),
};

/** Set text without interpreting server-provided content as markup. */
function setText(id, value) {
    const element = byId(id);
    if (element) element.textContent = value;
}

/** Render JSON only when its serialized value changed. */
function setPreJson(id, value) {
    const text = JSON.stringify(value ?? {}, null, 2);
    if (state.textCache.get(id) === text) return;
    const element = byId(id);
    if (!element) return;
    element.textContent = text;
    state.textCache.set(id, text);
}

/** Replace one table body with safely constructed cells. */
function replaceRows(selector, rows, numericColumns = [], emptyText = "No data available.") {
    const body = qs(selector);
    if (!body) return;
    body.textContent = "";
    if (!rows.length) {
        const row = document.createElement("tr");
        const cell = document.createElement("td");
        cell.colSpan = Math.max(1, body.closest("table")?.tHead?.rows?.[0]?.cells?.length || 1);
        cell.className = "muted";
        cell.textContent = emptyText;
        row.appendChild(cell);
        body.appendChild(row);
        return;
    }
    for (const values of rows) {
        const row = document.createElement("tr");
        values.forEach((value, index) => {
            const cell = document.createElement("td");
            cell.textContent = value ?? "-";
            if (numericColumns.includes(index)) cell.className = "number";
            row.appendChild(cell);
        });
        body.appendChild(row);
    }
}

/** Update a summary metric and its explanatory note. */
function setMetric(valueId, noteId, value, note) {
    setText(valueId, value);
    setText(noteId, note);
}

/** Map a datasource lifecycle state to its presentation class. */
function datasourceStatusClass(status) {
    const normalized = String(status || "unknown").toLowerCase();
    if (normalized === "ready") return "state-ready";
    if (normalized === "initializing") return "state-initializing";
    if (normalized === "failed") return "state-failed";
    return "";
}

/** Render ordered datasource state with optional ownership measurement details. */
function renderDatasourceTable(selector, datasources, includeMeasurement) {
    const body = qs(selector);
    if (!body) return;
    body.textContent = "";
    if (!datasources.length) {
        replaceRows(selector, [], [], "No datasource state reported.");
        return;
    }
    for (const source of datasources) {
        const row = document.createElement("tr");
        const values = [source["source-id"] || "<runtime>", source["map-id"] || ""];
        for (const value of values) {
            const cell = document.createElement("td");
            cell.textContent = value;
            row.appendChild(cell);
        }
        const statusCell = document.createElement("td");
        const status = document.createElement("span");
        status.className = `state-pill ${datasourceStatusClass(source.status)}`;
        status.textContent = source.status || "unknown";
        statusCell.appendChild(status);
        row.appendChild(statusCell);
        if (includeMeasurement) {
            const measurement = document.createElement("td");
            measurement.textContent = source.measurement || "unavailable";
            row.appendChild(measurement);
        }
        const retained = document.createElement("td");
        retained.className = "number";
        retained.textContent = source["retained-bytes"] === undefined ? "-" : formatBytes(source["retained-bytes"]);
        row.appendChild(retained);
        body.appendChild(row);
    }
}

/** Derive the customer-facing service state from datasource lifecycle state. */
function renderHealth(payload) {
    const service = payload.service || {};
    const config = service["datasource-config"] || {};
    const datasources = payload.memory?.datasources || [];
    const failed = Number(config["construction-failed"] || 0) + datasources.filter((source) => source.status === "failed").length;
    const initializing = datasources.filter((source) => source.status === "initializing").length;
    const badge = byId("overallBadge");
    badge?.classList.remove("status-waiting", "status-healthy", "status-starting", "status-degraded");
    if (failed > 0) {
        badge?.classList.add("status-degraded");
        setText("overallBadgeText", "Degraded");
    } else if (initializing > 0) {
        badge?.classList.add("status-starting");
        setText("overallBadgeText", "Initializing");
    } else {
        badge?.classList.add("status-healthy");
        setText("overallBadgeText", "Operational");
    }
}

/** Render the concise operational overview. */
function renderOverview(payload) {
    const service = payload.service || {};
    const memory = payload.memory || {};
    const cache = payload.cache || {};
    const interactive = payload.tilesWebsocket || {};
    const rest = payload.tilesHttp || {};
    const config = service["datasource-config"] || {};
    const filters = service["filter-evaluation"] || {};
    const datasources = memory.datasources || [];
    const ready = datasources.filter((source) => source.status === "ready").length;
    const configured = Number(config.configured ?? datasources.length);
    setMetric("metricDatasources", "metricDatasourcesNote", `${ready} / ${configured}`, "ready / configured");

    const process = memory.process || {};
    const cgroup = process.cgroup || {};
    const rss = Number(process["resident-bytes"] || 0);
    const limit = Number(cgroup["limit-bytes"] || 0);
    setMetric("metricRss", "metricRssNote", formatBytes(rss), limit > 0 ? `${formatPct(rss / limit)} of cgroup limit` : "Current resident memory");

    const cacheBytes = Number(memory["cache-current-bytes"] || 0);
    const entries = cache["memcache-map-size"];
    setMetric("metricCache", "metricCacheNote", formatBytes(cacheBytes), entries === undefined ? "Retained cache state" : `${formatInt(entries)} entries`);

    const activeRequests = Number(service["active-requests"] || 0);
    const runningFilters = Number(filters.running || 0);
    const queuedFilters = Number(filters.queued || 0);
    setMetric("metricWork", "metricWorkNote", formatInt(activeRequests + runningFilters), `${formatInt(queuedFilters)} filters queued`);

    const sessions = Number(interactive["active-sessions"] || 0);
    setMetric("metricInteractive", "metricInteractiveNote", formatInt(sessions), `${formatInt(interactive["active-connections"] || 0)} connections`);

    const allocator = memory.allocator || {};
    setMetric("metricAllocator", "metricAllocatorNote", formatBytes(allocator["free-arena-bytes"] || 0), `${formatBytes(allocator["in-use-arena-bytes"] || 0)} in use`);

    renderDatasourceTable("#overviewDatasourceTable tbody", datasources, false);
    replaceRows("#currentWorkTable tbody", [
        ["Tile requests", formatInt(activeRequests)],
        ["Filter evaluations running", formatInt(runningFilters)],
        ["Filter evaluations queued", formatInt(queuedFilters)],
        ["REST streams", formatInt(rest["active-streams"] || 0)],
        ["Interactive sessions", formatInt(sessions)],
    ], [1]);
    replaceRows("#pressureTable tbody", [
        ["Interactive queue allocation", formatBytes(interactive["pending-controller-allocated-bytes"] || 0)],
        ["REST pending allocation", formatBytes(rest["pending-capacity-bytes"] || 0)],
        ["Allocator arena free", formatBytes(allocator["free-arena-bytes"] || 0)],
        ["Dropped interactive frames", formatInt(interactive["total-dropped-frames"] || 0)],
    ], [1]);
}

/** Append one current/peak memory row with an accessible scope explanation. */
function appendMemoryRow(body, label, help, current, peak = null) {
    const row = document.createElement("tr");
    const labelCell = document.createElement("td");
    const name = document.createElement("span");
    name.className = "metric-name";
    const labelText = document.createElement("span");
    labelText.textContent = label;
    const info = document.createElement("button");
    info.type = "button";
    info.className = "info-bubble";
    info.textContent = "i";
    info.title = help;
    info.setAttribute("aria-label", `${label}: ${help}`);
    name.append(labelText, info);
    labelCell.appendChild(name);
    const currentCell = document.createElement("td");
    currentCell.className = "number";
    currentCell.textContent = `${formatInt(current)} (${formatBytes(current)})`;
    const peakCell = document.createElement("td");
    peakCell.className = "number";
    peakCell.textContent = peak === null ? "-" : `${formatInt(peak)} (${formatBytes(peak)})`;
    row.append(labelCell, currentCell, peakCell);
    body.appendChild(row);
}

)STATUS"
R"STATUS(/** Render process, allocator, datasource, and active-filter memory ownership. */
function renderMemory(memory) {
    const body = qs("#memoryOverviewTable tbody");
    if (body) {
        body.textContent = "";
        const process = memory.process || {};
        const cgroup = process.cgroup || {};
        const allocator = memory.allocator || {};
        const reconciliation = memory.reconciliation || {};
        const mapget = memory.mapget || {};
        const transport = memory.transport || {};
        const rest = transport["rest-streams"] || {};
        const interactive = transport.interactive || {};
        appendMemoryRow(body, "Process RSS", "Physical pages currently resident for this process. RSS includes anonymous and file-backed pages and is not the same as allocated heap capacity.", process["resident-bytes"] || 0, process["resident-peak-bytes"] ?? null);
        if (process["resident-anonymous-bytes"] !== undefined) appendMemoryRow(body, "Anonymous resident pages", "Resident anonymous mappings, primarily allocator arenas, thread stacks, and opaque anonymous mappings.", process["resident-anonymous-bytes"]);
        if (process["resident-file-bytes"] !== undefined || process["resident-shared-bytes"] !== undefined) appendMemoryRow(body, "File and shared resident pages", "Resident executable, shared-library, file-mapping, and shared-memory pages. These pages are outside normal heap ownership estimates.", reconciliation["file-and-shared-resident-bytes"] || 0);
        if (cgroup["current-bytes"] !== null && cgroup["current-bytes"] !== undefined) appendMemoryRow(body, "Cgroup charged memory", "Memory charged to the containing cgroup. This can include charges not represented by the process RSS figure.", cgroup["current-bytes"], cgroup["peak-bytes"] ?? null);
        if (cgroup["limit-bytes"] !== null && cgroup["limit-bytes"] !== undefined) appendMemoryRow(body, "Cgroup memory limit", "Hard memory limit configured for the containing cgroup. A missing value means no finite cgroup limit was detected.", cgroup["limit-bytes"]);
        if (allocator["in-use-arena-bytes"] !== undefined) {
            appendMemoryRow(body, "Allocator live allocations", "glibc arena allocations still in use plus allocator-managed mmap allocations. Allocated pages are not necessarily resident.", reconciliation["allocator-live-bytes"] || allocator["in-use-arena-bytes"]);
            appendMemoryRow(body, "Allocator arena free", "Arena capacity available for reuse by glibc. This is not all resident and cannot necessarily be returned to the operating system because of fragmentation.", allocator["free-arena-bytes"] || 0);
        }
        appendMemoryRow(body, "Known ownership estimate", "Sum of mapget, datasource, cache, and transport capacity estimates. It is an allocation estimate, not an RSS subtotal.", memory["known-current-bytes"] || 0);
        appendMemoryRow(body, "Mapget metadata, scheduler, filters", "Capacity-oriented lower bound for metadata snapshots, scheduler containers, active filter models, and mapget bookkeeping.", mapget["allocated-bytes"] || 0);
        appendMemoryRow(body, "Datasource-owned state", "Datasource-provided cooperative estimates of retained state. Estimator coverage and exactness depend on each datasource implementation.", memory["datasource-measured-bytes"] || 0);
        appendMemoryRow(body, "Cache", "Allocated serialized tile blobs, indexes, and retained datasource string-pool state owned by the selected cache implementation.", memory["cache-current-bytes"] || 0);
        appendMemoryRow(body, "REST pending stream buffers", "Capacity retained by active stateless streaming responses while clients consume their payloads.", rest["pending-capacity-bytes"] || 0, rest["peak-pending-capacity-bytes"] ?? null);
        appendMemoryRow(body, "Interactive pending frame buffers", "Capacity retained by interactive sessions for binary frames waiting to be pulled by clients.", interactive["pending-controller-allocated-bytes"] || 0, interactive["peak-pending-controller-allocated-bytes"] ?? null);
        if (reconciliation["allocator-live-outside-known-ownership-bytes"] !== undefined) appendMemoryRow(body, "Allocator live outside known ownership", "Diagnostic difference between allocator-live bytes and instrumented ownership. It can include uninstrumented heap, allocator bookkeeping, and estimator mismatch.", reconciliation["allocator-live-outside-known-ownership-bytes"]);
        if (reconciliation["anonymous-resident-outside-allocator-live-bytes"] !== undefined) appendMemoryRow(body, "Anonymous residency outside allocator live", "Diagnostic difference between anonymous RSS and allocator-live bytes. Common causes are resident allocator slack or fragmentation, thread stacks, and opaque anonymous mappings.", reconciliation["anonymous-resident-outside-allocator-live-bytes"]);
    }

    const trim = memory["allocator-trim"] || {};
    replaceRows("#allocatorTrimTable tbody", [
        ["Supported", trim.supported === undefined ? "Unavailable" : (trim.supported ? "Yes" : "No")],
        ["Enabled", trim.enabled ? "Yes" : "No"],
        ["Period", `${formatInt(trim["period-seconds"] || 0)} s`],
        ["Attempts", formatInt(trim.attempts || 0)],
        ["Successful trims", formatInt(trim["successful-trims"] || 0)],
        ["Last duration", `${formatInt(trim["last-duration-microseconds"] || 0)} us`],
        ["Free arena before", formatBytes(trim["last-free-arena-before-bytes"] || 0)],
        ["Free arena after", formatBytes(trim["last-free-arena-after-bytes"] || 0)],
    ], [1]);

    renderDatasourceTable("#datasourceMemoryTable tbody", memory.datasources || [], true);
    const filters = memory["active-filters"] || [];
    replaceRows("#activeFilterMemoryTable tbody", filters.map((filter) => [
        `${filter["filter-id"] || "<unnamed>"} @ ${filter.generation || 0}`,
        `${filter["map-id"] || ""} / ${filter["layer-id"] || ""}`,
        formatInt(filter["requested-tiles"] || 0),
        formatBytes(filter["current-bytes"] || 0),
        formatBytes(filter["source-tile-models"]?.["current-bytes"] || 0),
        formatBytes(filter["output-subset-models"]?.["current-bytes"] || 0),
        formatBytes(filter["relation-target-models"]?.["current-bytes"] || 0),
        formatBytes(filter["evaluation-temporaries"]?.["current-bytes"] || 0),
        formatBytes(filter.orchestration?.["current-bytes"] || 0),
    ]), [2, 3, 4, 5, 6, 7, 8], "No active filter execution owns retained state.");
    setPreJson("memoryStats", memory);
}

const interactiveMetricDefinitions = [
    ["Active connections", "active-connections", formatInt],
    ["Active sessions", "active-sessions", formatInt],
    ["Queued payload frames", "pending-controller-frames", formatInt],
    ["Queued payload bytes", "pending-controller-bytes", formatBytes],
    ["Allocated queue capacity", "pending-controller-allocated-bytes", formatBytes],
    ["Peak queue capacity", "peak-pending-controller-allocated-bytes", formatBytes],
    ["Waiting payload pulls", "pending-pull-requests", formatInt],
    ["Forwarded frames", "total-forwarded-frames", formatInt],
    ["Forwarded bytes", "total-forwarded-bytes", formatBytes],
    ["Dropped frames", "total-dropped-frames", formatInt],
    ["Dropped bytes", "total-dropped-bytes", formatBytes],
    ["Pull requests", "total-pull-requests", formatInt],
    ["Pull timeouts", "total-pull-timeouts", formatInt],
    ["Session misses", "total-pull-session-misses", formatInt],
    ["Replaced requests", "replaced-requests", formatInt],
];

const restMetricDefinitions = [
    ["Active streams", "active-streams", formatInt],
    ["Pending payload bytes", "pending-bytes", formatBytes],
    ["Allocated pending capacity", "pending-capacity-bytes", formatBytes],
    ["Peak pending bytes", "peak-pending-bytes", formatBytes],
    ["Peak pending capacity", "peak-pending-capacity-bytes", formatBytes],
    ["Measurement", "measurement", (value) => String(value || "unavailable")],
];

/** Render a declarative list of transport metrics. */
function renderMetricDefinitions(selector, metrics, definitions) {
    replaceRows(selector, definitions.map(([label, key, formatter]) => [label, formatter(metrics[key] ?? 0)]), [1]);
}

/** Render interactive and REST transport pressure. */
function renderTraffic(payload) {
    renderMetricDefinitions("#interactiveMetricsTable tbody", payload.tilesWebsocket || {}, interactiveMetricDefinitions);
    renderMetricDefinitions("#restMetricsTable tbody", payload.tilesHttp || {}, restMetricDefinitions);
}

/** Render cheap live cache counters without parsing cached tiles. */
function renderCache(cache, memory) {
    const hits = Number(cache["cache-hits"] || 0);
    const misses = Number(cache["cache-misses"] || 0);
    const lookups = hits + misses;
    const tileBytes = Number(cache.memory?.["tile-blobs"]?.["allocated-bytes"] || 0);
    setText("cacheEntries", cache["memcache-map-size"] === undefined ? "-" : formatInt(cache["memcache-map-size"]));
    setText("cacheTileBytes", formatBytes(tileBytes || memory["cache-current-bytes"] || 0));
    setText("cacheHitRatio", lookups > 0 ? formatPct(hits / lookups) : "-");
    setText("cacheStringPools", formatInt(cache["loaded-string-pools"] || 0));
    setText("cacheHits", formatInt(hits));
    setText("cacheMisses", formatInt(misses));
    updateCacheReportStaleness(cache);
}

/** Render a descending byte-accounting breakdown. */
function renderBreakdown(selector, breakdown, totalBytes) {
    const rows = Object.entries(breakdown || {})
        .filter(([, value]) => Number.isFinite(Number(value)))
        .sort((left, right) => Number(right[1]) - Number(left[1]))
        .map(([key, raw]) => {
            const value = Number(raw || 0);
            return [key, formatInt(value), formatBytes(value), formatPct(totalBytes > 0 ? value / totalBytes : 0)];
        });
    replaceRows(selector, rows, [1, 2, 3]);
}

/** Retain and render one explicitly generated cache report. */
function renderCacheReport(report) {
    state.cacheReport = report;
    byId("cacheReportEmpty").hidden = true;
    byId("cacheReportResults").hidden = false;
    byId("downloadCacheReport").disabled = false;
    const generated = new Date(report.generatedAtMs || Date.now());
    const tree = report.featureTree || {};
    const distribution = report.tileSizeDistribution || {};
    setText("cacheReportMeta", `Generated ${generated.toLocaleString()} in ${formatDuration(report.durationMs)}.`);
    replaceRows("#treeBreakdownSummary tbody", [
        ["Feature tiles analyzed", formatInt(tree["tile-count"] || distribution["tile-count"] || 0)],
        ["Total serialized bytes", `${formatInt(tree["total-tile-bytes"] || distribution["total-tile-bytes"] || 0)} (${formatBytes(tree["total-tile-bytes"] || distribution["total-tile-bytes"] || 0)})`],
        ["Parse errors", formatInt(tree["parse-errors"] || 0)],
    ], [1]);
    const totalBytes = Number(tree["total-tile-bytes"] || 0);
    renderBreakdown("#featureLayerBreakdown tbody", tree["feature-layer"], totalBytes);
    renderBreakdown("#modelPoolBreakdown tbody", tree["model-pool"], totalBytes);

    const singletonRows = Object.entries(tree["array-arena-singletons"] || {}).map(([name, statsRaw]) => {
        const stats = statsRaw || {};
        const regular = Number(stats["hypothetical-regular-bytes"] || 0);
        const saved = Number(stats["estimated-saved-bytes"] || 0);
        return [
            name,
            formatInt(stats.handles || 0),
            formatInt(stats.occupied || 0),
            formatInt(stats.empty || 0),
            formatBytes(stats["singleton-storage-bytes"] || 0),
            formatBytes(regular),
            formatBytes(saved),
            formatPct(regular > 0 ? saved / regular : 0),
        ];
    });
    replaceRows("#arrayArenaSingletonsTable tbody", singletonRows, [1, 2, 3, 4, 5, 6, 7]);

    replaceRows("#tileSizeSummary tbody", [
        ["Tile count", formatInt(distribution["tile-count"] || 0)],
        ["Total size", formatBytes(distribution["total-tile-bytes"] || 0)],
        ["Minimum", formatBytes(distribution["min-bytes"] || 0)],
        ["Mean", formatBytes(distribution["mean-bytes"] || 0)],
        ["Maximum", formatBytes(distribution["max-bytes"] || 0)],
    ], [1]);
    const totalCount = Number(distribution["tile-count"] || 0);
    replaceRows("#tileSizeHistogram tbody", (distribution.histogram || []).map((bin) => [
        bin.label || "",
        formatInt(bin.count || 0),
        formatPct(totalCount > 0 ? Number(bin.count || 0) / totalCount : 0),
    ]), [1, 2]);
    setPreJson("cacheReportRaw", report);
    updateCacheReportStaleness(state.payload?.cache || {});
}

/** Mark a retained report stale when the live cache entry count changes. */
function updateCacheReportStaleness(currentCache) {
    if (!state.cacheReport) return;
    const reportEntries = state.cacheReport.cache?.["memcache-map-size"];
    const currentEntries = currentCache?.["memcache-map-size"];
    const stale = reportEntries !== undefined && currentEntries !== undefined && Number(reportEntries) !== Number(currentEntries);
    setText("cacheReportStale", stale ? `Cache changed: ${formatInt(reportEntries)} to ${formatInt(currentEntries)} entries` : "Point-in-time snapshot");
}

)STATUS"
R"STATUS(/** Render raw support snapshots behind collapsed details. */
function renderDiagnostics(payload) {
    setPreJson("serviceStats", payload.service || {});
    setPreJson("cacheStats", payload.cache || {});
    setPreJson("completeStats", payload);
}

/** Apply one live status snapshot to every lightweight dashboard panel. */
function renderPayload(payload) {
    state.payload = payload;
    renderHealth(payload);
    renderOverview(payload);
    renderMemory(payload.memory || {});
    renderTraffic(payload);
    renderCache(payload.cache || {}, payload.memory || {});
    renderDiagnostics(payload);
}

/** Fetch one lightweight status snapshot while coalescing overlapping refreshes. */
async function refreshStatus(force = false) {
    if (document.hidden && !force) return;
    if (state.refreshInFlight) {
        if (force) state.pendingForcedRefresh = true;
        return;
    }
    state.refreshInFlight = true;
    byId("refreshNow").disabled = true;
    byId("errorBanner").hidden = true;
    try {
        const response = await fetch(`/status-data?_=${Date.now()}`, {cache: "no-store"});
        if (!response.ok) throw new Error(`Status request failed with HTTP ${response.status}.`);
        const payload = await response.json();
        renderPayload(payload);
        setText("lastUpdated", `Updated ${new Date(payload.timestampMs || Date.now()).toLocaleTimeString()}`);
    } catch (error) {
        const banner = byId("errorBanner");
        banner.textContent = String(error);
        banner.hidden = false;
        const badge = byId("overallBadge");
        badge?.classList.remove("status-waiting", "status-healthy", "status-starting");
        badge?.classList.add("status-degraded");
        setText("overallBadgeText", "Unavailable");
    } finally {
        state.refreshInFlight = false;
        byId("refreshNow").disabled = false;
        if (state.pendingForcedRefresh) {
            state.pendingForcedRefresh = false;
            queueMicrotask(() => refreshStatus(false));
        }
    }
}

/** Request one expensive report only after explicit user action. */
async function generateCacheReport() {
    if (state.cacheReportInFlight) return;
    state.cacheReportInFlight = true;
    const button = byId("generateCacheReport");
    const status = byId("cacheReportStatus");
    button.disabled = true;
    button.textContent = "Generating...";
    status.classList.add("busy");
    status.textContent = "Analyzing cached feature tiles. Live status refresh remains lightweight.";
    try {
        const response = await fetch("/status-data/cache-report", {method: "POST", cache: "no-store"});
        const report = await response.json();
        if (!response.ok) throw new Error(report.error || `Cache report failed with HTTP ${response.status}.`);
        renderCacheReport(report);
        status.textContent = `Report completed in ${formatDuration(report.durationMs)}.`;
    } catch (error) {
        status.textContent = String(error);
    } finally {
        status.classList.remove("busy");
        button.disabled = false;
        button.textContent = state.cacheReport ? "Regenerate report" : "Generate report";
        state.cacheReportInFlight = false;
    }
}

/** Apply the selected polling interval. */
function resetTimer() {
    if (state.timer !== null) clearInterval(state.timer);
    state.timer = null;
    const interval = Number(byId("refreshInterval")?.value || 0);
    if (interval > 0) state.timer = setInterval(() => refreshStatus(false), interval);
}

/** Activate one accessible tab and preserve it in the URL fragment. */
function activateTab(name, updateHash = true) {
    const tabs = Array.from(document.querySelectorAll("[data-tab]"));
    const valid = tabs.some((tab) => tab.dataset.tab === name) ? name : "overview";
    for (const tab of tabs) {
        const active = tab.dataset.tab === valid;
        tab.setAttribute("aria-selected", active ? "true" : "false");
        tab.tabIndex = active ? 0 : -1;
    }
    for (const panel of document.querySelectorAll("[data-panel]")) panel.hidden = panel.dataset.panel !== valid;
    if (updateHash && location.hash !== `#${valid}`) history.replaceState(null, "", `#${valid}`);
}

/** Copy diagnostics with a fallback for non-secure browser contexts. */
async function copyText(text) {
    if (navigator.clipboard?.writeText) {
        try {
            await navigator.clipboard.writeText(text);
            return;
        } catch {
            // Clipboard access can be denied on plain HTTP; retain the local fallback.
        }
    }
    const area = document.createElement("textarea");
    area.value = text;
    document.body.appendChild(area);
    area.select();
    document.execCommand("copy");
    area.remove();
}

/** Download a point-in-time JSON snapshot without server-side persistence. */
function downloadJson(value, prefix) {
    if (!value) return;
    const blob = new Blob([JSON.stringify(value, null, 2)], {type: "application/json"});
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = `${prefix}-${new Date().toISOString().replace(/[:.]/g, "-")}.json`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 0);
}

for (const tab of document.querySelectorAll("[data-tab]")) {
    tab.addEventListener("click", () => activateTab(tab.dataset.tab));
    tab.addEventListener("keydown", (event) => {
        if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
        const tabs = Array.from(document.querySelectorAll("[data-tab]"));
        const current = tabs.indexOf(tab);
        let next = current;
        if (event.key === "ArrowLeft") next = (current - 1 + tabs.length) % tabs.length;
        if (event.key === "ArrowRight") next = (current + 1) % tabs.length;
        if (event.key === "Home") next = 0;
        if (event.key === "End") next = tabs.length - 1;
        event.preventDefault();
        activateTab(tabs[next].dataset.tab);
        tabs[next].focus();
    });
}

byId("host").textContent = window.location.host;
byId("refreshInterval")?.addEventListener("change", resetTimer);
byId("refreshNow")?.addEventListener("click", () => refreshStatus(true));
byId("generateCacheReport")?.addEventListener("click", generateCacheReport);
byId("downloadCacheReport")?.addEventListener("click", () => downloadJson(state.cacheReport, "mapget-cache-report"));
byId("copyStatusPayload")?.addEventListener("click", async () => {
    if (!state.payload) return;
    await copyText(JSON.stringify(state.payload, null, 2));
    setText("copyStatusPayload", "Copied");
    setTimeout(() => setText("copyStatusPayload", "Copy snapshot"), 1200);
});
byId("downloadStatusPayload")?.addEventListener("click", () => downloadJson(state.payload, "mapget-status"));
document.addEventListener("visibilitychange", () => {
    if (!document.hidden) refreshStatus(true);
});
window.addEventListener("hashchange", () => activateTab(location.hash.slice(1), false));

activateTab(location.hash.slice(1) || "overview", false);
resetTimer();
refreshStatus(true);
</script>
</body>
</html>)STATUS";
    return page;
}

}  // namespace mapget::detail
