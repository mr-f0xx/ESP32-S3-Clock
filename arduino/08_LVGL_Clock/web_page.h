#pragma once

static const char CLOCK_WEB_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
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
  <header><h1>AMOLED Clock</h1><div id="state" class="state"></div></header>

  <section class="card">
    <h2 data-i18n="appearance">Appearance</h2>
    <div class="row">
      <div>
        <label for="deviceLang" data-i18n="device_language">Device language</label>
        <select id="deviceLang">
          <option value="0">English</option>
          <option value="1">Français</option>
        </select>
      </div>
      <div>
        <label for="deviceTheme" data-i18n="theme">Theme</label>
        <select id="deviceTheme">
          <option value="0" data-i18n="theme_black">AMOLED Black</option>
          <option value="1" data-i18n="theme_amber">CRT Amber</option>
        </select>
      </div>
    </div>
    <label for="pageLang" data-i18n="page_language">Web page language</label>
    <select id="pageLang">
      <option value="en">English</option>
      <option value="fr">Français</option>
    </select>
    <p class="hint" data-i18n="appearance_hint">The device screen and this web page support English and French.</p>
    <div id="appearanceMsg" class="msg"></div>
  </section>

  <section class="card">
    <h2 data-i18n="background_image">Background image</h2>
    <input id="file" type="file" accept="image/*">
    <div class="row modes" style="margin-top:10px">
      <button type="button" data-mode="cover" class="active" data-i18n="mode_cover">Crop to fill</button>
      <button type="button" data-mode="contain" data-i18n="mode_contain">Fit / black bars</button>
    </div>
    <div id="crop" class="crop"><canvas id="preview" width="480" height="480"></canvas></div>
    <div class="zoomline"><span data-i18n="zoom">Zoom</span><input id="zoom" type="range" min="100" max="400" value="100"><output id="zoomValue">100%</output></div>
    <button id="reset" class="ghost" type="button" data-i18n="reset_position">Reset position</button>
    <p class="hint" data-i18n="crop_hint">Drag the image to position it. The square is the final 480×480 picture; "Fit" pads the empty area with black.</p>
    <progress id="progress" value="0" max="100"></progress>
    <div class="row">
      <button id="upload" type="button" disabled data-i18n="upload">Upload image</button>
      <button id="clear" class="danger" type="button" data-i18n="clear_album">Clear album</button>
    </div>
    <div id="msg" class="msg"></div>
  </section>

  <section class="card">
    <h2 data-i18n="auto_switch">Automatic switching</h2>
    <div class="row">
      <div>
        <label class="check"><input id="albumAuto" type="checkbox"><span data-i18n="album_auto">Album auto-switch</span></label>
        <label for="albumMinutes" data-i18n="album_interval">Album interval (minutes)</label>
        <input id="albumMinutes" type="number" min="1" max="1440" value="30">
        <label for="albumLimit" data-i18n="album_limit">Album image limit (1–20)</label>
        <input id="albumLimit" type="number" min="1" max="20" value="20">
        <p id="albumState" class="hint"></p>
      </div>
      <div>
        <label class="check"><input id="weatherFollow" type="checkbox" checked><span data-i18n="weather_follow">Wallpaper follows weather</span></label>
        <label class="check"><input id="weatherAuto" type="checkbox"><span data-i18n="weather_auto">Weather wallpaper auto-switch</span></label>
        <label for="weatherMinutes" data-i18n="weather_interval">Weather interval (minutes)</label>
        <input id="weatherMinutes" type="number" min="1" max="1440" value="30">
      </div>
    </div>
    <p class="hint" data-i18n="switch_hint">"Follows weather" controls whether weather and day/night changes switch the scene; turning it off remembers a manual choice. The two timers are independent.</p>
    <button id="saveBackgroundSwitch" type="button" data-i18n="save_switch">Save switching settings</button>
    <div id="backgroundSwitchMsg" class="msg"></div>
  </section>

  <section class="card">
    <h2 data-i18n="weather_service">Weather service</h2>
    <label for="weatherHost">QWeather API Host</label>
    <input id="weatherHost" maxlength="96" placeholder="abc123.re.qweatherapi.com">
    <label for="weatherKey">API Key</label>
    <input id="weatherKey" type="password" maxlength="128" autocomplete="off">
    <label class="check"><input id="weatherAutoLocation" type="checkbox"><span data-i18n="auto_location">Locate automatically via public IP</span></label>
    <div class="row">
      <div><label for="weatherLatitude" data-i18n="latitude">Latitude</label><input id="weatherLatitude" inputmode="decimal" placeholder="48.56"></div>
      <div><label for="weatherLongitude" data-i18n="longitude">Longitude</label><input id="weatherLongitude" inputmode="decimal" placeholder="-3.15"></div>
    </div>
    <p id="weatherLocationState" class="hint"></p>
    <p class="hint" data-i18n="weather_hint">Do not include https:// in the API Host. The device fetches current weather and a 3-day forecast.</p>
    <button id="saveWeather" type="button" data-i18n="save_weather">Save weather configuration</button>
    <div id="weatherMsg" class="msg"></div>
  </section>

  <section class="card">
    <h2 data-i18n="time_font">Time font</h2>
    <select id="clockFont">
      <option value="0" data-i18n="font_montserrat">Montserrat (default)</option>
      <option value="1">Kameron</option>
      <option value="2">Silkscreen</option>
      <option value="3">DotGothic16</option>
    </select>
    <p class="hint" data-i18n="font_hint">Only changes the time font on the main and standby pages; the size stays 48px.</p>
    <div id="fontMsg" class="msg"></div>
  </section>

  <section class="card">
    <h2 data-i18n="sleep_settings">Sleep settings</h2>
    <label class="check"><input id="idleEnabled" type="checkbox"><span data-i18n="idle_enabled">Enable idle auto-sleep</span></label>
    <label class="check"><input id="sleepShowTime" type="checkbox" checked><span data-i18n="sleep_show_time">Show time while asleep</span></label>
    <label for="idleMinutes" data-i18n="idle_minutes">Sleep after idle (minutes)</label>
    <input id="idleMinutes" type="number" min="1" max="1440" step="1" value="15" inputmode="numeric">
    <div class="row">
      <div><label for="nightStart" data-i18n="night_start">Night start</label><input id="nightStart" type="time" value="00:00"></div>
      <div><label for="nightEnd" data-i18n="night_end">Night end</label><input id="nightEnd" type="time" value="08:00"></div>
    </div>
    <p class="hint" data-i18n="sleep_hint">With "Show time while asleep" off, the AMOLED enters hardware sleep; without alarms the ESP32 also stops waking every minute, further reducing standby power.</p>
    <button id="saveSleep" type="button" data-i18n="save_sleep">Save sleep settings</button>
    <div id="sleepMsg" class="msg"></div>
  </section>

  <details class="card">
    <summary data-i18n="wifi_timezone">Wi-Fi &amp; time zone</summary>
    <form method="post" action="/save">
      <label for="ssid" data-i18n="wifi_name">Wi-Fi name</label>
      <input id="ssid" name="ssid" maxlength="32" required autocomplete="off" data-i18n-placeholder="wifi_placeholder" placeholder="2.4GHz only">
      <label for="password" data-i18n="wifi_password">Wi-Fi password</label>
      <input id="password" name="password" type="password" maxlength="64" autocomplete="new-password">
      <label for="tz" data-i18n="posix_tz">POSIX time zone</label>
      <input id="tz" name="tz" maxlength="63" value="CET-1CEST,M3.5.0,M10.5.0/3" required>
      <p class="hint" data-i18n="tz_hint">Example: CET-1CEST,M3.5.0,M10.5.0/3 for France. The device restarts after saving.</p>
      <button type="submit" data-i18n="save_reboot">Save and restart</button>
    </form>
  </details>
