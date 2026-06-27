#pragma once
#include <Arduino.h>

// Web app served by the INPUT_TOF_WIFI build. Kept in a header on purpose:
// the Arduino IDE's ctags prototype generator does not understand C++11 raw
// string literals and tries to parse the JavaScript inside as C, which fails
// the build with errors like "'function' does not name a type". ctags only
// scans the main .ino for prototype generation, not #included files, so moving
// the literal here makes the problem disappear. Rule of thumb for this
// toolchain: every large PROGMEM web asset goes in a header, never the .ino.

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Kinetic</title><style>
:root{--h:210;--ink:#070a10;--mist:#dde3ef;--dim:#697287;--line:rgba(255,255,255,.09);--glass:rgba(10,13,20,.55)}
*{box-sizing:border-box;margin:0;-webkit-tap-highlight-color:transparent}
html,body{height:100%}
body{background:var(--ink);color:var(--mist);font-family:system-ui,-apple-system,sans-serif;overflow-x:hidden}
#moire{position:fixed;inset:0;width:100%;height:100%;display:block;z-index:0}
.wrap{position:relative;z-index:1;max-width:440px;margin:0 auto;padding:24px 16px 44px}
.title{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace;font-weight:400;font-size:13px;letter-spacing:.55em;text-transform:uppercase;color:var(--mist);text-shadow:0 0 18px hsla(var(--h),90%,60%,.55);padding-left:.55em}
.pills{display:flex;flex-wrap:wrap;gap:6px;margin:11px 0 16px}
.pill{font-family:ui-monospace,Menlo,monospace;font-size:10px;letter-spacing:.12em;text-transform:uppercase;padding:4px 9px;border-radius:99px;border:1px solid var(--line);background:var(--glass);color:var(--dim);backdrop-filter:blur(8px)}
.pill.ok{color:hsl(var(--h),80%,70%);border-color:hsla(var(--h),80%,60%,.35)}
.pill.bad{color:#ff6b6b;border-color:rgba(255,107,107,.4)}
.tabs{display:flex;gap:4px;margin-bottom:14px;background:var(--glass);border:1px solid var(--line);border-radius:12px;padding:4px;backdrop-filter:blur(10px)}
.tab{flex:1;text-align:center;font-family:ui-monospace,Menlo,monospace;font-size:11px;letter-spacing:.1em;text-transform:uppercase;color:var(--dim);padding:9px 0;border-radius:9px;transition:.2s}
.tab.on{color:#fff;background:hsla(var(--h),80%,55%,.18);box-shadow:inset 0 0 16px hsla(var(--h),85%,60%,.14)}
.card{background:var(--glass);border:1px solid var(--line);border-radius:18px;padding:16px;margin-bottom:12px;backdrop-filter:blur(16px) saturate(1.3);box-shadow:0 8px 40px rgba(0,0,0,.35)}
label{font-family:ui-monospace,Menlo,monospace;font-size:10px;letter-spacing:.14em;text-transform:uppercase;color:var(--dim)}
.row{display:flex;gap:8px;align-items:center;justify-content:space-between}
.hero{text-align:center;padding:24px 16px}
.mname{font-family:ui-monospace,Menlo,monospace;font-size:30px;font-weight:600;letter-spacing:.22em;text-transform:uppercase;color:var(--mist);text-shadow:0 0 30px hsla(var(--h),90%,60%,.7);line-height:1}
.mstate{font-family:ui-monospace,Menlo,monospace;font-size:11px;letter-spacing:.2em;text-transform:uppercase;color:var(--dim);margin-top:11px}
.read{font-family:ui-monospace,Menlo,monospace;font-size:13px;color:hsl(var(--h),70%,72%);margin-top:6px;font-variant-numeric:tabular-nums}
.val{font-family:ui-monospace,Menlo,monospace;font-variant-numeric:tabular-nums;color:var(--mist);font-size:13px}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:3px;border-radius:3px;margin:16px 0 8px;background:linear-gradient(90deg,hsla(var(--h),70%,60%,.22),hsla(var(--h),70%,60%,.55) 50%,hsla(var(--h),70%,60%,.22))}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:radial-gradient(circle at 35% 30%,#fff,hsl(var(--h),85%,60%));box-shadow:0 0 22px hsla(var(--h),90%,60%,.85),0 0 4px #fff;cursor:pointer}
input[type=range]::-moz-range-thumb{width:24px;height:24px;border:0;border-radius:50%;background:hsl(var(--h),85%,62%);box-shadow:0 0 22px hsla(var(--h),90%,60%,.85)}
.ends{display:flex;justify-content:space-between}
.ends span{font-family:ui-monospace,Menlo,monospace;font-size:9px;letter-spacing:.1em;text-transform:uppercase;color:var(--dim)}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px}
.grid4{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}
.chip{font-family:ui-monospace,Menlo,monospace;font-size:12px;letter-spacing:.12em;text-transform:uppercase;border:1px solid var(--line);border-radius:12px;padding:13px 0;color:var(--dim);background:rgba(255,255,255,.02);text-align:center;transition:.25s}
.chip.on{color:#fff;border-color:hsl(var(--h),85%,62%);background:hsla(var(--h),80%,55%,.16);box-shadow:0 0 22px hsla(var(--h),85%,55%,.35),inset 0 0 18px hsla(var(--h),85%,60%,.12)}
.power{width:100%;font-family:ui-monospace,Menlo,monospace;font-size:14px;letter-spacing:.2em;text-transform:uppercase;border:1px solid var(--line);border-radius:14px;padding:16px;color:var(--dim);background:rgba(255,255,255,.02);transition:.25s}
.power.on{color:#fff;border-color:hsl(var(--h),85%,62%);background:hsla(var(--h),80%,55%,.2);box-shadow:0 0 30px hsla(var(--h),85%,55%,.5),inset 0 0 24px hsla(var(--h),85%,60%,.15)}
.ti{width:100%;padding:11px;margin-top:4px;border-radius:10px;border:1px solid var(--line);background:rgba(0,0,0,.35);color:var(--mist);font-size:14px;font-family:ui-monospace,Menlo,monospace}
.fr{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:10px}
.fr .ti{width:96px;margin-top:0;text-align:right}
.sub{color:var(--dim);font-size:12px;line-height:1.75}
.nbtn{font-family:ui-monospace,Menlo,monospace;font-size:12px;letter-spacing:.1em;text-transform:uppercase;border:1px solid var(--line);border-radius:10px;padding:11px;color:var(--dim);background:rgba(255,255,255,.02);text-align:center}
.nbtn.on{color:#fff;border-color:hsl(var(--h),85%,62%);background:hsla(var(--h),80%,55%,.16)}
.save{width:100%;margin-top:14px;font-family:ui-monospace,Menlo,monospace;letter-spacing:.15em;text-transform:uppercase;border:0;border-radius:12px;padding:13px;color:#06121f;font-weight:700;background:hsl(var(--h),85%,62%)}
.qrow{display:flex;gap:8px;align-items:center;margin-top:8px}
.qrow select,.qrow input{font-family:ui-monospace,Menlo,monospace;font-size:13px;padding:9px;border-radius:9px;border:1px solid var(--line);background:rgba(0,0,0,.35);color:var(--mist)}
.qrow select{flex:1}
.qrow input{width:74px;text-align:right}
.qx{border:1px solid var(--line);background:rgba(255,80,80,.08);color:#ff8a8a;border-radius:9px;padding:9px 12px;font-family:ui-monospace,monospace}
.addq{width:100%;margin-top:10px;border:1px dashed var(--line);background:transparent;color:var(--dim);border-radius:10px;padding:11px;font-family:ui-monospace,monospace;letter-spacing:.1em;text-transform:uppercase;font-size:11px}
.sw{width:auto;flex:0 0 auto}
.eyebrow{font-family:ui-monospace,Menlo,monospace;font-size:10px;letter-spacing:.18em;text-transform:uppercase;color:hsl(var(--h),60%,68%);margin-bottom:4px;display:block}
@media(prefers-reduced-motion:reduce){.chip,.power,.tab{transition:none}}
</style></head><body>
<canvas id="moire"></canvas>
<div class="wrap">
<div class="title">Kinetic</div>
<div class="pills"><span id="conn" class="pill">offline</span>
<span id="gs" class="pill">idle</span>
<span id="flt" class="pill">health ok</span>
<span id="nip" class="pill">net</span></div>

<div class="tabs">
 <div class="tab on" id="tc" onclick="tab('c')">Control</div>
 <div class="tab" id="tm" onclick="tab('m')">Motion</div>
 <div class="tab" id="tn" onclick="tab('n')">Setup</div>
 <div class="tab" id="th" onclick="tab('h')">Help</div>
</div>

<div id="pc">
 <div class="card hero">
  <div class="mname" id="mname">MANUAL</div>
  <div class="mstate" id="mstate">standby</div>
  <div class="read" id="act">0 st/s</div>
 </div>
 <div class="card">
  <div class="row"><label>Speed / Direction</label><span class="val" id="spv">stop</span></div>
  <input type="range" id="sp" min="-100" max="100" value="0" oninput="spShow(this.value)" onchange="cmd({cmd:'speed',v:+this.value})">
  <div class="ends"><span>&#9664; reverse</span><span>stop</span><span>forward &#9654;</span></div>
 </div>
 <div class="card">
  <label>Mode</label>
  <div class="grid4">
   <div class="chip" id="m0" onclick="cmd({cmd:'mode',v:0})">Manual</div>
   <div class="chip" id="m1" onclick="cmd({cmd:'mode',v:1})">Breathe</div>
   <div class="chip" id="m2" onclick="cmd({cmd:'mode',v:2})">Sweep</div>
   <div class="chip" id="m3" onclick="cmd({cmd:'mode',v:3})">Wander</div>
  </div>
 </div>
 <div class="card"><div class="power" id="en" onclick="tgl()">Motor enable</div></div>
</div>

<div id="pm" style="display:none">
 <div class="card">
  <span class="eyebrow">Cycle durations</span>
  <label>How long each auto mode takes to complete one cycle</label>
  <div class="fr"><label>Breathe period</label><input id="bms" class="ti" type="number" min="2" max="600"> </div>
  <div class="fr"><label>Sweep period</label><input id="sms" class="ti" type="number" min="2" max="600"></div>
  <div class="fr"><label>Wander pace</label><input id="wms" class="ti" type="number" min="4" max="300"></div>
  <div class="sub" style="margin-top:6px">seconds</div>
 </div>
 <div class="card">
  <span class="eyebrow">Speed profile</span>
  <label>How the motor eases between speeds, and how the curves dwell</label>
  <div class="fr"><label>Ramp up</label><input id="up" class="ti" type="number" step="0.1" min="0.1" max="3"></div>
  <div class="fr"><label>Wind down</label><input id="dn" class="ti" type="number" step="0.1" min="0.1" max="5"></div>
  <div class="fr"><label>Breathe dwell</label><input id="bsh" class="ti" type="number" step="0.5" min="1" max="8"></div>
  <div class="fr"><label>Sweep dwell</label><input id="ssh" class="ti" type="number" step="0.1" min="1" max="6"></div>
  <div class="sub" style="margin-top:6px">ramp times in seconds, dwell is a curve sharpness</div>
 </div>
 <div class="card">
  <span class="eyebrow">Mode queue</span>
  <div class="row"><label>Auto-play a sequence of modes</label><input id="qen" type="checkbox" class="sw"></div>
  <div id="qlist"></div>
  <button class="addq" onclick="addStep()">+ Add step</button>
  <div class="sub" style="margin-top:8px">When on, the sculpture steps through this list and loops. It overrides manual mode selection.</div>
 </div>
 <button class="save" onclick="saveMotion()">Save motion settings</button>
 <div class="sub" id="mnote" style="margin-top:10px;text-align:center"></div>
</div>

<div id="pn" style="display:none">
 <div class="card">
  <div class="grid2">
   <div class="nbtn" id="nmAp" onclick="setSta(false)">Access point</div>
   <div class="nbtn" id="nmSta" onclick="setSta(true)">Join wifi</div>
  </div>
  <div id="staF" style="margin-top:12px;display:none">
   <label>Home wifi name</label><input id="ssid" class="ti">
   <label>Home wifi password</label><input id="pass" class="ti" type="password" placeholder="(unchanged)">
   <div class="row" style="margin-top:10px"><label>Use static IP</label><input id="us" type="checkbox" class="sw" onchange="updS()"></div>
   <div id="stF" style="display:none">
    <input id="ip" class="ti" placeholder="IP e.g. 192.168.1.50">
    <input id="gw" class="ti" placeholder="Gateway e.g. 192.168.1.1">
    <input id="mask" class="ti" placeholder="Mask e.g. 255.255.255.0">
   </div>
  </div>
  <div id="apF" style="margin-top:12px">
   <label>Access point name</label><input id="apssid" class="ti">
   <label>Access point password (8+ chars, blank = open)</label><input id="appass" class="ti" type="password" placeholder="(unchanged)">
   <label>Access point IP</label><input id="apip" class="ti">
  </div>
  <label style="margin-top:12px;display:block">Hostname (reach it at name.local)</label>
  <input id="host" class="ti">
  <button class="save" onclick="saveNet()">Save &amp; reboot</button>
  <div class="sub" id="note" style="margin-top:10px"></div>
 </div>
 <div class="card">
  <span class="eyebrow">Firmware</span>
  <div class="row"><label>Installed version</label><span class="val" id="fwver">-</span></div>
  <label style="margin-top:10px;display:block">Update from URL (a .bin, e.g. a GitHub release)</label>
  <input id="fwurl" class="ti" placeholder="https://github.com/you/repo/releases/latest/download/firmware.bin">
  <button class="save" onclick="doUpdate()">Download &amp; install</button>
  <div class="sub" id="fwnote" style="margin-top:10px">Needs the sculpture joined to an internet network (Join wifi above). It downloads, installs, and reboots. Keep it powered.</div>
 </div>
</div>

<div id="ph" style="display:none">
 <div class="card">
  <label style="display:block;margin-bottom:6px">Hand gestures (ToF sensor, hold still then withdraw)</label>
  <div class="sub">Move hand more than 3cm: speed control (closer = slower, bidirectional).<br>Hold still 2 to 5s, then withdraw: next mode.<br>Hold still 5 to 15s, then withdraw: enable / disable.<br>Hold still over 15s, then withdraw: reset network to the default access point.</div>
  <label style="display:block;margin:14px 0 6px">Modes</label>
  <div class="sub">Manual: speed and direction follow your input.<br>Breathe: slow sinusoidal swell, lingers low.<br>Sweep: ramps up, eases through zero, reverses.<br>Wander: organic drifting speed and direction.</div>
  <label style="display:block;margin:14px 0 6px">Status pills (top)</label>
  <div class="sub">Connection, current gesture state, health (turns red on overtemp, driver comms, or sensor fault), and the active network mode and address.</div>
  <label style="display:block;margin:14px 0 6px">Reaching the interface</label>
  <div class="sub">Access point: join the sculpture's wifi, the page opens automatically.<br>Joined to your network: open the hostname above with .local, or the address in the network pill.</div>
 </div>
</div>
</div>

<script>
const MODES=['MANUAL','BREATHE','SWEEP','WANDER'],HUE=[210,165,280,35];
let w,en=false,staMode=false,Q=[],St={speed:0,mode:0,en:false};
function spShow(v){v=+v;spv.textContent=v?((v>0?'+':'')+v+'% '+(v>0?'fwd':'rev')):'stop';}
function tab(t){let p={c:pc,m:pm,n:pn,h:ph},b={c:tc,m:tm,n:tn,h:th};for(let k in p){p[k].style.display=k==t?'block':'none';b[k].className='tab'+(k==t?' on':'');}if(t=='n')loadNet();if(t=='m')loadMotion();}
function connect(){
 w=new WebSocket('ws://'+location.hostname+':81');
 w.onopen=()=>{conn.textContent='online';conn.className='pill ok'};
 w.onclose=()=>{conn.textContent='offline';conn.className='pill';setTimeout(connect,1500)};
 w.onmessage=e=>{const t=JSON.parse(e.data);
  if(t.type=='netsaved'){note.textContent='Saved. Rebooting. Reconnect to your network, then open '+host.value+'.local';return;}
  if(t.type=='motionsaved'){mnote.textContent='Saved and applied';return;}
  if(t.type=='fwstatus'){fwnote.textContent=t.s=='downloading'?'Downloading and installing, do not power off...':('Update failed: '+(t.m||'')); return;}
  if(t.type!='tele')return;
  St.speed=t.speed;St.mode=t.mode;St.en=t.enabled;
  document.documentElement.style.setProperty('--h',HUE[t.mode]||210);
  act.textContent=t.speed+' st/s';
  mname.textContent=MODES[t.mode]||'-';
  mstate.textContent=t.enabled?(t.speed==0?'holding':'running'):'standby';
  gs.textContent=t.gesture;gs.className='pill ok';
  en=t.enabled;let eb=document.getElementById('en');eb.className='power'+(en?' on':'');eb.textContent=en?'Motor enabled':'Motor enable';
  for(let i=0;i<4;i++)document.getElementById('m'+i).className='chip'+(t.mode==i?' on':'');
  let f=t.fault||{};let bad=f.tmc||f.otp||f.tof;
  flt.textContent=f.tmc?'TMC comm':f.otp?('OVERTEMP '+t.derate+'%'):f.tof?'ToF fault':'health ok';
  flt.className=bad?'pill bad':'pill ok';
  nip.textContent=t.netmode+' '+t.netip;
  if(reduce)draw();
 };
}
function cmd(o){if(w&&w.readyState==1)w.send(JSON.stringify(o))}
function tgl(){cmd({cmd:'enable',v:!en})}
function setSta(v){staMode=v;document.getElementById('nmSta').className='nbtn'+(v?' on':'');document.getElementById('nmAp').className='nbtn'+(v?'':' on');staF.style.display=v?'block':'none';apF.style.display=v?'none':'block';}
function updS(){stF.style.display=us.checked?'block':'none';}
function loadNet(){fetch('/net').then(r=>r.json()).then(j=>{
 ssid.value=j.ssid;apssid.value=j.apssid;apip.value=j.apip;host.value=j.host;
 ip.value=j.ip;gw.value=j.gw;mask.value=j.mask;us.checked=j.static;updS();setSta(j.sta);
 fwver.textContent=j.fwver||'-';fwurl.value=j.fwurl||'';
 note.textContent='Now: '+j.mode+' '+j.cur;});}
function doUpdate(){if(!fwurl.value){fwnote.textContent='Enter a .bin URL first';return;}fwnote.textContent='Starting...';cmd({cmd:'fwupdate',url:fwurl.value});}
function saveNet(){
 let o={cmd:'netcfg',sta:staMode,ssid:ssid.value,apssid:apssid.value,apip:apip.value,host:host.value,
  static:us.checked,ip:ip.value,gw:gw.value,mask:mask.value};
 if(pass.value)o.pass=pass.value;if(appass.value)o.appass=appass.value;
 note.textContent='Saving...';cmd(o);
}
function renderQueue(){let h='';Q.forEach((s,i)=>{
 let opts=MODES.map((m,k)=>'<option value="'+k+'"'+(k==s.m?' selected':'')+'>'+m+'</option>').join('');
 h+='<div class="qrow"><select onchange="Q['+i+'].m=+this.value"> '+opts+'</select>'
  +'<input type="number" min="1" max="3600" value="'+s.s+'" onchange="Q['+i+'].s=+this.value"><div class="qx" onclick="delStep('+i+')">&times;</div></div>';});
 qlist.innerHTML=h;}
function addStep(){if(Q.length<8){Q.push({m:1,s:60});renderQueue();}}
function delStep(i){Q.splice(i,1);renderQueue();}
function loadMotion(){fetch('/motion').then(r=>r.json()).then(j=>{
 bms.value=j.bms;sms.value=j.sms;wms.value=j.wms;up.value=j.up;dn.value=j.dn;bsh.value=j.bsh;ssh.value=j.ssh;
 qen.checked=j.qen;Q=(j.q||[]).map(a=>({m:a[0],s:a[1]}));renderQueue();});}
function saveMotion(){
 let o={cmd:'motion',bms:+bms.value,sms:+sms.value,wms:+wms.value,up:+up.value,dn:+dn.value,bsh:+bsh.value,ssh:+ssh.value,
  qen:qen.checked,q:Q.map(s=>[s.m,s.s])};
 mnote.textContent='Saving...';cmd(o);
}
const cv=document.getElementById('moire'),cx=cv.getContext('2d');
const reduce=matchMedia('(prefers-reduced-motion:reduce)').matches;
let pa=0,pb=0,dpr=Math.min(devicePixelRatio||1,2),raf;
function resize(){cv.width=innerWidth*dpr;cv.height=innerHeight*dpr;}
function disc(x,y,ri,ro,ph,hue,al){
 const N=30;
 for(let i=0;i<N;i++){let a=ph+i/N*6.2832;
  cx.beginPath();cx.moveTo(x+Math.cos(a)*ri,y+Math.sin(a)*ri);
  cx.lineTo(x+Math.cos(a)*ro,y+Math.sin(a)*ro);
  cx.strokeStyle='hsla('+hue+',88%,62%,'+al+')';cx.lineWidth=1.1*dpr;cx.stroke();}
 for(let r=1;r<=4;r++){let rr=ri+(ro-ri)*r/4;
  cx.beginPath();cx.arc(x,y,rr,0,6.2832);
  cx.strokeStyle='hsla('+hue+',80%,58%,'+(al*0.6)+')';cx.lineWidth=1*dpr;cx.stroke();}
}
function draw(){
 const ww=cv.width,hh=cv.height,x=ww/2,y=hh*0.42,ro=Math.hypot(ww,hh)*0.55,ri=ro*0.06;
 cx.clearRect(0,0,ww,hh);
 let v=Math.max(-1,Math.min(1,St.speed/4000));
 if(!reduce){pa+=v*0.05;pb-=v*0.05;}
 let hue=HUE[St.mode]||210,al=(St.en?1:0.28);
 cx.globalCompositeOperation='lighter';
 disc(x,y,ri,ro,pa,hue,0.05*al);
 disc(x,y,ri,ro,pb,hue+26,0.05*al);
 cx.globalCompositeOperation='source-over';
 let g=cx.createRadialGradient(x,y,0,x,y,ro);
 g.addColorStop(0,'rgba(7,10,16,0)');g.addColorStop(0.62,'rgba(7,10,16,0)');g.addColorStop(1,'rgba(7,10,16,0.85)');
 cx.fillStyle=g;cx.fillRect(0,0,ww,hh);
}
function loop(){draw();raf=requestAnimationFrame(loop);}
addEventListener('resize',()=>{resize();draw();});
document.addEventListener('visibilitychange',()=>{if(document.hidden)cancelAnimationFrame(raf);else if(!reduce)loop();});
resize();draw();if(!reduce)loop();
connect();
</script></body></html>
)rawliteral";
