#include "status-page.h"

namespace mapget::detail
{

std::string_view statusPageHtml()
{
    // Keep each adjacent literal below MSVC's 16,380-byte per-literal limit.
    static constexpr std::string_view page =
        R"STATUS(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>mapget service status</title>
<script>
/** Resolve the persisted or system color theme before the page is painted. */
(() => {
    const systemTheme = window.matchMedia?.("(prefers-color-scheme: dark)").matches ? "dark" : "light";
    try {
        const storedTheme = localStorage.getItem("mapget-status-theme");
        document.documentElement.dataset.theme = ["light", "dark"].includes(storedTheme) ? storedTheme : systemTheme;
    } catch {
        // Private browsing can deny storage; the system preference is still a safe default.
        document.documentElement.dataset.theme = systemTheme;
    }
})();
</script>
<style>
:root,
:root[data-theme="light"] {
    color-scheme: light;
    --canvas: #f2f2ed;
    --surface: #ffffff;
    --surface-alt: #f7f7f3;
    --ink: #090909;
    --muted: #5d5d57;
    --line: #090909;
    --line-soft: #a5a59e;
    --inverse: #090909;
    --inverse-ink: #ffffff;
    --focus: #005fcc;
    --ok: #087534;
    --ok-soft: #e7f5eb;
    --warn: #8a5500;
    --warn-soft: #fff3d2;
    --bad: #a21b1b;
    --bad-soft: #fbe7e7;
    --code: #f7f7f3;
    --code-ink: #090909;
}

:root[data-theme="dark"] {
    color-scheme: dark;
    --canvas: #090909;
    --surface: #111111;
    --surface-alt: #191919;
    --ink: #f5f5ef;
    --muted: #aaa9a1;
    --line: #f5f5ef;
    --line-soft: #5e5e59;
    --inverse: #f5f5ef;
    --inverse-ink: #090909;
    --focus: #72b7ff;
    --ok: #63e391;
    --ok-soft: #0d2c18;
    --warn: #ffd16f;
    --warn-soft: #35280e;
    --bad: #ff8d8d;
    --bad-soft: #361616;
    --code: #050505;
    --code-ink: #f5f5ef;
}

* { box-sizing: border-box; }
[hidden] { display: none !important; }
::selection { color: var(--inverse-ink); background: var(--inverse); }

body {
    margin: 0;
    min-width: 320px;
    color: var(--ink);
    background: var(--canvas);
    font: 14px/1.42 "IBM Plex Mono", "Cascadia Code", "SFMono-Regular", Consolas, "Liberation Mono", monospace;
}

button, select, input { font: inherit; }
button { cursor: pointer; }
button:focus-visible, select:focus-visible, input:focus-visible, summary:focus-visible {
    outline: 2px solid var(--focus);
    outline-offset: 2px;
}

.shell {
    width: min(1600px, calc(100% - 32px));
    margin: 0 auto;
    padding: 18px 0 40px;
}

.masthead {
    display: flex;
    justify-content: space-between;
    gap: 20px;
    align-items: center;
    padding: 16px;
    border: 1px solid var(--line);
    border-top-width: 4px;
    background: var(--surface);
}

.masthead-title { min-width: 0; }
h1, h2, h3 { margin: 0; font: inherit; font-weight: 800; text-transform: uppercase; }
h1 { font-size: clamp(1.15rem, 2.4vw, 1.55rem); letter-spacing: 0.04em; }
h2 { font-size: 1rem; letter-spacing: 0.035em; }
h3 { font-size: 0.9rem; }
.host { margin-top: 3px; color: var(--muted); font-size: 0.78rem; }

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
    font-weight: 650;
}

select, input[type="search"], .button {
    min-height: 36px;
    border: 1px solid var(--line);
    border-radius: 0;
    background: var(--surface);
    color: var(--ink);
    padding: 7px 11px;
}

.button { font-weight: 750; }
.button:hover, select:hover {
    color: var(--inverse-ink);
    background: var(--inverse);
}
.button:disabled, .button:disabled:hover {
    cursor: wait;
    color: var(--muted);
    background: var(--surface-alt);
    opacity: 0.72;
}
.button-primary { font-weight: 900; }
.button-quiet { background: var(--surface); }

.status-badge {
    display: inline-flex;
    align-items: center;
    gap: 7px;
    min-height: 32px;
    border: 1px solid var(--line-soft);
    border-radius: 0;
    padding: 5px 11px;
    background: var(--surface-alt);
    color: var(--muted);
    font-size: 0.8rem;
    font-weight: 800;
}

.status-dot { width: 8px; height: 12px; background: currentColor; }
.status-healthy { color: var(--ok); border-color: currentColor; background: var(--ok-soft); }
.status-starting { color: var(--warn); border-color: currentColor; background: var(--warn-soft); }
.status-degraded { color: var(--bad); border-color: currentColor; background: var(--bad-soft); }
.status-waiting .status-dot { animation: pulse 1.1s steps(2, end) infinite; }

@keyframes pulse { 50% { opacity: 0; } }

.refresh-meta { min-width: 145px; color: var(--muted); font-size: 0.78rem; text-align: right; }
.error-banner {
    margin-top: 12px;
    border: 2px solid var(--bad);
    border-radius: 0;
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
    margin: 18px 0 12px;
    padding: 6px;
    overflow-x: auto;
    border: 1px solid var(--line);
    border-radius: 0;
    background: var(--surface);
}

.tab {
    flex: 0 0 auto;
    border: 1px solid var(--line);
    border-radius: 0;
    padding: 9px 15px;
    color: var(--ink);
    background: var(--surface);
    font-weight: 800;
}
.tab:hover, .tab[aria-selected="true"] { color: var(--inverse-ink); background: var(--inverse); }

.history-graph {
    margin-bottom: 16px;
    padding: 0;
    overflow: hidden;
}
.history-head {
    display: flex;
    justify-content: space-between;
    align-items: flex-end;
    gap: 18px;
    border-bottom: 1px solid var(--line);
    padding: 14px 16px;
}
.history-subtitle { margin-top: 3px; color: var(--muted); font-size: 0.8rem; }
.history-controls { display: flex; flex-wrap: wrap; justify-content: flex-end; gap: 12px; }
.history-stats {
    display: flex;
    flex-wrap: wrap;
    gap: 10px 22px;
    border-bottom: 1px solid var(--line-soft);
    padding: 8px 16px;
    color: var(--muted);
    font-size: 0.76rem;
    text-transform: uppercase;
}
.history-stats strong { color: var(--ink); font-weight: 850; font-variant-numeric: tabular-nums; }
.history-plot-wrap { position: relative; min-height: 250px; background: var(--surface-alt); }
.history-plot { display: block; width: 100%; height: 250px; color: var(--ink); }
.history-grid { stroke: var(--line-soft); stroke-width: 1; stroke-dasharray: 2 4; }
.history-axis { fill: var(--muted); font: 11px "IBM Plex Mono", "Cascadia Code", "SFMono-Regular", Consolas, monospace; }
.history-line { fill: none; stroke: var(--ink); stroke-width: 2; vector-effect: non-scaling-stroke; }
.history-point { fill: var(--surface); stroke: var(--ink); stroke-width: 2; vector-effect: non-scaling-stroke; }
.history-empty {
    position: absolute;
    inset: 0;
    display: grid;
    place-items: center;
    padding: 20px;
    color: var(--muted);
    text-align: center;
    pointer-events: none;
}

.tab-panel { display: grid; gap: 16px; }
#cacheReportResults { display: grid; gap: 14px; }

.section-head {
    display: flex;
    justify-content: space-between;
    align-items: flex-end;
    gap: 16px;
    margin: 0;
    border-bottom: 1px solid var(--line);
    padding-bottom: 8px;
}
.section-head p { margin: 3px 0 0; color: var(--muted); font-size: 0.88rem; }

.metric-grid {
    display: grid;
    grid-template-columns: repeat(6, minmax(150px, 1fr));
    gap: 12px;
    margin-bottom: 0;
}

.metric-card {
    min-height: 108px;
    border: 1px solid var(--line);
    border-radius: 0;
    padding: 15px;
    background: var(--surface);
}
.metric-label { display: block; color: var(--muted); font-size: 0.72rem; font-weight: 800; letter-spacing: 0.08em; text-transform: uppercase; }
.metric-label::before { content: "["; }
.metric-label::after { content: "]"; }
.metric-value { display: block; margin-top: 12px; font-size: 1.3rem; font-weight: 850; font-variant-numeric: tabular-nums; }
.metric-note { display: block; margin-top: 4px; color: var(--muted); font-size: 0.78rem; }

.grid-two { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 14px; align-items: start; }
.stack { display: grid; gap: 14px; }
.panel {
    min-width: 0;
    border: 1px solid var(--line);
    border-radius: 0;
    padding: 16px;
    background: var(--surface);
}
.panel-head { display: flex; justify-content: space-between; align-items: center; gap: 12px; margin-bottom: 12px; border-bottom: 1px solid var(--line); padding-bottom: 9px; }
.panel-subtitle { color: var(--muted); font-size: 0.8rem; }
.table-wrap { width: 100%; overflow-x: auto; border-radius: 0; }

table { width: 100%; border-collapse: collapse; font-size: 0.81rem; }
th, td { border: 1px solid var(--line-soft); padding: 8px 10px; text-align: left; vertical-align: middle; }
th {
    color: var(--inverse-ink);
    background: var(--inverse);
    border-color: var(--line);
    font-size: 0.7rem;
    letter-spacing: 0.055em;
    text-transform: uppercase;
    white-space: nowrap;
}
.number { text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }
#schemaInventory td:first-child { min-width: 10rem; }
#schemaInventory td:nth-child(2), #schemaInventory td:nth-child(3) { min-width: 13rem; }
.schema-choice { display: flex; flex-direction: column; align-items: flex-start; gap: 6px; }
.schema-artifact { display: block; max-width: 22rem; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.schema-versions { margin: 0; padding: 0; list-style: none; }
.schema-versions li { overflow-wrap: anywhere; }
.schema-versions span { color: var(--muted); }
.schema-toolbar { display: flex; flex-wrap: wrap; align-items: center; gap: 10px; margin-bottom: 14px; }
.schema-toolbar input { min-width: 0; flex: 1; }
.schema-workspace { display: grid; grid-template-columns: minmax(220px, 26%) minmax(0, 1fr); gap: 14px; align-items: start; }
.schema-navigation { position: sticky; top: 76px; max-height: min(72vh, calc(100vh - 90px)); overflow: auto; }
.schema-navigation input { width: 100%; margin: 10px 0; }
.schema-navigation h3 { margin: 16px 0 6px; overflow-wrap: anywhere; }
.schema-root { display: flex; justify-content: space-between; align-items: baseline; gap: 8px; width: 100%; text-align: left; margin-top: 5px; overflow-wrap: anywhere; }
.schema-root[aria-current="true"] { color: var(--inverse-ink); background: var(--inverse); }
.schema-root small { white-space: nowrap; font-weight: normal; }
.schema-heading { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; margin-bottom: 10px; }
.schema-heading h2 { overflow-wrap: anywhere; }
.schema-properties { margin: 12px 0; font-size: 0.82rem; }
.schema-properties > div { display: grid; grid-template-columns: minmax(100px, 25%) minmax(0, 1fr); gap: 12px; padding: 7px 0; border-bottom: 1px solid var(--line-soft); }
.schema-properties dt { color: var(--muted); }
.schema-properties dd { margin: 0; overflow-wrap: anywhere; }
.schema-properties .schema-properties { margin: 0; }
.schema-properties dd > ul { margin: 0; padding-left: 18px; }
.schema-properties th, .schema-properties td { overflow-wrap: anywhere; }
.schema-properties .table-wrap { margin: 6px 0; }
.schema-path { color: var(--muted); font-size: 0.78rem; overflow-wrap: anywhere; margin: 6px 0; }
.schema-branch { margin-top: 10px; }
.schema-branch > summary { overflow-wrap: anywhere; }
.schema-kind { font-weight: normal; font-size: 0.8rem; margin-left: 12px; }
.schema-content { padding: 0 0 4px 12px; border-left: 1px solid var(--line-soft); }
.schema-content > details { margin: 12px 0; }
.schema-branch-filter { display: block; margin: 14px 0 8px; }
.schema-branch-filter input { width: 100%; margin-top: 4px; }
.schema-message { margin: 10px 0; color: var(--muted); font-size: 0.82rem; }
.schema-error { color: var(--bad); }
#schemaDetails, #schemaRootTitle { scroll-margin-top: 78px; }
#schemaRootTitle { min-width: 0; overflow-wrap: anywhere; }
#schemaMetadata { margin-bottom: 14px; }
.muted { color: var(--muted); }
.explanation { margin: 12px 0 0; color: var(--muted); font-size: 0.8rem; }

.metric-name { display: inline-flex; align-items: center; gap: 7px; }
.info-bubble {
    display: inline-grid;
    place-items: center;
    width: 17px;
    height: 17px;
    flex: 0 0 auto;
    border: 1px solid var(--line);
    border-radius: 0;
    padding: 0;
    color: var(--ink);
    background: var(--surface);
    font-family: inherit;
    font-size: 0.68rem;
    font-weight: 800;
    line-height: 1;
    cursor: help;
}
.info-bubble:hover, .info-bubble:focus-visible {
    color: var(--inverse-ink);
    background: var(--inverse);
}

.state-pill {
    display: inline-flex;
    border: 1px solid var(--line-soft);
    border-radius: 0;
    padding: 3px 8px;
    color: var(--muted);
    background: var(--surface-alt);
    font-size: 0.72rem;
    font-weight: 800;
    text-transform: capitalize;
}
.state-ready { color: var(--ok); border-color: currentColor; background: var(--ok-soft); }
.state-initializing { color: var(--warn); border-color: currentColor; background: var(--warn-soft); }
.state-failed { color: var(--bad); border-color: currentColor; background: var(--bad-soft); }

.report-callout {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 18px;
    border: 2px solid var(--line);
    border-left-width: 6px;
    border-radius: 0;
    padding: 18px;
    background: var(--surface);
}
.report-callout p { margin: 4px 0 0; color: var(--muted); font-size: 0.86rem; }
.report-actions { display: flex; flex-wrap: wrap; gap: 8px; justify-content: flex-end; }
.report-status { margin-top: 10px; min-height: 22px; color: var(--muted); font-size: 0.82rem; }
.report-status.busy::before {
    content: "[working]";
    display: inline-block;
    margin-right: 7px;
    color: var(--ink);
    animation: pulse 1.1s steps(2, end) infinite;
}
.report-stale { color: var(--warn); font-weight: 750; }
.empty-state { padding: 34px 18px; text-align: center; color: var(--muted); }

details { padding: 0; }
details + details { margin-top: 10px; }
)STATUS"
        R"STATUS(summary {
    display: block;
    border: 1px solid var(--line);
    padding: 8px 10px;
    cursor: pointer;
    color: var(--ink);
    background: var(--surface);
    font-weight: 800;
}
summary::-webkit-details-marker { display: none; }
summary::before { content: "+ "; }
details[open] > summary::before { content: "- "; }
summary:hover, details[open] > summary { color: var(--inverse-ink); background: var(--inverse); }
pre {
    max-height: 64vh;
    margin: 10px 0 0;
    overflow: auto;
    border: 1px solid var(--line);
    border-radius: 0;
    padding: 12px;
    color: var(--code-ink);
    background: var(--code);
    white-space: pre-wrap;
    word-break: break-word;
    font-family: inherit;
    font-size: 0.76rem;
    line-height: 1.48;
}
.raw-actions { display: flex; gap: 8px; margin: 10px 0; }