</main>

<script>
const I18N={
 en:{appearance:"Appearance",device_language:"Device language",theme:"Theme",theme_black:"AMOLED Black",theme_amber:"CRT Amber",page_language:"Web page language",appearance_hint:"The device screen and this web page support English and French.",background_image:"Background image",mode_cover:"Crop to fill",mode_contain:"Fit / black bars",zoom:"Zoom",reset_position:"Reset position",crop_hint:"Drag the image to position it. The square is the final 480×480 picture; \"Fit\" pads the empty area with black.",upload:"Upload image",clear_album:"Clear album",auto_switch:"Automatic switching",album_auto:"Album auto-switch",album_interval:"Album interval (minutes)",album_limit:"Album image limit (1–20)",weather_follow:"Wallpaper follows weather",weather_auto:"Weather wallpaper auto-switch",weather_interval:"Weather interval (minutes)",switch_hint:"\"Follows weather\" controls whether weather and day/night changes switch the scene; turning it off remembers a manual choice. The two timers are independent.",save_switch:"Save switching settings",weather_service:"Weather service",auto_location:"Locate automatically via public IP",latitude:"Latitude",longitude:"Longitude",weather_hint:"Do not include https:// in the API Host. The device fetches current weather and a 3-day forecast.",save_weather:"Save weather configuration",time_font:"Time font",font_montserrat:"Montserrat (default)",font_hint:"Only changes the time font on the main and standby pages; the size stays 48px.",sleep_settings:"Sleep settings",idle_enabled:"Enable idle auto-sleep",sleep_show_time:"Show time while asleep",idle_minutes:"Sleep after idle (minutes)",night_start:"Night start",night_end:"Night end",sleep_hint:"With \"Show time while asleep\" off, the AMOLED enters hardware sleep; without alarms the ESP32 also stops waking every minute, further reducing standby power.",save_sleep:"Save sleep settings",wifi_timezone:"Wi-Fi & time zone",wifi_name:"Wi-Fi name",wifi_placeholder:"2.4GHz only",wifi_password:"Wi-Fi password",posix_tz:"POSIX time zone",tz_hint:"Example: CET-1CEST,M3.5.0,M10.5.0/3 for France. The device restarts after saving.",save_reboot:"Save and restart",connecting:"Connecting to device…",reading_image:"Reading image…",drag_to_upload:"Drag or zoom, then upload",cannot_read:"Cannot read image",decode_failed:"Image decode failed",converting:"Converting and uploading…",upload_success:"Uploaded; the screen will refresh shortly",upload_failed:"Upload failed: ",upload_interrupted:"Upload connection interrupted",confirm_clear:"Clear all album images on the device? This cannot be undone.",delete_failed:"Delete failed: connection interrupted",switching_font:"Switching font…",font_failed:"Font switch failed",saving:"Saving…",save_failed:"Save failed",device_unresponsive:"Device is not responding",wifi_disconnected:"Wi-Fi not connected",album_state_fmt:"{0} images now; limit {1}, device max {2}.",auto_loc_enabled:"Auto-location enabled; manual coordinates are the fallback.",auto_loc_disabled:"Auto-location off; manual coordinates will be used.",current_location:"Located: ",appearance_saved:"Appearance saved",appearance_failed:"Appearance save failed"},
 fr:{appearance:"Apparence",device_language:"Langue de l'appareil",theme:"Thème",theme_black:"Noir AMOLED",theme_amber:"Ambre CRT",page_language:"Langue de la page web",appearance_hint:"L'écran de l'appareil et cette page web prennent en charge l'anglais et le français.",background_image:"Image de fond",mode_cover:"Recadrer pour remplir",mode_contain:"Ajuster / bandes noires",zoom:"Zoom",reset_position:"Réinitialiser la position",crop_hint:"Faites glisser l'image pour la positionner. Le carré est l'image finale 480×480 ; « Ajuster » complète le vide avec du noir.",upload:"Téléverser l'image",clear_album:"Vider l'album",auto_switch:"Changement automatique",album_auto:"Changement auto de l'album",album_interval:"Intervalle de l'album (minutes)",album_limit:"Limite d'images de l'album (1–20)",weather_follow:"Le fond suit la météo",weather_auto:"Changement auto du fond météo",weather_interval:"Intervalle météo (minutes)",switch_hint:"« Suit la météo » détermine si la météo et le jour/nuit changent la scène ; le désactiver mémorise un choix manuel. Les deux minuteries sont indépendantes.",save_switch:"Enregistrer le changement auto",weather_service:"Service météo",auto_location:"Localisation automatique via IP publique",latitude:"Latitude",longitude:"Longitude",weather_hint:"N'incluez pas https:// dans l'API Host. L'appareil récupère la météo actuelle et les prévisions sur 3 jours.",save_weather:"Enregistrer la configuration météo",time_font:"Police de l'heure",font_montserrat:"Montserrat (défaut)",font_hint:"Ne change que la police de l'heure sur les pages principale et veille ; la taille reste 48px.",sleep_settings:"Réglages de veille",idle_enabled:"Activer la veille automatique",sleep_show_time:"Afficher l'heure en veille",idle_minutes:"Veille après inactivité (minutes)",night_start:"Début de nuit",night_end:"Fin de nuit",sleep_hint:"Avec « Afficher l'heure en veille » désactivé, l'AMOLED passe en veille matérielle ; sans alarme, l'ESP32 cesse aussi de se réveiller chaque minute, réduisant encore la consommation.",save_sleep:"Enregistrer la veille",wifi_timezone:"Wi-Fi et fuseau horaire",wifi_name:"Nom du Wi-Fi",wifi_placeholder:"2,4 GHz uniquement",wifi_password:"Mot de passe Wi-Fi",posix_tz:"Fuseau horaire POSIX",tz_hint:"Exemple : CET-1CEST,M3.5.0,M10.5.0/3 pour la France. L'appareil redémarre après l'enregistrement.",save_reboot:"Enregistrer et redémarrer",connecting:"Connexion à l'appareil…",reading_image:"Lecture de l'image…",drag_to_upload:"Glissez ou zoomez, puis téléversez",cannot_read:"Impossible de lire l'image",decode_failed:"Échec du décodage de l'image",converting:"Conversion et téléversement…",upload_success:"Téléversé ; l'écran va se rafraîchir",upload_failed:"Échec du téléversement : ",upload_interrupted:"Connexion de téléversement interrompue",confirm_clear:"Vider toutes les images de l'album sur l'appareil ? Action irréversible.",delete_failed:"Échec de la suppression : connexion interrompue",switching_font:"Changement de police…",font_failed:"Échec du changement de police",saving:"Enregistrement…",save_failed:"Échec de l'enregistrement",device_unresponsive:"L'appareil ne répond pas",wifi_disconnected:"Wi-Fi non connecté",album_state_fmt:"{0} images actuellement ; limite {1}, max appareil {2}.",auto_loc_enabled:"Localisation auto activée ; les coordonnées manuelles servent de secours.",auto_loc_disabled:"Localisation auto désactivée ; les coordonnées manuelles seront utilisées.",current_location:"Localisé : ",appearance_saved:"Apparence enregistrée",appearance_failed:"Échec de l'enregistrement de l'apparence"}
};
let webLang=localStorage.getItem('clockWebLang');
if(webLang!=='en'&&webLang!=='fr')webLang=null;
let deviceLang=0;
function effLang(){return webLang||'en'}
function t(key){const l=effLang();return (I18N[l]&&I18N[l][key])||I18N.en[key]||key}
function fmt(key,...args){let s=t(key);args.forEach((a,i)=>{s=s.replace('{'+i+'}',a)});return s}
const _fetch=window.fetch;
window.fetch=(url,opts)=>{const u=new URL(url,location.href);u.searchParams.set('lang',effLang());return _fetch(u.toString(),opts)};
const _xhrOpen=XMLHttpRequest.prototype.open;
XMLHttpRequest.prototype.open=function(method,url,...rest){const u=new URL(url,location.href);u.searchParams.set('lang',effLang());return _xhrOpen.call(this,method,u.toString(),...rest)};
function applyI18n(){
  document.documentElement.lang=effLang();
  document.querySelectorAll('[data-i18n]').forEach(el=>{el.textContent=t(el.dataset.i18n)});
  document.querySelectorAll('[data-i18n-placeholder]').forEach(el=>{el.placeholder=t(el.dataset.i18nPlaceholder)});
  $('#pageLang').value=effLang();
  syncSettingState();
}

