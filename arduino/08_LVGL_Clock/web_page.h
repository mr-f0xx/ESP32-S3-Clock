#pragma once

static const char CLOCK_WEB_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
  <title>AMOLED Clock</title>
  <style>
    :root{color-scheme:dark;--bg:#000;--card:#0b0b0b;--line:#303030;--text:#fff;--muted:#aaa}
    *{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
    main{width:min(100%,520px);margin:auto;padding:16px 14px 30px}header{display:flex;align-items:center;justify-content:space-between;margin:2px 2px 14px}
    h1{font-size:22px;margin:0}.state{font-size:12px;color:var(--muted);max-width:55%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
    .card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:14px;margin:12px 0}h2{font-size:17px;margin:0 0 12px}
    input,select,button{width:100%;border:1px solid #3a3a3a;border-radius:10px;background:#050505;color:var(--text);padding:11px 12px;font-size:15px}
    button{border:1px solid #fff;background:#fff;color:#000;font-weight:700;cursor:pointer}button:disabled{opacity:.45}.danger,.ghost{background:#151515;color:#fff;border-color:#444}
    label{display:block;color:#ddd;font-size:13px;margin:10px 0 5px}.row{display:grid;grid-template-columns:1fr 1fr;gap:8px}.modes button{background:#151515;color:#fff;border-color:#444}.modes button.active{background:#fff;color:#000;border-color:#fff}
    .crop{position:relative;width:min(100%,420px);aspect-ratio:1;margin:12px auto;background:#000;border-radius:12px;overflow:hidden;touch-action:none}
    .crop:after{content:"";position:absolute;inset:0;pointer-events:none;border:2px solid #d9f3ffb8;border-radius:12px;box-shadow:inset 0 0 0 1px #0008}
    canvas{display:block;width:100%;height:100%;background:#000}.zoomline{display:grid;grid-template-columns:auto 1fr 48px;align-items:center;gap:10px;margin:10px 0}.zoomline input{padding:0}.zoomline output{text-align:right;color:var(--muted);font-size:13px}
    .hint{color:var(--muted);font-size:12px;margin:8px 0}.msg{min-height:21px;color:#fff;font-size:13px;margin-top:8px}progress{width:100%;height:8px;margin-top:10px}
    .check{display:flex;align-items:center;gap:9px;margin:10px 0 4px}.check input{width:20px;height:20px;margin:0;accent-color:#fff}.check span{font-size:14px;color:#eee}
    details summary{cursor:pointer;font-weight:700;font-size:16px}details[open] summary{margin-bottom:10px}
  </style>
</head>
<body>
<main>
  <header><h1>AMOLED Clock</h1><div id="state" class="state">正在连接设备…</div></header>

  <section class="card">
    <h2>背景图片</h2>
    <input id="file" type="file" accept="image/*">
    <div class="row modes" style="margin-top:10px">
      <button type="button" data-mode="cover" class="active">裁剪填满</button>
      <button type="button" data-mode="contain">完整 / 黑边</button>
    </div>
    <div id="crop" class="crop"><canvas id="preview" width="480" height="480"></canvas></div>
    <div class="zoomline"><span>缩放</span><input id="zoom" type="range" min="100" max="400" value="100"><output id="zoomValue">100%</output></div>
    <button id="reset" class="ghost" type="button">复位位置</button>
    <p class="hint">拖动图片调整位置。方框内就是最终的 480×480 画面；“完整/黑边”会用黑色补齐空白。</p>
    <progress id="progress" value="0" max="100"></progress>
    <div class="row">
      <button id="upload" type="button" disabled>上传图片</button>
      <button id="clear" class="danger" type="button">清空相册</button>
    </div>
    <div id="msg" class="msg"></div>
  </section>

  <section class="card">
    <h2>背景自动切换</h2>
    <div class="row">
      <div>
        <label class="check"><input id="albumAuto" type="checkbox"><span>相册自动切换</span></label>
        <label for="albumMinutes">相册间隔（分钟）</label>
        <input id="albumMinutes" type="number" min="1" max="1440" value="30">
        <label for="albumLimit">相册图片上限（1–20）</label>
        <input id="albumLimit" type="number" min="1" max="20" value="20">
        <p id="albumState" class="hint">当前 0 张，最多保存 20 张。</p>
      </div>
      <div>
        <label class="check"><input id="weatherFollow" type="checkbox" checked><span>壁纸跟随天气</span></label>
        <label class="check"><input id="weatherAuto" type="checkbox"><span>天气壁纸自动切换</span></label>
        <label for="weatherMinutes">天气间隔（分钟）</label>
        <input id="weatherMinutes" type="number" min="1" max="1440" value="30">
      </div>
    </div>
    <p class="hint">“壁纸跟随天气”控制天气和昼夜变化是否切换场景；关闭后会记住手动选择的壁纸。两组定时切换互相独立。</p>
    <button id="saveBackgroundSwitch" type="button">保存自动切换设置</button>
    <div id="backgroundSwitchMsg" class="msg"></div>
  </section>

  <section class="card">
    <h2>天气服务</h2>
    <label for="weatherHost">QWeather API Host</label>
    <input id="weatherHost" maxlength="96" placeholder="abc123.re.qweatherapi.com">
    <label for="weatherKey">API Key</label>
    <input id="weatherKey" type="password" maxlength="128" autocomplete="off">
    <label class="check"><input id="weatherAutoLocation" type="checkbox"><span>使用公网 IP 自动定位</span></label>
    <div class="row">
      <div><label for="weatherLatitude">纬度</label><input id="weatherLatitude" inputmode="decimal" placeholder="39.92"></div>
      <div><label for="weatherLongitude">经度</label><input id="weatherLongitude" inputmode="decimal" placeholder="116.41"></div>
    </div>
    <p id="weatherLocationState" class="hint">自动定位时，经纬度作为定位失败后的后备地址。</p>
    <p class="hint">API Host 不要填写 https://。设备将获取当前天气和三日预报。</p>
    <button id="saveWeather" type="button">保存天气配置</button>
    <div id="weatherMsg" class="msg"></div>
  </section>

  <section class="card">
    <h2>时间字体</h2>
    <select id="clockFont">
      <option value="0">Montserrat（默认）</option>
      <option value="1">Kameron</option>
      <option value="2">Silkscreen</option>
      <option value="3">DotGothic16</option>
    </select>
    <p class="hint">仅更改主页面和待机页面的时间字体，字号均为 48px。</p>
    <div id="fontMsg" class="msg"></div>
  </section>

  <section class="card">
    <h2>休眠设置</h2>
    <label class="check"><input id="idleEnabled" type="checkbox"><span>启用无操作自动休眠</span></label>
    <label class="check"><input id="sleepShowTime" type="checkbox" checked><span>休眠时显示时间</span></label>
    <label for="idleMinutes">无操作后休眠（分钟）</label>
    <input id="idleMinutes" type="number" min="1" max="1440" step="1" value="15" inputmode="numeric">
    <div class="row">
      <div><label for="nightStart">夜间开始</label><input id="nightStart" type="time" value="00:00"></div>
      <div><label for="nightEnd">夜间结束</label><input id="nightEnd" type="time" value="08:00"></div>
    </div>
    <p class="hint">关闭“休眠时显示时间”后，AMOLED 会进入硬件睡眠；无闹钟时 ESP32 也不再按分钟定时唤醒，可进一步降低待机功耗。</p>
    <button id="saveSleep" type="button">保存休眠设置</button>
    <div id="sleepMsg" class="msg"></div>
  </section>

  <details class="card">
    <summary>Wi-Fi 与时区</summary>
    <form method="post" action="/save">
      <label for="ssid">Wi-Fi 名称</label>
      <input id="ssid" name="ssid" maxlength="32" required autocomplete="off" placeholder="仅支持 2.4GHz">
      <label for="password">Wi-Fi 密码</label>
      <input id="password" name="password" type="password" maxlength="64" autocomplete="new-password">
      <label for="tz">POSIX 时区</label>
      <input id="tz" name="tz" maxlength="63" value="CST-8" required>
      <p class="hint">中国大陆使用 CST-8。保存后设备将重新启动。</p>
      <button type="submit">保存并重启</button>
    </form>
  </details>
</main>

<script>
const $=s=>document.querySelector(s);
const canvas=$('#preview'),ctx=canvas.getContext('2d',{alpha:false,willReadFrequently:true});
const fileInput=$('#file'),zoomInput=$('#zoom'),zoomValue=$('#zoomValue');
const uploadButton=$('#upload'),msg=$('#msg'),progress=$('#progress');
const fontSelect=$('#clockFont'),fontMsg=$('#fontMsg');
const idleEnabled=$('#idleEnabled'),sleepShowTime=$('#sleepShowTime'),idleMinutes=$('#idleMinutes'),nightStart=$('#nightStart'),nightEnd=$('#nightEnd'),sleepMsg=$('#sleepMsg');
const albumAuto=$('#albumAuto'),albumMinutes=$('#albumMinutes'),albumLimit=$('#albumLimit'),albumState=$('#albumState'),weatherFollow=$('#weatherFollow'),weatherAuto=$('#weatherAuto'),weatherMinutes=$('#weatherMinutes');
const weatherHost=$('#weatherHost'),weatherKey=$('#weatherKey'),weatherAutoLocation=$('#weatherAutoLocation');
const weatherLatitude=$('#weatherLatitude'),weatherLongitude=$('#weatherLongitude'),weatherLocationState=$('#weatherLocationState');
let source=null,mode='cover',zoom=1,offsetX=0,offsetY=0,drag=null;

async function decodeImage(file){
  if('createImageBitmap' in window){
    const image=await createImageBitmap(file);
    return{image,width:image.width,height:image.height,close:()=>image.close()};
  }
  const url=URL.createObjectURL(file);
  const image=new Image();
  await new Promise((resolve,reject)=>{image.onload=resolve;image.onerror=()=>reject(new Error('图片解码失败'));image.src=url});
  return{image,width:image.naturalWidth,height:image.naturalHeight,close:()=>URL.revokeObjectURL(url)};
}

function baseScale(){
  if(!source)return 1;
  const sx=480/source.width,sy=480/source.height;
  return mode==='cover'?Math.max(sx,sy):Math.min(sx,sy);
}

function clampPosition(){
  if(!source)return;
  const w=source.width*baseScale()*zoom,h=source.height*baseScale()*zoom;
  if(mode==='cover'){
    offsetX=Math.max(-(w-480)/2,Math.min((w-480)/2,offsetX));
    offsetY=Math.max(-(h-480)/2,Math.min((h-480)/2,offsetY));
  }else{
    const maxX=Math.max(40,(w+480)/2-40),maxY=Math.max(40,(h+480)/2-40);
    offsetX=Math.max(-maxX,Math.min(maxX,offsetX));
    offsetY=Math.max(-maxY,Math.min(maxY,offsetY));
  }
}

function render(){
  ctx.fillStyle='#000';ctx.fillRect(0,0,480,480);
  if(!source)return;
  clampPosition();
  const scale=baseScale()*zoom,w=source.width*scale,h=source.height*scale;
  ctx.imageSmoothingEnabled=true;ctx.imageSmoothingQuality='high';
  ctx.drawImage(source.image,240-w/2+offsetX,240-h/2+offsetY,w,h);
}

function resetView(){zoom=1;offsetX=0;offsetY=0;zoomInput.value=100;zoomValue.value='100%';render()}

fileInput.addEventListener('change',async()=>{
  const file=fileInput.files[0];if(!file)return;
  msg.textContent='正在读取图片…';uploadButton.disabled=true;
  try{if(source)source.close();source=await decodeImage(file);resetView();uploadButton.disabled=false;msg.textContent='拖动或缩放后上传';}
  catch(e){source=null;msg.textContent=e.message||'无法读取图片'}
});

document.querySelectorAll('[data-mode]').forEach(button=>button.addEventListener('click',()=>{
  mode=button.dataset.mode;
  document.querySelectorAll('[data-mode]').forEach(x=>x.classList.toggle('active',x===button));
  resetView();
}));

zoomInput.addEventListener('input',()=>{zoom=Number(zoomInput.value)/100;zoomValue.value=zoomInput.value+'%';render()});
$('#reset').addEventListener('click',resetView);

function point(event){const r=canvas.getBoundingClientRect();return{x:(event.clientX-r.left)*480/r.width,y:(event.clientY-r.top)*480/r.height}}
$('#crop').addEventListener('pointerdown',event=>{if(!source)return;const p=point(event);drag={x:p.x,y:p.y,ox:offsetX,oy:offsetY};event.currentTarget.setPointerCapture(event.pointerId)});
$('#crop').addEventListener('pointermove',event=>{if(!drag)return;const p=point(event);offsetX=drag.ox+p.x-drag.x;offsetY=drag.oy+p.y-drag.y;render()});
['pointerup','pointercancel'].forEach(name=>$('#crop').addEventListener(name,()=>{drag=null}));

function toRgb565(){
  const rgba=ctx.getImageData(0,0,480,480).data,out=new Uint8Array(480*480*2);
  for(let i=0,j=0;i<rgba.length;i+=4,j+=2){const value=((rgba[i]&0xf8)<<8)|((rgba[i+1]&0xfc)<<3)|(rgba[i+2]>>3);out[j]=value&255;out[j+1]=value>>8}
  return out;
}

uploadButton.addEventListener('click',()=>{
  if(!source)return;
  msg.textContent='正在转换并上传…';progress.value=0;uploadButton.disabled=true;
  setTimeout(()=>{
    const form=new FormData();form.append('image',new Blob([toRgb565()],{type:'application/octet-stream'}),'clock.rgb565');
    const xhr=new XMLHttpRequest();xhr.open('POST','/upload');
    xhr.upload.onprogress=e=>{if(e.lengthComputable)progress.value=Math.round(e.loaded*100/e.total)};
    xhr.onload=()=>{uploadButton.disabled=false;msg.textContent=xhr.status===200?'上传成功，屏幕即将刷新':'上传失败：'+xhr.responseText;refreshStatus()};
    xhr.onerror=()=>{uploadButton.disabled=false;msg.textContent='上传连接中断'};xhr.send(form);
  },30);
});

$('#clear').addEventListener('click',async()=>{
  if(!confirm('确定清空设备中的全部相册图片？此操作不可撤销。'))return;
  try{const response=await fetch('/clear-image',{method:'POST'});msg.textContent=await response.text();refreshStatus()}
  catch(e){msg.textContent='删除失败：连接中断'}
});

fontSelect.addEventListener('change',async()=>{
  fontSelect.disabled=true;fontMsg.textContent='正在切换字体…';
  try{
    const response=await fetch('/font',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({font:fontSelect.value})});
    const text=await response.text();if(!response.ok)throw new Error(text);
    fontMsg.textContent=text;
  }catch(e){fontMsg.textContent=e.message||'字体切换失败'}
  finally{fontSelect.disabled=false}
});

$('#saveSleep').addEventListener('click',async()=>{
  const button=$('#saveSleep');button.disabled=true;sleepMsg.textContent='正在保存…';
  try{
    const response=await fetch('/sleep-settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({idle_enabled:idleEnabled.checked?'1':'0',show_time:sleepShowTime.checked?'1':'0',idle_minutes:idleMinutes.value,night_start:nightStart.value,night_end:nightEnd.value})});
    const text=await response.text();if(!response.ok)throw new Error(text);sleepMsg.textContent=text;
  }catch(e){sleepMsg.textContent=e.message||'保存失败'}
  finally{button.disabled=false}
});

$('#saveBackgroundSwitch').addEventListener('click',async()=>{
  const button=$('#saveBackgroundSwitch'),output=$('#backgroundSwitchMsg');button.disabled=true;output.textContent='正在保存…';
  try{
    const body=new URLSearchParams({album_auto:albumAuto.checked?'1':'0',album_minutes:albumMinutes.value,album_limit:albumLimit.value,weather_follow:weatherFollow.checked?'1':'0',weather_auto:weatherAuto.checked?'1':'0',weather_minutes:weatherMinutes.value});
    const response=await fetch('/background-switch',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    const text=await response.text();if(!response.ok)throw new Error(text);output.textContent=text;
  }catch(e){output.textContent=e.message||'保存失败'}finally{button.disabled=false}
});

$('#saveWeather').addEventListener('click',async()=>{
  const button=$('#saveWeather');button.disabled=true;$('#weatherMsg').textContent='正在保存…';
  try{
    const body=new URLSearchParams({host:weatherHost.value,key:weatherKey.value,auto_location:weatherAutoLocation.checked?'1':'0',latitude:weatherLatitude.value,longitude:weatherLongitude.value});
    const response=await fetch('/weather-settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    const text=await response.text();if(!response.ok)throw new Error(text);$('#weatherMsg').textContent=text;
  }catch(e){$('#weatherMsg').textContent=e.message||'保存失败'}finally{button.disabled=false}
});

function minuteToTime(value){const minute=Number(value)||0;return String(Math.floor(minute/60)).padStart(2,'0')+':'+String(minute%60).padStart(2,'0')}

function syncSettingState(){idleMinutes.disabled=!idleEnabled.checked;nightStart.disabled=!sleepShowTime.checked;nightEnd.disabled=!sleepShowTime.checked;weatherLocationState.textContent=weatherAutoLocation.checked?'自动定位已启用；手动坐标将作为后备。':'自动定位已关闭；将使用手动经纬度。'}
idleEnabled.addEventListener('change',syncSettingState);sleepShowTime.addEventListener('change',syncSettingState);weatherAutoLocation.addEventListener('change',syncSettingState);

async function refreshStatus(){
  try{
    const status=await(await fetch('/status',{cache:'no-store'})).json();
    $('#state').textContent=status.connected?status.ssid+' · '+status.ip:(status.portal?status.ap_ssid:'Wi-Fi 未连接');
    if(!$('#ssid').value)$('#ssid').value=status.ssid||'';
    if(document.activeElement!==fontSelect)fontSelect.value=String(status.font??0);
    if(document.activeElement!==idleMinutes)idleMinutes.value=String(status.idle_minutes??15);
    if(document.activeElement!==idleEnabled)idleEnabled.checked=status.idle_enabled!==false;
    if(document.activeElement!==sleepShowTime)sleepShowTime.checked=status.sleep_show_time!==false;
    nightStart.disabled=!sleepShowTime.checked;nightEnd.disabled=!sleepShowTime.checked;
    if(document.activeElement!==nightStart)nightStart.value=minuteToTime(status.night_start??0);
    if(document.activeElement!==nightEnd)nightEnd.value=minuteToTime(status.night_end??480);
    if(document.activeElement!==albumAuto)albumAuto.checked=!!status.album_auto;
    if(document.activeElement!==albumMinutes)albumMinutes.value=String(status.album_minutes??30);
    if(document.activeElement!==albumLimit)albumLimit.value=String(status.album_limit??20);
    albumLimit.max=String(status.album_capacity??20);albumState.textContent='当前 '+(status.image_count??0)+' 张；当前上限 '+(status.album_limit??20)+' 张，设备最高 '+(status.album_capacity??20)+' 张。';
    if(document.activeElement!==weatherAuto)weatherAuto.checked=!!status.weather_auto;
    if(document.activeElement!==weatherFollow)weatherFollow.checked=status.weather_follow!==false;
    if(document.activeElement!==weatherMinutes)weatherMinutes.value=String(status.weather_minutes??30);
    if(document.activeElement!==weatherHost)weatherHost.value=status.weather_host||'';
    if(document.activeElement!==weatherKey)weatherKey.value=status.weather_key||'';
    if(document.activeElement!==weatherAutoLocation)weatherAutoLocation.checked=status.weather_auto_location!==false;
    const parts=(status.weather_location||'').split(',');
    if(parts.length===2){if(document.activeElement!==weatherLongitude)weatherLongitude.value=parts[0];if(document.activeElement!==weatherLatitude)weatherLatitude.value=parts[1]}
    if(status.weather_city)weatherLocationState.textContent='当前定位：'+status.weather_city+(status.weather_timezone?' · '+status.weather_timezone:'');else syncSettingState();
  }catch(e){$('#state').textContent='设备暂时无响应'}
}
syncSettingState();refreshStatus();setInterval(refreshStatus,5000);render();
</script>
</body>
</html>
)HTML";