@media (max-width: 1180px) {
    .metric-grid { grid-template-columns: repeat(3, minmax(150px, 1fr)); }
}
@media (max-width: 780px) {
    .shell { width: min(100% - 20px, 1600px); padding-top: 10px; }
    .masthead { align-items: flex-start; padding: 17px; }
    .masthead, .report-callout { flex-direction: column; }
    .masthead-tools, .report-actions { justify-content: flex-start; }
    .history-head { align-items: flex-start; flex-direction: column; }
    .history-controls { width: 100%; justify-content: flex-start; }
    .refresh-meta { text-align: left; }
    .grid-two { grid-template-columns: 1fr; }
    .schema-workspace { grid-template-columns: 1fr; }
    .schema-toolbar input { flex-basis: 100%; }
    .schema-navigation { position: static; max-height: 260px; }
    .schema-properties > div { grid-template-columns: 1fr; gap: 3px; }
    .schema-content { padding-left: 5px; }
    .metric-grid { grid-template-columns: repeat(2, minmax(135px, 1fr)); }
    .panel { padding: 13px; }
}
@media (max-width: 470px) {
    .metric-grid { grid-template-columns: 1fr; }
    .metric-card { min-height: 98px; }
    .field { width: 100%; justify-content: space-between; }
    .history-controls select { max-width: 68%; }
}

@media (prefers-reduced-motion: reduce) {
    .status-waiting .status-dot, .report-status.busy::before { animation: none; }
}
</style>
)STATUS"
        R"STATUS(</head>