const $=s=>document.querySelector(s);
const canvas=$('#preview'),ctx=canvas.getContext('2d',{alpha:false,willReadFrequently:true});
const fileInput=$('#file'),zoomInput=$('#zoom'),zoomValue=$('#zoomValue');
const uploadButton=$('#upload'),msg=$('#msg'),progress=$('#progress');
const fontSelect=$('#clockFont'),fontMsg=$('#fontMsg');
const idleEnabled=$('#idleEnabled'),sleepShowTime=$('#sleepShowTime'),idleMinutes=$('#idleMinutes'),nightStart=$('#nightStart'),nightEnd=$('#nightEnd'),sleepMsg=$('#sleepMsg');
const albumAuto=$('#albumAuto'),albumMinutes=$('#albumMinutes'),albumLimit=$('#albumLimit'),albumState=$('#albumState'),weatherFollow=$('#weatherFollow'),weatherAuto=$('#weatherAuto'),weatherMinutes=$('#weatherMinutes');
const weatherHost=$('#weatherHost'),weatherKey=$('#weatherKey'),weatherAutoLocation=$('#weatherAutoLocation');
const weatherLatitude=$('#weatherLatitude'),weatherLongitude=$('#weatherLongitude'),weatherLocationState=$('#weatherLocationState');
const deviceLangSelect=$('#deviceLang'),deviceThemeSelect=$('#deviceTheme'),pageLangSelect=$('#pageLang'),appearanceMsg=$('#appearanceMsg');
let source=null,mode='cover',zoom=1,offsetX=0,offsetY=0,drag=null;

