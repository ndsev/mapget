// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "devui.h"
#include "gridsource/gridsource.h"
#include "mapget/log.h"
#include "nlohmann/json.hpp"

namespace mapget
{

namespace
{

// Embedded Developer UI HTML page
static const char* kDevUIHtml = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Developer UI</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,monospace;
  background:#1a1a2e;color:#e0e0e0;padding:20px;max-width:800px;margin:0 auto}
h1{color:#00d4ff;margin-bottom:20px;font-size:1.4em;display:flex;
  justify-content:space-between;align-items:center}
.section{background:#16213e;border:1px solid #0f3460;border-radius:6px;
  padding:16px;margin-bottom:16px}
.section-title{color:#00d4ff;font-size:0.9em;font-weight:bold;
  text-transform:uppercase;letter-spacing:1px;margin-bottom:12px}
.profile-btns{display:flex;gap:6px;margin-bottom:14px}
.profile-btn{padding:6px 14px;border:1px solid #0f3460;border-radius:4px;
  background:#1a1a2e;color:#e0e0e0;cursor:pointer;font-size:0.85em;
  font-family:inherit;transition:all 0.15s}
.profile-btn:hover{border-color:#00d4ff;color:#00d4ff}
.profile-btn.active{background:#0f3460;border-color:#00d4ff;color:#00d4ff}
.slider-row{display:flex;align-items:center;margin-bottom:8px;gap:10px}
.slider-label{width:180px;font-size:0.85em;color:#a0a0c0;flex-shrink:0}
.slider-row input[type=range]{flex:1;accent-color:#00d4ff;height:4px}
.slider-val{width:60px;text-align:right;font-size:0.85em;color:#00d4ff;
  font-family:monospace;flex-shrink:0}
.layer-row{display:flex;align-items:center;gap:10px;margin-bottom:8px;
  padding:6px 8px;background:#1a1a2e;border-radius:4px}
.layer-row label{font-size:0.85em;flex:1}
.layer-row input[type=number]{width:60px;background:#0f3460;border:1px solid #233d6e;
  color:#e0e0e0;padding:3px 6px;border-radius:3px;font-family:monospace;font-size:0.85em}
.btn-row{display:flex;gap:8px;flex-wrap:wrap}
.btn{padding:8px 16px;border:1px solid #0f3460;border-radius:4px;
  background:#1a1a2e;color:#e0e0e0;cursor:pointer;font-size:0.85em;
  font-family:inherit;transition:all 0.15s}
.btn:hover{border-color:#00d4ff;color:#00d4ff}
.btn-primary{background:#0f3460;border-color:#00d4ff;color:#00d4ff}
.btn-primary:hover{background:#00d4ff;color:#1a1a2e}
.btn-danger{border-color:#e94560}
.btn-danger:hover{background:#e94560;color:#fff}
.status{margin-left:auto;font-size:0.85em;padding:4px 10px;border-radius:3px}
.status-ok{color:#4ecca3}
.status-err{color:#e94560}
.status-busy{color:#f0c040}
.bottom-bar{display:flex;align-items:center;gap:12px;margin-top:8px}
</style>
</head>
<body>
<h1>Developer UI <button class="btn" onclick="refresh()">Refresh</button></h1>

<div class="section" id="sec-profile">
  <div class="section-title">GridSource: Attribute Tree Profile</div>
  <div class="profile-btns" id="profileBtns"></div>
  <div id="treeSliders"></div>
</div>

<div class="section" id="sec-delay">
  <div class="section-title">GridSource: Simulated Latency</div>
  <div id="delaySliders"></div>
</div>

<div class="section" id="sec-layers">
  <div class="section-title">GridSource: Layers</div>
  <div id="layerRows"></div>
</div>

<div class="section">
  <div class="section-title">Cache</div>
  <div class="btn-row">
    <button class="btn btn-danger" onclick="clearTileCache()">Clear Tile Cache</button>
    <button class="btn btn-danger" onclick="clearContextCache()">Clear Context Cache</button>
  </div>
</div>

<div class="bottom-bar">
  <button class="btn btn-primary" onclick="applyChanges()">Apply Changes</button>
  <span class="status" id="statusText"></span>
</div>

<script>
let config = null;
let currentProfile = 'none';

const profiles = {
  none:      {numLayers:0,attrsPerLayer:0,fieldsPerAttr:0,maxNestingDepth:0,maxArraySize:0,
              nestingProbability:0,directionalValidityProb:0,rangeValidityProb:0,topLevelExtraFields:0},
  minimal:   {numLayers:1,attrsPerLayer:2,fieldsPerAttr:2,maxNestingDepth:0,maxArraySize:0,
              nestingProbability:0,directionalValidityProb:0,rangeValidityProb:0,topLevelExtraFields:1},
  moderate:  {numLayers:3,attrsPerLayer:4,fieldsPerAttr:3,maxNestingDepth:1,maxArraySize:0,
              nestingProbability:0.15,directionalValidityProb:0.2,rangeValidityProb:0,topLevelExtraFields:3},
  realistic: {numLayers:6,attrsPerLayer:5,fieldsPerAttr:4,maxNestingDepth:2,maxArraySize:3,
              nestingProbability:0.25,directionalValidityProb:0.5,rangeValidityProb:0.3,topLevelExtraFields:5},
  stress:    {numLayers:12,attrsPerLayer:10,fieldsPerAttr:8,maxNestingDepth:4,maxArraySize:6,
              nestingProbability:0.4,directionalValidityProb:0.8,rangeValidityProb:0.5,topLevelExtraFields:10}
};

const treeDefs = [
  {key:'numLayers',label:'numLayers',min:0,max:16,step:1},
  {key:'attrsPerLayer',label:'attrsPerLayer',min:0,max:16,step:1},
  {key:'fieldsPerAttr',label:'fieldsPerAttr',min:0,max:12,step:1},
  {key:'maxNestingDepth',label:'maxNestingDepth',min:0,max:6,step:1},
  {key:'maxArraySize',label:'maxArraySize',min:0,max:10,step:1},
  {key:'nestingProbability',label:'nestingProb',min:0,max:1,step:0.05},
  {key:'directionalValidityProb',label:'dirValidityProb',min:0,max:1,step:0.05},
  {key:'rangeValidityProb',label:'rangeValidityProb',min:0,max:1,step:0.05},
  {key:'topLevelExtraFields',label:'topLevelExtraFields',min:0,max:20,step:1}
];

const delayDefs = [
  {key:'sourceDownloadDelayMs',label:'Download (IO)',min:0,max:2000,step:10},
  {key:'sourceUnpackDelayMs',label:'Unpack (CPU)',min:0,max:1000,step:10},
  {key:'sourceTransformDelayMs',label:'Transform (CPU)',min:0,max:1000,step:10}
];

function setStatus(msg,cls){
  const el=document.getElementById('statusText');
  el.textContent=msg;
  el.className='status '+cls;
}

function mkSlider(parent,def,getValue,setValue){
  const row=document.createElement('div');
  row.className='slider-row';
  const lbl=document.createElement('span');
  lbl.className='slider-label';
  lbl.textContent=def.label+':';
  const inp=document.createElement('input');
  inp.type='range';inp.min=def.min;inp.max=def.max;inp.step=def.step;
  inp.value=getValue();
  const val=document.createElement('span');
  val.className='slider-val';
  val.textContent=Number(inp.value).toFixed(def.step<1?2:0);
  inp.oninput=()=>{
    val.textContent=Number(inp.value).toFixed(def.step<1?2:0);
    setValue(Number(inp.value));
  };
  row.append(lbl,inp,val);
  parent.append(row);
  return {inp,val};
}

function buildProfileButtons(){
  const c=document.getElementById('profileBtns');
  c.innerHTML='';
  for(const p of Object.keys(profiles)){
    const b=document.createElement('button');
    b.className='profile-btn'+(p===currentProfile?' active':'');
    b.textContent=p;
    b.onclick=()=>selectProfile(p);
    c.append(b);
  }
}

function selectProfile(p){
  currentProfile=p;
  if(config && config.instances && config.instances[0]){
    config.instances[0].config.attributeTreeProfile=p;
    const preset=profiles[p];
    config.instances[0].config.attributeTreeParams={...preset};
  }
  buildUI();
}

function buildTreeSliders(){
  const c=document.getElementById('treeSliders');
  c.innerHTML='';
  if(!config||!config.instances||!config.instances[0]) return;
  const cfg=config.instances[0].config;
  const params=cfg.attributeTreeParams||{};
  for(const def of treeDefs){
    const preset=profiles[currentProfile]||profiles.none;
    const curVal=params[def.key]!=null?params[def.key]:preset[def.key];
    mkSlider(c,def,()=>curVal,(v)=>{
      if(!cfg.attributeTreeParams) cfg.attributeTreeParams={};
      cfg.attributeTreeParams[def.key]=v;
    });
  }
}

function buildDelaySliders(){
  const c=document.getElementById('delaySliders');
  c.innerHTML='';
  if(!config||!config.instances||!config.instances[0]) return;
  const cfg=config.instances[0].config;
  for(const def of delayDefs){
    mkSlider(c,def,()=>cfg[def.key]||0,(v)=>{cfg[def.key]=v;});
  }
}

function buildLayerRows(){
  const c=document.getElementById('layerRows');
  c.innerHTML='';
  if(!config||!config.instances||!config.instances[0]) return;
  const cfg=config.instances[0].config;
  if(!cfg.layers) return;
  for(let i=0;i<cfg.layers.length;i++){
    const layer=cfg.layers[i];
    const row=document.createElement('div');
    row.className='layer-row';
    const cb=document.createElement('input');
    cb.type='checkbox';cb.checked=layer.enabled!==false;
    cb.onchange=()=>{layer.enabled=cb.checked;};
    const lbl=document.createElement('label');
    lbl.textContent=layer.name;
    const densLbl=document.createElement('span');
    densLbl.className='slider-label';
    densLbl.style.width='auto';densLbl.textContent='density:';
    const densInp=document.createElement('input');
    densInp.type='number';densInp.step='0.01';densInp.min='0';densInp.max='1';
    densInp.value=layer.geometry?layer.geometry.density||0:0;
    densInp.onchange=()=>{
      if(!layer.geometry) layer.geometry={};
      layer.geometry.density=Number(densInp.value);
    };
    row.append(cb,lbl,densLbl,densInp);
    c.append(row);
  }
}

function buildUI(){
  buildProfileButtons();
  buildTreeSliders();
  buildDelaySliders();
  buildLayerRows();
}

async function refresh(){
  setStatus('Loading...','status-busy');
  try{
    const r=await fetch('/dev/gridsource/config');
    if(!r.ok) throw new Error(r.statusText);
    config=await r.json();
    if(config.instances&&config.instances[0]){
      currentProfile=config.instances[0].config.attributeTreeProfile||'none';
    }
    buildUI();
    setStatus('OK','status-ok');
  }catch(e){
    setStatus('Error: '+e.message,'status-err');
  }
}

async function applyChanges(){
  if(!config||!config.instances||!config.instances[0]) return;
  setStatus('Applying...','status-busy');
  try{
    const r=await fetch('/dev/gridsource/config',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify(config.instances[0].config)
    });
    if(!r.ok) throw new Error(r.statusText);
    setStatus('OK','status-ok');
  }catch(e){
    setStatus('Error: '+e.message,'status-err');
  }
}

async function clearTileCache(){
  setStatus('Clearing...','status-busy');
  try{
    const r=await fetch('/dev/cache/clear',{method:'POST'});
    if(!r.ok) throw new Error(r.statusText);
    setStatus('Cache cleared','status-ok');
  }catch(e){
    setStatus('Error: '+e.message,'status-err');
  }
}

async function clearContextCache(){
  setStatus('Clearing...','status-busy');
  try{
    const r=await fetch('/dev/gridsource/config',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({_clearContextCache:true})
    });
    if(!r.ok) throw new Error(r.statusText);
    setStatus('Context cache cleared','status-ok');
  }catch(e){
    setStatus('Error: '+e.message,'status-err');
  }
}

refresh();
</script>
</body>
</html>)html";

}  // anonymous namespace

void setupDevUI(httplib::Server& server, Cache::Ptr cache)
{
    // GET /dev/ - Serve the Developer UI HTML page
    server.Get("/dev/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kDevUIHtml, "text/html");
    });
    // Also handle /dev without trailing slash
    server.Get("/dev", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/dev/");
    });

    // GET /dev/gridsource/config - Return current config as JSON
    server.Get("/dev/gridsource/config",
        [](const httplib::Request&, httplib::Response& res) {
            auto instances = gridsource::GridDataSource::getInstances();
            nlohmann::json result;
            result["instances"] = nlohmann::json::array();
            for (auto& inst : instances) {
                auto cfg = inst->getConfig();
                nlohmann::json entry;
                entry["mapId"] = cfg.mapId;
                entry["config"] = cfg.toJson();
                // Stats
                nlohmann::json stats;
                // Context cache size is not directly accessible, but we can signal it
                entry["stats"] = stats;
                result["instances"].push_back(entry);
            }
            res.set_content(result.dump(), "application/json");
        });

    // POST /dev/gridsource/config - Partial config update
    server.Post("/dev/gridsource/config",
        [cache](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = nlohmann::json::parse(req.body);

                // Special: context cache clear only
                if (j.contains("_clearContextCache") && j["_clearContextCache"].get<bool>()) {
                    for (auto& inst : gridsource::GridDataSource::getInstances()) {
                        inst->clearContextCache();
                    }
                    res.set_content(R"({"status":"ok"})", "application/json");
                    return;
                }

                auto instances = gridsource::GridDataSource::getInstances();
                if (instances.empty()) {
                    res.status = 404;
                    res.set_content(R"({"error":"No GridDataSource instances found"})", "application/json");
                    return;
                }

                // Apply to the first instance (most common case)
                auto& inst = instances[0];
                auto currentCfg = inst->getConfig();

                // Merge provided fields into current config
                if (j.contains("sourceDownloadDelayMs"))
                    currentCfg.sourceDownloadDelayMs = j["sourceDownloadDelayMs"].get<uint32_t>();
                if (j.contains("sourceUnpackDelayMs"))
                    currentCfg.sourceUnpackDelayMs = j["sourceUnpackDelayMs"].get<uint32_t>();
                if (j.contains("sourceTransformDelayMs"))
                    currentCfg.sourceTransformDelayMs = j["sourceTransformDelayMs"].get<uint32_t>();
                if (j.contains("attributeTreeProfile")) {
                    static const std::map<std::string, gridsource::AttributeTreeProfile> pmap = {
                        {"none", gridsource::AttributeTreeProfile::None},
                        {"minimal", gridsource::AttributeTreeProfile::Minimal},
                        {"moderate", gridsource::AttributeTreeProfile::Moderate},
                        {"realistic", gridsource::AttributeTreeProfile::Realistic},
                        {"stress", gridsource::AttributeTreeProfile::Stress}
                    };
                    auto profileStr = j["attributeTreeProfile"].get<std::string>();
                    auto it = pmap.find(profileStr);
                    currentCfg.attributeTreeProfile = (it != pmap.end()) ? it->second : gridsource::AttributeTreeProfile::None;
                }
                if (j.contains("attributeTreeParams")) {
                    auto& atp = j["attributeTreeParams"];
                    if (atp.contains("numLayers")) currentCfg.attributeTreeParams.numLayers = atp["numLayers"].get<int>();
                    if (atp.contains("attrsPerLayer")) currentCfg.attributeTreeParams.attrsPerLayer = atp["attrsPerLayer"].get<int>();
                    if (atp.contains("fieldsPerAttr")) currentCfg.attributeTreeParams.fieldsPerAttr = atp["fieldsPerAttr"].get<int>();
                    if (atp.contains("maxNestingDepth")) currentCfg.attributeTreeParams.maxNestingDepth = atp["maxNestingDepth"].get<int>();
                    if (atp.contains("maxArraySize")) currentCfg.attributeTreeParams.maxArraySize = atp["maxArraySize"].get<int>();
                    if (atp.contains("nestingProbability")) currentCfg.attributeTreeParams.nestingProbability = atp["nestingProbability"].get<double>();
                    if (atp.contains("directionalValidityProb")) currentCfg.attributeTreeParams.directionalValidityProb = atp["directionalValidityProb"].get<double>();
                    if (atp.contains("rangeValidityProb")) currentCfg.attributeTreeParams.rangeValidityProb = atp["rangeValidityProb"].get<double>();
                    if (atp.contains("topLevelExtraFields")) currentCfg.attributeTreeParams.topLevelExtraFields = atp["topLevelExtraFields"].get<int>();
                }
                if (j.contains("layers") && j["layers"].is_array()) {
                    // Match by index for now
                    for (size_t i = 0; i < j["layers"].size() && i < currentCfg.layers.size(); ++i) {
                        auto& lj = j["layers"][i];
                        auto& layer = currentCfg.layers[i];
                        if (lj.contains("enabled")) layer.enabled = lj["enabled"].get<bool>();
                        if (lj.contains("geometry") && lj["geometry"].contains("density")) {
                            layer.geometry.density = lj["geometry"]["density"].get<double>();
                        }
                        if (lj.contains("attributeTreeProfile")) {
                            static const std::map<std::string, gridsource::AttributeTreeProfile> pmap = {
                                {"none", gridsource::AttributeTreeProfile::None},
                                {"minimal", gridsource::AttributeTreeProfile::Minimal},
                                {"moderate", gridsource::AttributeTreeProfile::Moderate},
                                {"realistic", gridsource::AttributeTreeProfile::Realistic},
                                {"stress", gridsource::AttributeTreeProfile::Stress}
                            };
                            auto profileStr = lj["attributeTreeProfile"].get<std::string>();
                            auto it = pmap.find(profileStr);
                            layer.attributeTreeProfile = (it != pmap.end()) ? it->second : gridsource::AttributeTreeProfile::None;
                        }
                    }
                }

                // Apply the updated config
                inst->setConfig(std::move(currentCfg));

                // Also clear the tile cache so next requests regenerate
                if (cache) cache->clear();

                log().info("Dev UI: GridDataSource config updated");
                res.set_content(R"({"status":"ok"})", "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                nlohmann::json err;
                err["error"] = e.what();
                res.set_content(err.dump(), "application/json");
            }
        });

    // POST /dev/cache/clear - Clear tile cache
    server.Post("/dev/cache/clear",
        [cache](const httplib::Request&, httplib::Response& res) {
            if (cache) {
                cache->clear();
                log().info("Dev UI: Tile cache cleared");
            }
            // Also clear context caches on all GridDataSource instances
            for (auto& inst : gridsource::GridDataSource::getInstances()) {
                inst->clearContextCache();
            }
            res.set_content(R"({"status":"ok"})", "application/json");
        });

    log().info("Developer UI enabled at /dev/");
}

}  // namespace mapget