<body>
<div class="shell">
    <header class="masthead">
        <div class="masthead-title">
            <h1>mapget / service status</h1>
            <div id="host" class="host"></div>
        </div>
        <div class="masthead-tools">
            <span id="overallBadge" class="status-badge status-waiting"><span class="status-dot"></span><span id="overallBadgeText">Connecting</span></span>
            <label class="field">Theme
                <select id="themeSelect">
                    <option value="light">Light</option>
                    <option value="dark">Dark</option>
                </select>
            </label>
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
        <button class="tab" role="tab" id="tab-schemas" aria-controls="panel-schemas" aria-selected="false" data-tab="schemas" tabindex="-1">Schemas</button>
        <button class="tab" role="tab" id="tab-diagnostics" aria-controls="panel-diagnostics" aria-selected="false" data-tab="diagnostics" tabindex="-1">Diagnostics</button>
    </nav>

    <section class="panel history-graph" aria-labelledby="historyTitle">
        <div class="history-head">
            <div><h2 id="historyTitle">Live History</h2><div id="historySubtitle" class="history-subtitle">Client-side samples from this browser session</div></div>
            <div class="history-controls">
                <label class="field">Metric <select id="historyMetric"></select></label>
                <label class="field">Window
                    <select id="historyWindow">
                        <option value="30000">30 seconds</option>
                        <option value="60000" selected>1 minute</option>
                        <option value="300000">5 minutes</option>
                        <option value="900000">15 minutes</option>
                    </select>
                </label>
            </div>
        </div>
        <div class="history-stats" aria-live="polite">
            <span>Now <strong id="historyCurrent">-</strong></span>
            <span>Min <strong id="historyMinimum">-</strong></span>
            <span>Max <strong id="historyMaximum">-</strong></span>
            <span>Samples <strong id="historySamples">0</strong></span>
        </div>
        <div class="history-plot-wrap">
            <svg id="historyPlot" class="history-plot" role="img" aria-label="No status history collected yet"></svg>
            <div id="historyEmpty" class="history-empty">Collecting the first status sample...</div>
        </div>
    </section>

    <main>
        <section id="panel-overview" class="tab-panel" role="tabpanel" aria-labelledby="tab-overview" data-panel="overview">
            <div class="metric-grid">
                <article class="metric-card"><span class="metric-label">Datasources</span><strong id="metricDatasources" class="metric-value">-</strong><span id="metricDatasourcesNote" class="metric-note">Waiting</span></article>
                <article class="metric-card"><span class="metric-label">Process RSS</span><strong id="metricRss" class="metric-value">-</strong><span id="metricRssNote" class="metric-note">Current resident memory</span></article>
                <article class="metric-card"><span class="metric-label">Cache</span><strong id="metricCache" class="metric-value">-</strong><span id="metricCacheNote" class="metric-note">Retained tile data</span></article>
                <article class="metric-card"><span class="metric-label">Active work</span><strong id="metricWork" class="metric-value">-</strong><span id="metricWorkNote" class="metric-note">Workers processing source tiles</span></article>
                <article class="metric-card"><span class="metric-label">Interactive</span><strong id="metricInteractive" class="metric-value">-</strong><span id="metricInteractiveNote" class="metric-note">Connected sessions</span></article>
                <article class="metric-card"><span class="metric-label">Allocator live</span><strong id="metricAllocator" class="metric-value">-</strong><span id="metricAllocatorNote" class="metric-note">Live heap allocations</span></article>
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
)STATUS"
        R"STATUS(                <section class="panel"><div class="panel-head"><h2>Array Arena Singleton Usage</h2></div><div class="table-wrap"><table id="arrayArenaSingletonsTable"><thead><tr><th>Arena</th><th class="number">Handles</th><th class="number">Occupied</th><th class="number">Empty</th><th class="number">Singleton</th><th class="number">Regular equivalent</th><th class="number">Saved</th><th class="number">Saved share</th></tr></thead><tbody></tbody></table></div></section>
                <div class="grid-two">
                    <section class="panel"><div class="panel-head"><h2>Tile Size Summary</h2></div><div class="table-wrap"><table id="tileSizeSummary"><thead><tr><th>Metric</th><th class="number">Value</th></tr></thead><tbody></tbody></table></div></section>
                    <section class="panel"><div class="panel-head"><h2>Tile Size Distribution</h2></div><div class="table-wrap"><table id="tileSizeHistogram"><thead><tr><th>Bin</th><th class="number">Count</th><th class="number">Share</th></tr></thead><tbody></tbody></table></div></section>
                </div>
                <section class="panel"><details><summary>Raw cache report</summary><pre id="cacheReportRaw"></pre></details></section>
            </div>
        </section>

)STATUS"
        R"STATUS(        <section id="panel-schemas" class="tab-panel" role="tabpanel" aria-labelledby="tab-schemas" data-panel="schemas" hidden>
            <div id="schemaStatus" class="schema-message" role="status" aria-live="polite">Open this tab to load the inventory.</div>
            <div id="schemaOverview">
                <div class="section-head"><div><h2>Loaded Schemas</h2><p>Shared native contracts. Explore an artifact without loading map partitions.</p></div></div>
                <section class="panel">
                    <label class="schema-toolbar">Filter inventory <input id="schemaInventoryFilter" type="search" placeholder="Format, subsystem, map or version"></label>
                    <div class="table-wrap"><table id="schemaInventory"><thead><tr><th>Format / version</th><th>Subsystem versions</th><th>Consumers</th><th>Artifact</th><th class="number">Domains</th><th class="number">Names</th><th class="number">Retained capacity</th></tr></thead><tbody></tbody></table></div>
                    <p class="panel-subtitle">Retained capacity is not RSS or serialized size. Current shared graphs are counted once; retired pinned graphs are accounted in Memory. Temporary codec allocations remain unmeasured.</p>
                </section>
            </div>
            <div id="schemaDetails" hidden>
                <div class="schema-heading"><button id="schemaBack" class="button" type="button">Back to schemas</button><h2 id="schemaTitle" tabindex="-1">Schema</h2></div>
                <details id="schemaMetadata"><summary>Artifact, versions, consumers and memory</summary><div id="schemaSummary" class="panel"></div></details>
                <div class="schema-workspace">
                    <nav class="panel schema-navigation" aria-label="Schema layers and types">
                        <h3>Layers &amp; types</h3>
                        <label class="schema-branch-filter">Filter layers or feature types<input id="schemaRootFilter" type="search" placeholder="e.g. Road or SourceData"></label>
                        <div id="schemaRoots"></div>
                    </nav>
                    <section class="panel" aria-labelledby="schemaRootTitle">
                        <div class="panel-head"><h3 id="schemaRootTitle" tabindex="-1">Select a root</h3></div>
                        <form id="schemaSearch" class="schema-toolbar" role="search" aria-label="Search selected schema root">
                            <label for="schemaFind">Find in root</label><input id="schemaFind" type="search" maxlength="128" placeholder="Field, type or enum name" disabled>
                            <button id="schemaFindSubmit" class="button" type="submit" disabled>Find</button><button id="schemaFindClear" class="button" type="button" hidden>Show root</button>
                        </form>
                        <p id="schemaSearchStatus" class="schema-message" role="status" aria-live="polite">Descendant search shares the MCP schema finder. Context-only links are not searched.</p>
                        <div id="schemaTree" aria-label="Schema domains"></div>
                    </section>
                </div>
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
    historyGraph: null,
    schemaExplorer: null,
    textCache: new Map(),
};

/** Apply one explicit color theme and optionally retain the user's choice. */
function applyTheme(theme, persist = false) {
    const selectedTheme = theme === "dark" ? "dark" : "light";
    document.documentElement.dataset.theme = selectedTheme;
    const select = byId("themeSelect");
    if (select) select.value = selectedTheme;
    if (!persist) return;
    try {
        localStorage.setItem("mapget-status-theme", selectedTheme);
    } catch {
        // Theme selection remains effective even when browser storage is unavailable.
    }
}

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

)STATUS"
        R"STATUS(/** Client-owned rolling history for lightweight numeric status metrics. */