async function decodeImage(file){
  if('createImageBitmap' in window){
    const image=await createImageBitmap(file);
    return{image,width:image.width,height:image.height,close:()=>image.close()};
  }
  const url=URL.createObjectURL(file);
  const image=new Image();
  await new Promise((resolve,reject)=>{image.onload=resolve;image.onerror=()=>reject(new Error(t('decode_failed')));image.src=url});
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
  msg.textContent=t('reading_image');uploadButton.disabled=true;
  try{if(source)source.close();source=await decodeImage(file);resetView();uploadButton.disabled=false;msg.textContent=t('drag_to_upload');}
  catch(e){source=null;msg.textContent=e.message||t('cannot_read')}
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
  msg.textContent=t('converting');progress.value=0;uploadButton.disabled=true;
  setTimeout(()=>{
    const form=new FormData();form.append('image',new Blob([toRgb565()],{type:'application/octet-stream'}),'clock.rgb565');
    const xhr=new XMLHttpRequest();xhr.open('POST','/upload');
    xhr.upload.onprogress=e=>{if(e.lengthComputable)progress.value=Math.round(e.loaded*100/e.total)};
    xhr.onload=()=>{uploadButton.disabled=false;msg.textContent=xhr.status===200?t('upload_success'):t('upload_failed')+xhr.responseText;refreshStatus()};
    xhr.onerror=()=>{uploadButton.disabled=false;msg.textContent=t('upload_interrupted')};xhr.send(form);
  },30);
});

