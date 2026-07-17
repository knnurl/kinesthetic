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
<title>Kinesthetic</title><style>
:root{--h:210;--ink:#070a10;--mist:#e6ebf7;--dim:#8d96ad;--line:rgba(255,255,255,.17);
--glass:rgba(16,20,30,.78);--btn:rgba(255,255,255,.07);--btntxt:#c3cbdf;
--inputbg:rgba(255,255,255,.05);--mcard:rgba(16,20,30,.95);--scrim:rgba(4,6,10,.62)}
html.light{--ink:#eef0f5;--mist:#1b2334;--dim:#5b6478;--line:rgba(15,25,50,.16);
--glass:rgba(255,255,255,.82);--btn:rgba(15,25,50,.06);--btntxt:#3c465e;
--inputbg:rgba(15,25,50,.05);--mcard:rgba(255,255,255,.96);--scrim:rgba(225,229,238,.66)}
html.light #moire{opacity:.35}
html.light .chip.on,html.light .power.on,html.light .tab.on,html.light .nbtn.on{color:hsl(var(--h),90%,30%)}
html.light .pill.ok{color:hsl(var(--h),80%,36%)}
html.light .read{color:hsl(var(--h),75%,38%)}
html.light .mname{text-shadow:0 0 22px hsla(var(--h),90%,55%,.3)}
html.light .qx{color:#b33;background:rgba(200,60,60,.08)}
html.light .card{box-shadow:0 8px 32px rgba(30,40,70,.12)}
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
.stopbtn{border:1px solid var(--line);border-radius:99px;padding:4px 14px;color:var(--btntxt);background:var(--btn);cursor:pointer;margin-top:-3px;transition:.2s}
.stopbtn:active{color:#fff;border-color:hsl(var(--h),85%,62%);box-shadow:0 0 14px hsla(var(--h),85%,55%,.4)}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px}
.grid4{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}
.chip{font-family:ui-monospace,Menlo,monospace;font-size:12px;letter-spacing:.12em;text-transform:uppercase;border:1px solid var(--line);border-radius:12px;padding:13px 0;color:var(--btntxt);background:var(--btn);text-align:center;transition:.25s}
.chip.on{color:#fff;border-color:hsl(var(--h),85%,62%);background:hsla(var(--h),80%,55%,.16);box-shadow:0 0 22px hsla(var(--h),85%,55%,.35),inset 0 0 18px hsla(var(--h),85%,60%,.12)}
.power{width:100%;font-family:ui-monospace,Menlo,monospace;font-size:14px;letter-spacing:.2em;text-transform:uppercase;border:1px solid var(--line);border-radius:14px;padding:16px;color:var(--btntxt);background:var(--btn);transition:.25s}
.power.on{color:#fff;border-color:hsl(var(--h),85%,62%);background:hsla(var(--h),80%,55%,.2);box-shadow:0 0 30px hsla(var(--h),85%,55%,.5),inset 0 0 24px hsla(var(--h),85%,60%,.15)}
.ti{width:100%;padding:11px;margin-top:4px;border-radius:10px;border:1px solid var(--line);background:var(--inputbg);color:var(--mist);font-size:14px;font-family:ui-monospace,Menlo,monospace}
.fr{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:10px}
.fr .ti{width:96px;margin-top:0;text-align:right}
.sub{color:var(--dim);font-size:12px;line-height:1.75}
.nbtn{font-family:ui-monospace,Menlo,monospace;font-size:12px;letter-spacing:.1em;text-transform:uppercase;border:1px solid var(--line);border-radius:10px;padding:11px;color:var(--btntxt);background:var(--btn);text-align:center;cursor:pointer}
.nbtn.on{color:#fff;border-color:hsl(var(--h),85%,62%);background:hsla(var(--h),80%,55%,.16)}
.save{width:100%;margin-top:14px;font-family:ui-monospace,Menlo,monospace;letter-spacing:.15em;text-transform:uppercase;border:0;border-radius:12px;padding:13px;color:#06121f;font-weight:700;background:hsl(var(--h),85%,62%)}
.qrow{display:flex;gap:8px;align-items:center;margin-top:8px}
.qrow select,.qrow input{font-family:ui-monospace,Menlo,monospace;font-size:13px;padding:9px;border-radius:9px;border:1px solid var(--line);background:var(--inputbg);color:var(--mist)}
.qrow select{flex:1}
.qrow input{width:74px;text-align:right}
.qx{border:1px solid var(--line);background:rgba(255,80,80,.08);color:#ff8a8a;border-radius:9px;padding:9px 12px;font-family:ui-monospace,monospace}
.addq{width:100%;margin-top:10px;border:1px dashed var(--line);background:transparent;color:var(--dim);border-radius:10px;padding:11px;font-family:ui-monospace,monospace;letter-spacing:.1em;text-transform:uppercase;font-size:11px}
.sw{width:auto;flex:0 0 auto}
.eyebrow{font-family:ui-monospace,Menlo,monospace;font-size:10px;letter-spacing:.18em;text-transform:uppercase;color:hsl(var(--h),60%,68%);margin-bottom:4px;display:block}
.chip,.tab,.power,.nbtn,.qx,.addq,.save,.sw{cursor:pointer}
.hbtn{width:24px;height:24px;flex:0 0 auto;display:flex;align-items:center;justify-content:center;border-radius:50%;border:1px solid var(--line);background:var(--btn);color:var(--btntxt);font-family:ui-monospace,Menlo,monospace;font-size:12px;cursor:pointer;transition:.2s}
.hbtn:hover,.hbtn:active{color:#fff;border-color:hsl(var(--h),85%,62%);box-shadow:0 0 14px hsla(var(--h),85%,55%,.4)}
.mwrap{position:fixed;inset:0;z-index:9;display:flex;align-items:center;justify-content:center;padding:18px;background:var(--scrim);backdrop-filter:blur(8px)}
.mcard{width:100%;max-width:420px;max-height:82vh;overflow:auto;background:var(--mcard);border:1px solid var(--line);border-radius:18px;padding:18px;box-shadow:0 20px 70px rgba(0,0,0,.45)}
.mrow{padding:11px 0;border-bottom:1px solid var(--line)}
.mrow:last-child{border-bottom:0}
.mrow b{font-family:ui-monospace,Menlo,monospace;font-size:12px;letter-spacing:.14em;text-transform:uppercase;color:var(--mist);display:block;margin-bottom:3px}
@media(prefers-reduced-motion:reduce){.chip,.power,.tab{transition:none}}
</style></head><body>
<canvas id="moire"></canvas>
<div class="wrap">
<div class="title">Kinesthetic</div>
<div class="pills"><span id="conn" class="pill">offline</span>
<span id="gs" class="pill" style="display:none">idle</span>
<span id="flt" class="pill">health ok</span>
<span id="nip" class="pill">net</span></div>

<div class="tabs">
 <div class="tab on" id="tc" onclick="tab('c')">Control</div>
 <div class="tab" id="tm" onclick="tab('m')">Motion</div>
 <div class="tab" id="tl" onclick="tab('l')" style="display:none">Light</div>
 <div class="tab" id="tf" onclick="tab('f')">Fleet</div>
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
  <input type="range" id="sp" min="-100" max="100" value="0" oninput="spShow(this.value)" onpointerdown="spDrag=true" onpointerup="spDrag=false" onchange="spDrag=false;lastSpSent=Date.now();const o={cmd:'speed',v:+this.value};cmd(o);if(fmir.checked)fleetSend(o)">
  <div class="ends"><span>&#9664; reverse</span><span class="stopbtn" onclick="spStop()">stop</span><span>forward &#9654;</span></div>
 </div>
 <div class="card">
  <div class="row"><label>Mode</label><div class="hbtn" onclick="showHelp()" title="What do these modes do?">?</div></div>
  <div class="grid4">
   <div class="chip" id="m0" onclick="modeCmd(0)">Manual</div>
   <div class="chip" id="m1" onclick="modeCmd(1)">Breathe</div>
   <div class="chip" id="m2" onclick="modeCmd(2)">Sweep</div>
   <div class="chip" id="m3" onclick="modeCmd(3)">Wander</div>
   <div class="chip" id="m4" onclick="modeCmd(4)">Tide</div>
   <div class="chip" id="m5" onclick="modeCmd(5)">Pendulum</div>
   <div class="chip" id="m6" onclick="modeCmd(6)">Heartbeat</div>
   <div class="chip" id="m7" onclick="modeCmd(7)">Stutter</div>
  </div>
 </div>
 <div class="card"><div class="power" id="en" onclick="tgl()">Motor enable</div></div>
 <div class="card">
  <span class="eyebrow">Mode queue</span>
  <div class="row"><label>Auto-play a sequence of modes</label><input id="qen" type="checkbox" class="sw"></div>
  <div id="qlist"></div>
  <button class="addq" onclick="addStep()">+ Add step</button>
  <button class="save" onclick="saveMotion()">Apply queue</button>
  <div class="sub" id="qnote" style="margin-top:8px">When on, the sculpture steps through this list and loops. It overrides manual mode selection.</div>
 </div>
</div>

<div id="pm" style="display:none">
 <div class="card">
  <span class="eyebrow">Presets</span>
  <label>One tap sets the feel, then save. Fine-tune below if you like.</label>
  <div class="grid4" style="margin-top:10px">
   <div class="chip" id="p_calm" onclick="preset('calm')">Calm</div>
   <div class="chip" id="p_balanced" onclick="preset('balanced')">Balanced</div>
   <div class="chip" id="p_lively" onclick="preset('lively')">Lively</div>
   <div class="chip" id="p_hypnotic" onclick="preset('hypnotic')">Hypnotic</div>
  </div>
 </div>
 <div class="card">
  <span class="eyebrow">Cycle durations</span>
  <label>How long each auto mode takes to complete one cycle</label>
  <div class="fr"><label>Breathe period</label><input id="bms" class="ti" type="number" min="2" max="600" oninput="markPreset()"> </div>
  <div class="fr"><label>Sweep period</label><input id="sms" class="ti" type="number" min="2" max="600" oninput="markPreset()"></div>
  <div class="fr"><label>Wander pace</label><input id="wms" class="ti" type="number" min="4" max="300" oninput="markPreset()"></div>
  <div class="fr"><label>Tide cycle (min)</label><input id="tmin" class="ti" type="number" min="1" max="60" oninput="markPreset()"></div>
  <div class="fr"><label>Pendulum swing</label><input id="pms" class="ti" type="number" min="4" max="120" oninput="markPreset()"></div>
  <div class="fr"><label>Heartbeat</label><input id="hbs" class="ti" type="number" min="2" max="20" oninput="markPreset()"></div>
  <div class="fr"><label>Stutter step (ms)</label><input id="sts" class="ti" type="number" min="100" max="4000" step="50" oninput="markPreset()"></div>
  <div class="sub" style="margin-top:6px">seconds, except tide (minutes) and stutter step (milliseconds)</div>
 </div>
 <div class="card">
  <span class="eyebrow">Speed profile</span>
  <label>How the motor eases between speeds, and how the curves dwell</label>
  <div class="fr"><label>Ramp up</label><input id="up" class="ti" type="number" step="0.1" min="0.1" max="3" oninput="markPreset()"></div>
  <div class="fr"><label>Wind down</label><input id="dn" class="ti" type="number" step="0.1" min="0.1" max="5" oninput="markPreset()"></div>
  <div class="fr"><label>Breathe dwell</label><input id="bsh" class="ti" type="number" step="0.5" min="1" max="8" oninput="markPreset()"></div>
  <div class="fr"><label>Sweep dwell</label><input id="ssh" class="ti" type="number" step="0.1" min="1" max="6" oninput="markPreset()"></div>
  <div class="fr"><label>Min auto speed</label><input id="flo" class="ti" type="number" min="0" max="1800"></div>
  <div class="sub" style="margin-top:6px">ramp times in seconds, dwell is a curve sharpness. Min auto speed (st/s) is the slowest the auto modes are allowed to run; 0 lets them come to a complete stop.</div>
 </div>
 <button class="save" onclick="saveMotion()">Save motion settings</button>
 <div class="sub" id="mnote" style="margin-top:10px;text-align:center"></div>
</div>

<div id="pl" style="display:none">
 <div class="card">
  <label>Light mode</label>
  <div class="grid4">
   <div class="chip" id="l0" onclick="ledMode(0)">Off</div>
   <div class="chip" id="l1" onclick="ledMode(1)">Solid</div>
   <div class="chip" id="l2" onclick="ledMode(2)">Glow</div>
   <div class="chip" id="l3" onclick="ledMode(3)">Chase</div>
   <div class="chip" id="l4" onclick="ledMode(4)">Rainbow</div>
  </div>
  <div class="sub" style="margin-top:8px">Glow breathes with the sculpture's real speed. Chase spins with the discs, opposite ways on each ring. Rainbow drifts on its own, rings counter-flowing.</div>
 </div>
 <div class="card">
  <div class="row"><label>Colour</label><span class="val" id="lhv">30&#176;</span></div>
  <input type="range" id="lhue" min="0" max="359" value="30" style="background:linear-gradient(90deg,#f43,#fb0,#5d5,#3cc,#36f,#c3f,#f43)" oninput="lhv.textContent=this.value+'\u00b0';ledSend()">
  <div class="row" style="margin-top:10px"><label>Brightness</label><span class="val" id="lbv">60%</span></div>
  <input type="range" id="lbri" min="0" max="100" value="60" oninput="lbv.textContent=this.value+'%';ledSend()">
  <div class="row" style="margin-top:10px"><label>Animation speed</label><span class="val" id="lrv">50</span></div>
  <input type="range" id="lrt" min="1" max="100" value="50" oninput="lrv.textContent=this.value;ledSend()">
  <div class="sub" style="margin-top:8px">Changes apply live and save automatically. Colour applies to Solid, Glow and Chase.</div>
 </div>
</div>

<div id="pf" style="display:none">
 <div class="card">
  <span class="eyebrow">Fleet</span>
  <div class="sub" style="margin:8px 0 12px">Command several sculptures from this one page. Add each one's address (e.g. <b>sculpture-3F2A.local</b> or its IP). If they share this sculpture's password, they unlock automatically.</div>
  <div id="flist"></div>
  <div class="row" style="margin-top:8px"><input class="ti" id="fhost" placeholder="sculpture-XXXX.local" style="flex:1;min-width:0" onkeydown="if(event.key=='Enter')fleetAdd()"><button class="nbtn" style="margin-left:8px" onclick="fleetAdd()">Add</button></div>
 </div>
 <div class="card">
  <div class="row"><label>Mirror my controls to the fleet</label><input id="fmir" type="checkbox" class="sw" onchange="localStorage.ks_fmir=this.checked?'1':''"></div>
  <div class="sub" style="margin:6px 0 12px">While on, every mode, speed, enable and light change you make here is sent to all connected sculptures too.</div>
  <div class="grid4">
   <div class="chip" onclick="fleetSend({cmd:'enable',v:true});cmd({cmd:'enable',v:true})">Enable all</div>
   <div class="chip" onclick="fleetSend({cmd:'enable',v:false});cmd({cmd:'enable',v:false})">Stop all</div>
  </div>
  <button class="save" style="margin-top:10px;white-space:nowrap;font-size:12px" onclick="fleetSyncNow()">Send mode + speed to all</button>
  <div class="sub" id="fnote" style="margin-top:8px"></div>
 </div>
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
  <span class="eyebrow">Appearance</span>
  <div class="grid2">
   <div class="nbtn" id="thD" onclick="setTheme('dark')">Dark</div>
   <div class="nbtn" id="thL" onclick="setTheme('light')">Light</div>
  </div>
  <div class="sub" style="margin-top:8px">Saved on this device.</div>
 </div>
 <div class="card">
  <span class="eyebrow">Interface password</span>
  <div class="sub" style="margin:8px 0 10px">Protects this control page. Leave blank and Save to remove it. The physical network-reset gesture also clears it.</div>
  <input class="ti" id="supw" type="password" placeholder="New password (blank = none)" style="width:100%;box-sizing:border-box;margin-bottom:8px">
  <button class="nbtn" onclick="setPass(supw.value)">Save password</button><span class="sub" id="spnote" style="margin-left:10px"></span>
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
  <div class="sub">Manual: speed and direction follow your input.<br>Breathe: slow sinusoidal swell, lingers low.<br>Sweep: ramps up, eases through zero, reverses.<br>Wander: organic drifting speed and direction.<br>Tide: minutes-long swell, direction turns each cycle.<br>Pendulum: rocks to and fro, fastest mid-swing.<br>Heartbeat: pulse, echo, long rest.<br>Tap the ? in the mode grid for fuller descriptions.</div>
  <label style="display:block;margin:14px 0 6px">Status pills (top)</label>
  <div class="sub">Connection, current gesture state, health (turns red on overtemp, driver comms, or sensor fault), and the active network mode and address.</div>
  <label style="display:block;margin:14px 0 6px">Reaching the interface</label>
  <div class="sub">Access point: join the sculpture's wifi, the page opens automatically.<br>Joined to your network: open the hostname above with .local, or the address in the network pill.</div>
 </div>
</div>
</div>

<div id="lock" class="mwrap" style="display:none;z-index:11">
 <div class="mcard" style="max-width:340px">
  <span class="eyebrow">Locked</span>
  <div class="sub" style="margin:10px 0 14px">This sculpture is password protected. Enter the interface password to take control.</div>
  <input class="ti" id="lkpw" type="password" placeholder="Password" style="width:100%;box-sizing:border-box" onkeydown="if(event.key=='Enter')doAuth()">
  <div class="sub" id="lkerr" style="color:#f66;min-height:16px;margin:6px 0"></div>
  <button class="save" onclick="doAuth()">Unlock</button>
  <div class="sub" style="margin-top:10px">Forgotten? Hold your hand over the sensor 15s (or hold the reset button) to factory-reset network settings and clear the password.</div>
 </div>
</div>

<div id="wel" class="mwrap" style="display:none;z-index:10">
 <div class="mcard">
  <span class="eyebrow">Welcome</span>
  <div class="mrow"><b>What this is</b><div class="sub">Two coaxial discs counter-rotate to weave a living moir&eacute; pattern. This page is its remote control: it lives on the sculpture itself, nothing is sent to any cloud.</div></div>
  <div class="mrow"><b>How to use it</b><div class="sub">The slider sets speed and direction. Pick a movement mode below it, or tap ? to read what each one does. The Mode queue plays modes in a timed sequence, and the Light tab drives the LED rings if fitted.</div></div>
  <div class="mrow"><b>Join your WiFi</b><div class="sub" style="margin-bottom:8px">On your home network the sculpture can fetch wireless updates and be reached at its own address.</div>
   <button class="nbtn" onclick="welWifi()">Open WiFi setup</button></div>
  <div class="mrow"><b>Protect it (optional)</b><div class="sub" style="margin-bottom:8px">Set a password so only you can control it. Anyone on the same network can otherwise open this page.</div>
   <input class="ti" id="wpw" type="password" placeholder="Choose a password" style="width:100%;box-sizing:border-box;margin-bottom:8px">
   <button class="nbtn" onclick="welPass()">Set password</button><span class="sub" id="wpnote" style="margin-left:10px"></span></div>
  <button class="save" style="margin-top:14px" onclick="welDone()">Start</button>
 </div>
</div>

<div id="modal" class="mwrap" style="display:none" onclick="if(event.target===this)hideHelp()">
 <div class="mcard">
  <div class="row"><span class="eyebrow" style="margin:0">Movement modes</span><div class="qx" onclick="hideHelp()">&times;</div></div>
  <div class="mrow"><b>Manual</b><div class="sub">You drive. The slider sets speed and direction directly; the sculpture holds whatever you set.</div></div>
  <div class="mrow"><b>Breathe</b><div class="sub">A slow swell up to the set speed and back down, like breathing. Never stops; lingers at the quiet end. One direction.</div></div>
  <div class="mrow"><b>Sweep</b><div class="sub">Ramps up to full speed, eases all the way down, then reverses. Slowest at each turn, so the pattern hangs at the reversal.</div></div>
  <div class="mrow"><b>Wander</b><div class="sub">Organic drifting. Speed and direction meander unpredictably, like something alive idly exploring. Never repeats.</div></div>
  <div class="mrow"><b>Tide</b><div class="sub">A very slow swell over minutes, turning direction each cycle like a tide. Made for long, ambient display; you rarely see it repeat.</div></div>
  <div class="mrow"><b>Pendulum</b><div class="sub">Rocks to and fro. Fastest mid-swing, pausing gracefully at each extreme, the opposite feel to Sweep.</div></div>
  <div class="mrow"><b>Heartbeat</b><div class="sub">A strong pulse, a softer echo, then a long rest. The rhythm reads as a heartbeat in the pattern.</div></div>
  <div class="mrow"><b>Stutter</b><div class="sub">Steps between fixed speeds, holding each for a moment before jumping to the next, with the odd direction flip. A mechanical, clockwork character.</div></div>
  <div class="mrow"><div class="sub">In every mode except Manual, the slider's distance from centre sets the top speed the movement builds to, and which side of centre sets its direction. The Mode queue below the enable button can play these in a timed sequence.</div></div>
 </div>
</div>

<script>
const MODES=['MANUAL','BREATHE','SWEEP','WANDER','TIDE','PENDULUM','HEARTBEAT','STUTTER'],HUE=[210,165,280,35,195,320,355,55];
// Q is the offline command queue. MQ is the mode queue playlist. They were one
// variable, so opening the Motion tab clobbered any commands queued while offline.
let w,en=false,staMode=false,Q=[],MQ=[],qi=-1,St={speed:0,mode:0,en:false};
let LM=2,ledLoaded=false,ledSeen=false;
let TK='',PW=localStorage.ks_pw||'',booted=false,spDrag=false,lastLedSent=0,lastMotionSave=0,lastSpSent=0;
function api(u){return fetch(u+(TK?'?t='+TK:''))}
function ledMode(m){LM=m;for(let i=0;i<5;i++)document.getElementById('l'+i).className='chip'+(i==m?' on':'');ledSend();}
function ledSend(){lastLedSent=Date.now();const o={cmd:'led',m:LM,hue:+lhue.value,bri:+lbri.value,rt:+lrt.value};cmd(o);if(fmir.checked)fleetSend(o);}
function loadLed(){api('/led').then(r=>r.json()).then(j=>{LM=j.m;lhue.value=j.hue;lbri.value=j.bri;lrt.value=j.rt;
 lhv.textContent=j.hue+'\u00b0';lbv.textContent=j.bri+'%';lrv.textContent=j.rt;
 for(let i=0;i<5;i++)document.getElementById('l'+i).className='chip'+(i==j.m?' on':'');ledLoaded=true;});}
function spStop(){sp.value=0;spShow(0);lastSpSent=Date.now();const o={cmd:'speed',v:0};cmd(o);if(fmir.checked)fleetSend(o);}
function spShow(v){v=+v;spv.textContent=v?((v>0?'+':'')+v+'% '+(v>0?'fwd':'rev')):'stop';}
function tab(t){let p={c:pc,m:pm,l:pl,f:pf,n:pn,h:ph},b={c:tc,m:tm,l:tl,f:tf,n:tn,h:th};for(let k in p){p[k].style.display=k==t?'block':'none';b[k].className='tab'+(k==t?' on':'');}if(t=='n')loadNet();if(t=='m')loadMotion();if(t=='l'&&!ledLoaded)loadLed();if(t=='f'){renderFleet();for(let h of FP)fleetConnect(h);}}
function boot(){flushQ();if(booted)return;booted=true;loadMotion();
 if(!localStorage.ks_seen){wel.style.display='flex';}}
// Flush queued commands only once the session is authorized; flushing in
// onopen fired before the auth handshake, so every queued command bounced
// off the gate with authfail and was lost.
function flushQ(){while(Q.length&&w&&w.readyState==1)w.send(JSON.stringify(Q.shift()));}
function doAuth(){lkerr.textContent='';if(w&&w.readyState==1)w.send(JSON.stringify({cmd:'auth',pw:lkpw.value}));}
function setPass(p,noteEl){cmd({cmd:'setpass',pw:p||''});PW=p||'';if(PW)localStorage.ks_pw=PW;else localStorage.removeItem('ks_pw');
 if(noteEl)flash(noteEl,p?'Password set':'Password removed','');}
function setTheme(t){document.documentElement.classList.toggle('light',t=='light');localStorage.ks_theme=t;
 thD.className='nbtn'+(t=='dark'?' on':'');thL.className='nbtn'+(t=='light'?' on':'');}
function welWifi(){welDone();tab('n');}
function welPass(){if(!wpw.value)return;setPass(wpw.value,wpnote);}
function welDone(){localStorage.ks_seen='1';wel.style.display='none';}
function connect(){
 w=new WebSocket('ws://'+location.hostname+':81');
 w.onopen=()=>{conn.textContent='online';conn.className='pill ok';};
 w.onclose=()=>{conn.textContent='offline';conn.className='pill';setTimeout(connect,1500)};
 w.onerror=()=>{try{w.close();}catch(e){}};  // mobile sockets half-die; force a clean reconnect
 w.onmessage=e=>{const t=JSON.parse(e.data);
  if(t.type=='hello'){
   if(t.auth){if(PW){w.send(JSON.stringify({cmd:'auth',pw:PW}));}else{lock.style.display='flex';}}
   else{TK=t.tok||'';lock.style.display='none';boot();}
   return;}
  if(t.type=='authok'){TK=t.tok||'';if(lkpw.value){PW=lkpw.value;localStorage.ks_pw=PW;}lock.style.display='none';lkpw.value='';boot();return;}
  if(t.type=='authfail'){if(PW&&lock.style.display=='none'){PW='';localStorage.removeItem('ks_pw');}
   lock.style.display='flex';lkerr.textContent=lkpw.value?'Wrong password':'';return;}
  if(t.type=='passset'){flash(spnote,PW?'Password set':'Password removed','');return;}
  if(t.type=='led'){if(Date.now()-lastLedSent>1200){LM=t.m;lhue.value=t.hue;lbri.value=t.bri;lrt.value=t.rt;
   lhv.textContent=t.hue+'\u00b0';lbv.textContent=t.bri+'%';lrv.textContent=t.rt;
   for(let i=0;i<5;i++)document.getElementById('l'+i).className='chip'+(i==t.m?' on':'');}return;}
  if(t.type=='netsaved'){note.textContent='Saved. Rebooting. Reconnect to your network, then open '+host.value+'.local';return;}
  if(t.type=='motionsaved'){flash(mnote,'Saved and applied','');flash(qnote,'Saved and applied',QHELP);if(Date.now()-lastMotionSave>1200)setTimeout(loadMotion,300);return;}
  if(t.type=='queueoff'){qen.checked=false;qi=-1;markQueueRow();flash(qnote,'Queue stopped: a mode was chosen directly',QHELP);return;}
  if(t.type=='fwstatus'){fwnote.textContent=t.s=='downloading'?'Downloading and installing, do not power off...':('Update failed: '+(t.m||'')); return;}
  if(t.type!='tele')return;
  St.speed=t.speed;St.mode=t.mode;St.en=t.enabled;
  document.documentElement.style.setProperty('--h',HUE[t.mode]||210);
  act.textContent=t.speed+' st/s';
  mname.textContent=MODES[t.mode]||'-';
  mstate.textContent=t.enabled?(t.speed==0?'holding':'running'):'standby';
  if(t.gesture=='idle'){gs.style.display='none';}else{gs.style.display='';gs.textContent=t.gesture.replace(/_/g,' ');gs.className='pill ok';}
  en=t.enabled;let eb=document.getElementById('en');eb.className='power'+(en?' on':'');eb.textContent=en?'Motor enabled':'Motor enable';
  for(let i=0;i<MODES.length;i++)document.getElementById('m'+i).className='chip'+(t.mode==i?' on':'');
  if(t.qi!==qi){qi=t.qi;markQueueRow();}
  if(t.leds&&!ledSeen){ledSeen=true;tl.style.display='';loadLed();}
  if(t.sl!==undefined&&!spDrag&&Date.now()-lastSpSent>1200&&document.activeElement!==sp&&+sp.value!==t.sl){sp.value=t.sl;spShow(t.sl);}
  let f=t.fault||{};let bad=f.tmc||f.otp||f.tof;
  flt.textContent=f.tmc?'TMC comm':f.otp?('OVERTEMP '+t.derate+'%'):f.tof?'ToF fault':'health ok';
  flt.className=bad?'pill bad':'pill ok';
  nip.textContent=t.netmode+' '+t.netip;
  if(reduce)draw();
 };
}
function cmd(o){
 if(w&&w.readyState==1){w.send(JSON.stringify(o));return;}
 Q=Q.filter(x=>x.cmd!=o.cmd);Q.push(o);  // coalesce: keep only the newest of each cmd
 if(Q.length>12)Q.shift();               // bound the backlog
}
function modeCmd(v){const o={cmd:'mode',v:v};cmd(o);if(fmir.checked)fleetSend(o);}
function tgl(){const o={cmd:'enable',v:!en};cmd(o);if(fmir.checked)fleetSend(o);}

// ---------- Fleet: command several sculptures from this page ----------
// Peers live in localStorage on this phone; sockets are plain extra WebSockets
// to each peer's :81. Peers protected with the same password unlock with it.
let FP=JSON.parse(localStorage.ks_fleet||'[]'),FS={},FST={};
function fleetSave(){localStorage.ks_fleet=JSON.stringify(FP);}
function fleetAdd(){let h=fhost.value.trim();if(!h||FP.includes(h))return;FP.push(h);fhost.value='';fleetSave();renderFleet();fleetConnect(h);}
function fleetDel(i){let h=FP[i];FP.splice(i,1);fleetSave();try{FS[h]&&FS[h].close();}catch(e){}delete FS[h];delete FST[h];renderFleet();}
function fleetConnect(h){
 if(FS[h]&&FS[h].readyState<2)return;
 FST[h]=0;renderFleet();
 let sk;try{sk=new WebSocket('ws://'+h+':81');}catch(e){return;}
 FS[h]=sk;
 sk.onopen=()=>{FST[h]=1;renderFleet();};
 sk.onclose=()=>{FST[h]=0;renderFleet();};
 sk.onerror=()=>{try{sk.close();}catch(e){}};
 sk.onmessage=e=>{let t;try{t=JSON.parse(e.data);}catch(x){return;}
  if(t.type=='hello'){if(t.auth){sk.send(JSON.stringify({cmd:'auth',pw:PW}));}else{FST[h]=2;renderFleet();}return;}
  if(t.type=='authok'){FST[h]=2;renderFleet();return;}
  if(t.type=='authfail'){FST[h]=3;renderFleet();return;}};
}
function renderFleet(){let html='';FP.forEach((h,i)=>{
 let st=FST[h]||0,col=st==2?'#4c8':st==1?'#cc4':st==3?'#f66':'#666',
     lbl=st==2?'ready':st==1?'connecting':st==3?'locked (wrong password)':'offline';
 html+='<div class="row" style="padding:7px 0;border-bottom:1px solid var(--line)">'
  +'<span style="width:9px;height:9px;border-radius:50%;background:'+col+';box-shadow:0 0 8px '+col+';flex:0 0 auto;margin-right:10px"></span>'
  +'<span style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">'+h+' <span class="sub">'+lbl+'</span></span>'
  +'<div class="qx" onclick="fleetDel('+i+')">&times;</div></div>';});
 flist.innerHTML=html||'<div class="sub">No sculptures added yet.</div>';}
function fleetSend(o){let n=0;for(let h of FP){let sk=FS[h];if(sk&&sk.readyState==1&&FST[h]==2){sk.send(JSON.stringify(o));n++;}}
 if(fnote)flash(fnote,n?('Sent to '+n+' sculpture'+(n>1?'s':'')):'No connected sculptures','');return n;}
function fleetSyncNow(){fleetSend({cmd:'mode',v:St.mode});fleetSend({cmd:'speed',v:+sp.value});}
setInterval(()=>{for(let h of FP){if(!FS[h]||FS[h].readyState>=2)fleetConnect(h);}},8000);

function setSta(v){staMode=v;document.getElementById('nmSta').className='nbtn'+(v?' on':'');document.getElementById('nmAp').className='nbtn'+(v?'':' on');staF.style.display=v?'block':'none';apF.style.display=v?'none':'block';}
function updS(){stF.style.display=us.checked?'block':'none';}
function loadNet(){api('/net').then(r=>r.json()).then(j=>{
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
function markQueueRow(){MQ.forEach((s,i)=>{let r=document.getElementById('qr'+i);
 if(r)r.style.borderColor=(i===qi&&qen.checked)?'hsl(var(--h),85%,62%)':'';});}
function renderQueue(){let h='';MQ.forEach((s,i)=>{
 let opts=MODES.map((m,k)=>'<option value="'+k+'"'+(k==s.m?' selected':'')+'>'+m+'</option>').join('');
 h+='<div class="qrow" id="qr'+i+'" style="border:1px solid transparent;border-radius:10px;padding:2px;transition:.25s"><select onchange="MQ['+i+'].m=+this.value"> '+opts+'</select>'
  +'<input type="number" min="1" max="3600" value="'+s.s+'" onchange="MQ['+i+'].s=+this.value"><div class="qx" onclick="delStep('+i+')">&times;</div></div>';});
 qlist.innerHTML=h;markQueueRow();}
function addStep(){if(MQ.length<8){MQ.push({m:1,s:60});renderQueue();}}
function delStep(i){MQ.splice(i,1);renderQueue();}
function loadMotion(){api('/motion').then(r=>r.json()).then(j=>{
 bms.value=j.bms;sms.value=j.sms;wms.value=j.wms;tmin.value=j.tmin;pms.value=j.pms;hbs.value=j.hbs;sts.value=j.sts;
 up.value=j.up;dn.value=j.dn;bsh.value=j.bsh;ssh.value=j.ssh;flo.value=j.flo;
 qen.checked=j.qen;MQ=(j.q||[]).map(a=>({m:a[0],s:a[1]}));renderQueue();markPreset();});}
function saveMotion(){
 let o={cmd:'motion',bms:+bms.value,sms:+sms.value,wms:+wms.value,tmin:+tmin.value,pms:+pms.value,hbs:+hbs.value,sts:+sts.value,
  up:+up.value,dn:+dn.value,bsh:+bsh.value,ssh:+ssh.value,flo:+flo.value,
  qen:qen.checked,q:MQ.map(s=>[s.m,s.s])};
 mnote.textContent='Saving...';qnote.textContent='Saving...';lastMotionSave=Date.now();cmd(o);
}
function showHelp(){modal.style.display='flex';}
function hideHelp(){modal.style.display='none';}
addEventListener('keydown',e=>{if(e.key=='Escape')hideHelp();});
const PRESETS={
 calm:{bms:34,sms:44,wms:64,tmin:14,pms:22,hbs:6,sts:1400,up:0.9,dn:1.8,bsh:5,ssh:3.2},
 balanced:{bms:20,sms:20,wms:28,tmin:8,pms:12,hbs:4,sts:900,up:0.5,dn:1.1,bsh:4,ssh:2.2},
 lively:{bms:10,sms:10,wms:14,tmin:4,pms:7,hbs:3,sts:450,up:0.3,dn:0.6,bsh:3,ssh:1.8},
 hypnotic:{bms:26,sms:16,wms:40,tmin:10,pms:16,hbs:5,sts:700,up:0.6,dn:0.9,bsh:6,ssh:2.6}
};
// Show a transient status in a note element, then restore its resting text.
const QHELP='When on, the sculpture steps through this list and loops. It overrides manual mode selection.';
function flash(el,msg,rest){el.textContent=msg;clearTimeout(el._t);el._t=setTimeout(()=>{el.textContent=rest;},4000);}
function markPreset(){for(const k in PRESETS){const p=PRESETS[k];
 const hit=(+bms.value==p.bms&&+sms.value==p.sms&&+wms.value==p.wms&&+tmin.value==p.tmin
  &&+pms.value==p.pms&&+hbs.value==p.hbs&&+sts.value==p.sts&&+up.value==p.up
  &&+dn.value==p.dn&&+bsh.value==p.bsh&&+ssh.value==p.ssh);
 document.getElementById('p_'+k).className='chip'+(hit?' on':'');}}
function preset(name){let p=PRESETS[name];if(!p)return;
 bms.value=p.bms;sms.value=p.sms;wms.value=p.wms;tmin.value=p.tmin;pms.value=p.pms;hbs.value=p.hbs;sts.value=p.sts;
 up.value=p.up;dn.value=p.dn;bsh.value=p.bsh;ssh.value=p.ssh;
 markPreset();saveMotion();}
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
 let v=Math.max(-1,Math.min(1,St.speed/1800));  // matches firmware FREQ_MAX
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
qen.onchange=markQueueRow;
setTheme(localStorage.ks_theme=='light'?'light':'dark');
fmir.checked=!!localStorage.ks_fmir;
renderFleet();
connect();   // boot() runs after the hello/auth handshake
</script></body></html>
)rawliteral";