class StatusHistoryGraph {
    constructor() {
        this.history = [];
        this.activeTab = "overview";
        this.selectedMetricByTab = new Map();
        this.defaults = {
            overview: "queued-tile-work-items",
            memory: "process-rss",
            traffic: "interactive-queued-frames",
            cache: "cache-bytes",
            diagnostics: "queued-tile-work-items",
        };
        this.metrics = new Map([
            ["queued-tile-work-items", {
                group: "Work",
                label: "Queued tile/filter work",
                format: formatInt,
                zeroBaseline: true,
                read: (payload) => payload.service?.["queued-tile-work-items"],
            }],
            ["in-flight-tile-jobs", {
                group: "Work",
                label: "In-flight source tiles",
                format: formatInt,
                zeroBaseline: true,
                read: (payload) => payload.service?.["in-flight-tile-jobs"],
            }],
            ["running-workers", {
                group: "Work",
                label: "Running workers",
                format: formatInt,
                zeroBaseline: true,
                read: (payload) => payload.service?.workers?.running,
            }],
            ["active-requests", {
                group: "Work",
                label: "Active tile/filter requests",
                format: formatInt,
                zeroBaseline: true,
                read: (payload) => payload.service?.["active-requests"],
            }],
            ["process-rss", {
                group: "Memory",
                label: "Process RSS",
                format: formatBytes,
                zeroBaseline: false,
                read: (payload) => payload.memory?.process?.["resident-bytes"],
            }],
            ["allocator-live", {
                group: "Memory",
                label: "Allocator live allocations",
                format: formatBytes,
                zeroBaseline: false,
                read: (payload) => payload.memory?.allocator?.["allocated-bytes"],
            }],
            ["allocator-resident", {
                group: "Memory",
                label: "jemalloc resident estimate",
                format: formatBytes,
                zeroBaseline: false,
                read: (payload) => payload.memory?.allocator?.["resident-bytes"],
            }],
            ["allocator-active", {
                group: "Memory",
                label: "jemalloc active pages",
                format: formatBytes,
                zeroBaseline: false,
                read: (payload) => payload.memory?.allocator?.["active-bytes"],
            }],
            ["allocator-free", {
                group: "Memory",
                label: "glibc arena free",
                format: formatBytes,
                zeroBaseline: false,
                read: (payload) => payload.memory?.allocator?.["free-arena-bytes"],
            }],
            ["known-memory", {
                group: "Memory",
                label: "Known ownership estimate",
                format: formatBytes,
                zeroBaseline: false,
                read: (payload) => payload.memory?.["known-current-bytes"],
            }],
            ["datasource-memory", {
                group: "Memory",
                label: "Datasource-owned state",
                format: formatBytes,
                zeroBaseline: false,
                read: (payload) => payload.memory?.["datasource-measured-bytes"],
            }],
            ["cache-bytes", {
                group: "Memory",
                label: "Cache retained bytes",
                format: formatBytes,
                zeroBaseline: false,
                read: (payload) => payload.memory?.["cache-current-bytes"],
            }],
            ["interactive-sessions", {
                group: "Transport",
                label: "Interactive sessions",
                format: formatInt,
                zeroBaseline: true,
                read: (payload) => payload.tilesWebsocket?.["active-sessions"],
            }],
            ["interactive-queued-frames", {
                group: "Transport",
                label: "Interactive queued frames",
                format: formatInt,
                zeroBaseline: true,
                read: (payload) => payload.tilesWebsocket?.["pending-controller-frames"],
            }],
            ["interactive-queued-bytes", {
                group: "Transport",
                label: "Interactive queued bytes",
                format: formatBytes,
                zeroBaseline: true,
                read: (payload) => payload.tilesWebsocket?.["pending-controller-bytes"],
            }],
            ["rest-streams", {
                group: "Transport",
                label: "Active REST streams",
                format: formatInt,
                zeroBaseline: true,
                read: (payload) => payload.tilesHttp?.["active-streams"],
            }],
            ["rest-pending-bytes", {
                group: "Transport",
                label: "REST pending bytes",
                format: formatBytes,
                zeroBaseline: true,
                read: (payload) => payload.tilesHttp?.["pending-bytes"],
            }],
            ["cache-entries", {
                group: "Cache",
                label: "Cache entries",
                format: formatInt,
                zeroBaseline: true,
                read: (payload) => payload.cache?.["memcache-map-size"],
            }],
            ["cache-hit-ratio", {
                group: "Cache",
                label: "Cache hit ratio",
                format: formatPct,
                zeroBaseline: true,
                read: (payload) => {
                    const hits = Number(payload.cache?.["cache-hits"] || 0);
                    const misses = Number(payload.cache?.["cache-misses"] || 0);
                    return hits + misses > 0 ? hits / (hits + misses) : 0;
                },
            }],
        ]);
        this.metricSelect = byId("historyMetric");
        this.windowSelect = byId("historyWindow");
        this.svg = byId("historyPlot");
        this.populateMetricSelect();
        this.metricSelect?.addEventListener("change", () => {
            this.selectedMetricByTab.set(this.activeTab, this.metricSelect.value);
            this.render();
        });
        this.windowSelect?.addEventListener("change", () => this.render());
        if (window.ResizeObserver && this.svg) {
            this.resizeObserver = new ResizeObserver(() => this.render());
            this.resizeObserver.observe(this.svg);
        } else {
            window.addEventListener("resize", () => this.render());
        }
        this.activateTab("overview");
    }

    /** Populate grouped metric choices from the graph's numeric extractors. */
    populateMetricSelect() {
        if (!this.metricSelect) return;
        const groups = new Map();
        for (const [id, metric] of this.metrics) {
            let group = groups.get(metric.group);
            if (!group) {
                group = document.createElement("optgroup");
                group.label = metric.group;
                groups.set(metric.group, group);
                this.metricSelect.appendChild(group);
            }
            const option = document.createElement("option");
            option.value = id;
            option.textContent = metric.label;
            group.appendChild(option);
        }
    }