$('#clear').addEventListener('click',async()=>{
  if(!confirm(t('confirm_clear')))return;
  try{const response=await fetch('/clear-image',{method:'POST'});msg.textContent=await response.text();refreshStatus()}
  catch(e){msg.textContent=t('delete_failed')}
});

fontSelect.addEventListener('change',async()=>{
  fontSelect.disabled=true;fontMsg.textContent=t('switching_font');
  try{
    const response=await fetch('/font',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({font:fontSelect.value})});
    const text=await response.text();if(!response.ok)throw new Error(text);
    fontMsg.textContent=text;
  }catch(e){fontMsg.textContent=e.message||t('font_failed')}
  finally{fontSelect.disabled=false}
});

$('#saveSleep').addEventListener('click',async()=>{
  const button=$('#saveSleep');button.disabled=true;sleepMsg.textContent=t('saving');
  try{
    const response=await fetch('/sleep-settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({idle_enabled:idleEnabled.checked?'1':'0',show_time:sleepShowTime.checked?'1':'0',idle_minutes:idleMinutes.value,night_start:nightStart.value,night_end:nightEnd.value})});
    const text=await response.text();if(!response.ok)throw new Error(text);sleepMsg.textContent=text;
  }catch(e){sleepMsg.textContent=e.message||t('save_failed')}
  finally{button.disabled=false}
});

$('#saveBackgroundSwitch').addEventListener('click',async()=>{
  const button=$('#saveBackgroundSwitch'),output=$('#backgroundSwitchMsg');button.disabled=true;output.textContent=t('saving');
  try{
    const body=new URLSearchParams({album_auto:albumAuto.checked?'1':'0',album_minutes:albumMinutes.value,album_limit:albumLimit.value,weather_follow:weatherFollow.checked?'1':'0',weather_auto:weatherAuto.checked?'1':'0',weather_minutes:weatherMinutes.value});
    const response=await fetch('/background-switch',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    const text=await response.text();if(!response.ok)throw new Error(text);output.textContent=text;
  }catch(e){output.textContent=e.message||t('save_failed')}finally{button.disabled=false}
});

$('#saveWeather').addEventListener('click',async()=>{
  const button=$('#saveWeather');button.disabled=true;$('#weatherMsg').textContent=t('saving');
  try{
    const body=new URLSearchParams({host:weatherHost.value,key:weatherKey.value,auto_location:weatherAutoLocation.checked?'1':'0',latitude:weatherLatitude.value,longitude:weatherLongitude.value});
    const response=await fetch('/weather-settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    const text=await response.text();if(!response.ok)throw new Error(text);$('#weatherMsg').textContent=text;
  }catch(e){$('#weatherMsg').textContent=e.message||t('save_failed')}finally{button.disabled=false}
});

async function postUiSettings(body){
  appearanceMsg.textContent=t('saving');
  try{
    const response=await fetch('/ui-settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(body)});
    const text=await response.text();if(!response.ok)throw new Error(text);
    appearanceMsg.textContent=t('appearance_saved');refreshStatus();
  }catch(e){appearanceMsg.textContent=e.message||t('appearance_failed')}
}
deviceLangSelect.addEventListener('change',()=>{postUiSettings({lang:deviceLangSelect.value})});
deviceThemeSelect.addEventListener('change',()=>{postUiSettings({theme:deviceThemeSelect.value})});
pageLangSelect.addEventListener('change',()=>{webLang=pageLangSelect.value;localStorage.setItem('clockWebLang',webLang);applyI18n();refreshStatus()});

function minuteToTime(value){const minute=Number(value)||0;return String(Math.floor(minute/60)).padStart(2,'0')+':'+String(minute%60).padStart(2,'0')}

function syncSettingState(){idleMinutes.disabled=!idleEnabled.checked;nightStart.disabled=!sleepShowTime.checked;nightEnd.disabled=!sleepShowTime.checked;weatherLocationState.textContent=weatherAutoLocation.checked?t('auto_loc_enabled'):t('auto_loc_disabled')}
idleEnabled.addEventListener('change',syncSettingState);sleepShowTime.addEventListener('change',syncSettingState);weatherAutoLocation.addEventListener('change',syncSettingState);

async function refreshStatus(){
  try{
    const status=await(await fetch('/status',{cache:'no-store'})).json();
    $('#state').textContent=status.connected?status.ssid+' · '+status.ip:(status.portal?status.ap_ssid:t('wifi_disconnected'));
    if(!$('#ssid').value)$('#ssid').value=status.ssid||'';
    deviceLang=status.lang??0;
    if(document.activeElement!==deviceLangSelect)deviceLangSelect.value=String(status.lang??0);
    if(document.activeElement!==deviceThemeSelect)deviceThemeSelect.value=String(status.theme??0);
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
    albumLimit.max=String(status.album_capacity??20);albumState.textContent=fmt('album_state_fmt',status.image_count??0,status.album_limit??20,status.album_capacity??20);
    if(document.activeElement!==weatherAuto)weatherAuto.checked=!!status.weather_auto;
    if(document.activeElement!==weatherFollow)weatherFollow.checked=status.weather_follow!==false;
    if(document.activeElement!==weatherMinutes)weatherMinutes.value=String(status.weather_minutes??30);
    if(document.activeElement!==weatherHost)weatherHost.value=status.weather_host||'';
    if(document.activeElement!==weatherKey)weatherKey.value=status.weather_key||'';
    if(document.activeElement!==weatherAutoLocation)weatherAutoLocation.checked=status.weather_auto_location!==false;
    const parts=(status.weather_location||'').split(',');
    if(parts.length===2){if(document.activeElement!==weatherLongitude)weatherLongitude.value=parts[0];if(document.activeElement!==weatherLatitude)weatherLatitude.value=parts[1]}
    if(status.weather_city)weatherLocationState.textContent=t('current_location')+status.weather_city+(status.weather_timezone?' · '+status.weather_timezone:'');else syncSettingState();
    applyI18n();
  }catch(e){$('#state').textContent=t('device_unresponsive')}
}
$('#state').textContent=t('connecting');
applyI18n();syncSettingState();refreshStatus();setInterval(refreshStatus,5000);render();
</script>
</body>
</html>
)HTML";
