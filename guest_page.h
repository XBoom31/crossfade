#pragma once

// Single-file guest page. No frameworks, no CDN, no external requests.
// The server substitutes only __PARTY_NAME__ (HTML-escaped) and
// __ACCENT__ (validated #rrggbb) before serving.
static const char *kGuestPageHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0e1014">
<title>__PARTY_NAME__</title>
<style>
:root{--accent:__ACCENT__;--bg:#0e1014;--surface:#171a20;--surface2:#1e222a;--text:#f2f4f8;--dim:#a0a7b4;--muted:#6b7280;--line:#2a2f39;--ok:#63d49a;--warn:#f0b45c;--err:#ef7b7b;--radius:16px}
@media (prefers-color-scheme:light){:root{--bg:#f6f7f9;--surface:#fff;--surface2:#eef0f4;--text:#171a20;--dim:#596170;--muted:#7a8494;--line:#e2e5eb}}
*{box-sizing:border-box}
[hidden]{display:none!important}
html,body{margin:0;padding:0}
body{background:var(--bg);color:var(--text);font:16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;min-height:100vh;padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left);-webkit-text-size-adjust:100%;overflow-x:hidden;position:relative}
body::before{content:"";position:fixed;inset:0;z-index:0;pointer-events:none;background:radial-gradient(78vw 68vw at 108% -12%,color-mix(in srgb,var(--accent) 24%,transparent),transparent 72%),radial-gradient(70vw 60vw at -18% 104%,color-mix(in srgb,var(--accent) 13%,transparent),transparent 74%);opacity:.9}
@media (prefers-color-scheme:light){body::before{opacity:.5}}
.wrap{position:relative;z-index:1;max-width:620px;margin:0 auto;padding:16px 16px 40px}
header{display:flex;align-items:center;gap:10px;padding:6px 2px 16px}
header .dot{width:10px;height:10px;border-radius:50%;background:var(--accent);box-shadow:0 0 0 4px color-mix(in srgb,var(--accent) 18%,transparent),0 0 24px color-mix(in srgb,var(--accent) 55%,transparent)}
header h1{font-size:17px;font-weight:650;margin:0;letter-spacing:.01em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
header .tag{margin-left:auto;font-size:12px;color:var(--dim);background:color-mix(in srgb,var(--surface) 72%,transparent);border:1px solid color-mix(in srgb,var(--line) 80%,transparent);border-radius:999px;padding:4px 10px;white-space:nowrap;backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px)}
.card{background:linear-gradient(180deg,color-mix(in srgb,var(--surface) 94%,white 2%),color-mix(in srgb,var(--surface) 88%,transparent));border:1px solid color-mix(in srgb,var(--line) 84%,transparent);border-radius:var(--radius);padding:14px;margin-bottom:14px;box-shadow:0 16px 42px rgba(0,0,0,.18),inset 0 1px 0 rgba(255,255,255,.035);backdrop-filter:blur(16px) saturate(120%);-webkit-backdrop-filter:blur(16px) saturate(120%)}
@media (prefers-color-scheme:light){.card{box-shadow:0 14px 34px rgba(25,35,55,.08),inset 0 1px 0 rgba(255,255,255,.7)}}
.now{display:flex;gap:13px;align-items:center}
.art{width:76px;height:76px;flex:0 0 76px;border-radius:12px;background:linear-gradient(135deg,var(--surface2),color-mix(in srgb,var(--accent) 30%,var(--surface2)));object-fit:cover;display:block}
.meta{min-width:0;flex:1}
.eyebrow{font-size:11px;text-transform:uppercase;letter-spacing:.12em;color:var(--muted);margin-bottom:4px}
.title{font-size:17px;font-weight:650;margin:0;overflow:hidden;text-overflow:ellipsis;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;line-height:1.3}
.artist{font-size:13px;color:var(--dim);margin-top:2px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.progress{height:6px;border-radius:999px;background:var(--surface2);margin-top:12px;overflow:hidden}
.progress i{display:block;height:100%;width:0;background:var(--accent);border-radius:999px;transition:width .9s linear}
.times{display:flex;justify-content:space-between;font-size:11px;color:var(--muted);margin-top:5px;font-variant-numeric:tabular-nums}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.11em;color:var(--muted);margin:22px 2px 9px;font-weight:600}
.add{display:flex;gap:8px;align-items:stretch}
.searchwrap{position:relative;flex:1;min-width:0}
.searchwrap::before{content:"";position:absolute;left:15px;top:50%;width:10px;height:10px;border:2px solid var(--muted);border-radius:50%;transform:translateY(-62%);pointer-events:none;z-index:2;opacity:.85}
.searchwrap::after{content:"";position:absolute;left:24px;top:calc(50% + 4px);width:7px;height:2px;border-radius:2px;background:var(--muted);transform:rotate(45deg);pointer-events:none;z-index:2;opacity:.85}
.searchwrap input{width:100%}
input[type=text],input[type=url]{flex:1;min-width:0;height:48px;border-radius:12px;border:1px solid var(--line);background:color-mix(in srgb,var(--surface2) 88%,transparent);color:var(--text);padding:0 13px;font-size:16px;outline:none;transition:border-color .15s ease,background .15s ease}
input[type=text]:focus,input[type=url]:focus{border-color:color-mix(in srgb,var(--accent) 55%,var(--line));background:color-mix(in srgb,var(--surface2) 96%,transparent)}
.searchwrap input{padding-left:42px}
input:focus-visible,button:focus-visible,a:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
button{font:inherit;cursor:pointer;touch-action:manipulation}
.primary{height:48px;border:0;border-radius:12px;background:var(--accent);color:#08111a;font-weight:700;padding:0 16px;min-width:74px;transition:transform .12s ease,filter .12s ease}
.primary:active{transform:scale(.97)}
.primary:disabled{opacity:.55;cursor:default}
.ghost{height:44px;border:1px solid var(--line);border-radius:12px;background:transparent;color:var(--dim);padding:0 14px}
.hint{font-size:12px;color:var(--muted);margin:8px 2px 0}
.list{list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:8px}
.item{display:flex;gap:11px;align-items:center;background:var(--surface);border:1px solid var(--line);border-radius:13px;padding:9px;animation:in .22s ease both}
.item .art{width:46px;height:46px;flex-basis:46px;border-radius:9px}
.item .title{font-size:14px;-webkit-line-clamp:1;display:block;white-space:nowrap;text-overflow:ellipsis}
.item .artist{font-size:12px}
.item .right{margin-left:auto;text-align:right;flex:0 0 auto;max-width:92px}
.eta{font-size:11px;color:var(--muted);font-variant-numeric:tabular-nums}
.badge{display:inline-block;font-size:10px;border-radius:999px;padding:2px 7px;margin-top:3px;background:color-mix(in srgb,var(--accent) 16%,transparent);color:var(--accent);white-space:nowrap;max-width:92px;overflow:hidden;text-overflow:ellipsis}
.badge.pending{background:color-mix(in srgb,var(--warn) 17%,transparent);color:var(--warn)}
.empty{text-align:center;color:var(--muted);padding:26px 10px;border:1px dashed var(--line);border-radius:13px;font-size:14px}
.results{list-style:none;margin:8px 0 0;padding:0;display:flex;flex-direction:column;gap:6px}
.results li{animation:in .18s ease both}
.results button{width:100%;display:flex;text-align:left;gap:10px;align-items:center;background:color-mix(in srgb,var(--surface) 90%,transparent);border:1px solid var(--line);border-radius:11px;padding:8px;color:var(--text);transition:background .15s ease,border-color .15s ease,transform .12s ease}
.results button:hover{background:color-mix(in srgb,var(--accent) 7%,var(--surface));border-color:color-mix(in srgb,var(--accent) 30%,var(--line))}
.results button:active{transform:scale(.992)}
.results img{width:38px;height:38px;border-radius:8px;background:linear-gradient(135deg,var(--surface2),color-mix(in srgb,var(--accent) 26%,var(--surface2)));object-fit:cover;flex:0 0 38px}
.results .msg{padding:14px;text-align:center;color:var(--muted);font-size:13px;border:1px dashed var(--line);border-radius:11px;list-style:none}
img.art-fallback{opacity:.9;background:linear-gradient(135deg,var(--surface2),color-mix(in srgb,var(--accent) 24%,var(--surface2)))}
.results .rt{min-width:0;flex:1}
.results .rt b{display:block;font-size:13px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.results .rt span{display:block;font-size:11px;color:var(--dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.results .plus{font-size:20px;color:var(--accent);padding:0 6px}
.dialog{position:fixed;inset:0;background:rgba(0,0,0,.55);display:flex;align-items:flex-end;justify-content:center;padding:16px;z-index:20;padding-bottom:calc(16px + env(safe-area-inset-bottom))}
.dialog .card{width:100%;max-width:520px;margin:0;padding:18px}
.dialog h3{margin:0 0 5px;font-size:18px}
.dialog p{margin:0 0 13px;color:var(--dim);font-size:13px}
.dialog .row{display:flex;gap:8px;margin-top:10px}
.dialog .row .primary{flex:1}
.toast{position:fixed;left:50%;bottom:calc(18px + env(safe-area-inset-bottom));transform:translateX(-50%) translateY(120%);background:var(--surface);border:1px solid var(--line);color:var(--text);padding:10px 14px;border-radius:12px;font-size:14px;box-shadow:0 8px 30px rgba(0,0,0,.25);transition:transform .25s ease;z-index:30;max-width:calc(100vw - 32px);display:flex;align-items:center;gap:10px}
.toast.show{transform:translateX(-50%) translateY(0)}
.toast button{border:0;background:transparent;color:var(--accent);font-weight:700;padding:6px}
.ended{position:fixed;inset:0;background:var(--bg);display:none;align-items:center;justify-content:center;text-align:center;padding:28px;z-index:40}
.ended.show{display:flex}
.ended .big{font-size:40px;margin-bottom:10px}
.ended h2{font-size:20px;text-transform:none;letter-spacing:0;color:var(--text);margin:0 0 6px}
.ended p{color:var(--dim);margin:0;font-size:14px;max-width:320px}
.skeleton{height:66px;border-radius:13px;background:linear-gradient(90deg,var(--surface) 25%,var(--surface2) 50%,var(--surface) 75%);background-size:200% 100%;animation:sh 1.2s infinite linear;border:1px solid var(--line)}
@keyframes in{from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:none}}
@keyframes sh{from{background-position:200% 0}to{background-position:-200% 0}}
@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
@media (min-width:700px){.wrap{padding-top:26px}}
</style>
</head>
<body>
<div class="wrap">
<header><span class="dot" aria-hidden="true"></span><h1 id="partyTitle">__PARTY_NAME__</h1><span class="tag" id="guestTag">party</span></header>

<section class="card" aria-label="Now playing">
  <div class="now">
    <img class="art" id="nowArt" alt="" aria-hidden="true">
    <div class="meta">
      <div class="eyebrow">Now playing</div>
      <div class="title" id="nowTitle">Nothing playing</div>
      <div class="artist" id="nowArtist">Waiting for the host</div>
    </div>
  </div>
  <div class="progress" role="progressbar" aria-label="Track progress" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0" id="progWrap"><i id="prog"></i></div>
  <div class="times"><span id="pos">0:00</span><span id="dur">0:00</span></div>
</section>

<section aria-label="Add a song">
  <h2>Add a song</h2>
  <div class="add">
    <div class="searchwrap"><input id="addInput" type="text" inputmode="search" autocapitalize="off" autocomplete="off" autocorrect="off" spellcheck="false" placeholder="Search YouTube or paste a link" aria-label="Search YouTube or paste a link"></div>
    <button class="primary" id="addBtn" aria-label="Search YouTube">Search</button>
  </div>
  <div class="hint" id="addHint">Search by song, artist or album - or paste a YouTube link. One song at a time.</div>
  <ul class="results" id="results" aria-live="polite"></ul>
</section>

<h2>Up next</h2>
<ul class="list" id="queue" aria-live="polite">
  <li class="skeleton" aria-hidden="true"></li><li class="skeleton" aria-hidden="true"></li>
</ul>
</div>

<div class="dialog" id="nameDialog" role="dialog" aria-modal="true" aria-labelledby="nameTitle" hidden>
  <div class="card">
    <h3 id="nameTitle">What should we call you?</h3>
    <p>Optional — it only shows next to songs you add during this party.</p>
    <input id="nameInput" type="text" maxlength="24" autocomplete="off" autocapitalize="words" placeholder="Your name" aria-label="Display name">
    <div class="row"><button class="ghost" id="skipName">Skip</button><button class="primary" id="saveName">Save</button></div>
  </div>
</div>

<div class="toast" id="toast" role="status" aria-live="polite"><span id="toastText"></span><button id="undoBtn" hidden>Undo</button></div>

<div class="ended" id="ended" role="alert">
  <div><div class="big">🎉</div><h2>This party has ended</h2><p>Thanks for listening. Ask the host to start a new party and scan the QR code again.</p></div>
</div>

<script>
(function(){
"use strict";
var ACCENT="__ACCENT__";
var S={v:-1,data:null,timer:null,hidden:document.hidden,unchanged:0,recvAt:0,myIds:new Set(),queueSig:null,searchTimer:null,searchSeq:0,adding:false};
// Monotonic local clock. Progress must not depend on the phone's wall clock
// matching the host's: phones on a hotspot without internet often have a
// skewed clock, which made the position stay at 0:00 and then run late.
function mono(){return (window.performance&&performance.now?performance.now():Date.now())/1000;}
var $=function(id){return document.getElementById(id);};
function el(tag,attrs,text){var n=document.createElement(tag);if(attrs){for(var k in attrs){if(k==="class")n.className=attrs[k];else if(k==="src")n.src=attrs[k];else if(k==="alt")n.alt=attrs[k];else n.setAttribute(k,attrs[k]);}}if(text!==undefined&&text!==null)n.textContent=String(text);return n;}
function fmt(s){s=Math.max(0,Math.floor(s||0));var m=Math.floor(s/60);var x=s%60;return m+":"+(x<10?"0":"")+x;}
function mins(s){var m=Math.round((s||0)/60);if(m<1)return "under a minute";if(m===1)return "about 1 min";return "about "+m+" min";}
function toast(msg,undoId){$("toastText").textContent=msg;var b=$("undoBtn");b.hidden=!undoId;b.dataset.id=undoId||"";$("toast").classList.add("show");clearTimeout(S.toastTimer);S.toastTimer=setTimeout(function(){$("toast").classList.remove("show");},4000);}
function ended(){S.ended=true;document.getElementById("ended").classList.add("show");if(S.timer)clearTimeout(S.timer);S.timer=null;}
var ART_NOTE="data:image/svg+xml,"+encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#8a93a3" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18V6l9-2v12"/><circle cx="6.5" cy="18" r="2.5"/><circle cx="15.5" cy="16" r="2.5"/></svg>');
function isUrl(v){return /youtu\.?be|youtube\.com/i.test(v);}
// Search results can appear before the host has finished caching their
// thumbnails. Retry the local art endpoint a few times and fall back to the
// gradient placeholder instead of leaving a broken image behind.
function startArt(img,id){var token=String((Number(img.dataset.artToken)||0)+1);img.dataset.artToken=token;var tries=0;
function load(){if(img.dataset.artToken!==token)return;img.src="/api/art/"+encodeURIComponent(id)+(tries?"?r="+tries:"");}
img.onload=function(){if(img.dataset.artToken!==token)return;img.classList.remove("art-fallback");img.style.opacity="1";};
img.onerror=function(){if(img.dataset.artToken!==token)return;tries++;if(tries<=5){setTimeout(load,450*tries);}else{img.onerror=null;img.classList.add("art-fallback");img.src=ART_NOTE;img.style.opacity="1";}};
load();}
function artFor(id,cls){var img=el("img",{class:cls||"art",alt:""});img.loading="lazy";startArt(img,id);return img;}
function setArt(img,id){if(!img)return;if(!id){img.removeAttribute("src");delete img.dataset.artId;delete img.dataset.artToken;img.style.visibility="hidden";return;}
if(img.dataset.artId===id){img.style.visibility="visible";return;}
img.dataset.artId=id;img.style.visibility="visible";img.classList.remove("art-fallback");startArt(img,id);}
function badge(by,pending){if(pending)return el("span",{class:"badge pending"},"waiting for host");if(by)return el("span",{class:"badge"},by);return null;}
function renderNow(d){var n=d.now||{};setArt($("nowArt"),n.id||"");
$("nowTitle").textContent=n.title||"Nothing playing";$("nowArtist").textContent=n.artist||"Waiting for the host";
S.now=n;S.recvAt=mono();updateProgress();}
function updateProgress(){var n=S.now;if(!n||!n.durationSec){$("prog").style.width="0%";$("pos").textContent="0:00";$("dur").textContent="0:00";return;}
var p=(n.positionSec||0);if(!n.paused&&S.recvAt)p+=(mono()-S.recvAt);p=Math.max(0,Math.min(n.durationSec,p));
var pct=Math.min(100,(p/n.durationSec)*100);$("prog").style.width=pct+"%";$("progWrap").setAttribute("aria-valuenow",String(Math.round(pct)));$("pos").textContent=fmt(p);$("dur").textContent=fmt(n.durationSec);}
function queueSig(q){var s="";q.forEach(function(it){s+=it.id+"|"+it.status+"|"+it.by+"|"+(S.myIds.has(it.id)?"1":"0")+";";});return s;}
function renderQueue(d){var q=d.queue||[];var sig=queueSig(q);if(sig===S.queueSig)return;S.queueSig=sig;
var box=$("queue");box.textContent="";
if(!q.length){var e=el("li",{class:"empty"},"Nothing queued yet. Be the first to add a song!");box.appendChild(e);return;}
q.forEach(function(it){var li=el("li",{class:"item"});li.appendChild(artFor(it.id,"art"));
var m=el("div",{class:"meta"});m.appendChild(el("div",{class:"title"},it.title||"Unknown title"));m.appendChild(el("div",{class:"artist"},it.artist||"Unknown artist"));li.appendChild(m);
var r=el("div",{class:"right"});var eta=it.status==="pending"?"waiting for host":(it.etaSec>0?mins(it.etaSec):"next up");r.appendChild(el("div",{class:"eta"},eta));
if(S.myIds.has(it.id))r.appendChild(el("span",{class:"badge",title:"Added by you"},"you"));
var b=badge(it.by,it.status==="pending");if(b)r.appendChild(b);li.appendChild(r);box.appendChild(li);});}
function updateAddButton(){var v=$("addInput").value.trim();var b=$("addBtn");b.textContent=isUrl(v)?"Add":"Search";b.setAttribute("aria-label",b.textContent);}
function setResultsMsg(text){var box=$("results");box.textContent="";if(text){var li=el("li",{class:"msg"},text);box.appendChild(li);}}
function render(d){if(!d||d.unchanged)return;S.data=d;S.v=d.v;S.myIds=new Set((d.you&&d.you.ids)||[]);var p=d.party||{};if(p.name)$("partyTitle").textContent=p.name;var g=(d.guests!=null?d.guests:0);$("guestTag").textContent=g+(g===1?" guest":" guests");if(p.accent&&/^#[0-9a-fA-F]{6}$/.test(p.accent))document.documentElement.style.setProperty("--accent",p.accent);
var searchOn=p.allowSearch!==false;$("addInput").placeholder=searchOn?"Search YouTube or paste a link":"Paste a YouTube link";if(!searchOn)$("addHint").textContent="Paste a YouTube link to add one song.";else $("addHint").textContent="Search by song, artist or album - or paste a YouTube link. One song at a time.";updateAddButton();renderNow(d);renderQueue(d);}
function poll(ms){if(S.hidden||S.ended)return;S.timer=setTimeout(doPoll,ms);}
function doPoll(){S.timer=null;if(S.hidden||S.ended)return;var url="/api/state";if(S.v>=0)url+="?v="+encodeURIComponent(S.v);
fetch(url,{credentials:"same-origin",cache:"no-store"}).then(function(r){if(r.status===401||r.status===403||r.status===410){ended();return null;}if(!r.ok)throw new Error("http");return r.json();}).then(function(d){if(!d)return;if(d.unchanged){S.unchanged++;poll(S.unchanged>10?5000:2000);return;}S.unchanged=0;render(d);poll(2000);}).catch(function(){poll(Math.min(15000,2000*Math.pow(2,Math.min(3,S.unchanged++))));});}
function addSong(input){if(S.adding||!input.trim())return;S.adding=true;$("addBtn").disabled=true;
var body=JSON.stringify({input:input.trim()});
fetch("/api/add",{method:"POST",credentials:"same-origin",cache:"no-store",headers:{"Content-Type":"application/json","X-CF":"1"},body:body}).then(function(r){return r.json().then(function(j){return {s:r.status,j:j};});}).then(function(x){var j=x.j||{};if(x.s===200&&j.ok){var it=j.item||{};if(it.id)S.myIds.add(it.id);if(navigator.vibrate)try{navigator.vibrate(18);}catch(e){}
toast(j.status==="pending"?"Waiting for host approval":"Added"+(it.title?" · "+it.title:""),it.id||"");$("addInput").value="";setResultsMsg("");updateAddButton();S.v=-1;doPoll();}
else if(x.s===401||x.s===403||x.s===410){ended();}
else if(x.s===429){toast(j.message||"Slow down a little.",null);}
else{toast(j.message||"Couldn't add that song.",null);}
}).catch(function(){toast("Connection lost. Check the Wi-Fi and try again.",null);}).then(function(){S.adding=false;$("addBtn").disabled=false;});}
function undo(id){if(!id)return;fetch("/api/add/"+encodeURIComponent(id),{method:"DELETE",credentials:"same-origin",headers:{"X-CF":"1"}}).then(function(r){if(r.ok){toast("Removed",null);S.v=-1;doPoll();}else toast("That song already started.",null);}).catch(function(){toast("Could not undo.",null);});}
function doSearch(q){q=(q||"").trim();if(!q)return;var seq=++S.searchSeq;S.searching=true;setResultsMsg("Searching YouTube...");var url="/api/search?q="+encodeURIComponent(q);fetch(url,{credentials:"same-origin",cache:"no-store"}).then(function(r){if(r.status===429)throw new Error("slow");if(!r.ok)throw new Error("bad");return r.json();}).then(function(list){if(seq!==S.searchSeq)return;S.searching=false;renderResults(Array.isArray(list)?list:[]);}).catch(function(e){if(seq!==S.searchSeq)return;S.searching=false;setResultsMsg("Search is unavailable right now. Paste a link instead.");if(String(e.message)==="slow")toast("One search at a time, please.",null);});}
function renderResults(list){var box=$("results");box.textContent="";if(!list.length){setResultsMsg("No matches. Try another search or paste a link.");return;}
list.slice(0,10).forEach(function(it,i){var li=el("li");li.style.animationDelay=(i*18)+"ms";var b=el("button",{type:"button"});b.appendChild(artFor(it.id,""));var t=el("span",{class:"rt"});t.appendChild(el("b",null,it.title||"Unknown"));t.appendChild(el("span",null,(it.artist||"")+(it.durationSec?" · "+fmt(it.durationSec):"")));b.appendChild(t);b.appendChild(el("span",{class:"plus"},"+"));b.addEventListener("click",function(){addSong(it.id);});li.appendChild(b);box.appendChild(li);});}
function submitInput(){var v=$("addInput").value.trim();if(!v)return;clearTimeout(S.searchTimer);if(isUrl(v)){addSong(v);return;}if(S.data&&S.data.party&&S.data.party.allowSearch===false){toast("Search is off. Paste a YouTube link instead.",null);return;}doSearch(v);}
function onInput(){var v=$("addInput").value;updateAddButton();clearTimeout(S.searchTimer);if(!v.trim()){S.searchSeq++;setResultsMsg("");return;}if(isUrl(v)){setResultsMsg("");S.searchTimer=setTimeout(function(){addSong(v);},250);return;}if(S.data&&S.data.party&&S.data.party.allowSearch!==false&&v.trim().length>1){S.searchTimer=setTimeout(function(){doSearch(v.trim());},300);}else{setResultsMsg("");}}
function boot(){if(!sessionStorage.getItem("cf_greet")){sessionStorage.setItem("cf_greet","1");$("nameDialog").hidden=false;}
var saved=sessionStorage.getItem("cf_name");if(saved){$("nameInput").value=saved;}
$("saveName").addEventListener("click",function(){var n=$("nameInput").value.trim();if(!n){$("nameDialog").hidden=true;return;}fetch("/api/name",{method:"POST",credentials:"same-origin",headers:{"Content-Type":"application/json","X-CF":"1"},body:JSON.stringify({name:n})}).then(function(){$("nameDialog").hidden=true;sessionStorage.setItem("cf_name",n);S.v=-1;doPoll();}).catch(function(){$("nameDialog").hidden=true;});});
$("nameInput").addEventListener("keydown",function(e){if(e.key==="Enter"){e.preventDefault();$("saveName").click();}});
$("skipName").addEventListener("click",function(){$("nameDialog").hidden=true;});
$("addBtn").addEventListener("click",submitInput);
$("addInput").addEventListener("input",onInput);
$("addInput").addEventListener("keydown",function(e){if(e.key==="Enter"){e.preventDefault();submitInput();}});
$("undoBtn").addEventListener("click",function(){undo(this.dataset.id);});
document.addEventListener("visibilitychange",function(){S.hidden=document.hidden;if(!S.hidden&&!S.timer&&!document.getElementById("ended").classList.contains("show")){S.unchanged=0;doPoll();}});
updateProgress();setInterval(updateProgress,1000);
var p=new URLSearchParams(location.search);if(p.get("added"))toast("Added",null);
doPoll();}
if(document.readyState==="loading")document.addEventListener("DOMContentLoaded",boot);else boot();
})();
</script>
</body>
</html>)HTML";