    /** Select a contextual default while retaining manual choices per tab. */
    activateTab(tab) {
        this.activeTab = tab;
        if (!this.metricSelect) return;
        this.metricSelect.value = this.selectedMetricByTab.get(tab) || this.defaults[tab] || this.defaults.overview;
        this.render();
    }

)STATUS"
        R"STATUS(    /** Sample numeric values only; full status payloads are never retained. */
    append(payload) {
        const previousTimestamp = this.history.at(-1)?.timestamp || 0;
        const reportedTimestamp = Number(payload.timestampMs);
        // Preserve a monotonic x-axis across duplicate timestamps or a server restart.
        const timestamp = Math.max(Number.isFinite(reportedTimestamp) ? reportedTimestamp : Date.now(), previousTimestamp + 1);
        const values = {};
        for (const [id, metric] of this.metrics) {
            const value = Number(metric.read(payload));
            values[id] = Number.isFinite(value) ? value : 0;
        }
        this.history.push({timestamp, values});
        const retention = Math.max(...Array.from(this.windowSelect?.options || [], (option) => Number(option.value || 0)));
        const cutoff = timestamp - retention;
        while (this.history.length && this.history[0].timestamp < cutoff) this.history.shift();
        this.render();
    }

    /** Create one namespaced SVG node without injecting status text as markup. */
    svgNode(name, attributes = {}, text = "") {
        const node = document.createElementNS("http://www.w3.org/2000/svg", name);
        for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
        if (text) node.textContent = text;
        return node;
    }

    /** Format a graph-window duration for the chart subtitle. */
    formatWindow(milliseconds) {
        if (milliseconds < 60000) return `${milliseconds / 1000} seconds`;
        return `${milliseconds / 60000} ${milliseconds === 60000 ? "minute" : "minutes"}`;
    }

    /** Render the active metric as a responsive SVG over the selected time domain. */
    render() {
        if (!this.svg || !this.metricSelect || !this.windowSelect) return;
        const metric = this.metrics.get(this.metricSelect.value);
        if (!metric) return;
        const windowMilliseconds = Number(this.windowSelect.value || 60000);
        const latestTimestamp = this.history.at(-1)?.timestamp || Date.now();
        const firstTimestamp = latestTimestamp - windowMilliseconds;
        const samples = this.history
            .filter((sample) => sample.timestamp >= firstTimestamp)
            .map((sample) => ({timestamp: sample.timestamp, value: sample.values[this.metricSelect.value]}));
        const values = samples.map((sample) => sample.value);
        setText("historySubtitle", `${metric.label}, ${this.formatWindow(windowMilliseconds)} rolling window`);
        setText("historyCurrent", values.length ? metric.format(values.at(-1)) : "-");
        setText("historyMinimum", values.length ? metric.format(Math.min(...values)) : "-");
        setText("historyMaximum", values.length ? metric.format(Math.max(...values)) : "-");
        setText("historySamples", formatInt(values.length));
        byId("historyEmpty").hidden = samples.length > 0;
        this.svg.setAttribute("aria-label", values.length
            ? `${metric.label}: current ${metric.format(values.at(-1))}, minimum ${metric.format(Math.min(...values))}, maximum ${metric.format(Math.max(...values))}`
            : `${metric.label}: no samples collected yet`);

        const width = Math.max(320, this.svg.clientWidth || 0);
        const height = Math.max(200, this.svg.clientHeight || 0);
        const bounds = {left: width < 520 ? 72 : 92, right: 16, top: 14, bottom: 30};
        const plotWidth = Math.max(1, width - bounds.left - bounds.right);
        const plotHeight = Math.max(1, height - bounds.top - bounds.bottom);
        this.svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
        this.svg.replaceChildren();
        this.svg.appendChild(this.svgNode("title", {}, `${metric.label} history`));

        let minimum = values.length ? Math.min(...values) : 0;
        let maximum = values.length ? Math.max(...values) : 1;
        if (metric.zeroBaseline) minimum = 0;
        if (metric.zeroBaseline && metric.format === formatInt) {
            // Three equal integer intervals keep count axes free of fractional jobs or sessions.
            maximum = Math.max(3, Math.ceil(maximum / 3) * 3);
        } else if (minimum === maximum) {
            const padding = Math.abs(maximum) * 0.05 || 1;
            if (!metric.zeroBaseline) minimum = Math.max(0, minimum - padding);
            maximum += padding;
        } else if (!metric.zeroBaseline) {
            const padding = (maximum - minimum) * 0.08;
            minimum = Math.max(0, minimum - padding);
            maximum += padding;
        }
        const valueRange = Math.max(Number.EPSILON, maximum - minimum);
        const x = (timestamp) => bounds.left + ((timestamp - firstTimestamp) / windowMilliseconds) * plotWidth;
        const y = (value) => bounds.top + (1 - (value - minimum) / valueRange) * plotHeight;

        for (let index = 0; index <= 3; index++) {
            const ratio = index / 3;
            const gridY = bounds.top + ratio * plotHeight;
            const value = maximum - ratio * valueRange;
            this.svg.appendChild(this.svgNode("line", {x1: bounds.left, y1: gridY, x2: width - bounds.right, y2: gridY, class: "history-grid"}));
            this.svg.appendChild(this.svgNode("text", {x: bounds.left - 8, y: gridY + 4, class: "history-axis", "text-anchor": "end"}, metric.format(value)));
        }
        for (let index = 0; index <= 2; index++) {
            const ratio = index / 2;
            const gridX = bounds.left + ratio * plotWidth;
            const timestamp = firstTimestamp + ratio * windowMilliseconds;
            this.svg.appendChild(this.svgNode("line", {x1: gridX, y1: bounds.top, x2: gridX, y2: height - bounds.bottom, class: "history-grid"}));
            this.svg.appendChild(this.svgNode("text", {x: gridX, y: height - 9, class: "history-axis", "text-anchor": index === 0 ? "start" : (index === 2 ? "end" : "middle")}, new Date(timestamp).toLocaleTimeString([], {hour: "2-digit", minute: "2-digit", second: "2-digit"})));
        }
        if (samples.length > 1) {
            const path = samples.map((sample, index) => `${index ? "L" : "M"} ${x(sample.timestamp)} ${y(sample.value)}`).join(" ");
            this.svg.appendChild(this.svgNode("path", {d: path, class: "history-line"}));
        }
        if (samples.length) {
            const latest = samples.at(-1);
            this.svg.appendChild(this.svgNode("rect", {x: x(latest.timestamp) - 3, y: y(latest.value) - 3, width: 6, height: 6, class: "history-point"}));
        }
    }
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
    const runningWorkers = Number(service.workers?.running || 0);
    const queuedWork = Number(service["queued-tile-work-items"] || 0);
    const inFlightTiles = Number(service["in-flight-tile-jobs"] || 0);
    setMetric("metricWork", "metricWorkNote", formatInt(runningWorkers), `${formatInt(queuedWork)} tile/filter work items queued`);

    const sessions = Number(interactive["active-sessions"] || 0);
    setMetric("metricInteractive", "metricInteractiveNote", formatInt(sessions), `${formatInt(interactive["active-connections"] || 0)} connections`);

    const allocator = memory.allocator || {};
    setMetric("metricAllocator", "metricAllocatorNote", allocator["allocated-bytes"] === undefined ? "Unavailable" : formatBytes(allocator["allocated-bytes"]), allocator.backend || "Unsupported allocator");

    renderDatasourceTable("#overviewDatasourceTable tbody", datasources, false);
    replaceRows("#currentWorkTable tbody", [
        ["Workers running", formatInt(runningWorkers)],
        ["Tile requests", formatInt(activeRequests)],
        ["Queued tile/filter work", formatInt(queuedWork)],
        ["In-flight source tiles", formatInt(inFlightTiles)],
        ["REST streams", formatInt(rest["active-streams"] || 0)],
        ["Interactive sessions", formatInt(sessions)],
    ], [1]);
    replaceRows("#pressureTable tbody", [
        ["Interactive queue allocation", formatBytes(interactive["pending-controller-allocated-bytes"] || 0)],
        ["REST pending allocation", formatBytes(rest["pending-capacity-bytes"] || 0)],
        allocator.backend === "jemalloc"
            ? ["jemalloc resident estimate", formatBytes(allocator["resident-bytes"] || 0)]
            : ["glibc arena free", allocator["free-arena-bytes"] === undefined ? "Unavailable" : formatBytes(allocator["free-arena-bytes"])],
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
        const schemas = transport["schema-artifacts"] || {};
        const interactive = transport.interactive || {};
        appendMemoryRow(body, "Process RSS", "Physical pages currently resident for this process. RSS includes anonymous and file-backed pages and is not the same as allocated heap capacity.", process["resident-bytes"] || 0, process["resident-peak-bytes"] ?? null);
        if (process["resident-anonymous-bytes"] !== undefined) appendMemoryRow(body, "Anonymous resident pages", "Resident anonymous mappings, primarily allocator arenas, thread stacks, and opaque anonymous mappings.", process["resident-anonymous-bytes"]);
        if (process["resident-file-bytes"] !== undefined || process["resident-shared-bytes"] !== undefined) appendMemoryRow(body, "File and shared resident pages", "Resident executable, shared-library, file-mapping, and shared-memory pages. These pages are outside normal heap ownership estimates.", reconciliation["file-and-shared-resident-bytes"] || 0);
        if (cgroup["current-bytes"] !== null && cgroup["current-bytes"] !== undefined) appendMemoryRow(body, "Cgroup charged memory", "Memory charged to the containing cgroup. This can include charges not represented by the process RSS figure.", cgroup["current-bytes"], cgroup["peak-bytes"] ?? null);
        if (cgroup["limit-bytes"] !== null && cgroup["limit-bytes"] !== undefined) appendMemoryRow(body, "Cgroup memory limit", "Hard memory limit configured for the containing cgroup. A missing value means no finite cgroup limit was detected.", cgroup["limit-bytes"]);
        if (allocator["allocated-bytes"] !== undefined) appendMemoryRow(body, "Allocator live allocations", "Application allocations measured by the active allocator: jemalloc stats.allocated, or glibc in-use arena plus mmap allocations. Not an RSS subtotal; thread caches can delay accounting updates.", allocator["allocated-bytes"]);
        if (allocator["free-arena-bytes"] !== undefined) appendMemoryRow(body, "glibc arena free", "Arena capacity available for reuse by glibc. This is not all resident and cannot necessarily be returned to the operating system because of fragmentation.", allocator["free-arena-bytes"]);
        if (allocator["active-bytes"] !== undefined) appendMemoryRow(body, "jemalloc active pages", "Pages backing live allocations, including space lost to page-level fragmentation. Excludes unused dirty pages and allocator metadata. Overlaps allocator live allocations; do not add the rows.", allocator["active-bytes"]);
        if (allocator["resident-bytes"] !== undefined) appendMemoryRow(body, "jemalloc resident estimate", "Upper estimate for allocator-resident pages: active pages, allocator metadata and unused dirty pages. Not measured process RSS; it can include untouched pages. Background purging reduces unused dirty pages, not live allocations.", allocator["resident-bytes"]);
        if (allocator["metadata-bytes"] !== undefined) appendMemoryRow(body, "jemalloc metadata", "Memory used by jemalloc's internal bookkeeping. Already included in its resident estimate.", allocator["metadata-bytes"]);
        if (allocator["mapped-bytes"] !== undefined) appendMemoryRow(body, "jemalloc mapped extents", "Virtual mappings in active allocator extents. Not resident memory and not additive with the other allocator rows.", allocator["mapped-bytes"]);
        if (allocator["retained-bytes"] !== undefined) appendMemoryRow(body, "jemalloc retained address space", "Virtual address space reserved for reuse, usually decommitted or purged. This is not retained physical memory or a memory leak, and is excluded from mapped extents.", allocator["retained-bytes"]);
        appendMemoryRow(body, "Known ownership estimate", "Sum of mapget, datasource, cache, and transport capacity estimates. It is an allocation estimate, not an RSS subtotal.", memory["known-current-bytes"] || 0);
        appendMemoryRow(body, "Mapget metadata, scheduler, filters", "Capacity-oriented lower bound for metadata snapshots, scheduler containers, active filter models, and mapget bookkeeping.", mapget["allocated-bytes"] || 0);
        appendMemoryRow(body, "Datasource-owned state", "Datasource-provided cooperative estimates of retained state. Estimator coverage and exactness depend on each datasource implementation.", memory["datasource-measured-bytes"] || 0);
        appendMemoryRow(body, "Cache", "Allocated serialized tile blobs, indexes, and retained datasource string-pool state owned by the selected cache implementation.", memory["cache-current-bytes"] || 0);
        appendMemoryRow(body, "REST pending stream buffers", "Capacity retained by active stateless streaming responses while clients consume their payloads.", rest["pending-capacity-bytes"] || 0, rest["peak-pending-capacity-bytes"] ?? null);
        appendMemoryRow(body, "Schema artifact buffers", "Encoded schema capacity retained by the one-entry HTTP cache or active schema responses, counted once even when several responses share a buffer. Evicted buffers remain counted until their last response finishes. Excludes decoded schemas, already counted under mapget metadata.", schemas["allocated-bytes"] || 0);
        appendMemoryRow(body, "Interactive pending frame buffers", "Capacity retained by interactive sessions for binary frames waiting to be pulled by clients.", interactive["pending-controller-allocated-bytes"] || 0, interactive["peak-pending-controller-allocated-bytes"] ?? null);
        if (reconciliation["allocator-live-outside-known-ownership-bytes"] !== undefined) appendMemoryRow(body, "Allocator live outside known ownership", "Diagnostic difference between allocator-live bytes and instrumented ownership. It can include uninstrumented heap, allocator bookkeeping, and estimator mismatch.", reconciliation["allocator-live-outside-known-ownership-bytes"]);
        if (reconciliation["anonymous-resident-outside-allocator-live-bytes"] !== undefined) appendMemoryRow(body, "Anonymous residency outside allocator live", "Diagnostic difference between anonymous RSS and allocator-live bytes. Common causes are resident allocator slack or fragmentation, thread stacks, and opaque anonymous mappings.", reconciliation["anonymous-resident-outside-allocator-live-bytes"]);
    }

    const trim = memory["allocator-trim"] || {};
    const allocator = memory.allocator || {};
    replaceRows("#allocatorTrimTable tbody", allocator.backend === "jemalloc" ? [
        ["Allocator", `jemalloc ${allocator.version || ""}`],
        ["Background purging", allocator["background-thread-enabled"] ? "Enabled" : "Disabled"],
        ["Background threads", formatInt(allocator["background-thread-count"] || 0)],
        ["Background runs", formatInt(allocator["background-thread-runs"] || 0)],
        ["Default dirty page decay", `${formatInt(allocator["dirty-decay-ms"])} ms`],
        ["Default muzzy page decay", `${formatInt(allocator["muzzy-decay-ms"])} ms`],
        ["glibc trim worker", "Not used with jemalloc"],
    ] : [
        ["Allocator", allocator.backend || "Unavailable"],
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
    ["Reconciled snapshots", "reconciled-snapshots", formatInt],
    ["Superseded snapshots", "superseded-snapshots", formatInt],
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

)STATUS"
        R"STATUS(    const singletonRows = Object.entries(tree["array-arena-singletons"] || {}).map(([name, statsRaw]) => {
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
        R"STATUS(/** Focused schema navigation; only explicitly opened domains or submitted searches do graph work. */
class SchemaExplorer {
    constructor() {
        this.refreshing = false;
        this.selection = null;
        this.root = null;
        this.generation = 0;
        this.inventory = [];
        this.inventoryKey = "";
        this.controllers = new Set();
        this.domains = new Map();
        byId("schemaInventoryFilter").addEventListener("input", () => this.renderInventory());
        byId("schemaRootFilter").addEventListener("input", () => this.renderRoots());
        byId("schemaBack").addEventListener("click", () => { location.hash = "schemas"; });
        byId("schemaSearch").addEventListener("submit", event => {
            event.preventDefault();
            this.search();
        });
        byId("schemaFindClear").addEventListener("click", () => this.showRoot());
    }

    /** Create text-only UI: schema names/descriptions are untrusted, never HTML. */
    element(tag, text = "", className = "") {
        const element = document.createElement(tag);
        element.textContent = text;
        if (className) element.className = className;
        return element;
    }

    /** Match inventory/navigation terms without walking any schema domains. */
    matches(text, query) {
        const haystack = text.toLocaleLowerCase();
        return query.toLocaleLowerCase().trim().split(/\s+/).every(term => haystack.includes(term));
    }

    /** Render paired module versions without collapsing repeated module names. */
    versions(format) {
        const list = this.element("ul", "", "schema-versions");
        for (const module of format.subsystems || []) {
            const row = this.element("li");
            row.append(this.element("span", `${module.name} `), document.createTextNode(module.version));
            list.appendChild(row);
        }
        if (!list.childElementCount) list.appendChild(this.element("li", "None declared", "muted"));
        return list;
    }

    /** Poll authorized inventory, retaining expanded nodes unless their artifact disappears. */
    async refresh() {
        if (this.refreshing || byId("panel-schemas").hidden) return;
        this.refreshing = true;
        try {
            const response = await fetch("schemas", {cache: "no-store", credentials: "same-origin"});
            if (!response.ok) throw new Error(`Schema inventory: HTTP ${response.status}`);
            const payload = await response.json();
            const key = JSON.stringify(payload.schemas);
            if (key !== this.inventoryKey) {
                this.inventory = payload.schemas;
                this.inventoryKey = key;
                this.renderInventory();
                const selected = this.inventory.find(item => item.mapSchemaId === this.selection);
                if (selected) this.renderMetadata(selected);
                else if (this.selection) this.resetSelection();
            }
            setText("schemaStatus", `${this.inventory.length} authorized artifact(s). No map partitions are loaded.`);
            this.navigate();
        } catch (error) {
            setText("schemaStatus", String(error));
        } finally {
            this.refreshing = false;
        }
    }

    /** Keep Explore and subsystem versions ahead of diagnostic identity/capacity columns. */
    renderInventory() {
        const body = qs("#schemaInventory tbody");
        body.replaceChildren();
        for (const item of this.inventory) {
            const format = item.mapFormat;
            const consumers = item.consumers.map(c => `${c.mapId} (${c.sourceId})`).join(", ");
            const versions = (format.subsystems || []).map(s => `${s.name} ${s.version}`).join(" ");
            if (!this.matches(`${format.name} ${format.version} ${versions} ${consumers} ${item.mapSchemaId}`,
                byId("schemaInventoryFilter").value)) continue;
            const row = this.element("tr");
            const cell = this.element("td");
            const choice = this.element("div", "", "schema-choice");
            const button = this.element("button", "Explore", "button");
            button.type = "button";
            button.addEventListener("click", () => {
                location.hash = `schemas/${encodeURIComponent(item.mapSchemaId)}`;
            });
            choice.append(this.element("span", `${format.name || "Legacy"} ${format.version || ""}`), button);
            cell.appendChild(choice);
            const modules = this.element("td");
            modules.appendChild(this.versions(format));
            const maps = this.element("td", consumers);
            maps.style.overflowWrap = "anywhere";
            const artifactCell = this.element("td");
            const artifact = this.element("code", item.mapSchemaId, "schema-artifact");
            artifact.title = item.mapSchemaId;
            artifactCell.appendChild(artifact);
            row.append(cell, modules, maps, artifactCell);
            const counts = item.statistics.counts;
            for (const text of [formatInt(counts.domains), formatInt(counts.internedNames), formatBytes(item.retainedBytes)])
                row.appendChild(this.element("td", text, "number"));
            body.appendChild(row);
        }
        if (!body.childElementCount) {
            const row = this.element("tr");
            const cell = this.element("td", "No schemas match this inventory filter.", "empty-state");
            cell.colSpan = 7;
            row.appendChild(cell);
            body.appendChild(row);
        }
    }

    /** Stop old searches/expansions; a late completion must never populate a new root. */
    cancelPending() {
        ++this.generation;
        for (const controller of this.controllers) controller.abort();
        this.controllers.clear();
        this.domains.clear();
        byId("schemaFindSubmit").disabled = !this.root;
    }

    /** Discard artifact-local IDs and their UI when returning to inventory or losing access. */
    resetSelection() {
        this.root = null;
        this.cancelPending();
        this.selection = null;
        byId("schemaTree").replaceChildren();
        byId("schemaRoots").replaceChildren();
        byId("schemaSummary").replaceChildren();
        byId("schemaDetails").hidden = true;
        byId("schemaOverview").hidden = false;
        qs(".history-graph").hidden = false;
    }

    /** Restore a bookmark/browser-history entry only through the current authorized inventory. */
    navigate() {
        if (byId("panel-schemas").hidden) return;
        let route;
        try { route = location.hash.slice(1).split("/").map(decodeURIComponent); }
        catch { route = ["schemas"]; }
        if (route[0] !== "schemas" || !route[1]) {
            const selected = this.selection;
            this.resetSelection();
            if (selected) byId("schemaInventoryFilter").focus({preventScroll: true});
            return;
        }
        if (!this.inventoryKey) {
            setText("schemaStatus", "Loading the authorized schema inventory...");
            return;
        }
        const item = this.inventory.find(candidate => candidate.mapSchemaId === route[1]);
        if (!item) {
            this.resetSelection();
            setText("schemaStatus", "This artifact is no longer available. Choose a schema from the current inventory.");
            return;
        }
        if (this.selection !== item.mapSchemaId) this.select(item);
        if (route[2] && route[3]) {
            const root = this.roots().find(r => r.layerId === route[2] && String(r.schemaId) === route[3]);
            if (root && (this.root?.layerId !== root.layerId || this.root?.schemaId !== root.schemaId)) {
                this.selectRoot(root);
                byId("schemaRootTitle").scrollIntoView({block: "start"});
                byId("schemaRootTitle").focus({preventScroll: true});
            }
        }
    }

    /** Replace the overview, rather than appending a tree below a potentially huge table. */
    select(item) {
        this.resetSelection();
        this.selection = item.mapSchemaId;
        byId("schemaOverview").hidden = true;
        byId("schemaDetails").hidden = false;
        // Keep the focused schema page under the tabs, not below the monitoring graph.
        qs(".history-graph").hidden = true;
        byId("schemaMetadata").open = false;
        byId("schemaRootFilter").value = "";
        setText("schemaTitle", `${item.mapFormat.name || "Legacy"} ${item.mapFormat.version || ""}`);
        this.renderMetadata(item);
        const roots = this.roots();
        this.selectRoot(roots[0] || null);
        byId("schemaDetails").scrollIntoView({block: "start"});
        byId("schemaTitle").focus({preventScroll: true});
    }

    /** Keep the navigation inventory cheap: roots come from /schemas, not descriptor requests. */
    roots() {
        const item = this.inventory.find(candidate => candidate.mapSchemaId === this.selection);
        // Feature roots are the useful entry point; large SourceData inventories remain searchable.
        const layers = [...(item?.layers || [])].sort((a, b) => Number(b.type === "Features") - Number(a.type === "Features"));
        return layers.flatMap(layer => {
            const roots = layer.featureTypes.length ? layer.featureTypes : [{name: "Root", schemaId: layer.schemaId}];
            return roots.filter(root => root.schemaId).map(root => ({...root, layerId: layer.layerId, type: layer.type}));
        });
    }

    /** Filter layer/type names locally; preserve the selected root when its menu row is filtered. */
    renderRoots() {
        const list = byId("schemaRoots");
        list.replaceChildren();
        let layer;
        for (const root of this.roots()) {
            if (!this.matches(`${root.layerId} ${root.name} ${root.type}`, byId("schemaRootFilter").value)) continue;
            if (root.layerId !== layer) {
                list.appendChild(this.element("h3", root.layerId));
                layer = root.layerId;
            }
            const button = this.element("button", "", "button schema-root");
            button.type = "button";
            button.setAttribute("aria-current", String(this.root?.layerId === root.layerId && this.root?.schemaId === root.schemaId));
            button.append(this.element("span", root.name), this.element("small", root.type));
            button.addEventListener("click", () => {
                const hash = `schemas/${encodeURIComponent(this.selection)}/${encodeURIComponent(root.layerId)}/${root.schemaId}`;
                if (location.hash === `#${hash}`) this.selectRoot(root);
                else location.hash = hash;
            });
            list.appendChild(button);
        }
        if (!list.childElementCount) list.appendChild(this.element("p", "No layers or types match.", "schema-message"));
    }

    /** Start one feature/SourceData root, never all roots of a large format. */
    selectRoot(root) {
        this.root = root;
        this.renderRoots();
        setText("schemaRootTitle", root ? `${root.layerId} / ${root.name}` : "No declared roots");
        byId("schemaFind").disabled = !root;
        this.showRoot();
    }

    /** Clear a search and restore the selected root's lazy tree. */
    showRoot() {
        this.cancelPending();
        byId("schemaFind").value = "";
        byId("schemaFindClear").hidden = true;
        setText("schemaSearchStatus", "Find searches descendant field, type and enum names, ignoring case, spaces, underscores and hyphens. Context-only links are excluded.");
        const tree = byId("schemaTree");
        tree.replaceChildren();
        if (this.root) tree.appendChild(this.domain(this.root.name, this.root.schemaId, new Set(), true));
    }

    /** Fetch under the current view generation; repeated references share only this view's request. */
    async request(parameters) {
        const controller = new AbortController();
        this.controllers.add(controller);
        try {
            const params = new URLSearchParams({mapSchemaId: this.selection, ...parameters});
            const response = await fetch(`schema/inspect?${params}`, {
                cache: "no-store", credentials: "same-origin", signal: controller.signal
            });
            if (!response.ok) throw new Error(`HTTP ${response.status}: ${await response.text()}`);
            return await response.json();
        } finally {
            this.controllers.delete(controller);
        }
    }

    /** Use the same bounded, ranked finder as MCP; no eager browser-side graph crawl. */
    async search() {
        const find = byId("schemaFind").value.trim();
        if (!this.root || !find) { this.showRoot(); return; }
        this.cancelPending();
        const generation = this.generation;
        byId("schemaFindSubmit").disabled = true;
        byId("schemaFindClear").hidden = false;
        byId("schemaTree").replaceChildren();
        setText("schemaSearchStatus", `Searching ${this.root.layerId} / ${this.root.name}...`);
        try {
            const result = await this.request({layerId: this.root.layerId, schemaId: this.root.schemaId, find});
            if (generation !== this.generation) return;
            const count = result.items.length;
            const partial = result.complete === false ? ` Partial results (${result.reason || "budget reached"}); refine the term or search within a narrower root.` : "";
            setText("schemaSearchStatus", `${count} matching domain(s). Paths are representative, not an exhaustive list of occurrences.${partial}`);
            for (const item of result.items) {
                const context = item.contexts?.[0];
                const label = context?.fieldName || item.typename || `Domain #${item.schemaId}`;
                const block = this.element("section", "", "schema-search-result");
                for (const context of item.contexts || [])
                    block.appendChild(this.element("p", (context.path || []).join(" / ") || this.root.name, "schema-path"));
                const symbols = [...(item.enum?.matchingSymbols || []), ...(item.namedValues?.matchingNames || [])];
                if (symbols.length) block.appendChild(this.element("p", `Matching symbols: ${symbols.join(", ")}`, "schema-path"));
                block.appendChild(this.domain(label, item.schemaId, new Set(), false, item.kind));
                byId("schemaTree").appendChild(block);
            }
)STATUS"
        R"STATUS(            if (!count) byId("schemaTree").appendChild(this.element("p",
                result.complete === false ? "No matches in the inspected portion. This is not proof of absence." : "No matching declarations in this root.", "empty-state"));
        } catch (error) {
            if (generation === this.generation) setText("schemaSearchStatus", `Search failed: ${error}. You can retry.`);
        } finally {
            if (generation === this.generation) byId("schemaFindSubmit").disabled = false;
        }
    }

    /** Show exact artifact identity and statistics as structured facts, not a JSON dump. */
    renderMetadata(item) {
        const box = byId("schemaSummary");
        box.replaceChildren(this.properties({artifact: item.mapSchemaId, format: item.mapFormat.name,
            version: item.mapFormat.version || "Defined by subsystems", retainedCapacity: formatBytes(item.retainedBytes)}));
        box.appendChild(this.versions(item.mapFormat));
        box.appendChild(this.disclosure("Consuming maps", this.value(item.consumers)));
        box.appendChild(this.disclosure("Counts and retained capacity", this.properties(item.statistics)));
    }

    /** A concise metadata section stays closed until its detail is relevant. */
    disclosure(title, content) {
        const section = this.element("details");
        section.append(this.element("summary", title), content);
        return section;
    }

    /** Display protocol keys as readable labels without changing field/type identifiers. */
    label(key) {
        const labels = {typename: "Type name", open: "Additional fields allowed", nullable: "Allows null"};
        if (labels[key]) return labels[key];
        return key.replace(/([a-z])([A-Z])/g, "$1 $2").replace(/[-_]/g, " ").replace(/^./, c => c.toUpperCase());
    }

    /** Render a property record with bounded wrapping instead of an opaque serialized block. */
    properties(record) {
        const list = this.element("dl", "", "schema-properties");
        for (const [key, value] of Object.entries(record)) {
            const row = this.element("div");
            const term = this.element("dt", this.label(key));
            term.title = key;
            const description = this.element("dd");
            description.appendChild(this.value(value));
            row.append(term, description);
            list.appendChild(row);
        }
        return list;
    }

    /** Preserve exact integer wrappers; arrays of records become tables, not JSON inside the tree. */
    value(value) {
        if (value === null || value === undefined) return this.element("span", "Unknown", "muted");
        if (typeof value !== "object") return this.element("span", typeof value === "boolean" ? (value ? "Yes" : "No") : String(value));
        if (["int64", "uint64"].includes(value.$mapget)) return this.element("code", value.value);
        if (!Array.isArray(value)) return this.properties(value);
        if (!value.length) return this.element("span", "None declared", "muted");
        if (value.every(item => item && typeof item === "object" && !Array.isArray(item) && !item.$mapget)) {
            const keys = [...new Set(value.flatMap(item => Object.keys(item)))];
            const wrapper = this.element("div", "", "table-wrap");
            const table = this.element("table");
            const head = this.element("thead"), headings = this.element("tr"), body = this.element("tbody");
            for (const key of keys) headings.appendChild(this.element("th", this.label(key)));
            head.appendChild(headings);
            for (const item of value) {
                const row = this.element("tr");
                for (const key of keys) {
                    const cell = this.element("td");
                    cell.appendChild(this.value(item[key]));
                    row.appendChild(cell);
                }
                body.appendChild(row);
            }
            table.append(head, body);
            wrapper.appendChild(table);
            return wrapper;
        }
        const list = this.element("ol");
        for (const item of value) {
            const row = this.element("li");
            row.appendChild(this.value(item));
            list.appendChild(row);
        }
        return list;
    }

    /** Put scalar semantics and feature contracts ahead of the navigable child fields. */
    renderDomain(content, node) {
        const {"$ref": reference, namedValues, enum: literals, numericDomain, ...metadata} = node.descriptor;
        content.appendChild(this.properties(metadata));
        if (numericDomain) content.appendChild(this.disclosure("Numeric encoding and units", this.properties(numericDomain)));
        if (namedValues?.length) content.appendChild(this.disclosure(`Named values (${namedValues.length})`, this.value(namedValues)));
        if (literals?.length) content.appendChild(this.disclosure(`Enum values (${literals.length})`, this.value(literals)));
        for (const type of node.featureTypes || []) {
            content.appendChild(this.disclosure(`${type.name}: identifier compositions`, this.value(type.uniqueIdCompositions || [])));
            content.appendChild(this.disclosure(`${type.name}: outgoing relations`, type.forwardRelationsKnown
                ? this.value(type.forwardRelations || []) : this.element("p", "This provider has not declared its relation inventory.", "schema-message")));
        }
    }

    /** Load one descriptor on expansion, preserving cycles, retries and selection generations. */
    domain(label, id, ancestors, open = false, kind = "") {
        const element = this.element("details", "", "schema-branch");
        element.dataset.schemaId = id;
        const summary = this.element("summary", label);
        const badge = this.element("span", `${kind ? kind + " " : ""}#${id}`, "schema-kind");
        summary.append(document.createTextNode(" "), badge);
        element.appendChild(summary);
        if (ancestors.has(id)) {
            badge.textContent += " / recursive reference";
            element.appendChild(this.element("p", "This domain already occurs above. Its shared definition is not expanded again on this path.", "schema-message"));
            return element;
        }
        let loaded = false;
        const content = this.element("div", "", "schema-content");
        element.appendChild(content);
        const generation = this.generation;
        const expand = async () => {
            if (!element.open || loaded || generation !== this.generation) return;
            loaded = true;
            content.textContent = "Loading domain...";
            try {
                if (!this.domains.has(id)) {
                    const pending = this.request({schemaId: id});
                    this.domains.set(id, pending);
                    pending.catch(() => { if (generation === this.generation) this.domains.delete(id); });
                }
                const node = await this.domains.get(id);
                if (generation !== this.generation) return;
                content.replaceChildren();
                badge.textContent = [node.descriptor.kind || "unknown",
                    node.descriptor.typename !== label ? node.descriptor.typename : "", `#${id}`].filter(Boolean).join(" / ");
                this.renderDomain(content, node);
                const next = new Set(ancestors);
                next.add(id);
                const members = [];
                for (const group of ["fields", "elements", "alternatives"]) {
                    for (const [index, edge] of (node[group] || []).entries()) {
                        const refs = Array.isArray(edge.$ref) ? edge.$ref : [edge.$ref];
                        const field = group === "fields" ? edge.name : `${group === "elements" ? "Element type" : "Alternative"} ${index + 1}`;
                        const flags = [edge.required === true ? "required" : edge.required === false ? "optional" : "", edge.role || ""].filter(Boolean);
                        const name = flags.length ? `${field} (${flags.join(", ")})` : field;
                        for (const ref of refs) members.push({name, element: Number.isInteger(ref) && ref > 0
                            ? this.domain(name, ref, next) : this.element("p", `${name}: unspecified domain`, "schema-message")});
                    }
                }
                if (members.length) {
                    const filter = this.element("label", `Filter immediate members (${members.length})`, "schema-branch-filter panel-subtitle");
                    const input = this.element("input");
                    input.type = "search";
                    input.placeholder = `Field name in ${label}`;
                    input.setAttribute("aria-label", `Filter immediate members of ${label}`);
                    const count = this.element("p", "", "schema-message");
                    input.addEventListener("input", () => {
                        for (const member of members) member.element.hidden = !this.matches(member.name, input.value);
                        count.textContent = `${members.filter(member => !member.element.hidden).length} of ${members.length} immediate members`;
                    });
                    filter.appendChild(input);
                    content.append(filter, count, ...members.map(member => member.element));
                }
            } catch (error) {
                if (generation !== this.generation) return;
                content.replaceChildren(this.element("p", `Unable to load domain: ${error}`, "schema-message schema-error"));
                const retry = this.element("button", "Retry", "button");
                retry.type = "button";
                retry.addEventListener("click", expand);
                content.appendChild(retry);
                loaded = false;
            }
        };
        element.addEventListener("toggle", expand);
        element.open = open;
        return element;
    }
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
    state.schemaExplorer?.refresh();
    state.payload = payload;
    renderHealth(payload);
    renderOverview(payload);
    renderMemory(payload.memory || {});
    renderTraffic(payload);
    renderCache(payload.cache || {}, payload.memory || {});
    renderDiagnostics(payload);
    state.historyGraph?.append(payload);
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
    const tabName = name.startsWith("schemas/") ? "schemas" : name;
    const valid = tabs.some((tab) => tab.dataset.tab === tabName) ? tabName : "overview";
    for (const tab of tabs) {
        const active = tab.dataset.tab === valid;
        tab.setAttribute("aria-selected", active ? "true" : "false");
        tab.tabIndex = active ? 0 : -1;
    }
    for (const panel of document.querySelectorAll("[data-panel]")) panel.hidden = panel.dataset.panel !== valid;
    qs(".history-graph").hidden = valid === "schemas" && Boolean(state.schemaExplorer?.selection);
    state.historyGraph?.activateTab(valid);
    if (updateHash && location.hash !== `#${valid}`) history.replaceState(null, "", `#${valid}`);
    if (valid === "schemas") {
        state.schemaExplorer?.navigate();
        state.schemaExplorer?.refresh();
    }
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

applyTheme(document.documentElement.dataset.theme || "light");
byId("host").textContent = window.location.host;
byId("themeSelect")?.addEventListener("change", (event) => applyTheme(event.target.value, true));
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

state.historyGraph = new StatusHistoryGraph();
state.schemaExplorer = new SchemaExplorer();
activateTab(location.hash.slice(1) || "overview", false);
resetTimer();
refreshStatus(true);
</script>
</body>
</html>)STATUS";
    return page;
}

}  // namespace mapget::detail
