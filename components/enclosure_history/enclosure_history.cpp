#include "enclosure_history.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace enclosure_history {

static const char *const TAG = "enclosure_history";

// Blob layout for a ring in flash. The preference key includes the series
// count, and the header repeats it, so a firmware with a different set of
// series starts empty instead of reading the old layout as the new one.
struct RingHeader {
  uint32_t magic;
  uint32_t step;
  uint32_t slots;
  uint32_t nseries;
  uint32_t last_slot;
};
static const uint32_t RING_MAGIC = 0x4C484953;  // "LHIS"
// Half an hour between saves: at most that much history is lost to a power
// cut, and NVS (448 KB on these builds) spreads the writes over its pages.
static const uint32_t SAVE_EVERY_S = 1800;

// The charts page. Self-contained (no CDN): the device has to be readable
// with the internet down, which is also when you most want to look at it.
// Draws one SVG per chart group; a null is a gap and breaks the line.
static const char CHARTS_HTML[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Enclosure history</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#ddd}
header{padding:12px 16px;display:flex;gap:12px;align-items:center;flex-wrap:wrap}
h1{font-size:18px;margin:0;flex:1}a{color:#8cf}
button{background:#222;color:#ddd;border:1px solid #444;padding:6px 12px;border-radius:6px;cursor:pointer}
button.on{background:#2a6;border-color:#2a6;color:#000}
.card{background:#1a1a1a;margin:8px 12px;padding:8px 12px;border-radius:8px}
.card h2{font-size:14px;margin:4px 0;color:#aaa;font-weight:600}
.leg{font-size:13px;display:flex;gap:16px;flex-wrap:wrap;margin:4px 0}
svg{width:100%;height:auto;display:block}
.ax{stroke:#333;stroke-width:1}.tx{fill:#888;font-size:11px}
#msg{padding:0 16px;color:#fa4}
</style></head><body>
<header><h1>Enclosure history</h1>
<button id="b_day" class="on" onclick="load('day')">24 hours</button>
<button id="b_month" onclick="load('month')">30 days</button>
<a href="/">&larr; device page</a></header>
<div id="msg"></div><div id="charts"></div>
<script>
const COL=['#ff7a59','#4fb3ff','#ffd24f','#7bdc7b','#d78cff','#5fe0d0','#ff9ec7','#c0c0c0','#ffa640','#9fa8ff'];
let range='day';
function fmtT(t,month){const d=new Date(t*1000);return month?(d.getMonth()+1)+'/'+d.getDate():
 String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0');}
function load(r){range=r;for(const k of['day','month'])document.getElementById('b_'+k).className=k==r?'on':'';
 fetch('/history.json?range='+r,{cache:'no-store'}).then(x=>x.json()).then(draw).catch(e=>{
 document.getElementById('msg').textContent='Could not load history: '+e;});}
function draw(j){
 const msg=document.getElementById('msg');
 msg.textContent=j.end==0?'No history yet: the device has not had a valid clock since it was flashed.':'';
 const groups={};
 j.series.forEach((s,i)=>{(groups[s.chart]=groups[s.chart]||[]).push([s,i]);});
 const W=900,H=220,L=44,R=10,T=10,B=24,n=j.slots,t0=j.end-(n-1)*j.step,month=range=='month';
 let html='';
 for(const g in groups){
  let lo=Infinity,hi=-Infinity;
  for(const[s]of groups[g])for(const v of s.data)if(v!==null){lo=Math.min(lo,v);hi=Math.max(hi,v);}
  if(lo==Infinity){lo=0;hi=1;}if(hi-lo<1){const m=(hi+lo)/2;lo=m-0.5;hi=m+0.5;}
  const pad=(hi-lo)*0.08;lo-=pad;hi+=pad;
  const x=i=>L+(W-L-R)*i/(n-1),y=v=>T+(H-T-B)*(1-(v-lo)/(hi-lo));
  let svg='<svg viewBox="0 0 '+W+' '+H+'">';
  for(let k=0;k<=4;k++){const v=lo+(hi-lo)*k/4,yy=y(v);
   svg+='<line class="ax" x1="'+L+'" x2="'+(W-R)+'" y1="'+yy+'" y2="'+yy+'"/>';
   svg+='<text class="tx" x="'+(L-4)+'" y="'+(yy+4)+'" text-anchor="end">'+v.toFixed(hi-lo<10?1:0)+'</text>';}
  for(let k=0;k<=6;k++){const i=Math.round((n-1)*k/6);
   svg+='<text class="tx" x="'+x(i)+'" y="'+(H-6)+'" text-anchor="middle">'+fmtT(t0+i*j.step,month)+'</text>';}
  let leg='';
  for(const[s,si]of groups[g]){
   const c=COL[si%COL.length];let d='',pen=false,mn=Infinity,mx=-Infinity,last=null;
   s.data.forEach((v,i)=>{if(v===null){pen=false;return;}
    d+=(pen?'L':'M')+x(i).toFixed(1)+' '+y(v).toFixed(1);pen=true;mn=Math.min(mn,v);mx=Math.max(mx,v);last=v;});
   svg+='<path d="'+d+'" fill="none" stroke="'+c+'" stroke-width="1.8"/>';
   leg+='<span style="color:'+c+'">&#9632; '+s.label+': '+(last===null?'no data':
    'now '+last+s.unit+' &middot; min '+mn+' &middot; max '+mx)+'</span>';}
  svg+='</svg>';
  html+='<div class="card"><h2>'+g+'</h2><div class="leg">'+leg+'</div>'+svg+'</div>';}
 document.getElementById('charts').innerHTML=html;}
load('day');setInterval(()=>load(range),60000);
</script></body></html>)HTML";

void EnclosureHistory::add_series(const char *key, sensor::Sensor *sens, float scale, const char *label,
                                  const char *chart, const char *unit) {
  this->series_.push_back(Series{key, sens, scale, label, chart, unit});
}

void EnclosureHistory::init_ring_(Ring &r, uint32_t step, uint32_t slots, uint32_t hash) {
  const size_t n = this->series_.size();
  r.step = step;
  r.slots = slots;
  r.data.assign(n * slots, MISSING);
  r.sum.assign(n, 0.0);
  r.count.assign(n, 0);
  r.pref = global_preferences->make_preference(sizeof(RingHeader) + r.data.size() * sizeof(int16_t),
                                               hash ^ (uint32_t) n);
}

void EnclosureHistory::setup() {
  this->init_ring_(this->fast_, 300, 288, fnv1_hash("enclosure_history_fast"));
  this->init_ring_(this->slow_, 3600, 720, fnv1_hash("enclosure_history_slow"));
  const bool fast_ok = this->load_ring_(this->fast_);
  const bool slow_ok = this->load_ring_(this->slow_);
  ESP_LOGI(TAG, "Restored: 24h ring %s, 30d ring %s", fast_ok ? "yes" : "no", slow_ok ? "yes" : "no");
  this->base_->add_handler(this);
}

bool EnclosureHistory::load_ring_(Ring &r) {
  std::vector<uint8_t> buf(sizeof(RingHeader) + r.data.size() * sizeof(int16_t));
  if (!r.pref.load(buf.data(), buf.size()))
    return false;
  RingHeader h;
  memcpy(&h, buf.data(), sizeof(h));
  if (h.magic != RING_MAGIC || h.step != r.step || h.slots != r.slots || h.nseries != this->series_.size())
    return false;
  memcpy(r.data.data(), buf.data() + sizeof(h), r.data.size() * sizeof(int16_t));
  r.last_slot = h.last_slot;
  return true;
}

void EnclosureHistory::save_ring_(Ring &r) {
  if (r.last_slot == 0)
    return;
  // The open slot goes in as a partial mean, so a reboot mid-slot keeps it.
  this->commit_(r, false);
  std::vector<uint8_t> buf(sizeof(RingHeader) + r.data.size() * sizeof(int16_t));
  const RingHeader h{RING_MAGIC, r.step, r.slots, (uint32_t) this->series_.size(), r.last_slot};
  memcpy(buf.data(), &h, sizeof(h));
  memcpy(buf.data() + sizeof(h), r.data.data(), r.data.size() * sizeof(int16_t));
  r.pref.save(buf.data(), buf.size());
}

uint32_t EnclosureHistory::now_() const {
  if (this->time_ == nullptr)
    return 0;
  const auto t = this->time_->now();
  return t.is_valid() ? (uint32_t) t.timestamp : 0;
}

int16_t EnclosureHistory::pack_(float v, float scale) const {
  if (std::isnan(v))
    return MISSING;
  const float x = roundf(v * scale);
  if (x > 32767.0f)
    return 32767;
  if (x < -32767.0f)
    return -32767;
  return (int16_t) x;
}

// Writes the mean of the open slot. A slot with no samples is left as it is:
// MISSING if it was cleared on entry, or the partial restored from flash.
void EnclosureHistory::commit_(Ring &r, bool reset) {
  const size_t idx = r.last_slot % r.slots;
  for (size_t s = 0; s < this->series_.size(); s++) {
    if (r.count[s] > 0)
      r.data[s * r.slots + idx] = this->pack_((float) (r.sum[s] / r.count[s]), this->series_[s].scale);
    if (reset) {
      r.sum[s] = 0.0;
      r.count[s] = 0;
    }
  }
}

void EnclosureHistory::advance_(Ring &r, uint32_t slot) {
  if (r.last_slot == 0) {
    r.last_slot = slot;
    return;
  }
  if (slot == r.last_slot)
    return;
  if (slot < r.last_slot) {
    // The clock went backwards (a bad SNTP answer, then a good one). Drop
    // what was being averaged and carry on from here.
    ESP_LOGW(TAG, "Clock moved back %u slots; restarting the open slot", (unsigned) (r.last_slot - slot));
    std::fill(r.sum.begin(), r.sum.end(), 0.0);
    std::fill(r.count.begin(), r.count.end(), 0);
    r.last_slot = slot;
    return;
  }
  this->commit_(r, true);
  // Every slot skipped -- and the new one -- is cleared: a ring index holds
  // its slot from one lap ago until then, and that must never read as now.
  const uint32_t clear = std::min(slot - r.last_slot, r.slots);
  for (uint32_t k = 1; k <= clear; k++) {
    const size_t idx = (r.last_slot + k) % r.slots;
    for (size_t s = 0; s < this->series_.size(); s++)
      r.data[s * r.slots + idx] = MISSING;
  }
  r.last_slot = slot;
}

void EnclosureHistory::update() {
  const uint32_t now = this->now_();
  if (now == 0)
    return;  // No clock yet: a sample with no time cannot be placed.
  this->advance_(this->fast_, now / this->fast_.step);
  this->advance_(this->slow_, now / this->slow_.step);
  for (size_t s = 0; s < this->series_.size(); s++) {
    const float v = this->series_[s].sensor->state;
    if (std::isnan(v))
      continue;  // A faulted sensor is a gap, not a zero.
    for (Ring *r : {&this->fast_, &this->slow_}) {
      r->sum[s] += v;
      r->count[s]++;
    }
  }
  if (this->last_save_ == 0) {
    this->last_save_ = now;
  } else if (now - this->last_save_ >= SAVE_EVERY_S) {
    this->save_ring_(this->fast_);
    this->save_ring_(this->slow_);
    global_preferences->sync();
    this->last_save_ = now;
  }
}

// OTA, the Restart button and a clean shutdown all come through here, so a
// planned reboot loses nothing.
void EnclosureHistory::on_shutdown() {
  this->save_ring_(this->fast_);
  this->save_ring_(this->slow_);
  global_preferences->sync();
}

bool EnclosureHistory::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() != HTTP_GET)
    return false;
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const auto url = request->url_to(url_buf);
  return url == "/history.json" || url == "/charts";
}

void EnclosureHistory::handleRequest(AsyncWebServerRequest *request) {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const auto url = request->url_to(url_buf);
  if (url == "/charts") {
    this->charts_page_(request);
  } else {
    this->history_json_(request, request->arg("range") == "month" ? this->slow_ : this->fast_);
  }
}

void EnclosureHistory::charts_page_(AsyncWebServerRequest *request) {
  auto *resp = request->beginResponse(200, "text/html", CHARTS_HTML);
  resp->addHeader("Cache-Control", "no-store");
  request->send(resp);
}

static void append_fixed(std::string &out, int16_t v, float scale) {
  if (v == EnclosureHistory::MISSING) {
    out += "null";
    return;
  }
  char b[16];
  snprintf(b, sizeof(b), "%g", v / scale);
  out += b;
}

// Labels and units come from this firmware's YAML; they never contain a
// quote or backslash (the schema keeps them short plain strings), but escape
// anyway so a future label cannot break the page's JSON.
static void append_str(std::string &out, const char *s) {
  out += '"';
  for (; *s; s++) {
    if (*s == '"' || *s == '\\')
      out += '\\';
    out += *s;
  }
  out += '"';
}

void EnclosureHistory::history_json_(AsyncWebServerRequest *request, Ring &r) {
  std::string out;
  out.reserve(64 + this->series_.size() * (80 + r.slots * 6));
  char b[96];
  snprintf(b, sizeof(b), "{\"now\":%u,\"step\":%u,\"slots\":%u,\"end\":%u,\"series\":[", (unsigned) this->now_(),
           (unsigned) r.step, (unsigned) r.slots, (unsigned) (r.last_slot * r.step));
  out += b;
  for (size_t s = 0; s < this->series_.size(); s++) {
    const Series &se = this->series_[s];
    if (s > 0)
      out += ',';
    out += "{\"key\":";
    append_str(out, se.key);
    out += ",\"label\":";
    append_str(out, se.label);
    out += ",\"chart\":";
    append_str(out, se.chart);
    out += ",\"unit\":";
    append_str(out, se.unit);
    out += ",\"data\":[";
    // Oldest first. The newest point is the open slot's running mean, so
    // the chart reaches now rather than ending up to an hour ago.
    for (uint32_t i = 0; i < r.slots; i++) {
      if (i > 0)
        out += ',';
      if (r.last_slot == 0) {
        out += "null";
        continue;
      }
      const uint32_t slot = r.last_slot - (r.slots - 1) + i;
      int16_t v = r.data[s * r.slots + slot % r.slots];
      if (i == r.slots - 1 && r.count[s] > 0)
        v = this->pack_((float) (r.sum[s] / r.count[s]), se.scale);
      append_fixed(out, v, se.scale);
    }
    out += "]}";
  }
  out += "]}";
  auto *resp = request->beginResponse(200, "application/json", out);
  resp->addHeader("Cache-Control", "no-store");
  request->send(resp);
}

void EnclosureHistory::dump_config() {
  ESP_LOGCONFIG(TAG, "Enclosure History:");
  ESP_LOGCONFIG(TAG, "  %u series; 24 h at 5 min, 30 d at 1 h; saved every %u s", (unsigned) this->series_.size(),
                (unsigned) SAVE_EVERY_S);
  for (const auto &s : this->series_)
    ESP_LOGCONFIG(TAG, "  - %s (x%g, %s)", s.key, s.scale, s.unit);
}

}  // namespace enclosure_history
}  // namespace esphome
