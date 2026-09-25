#ifndef WIFI_PAGE_H
#define WIFI_PAGE_H

#include <Arduino.h>

// WiFi setup page served at "/connect". The UID (and, for the charger, the
// one-time claim) come in via the page URL, e.g.
// http://192.168.4.1/connect?uid=abc123&claim=..., and are forwarded to /wifi.
// Progress / errors come from /connect-status (wifi, result, busy, reason,
// ssid, mqtt; charger also prov + acct).
// SHARED: this file is identical in esp32-inverter and esp32-charger.
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Cài đặt WiFi thiết bị</title>
<style>
  body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#f1f5f9;color:#0f172a;margin:0;padding:20px;}
  .card{max-width:420px;margin:24px auto;background:#ffffff;border-radius:14px;padding:24px;box-shadow:0 10px 30px rgba(15,23,42,.12);}
  h1{font-size:20px;margin:0 0 4px;}
  .sub{color:#64748b;font-size:13px;margin-bottom:20px;}
  label{display:block;font-size:13px;margin:14px 0 6px;color:#334155;}
  select,input{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #cbd5e1;background:#ffffff;color:#0f172a;font-size:15px;}
  .row{display:flex;gap:8px;}
  .row select{flex:1;}
  button{margin-top:20px;width:100%;padding:13px;border:0;border-radius:10px;background:#2563eb;color:#fff;font-size:16px;font-weight:600;cursor:pointer;}
  button:disabled{opacity:.5;cursor:not-allowed;}
  .ghost{background:#e2e8f0;color:#0f172a;width:auto;padding:12px 14px;margin-top:0;}
  .status{margin-top:16px;font-size:14px;line-height:1.45;text-align:left;min-height:20px;border-radius:10px;}
  .status.ok,.status.err,.status.warn,.status.info{padding:12px 14px;}
  .status.ok{color:#166534;background:#f0fdf4;}
  .status.err{color:#991b1b;background:#fef2f2;}
  .status.warn{color:#92400e;background:#fffbeb;}
  .status.info{color:#1e3a8a;background:#eff6ff;}
  .spin{display:inline-block;width:12px;height:12px;margin-right:8px;vertical-align:-1px;border:2px solid currentColor;border-right-color:transparent;border-radius:50%;animation:sp .8s linear infinite;}
  @keyframes sp{to{transform:rotate(360deg);}}
  .uid{font-size:12px;color:#94a3b8;margin-top:6px;word-break:break-all;}
</style>
</head>
<body>
<div class="card">
  <h1>Cài đặt WiFi</h1>
  <div class="sub">Chọn mạng WiFi và nhập mật khẩu để kết nối thiết bị.</div>

  <label>Mạng WiFi</label>
  <div class="row">
    <select id="ssid"><option value="">Đang quét...</option></select>
    <button type="button" class="ghost" id="refresh">&#8635;</button>
  </div>

  <label>Mật khẩu</label>
  <input id="password" type="password" placeholder="Mật khẩu WiFi" autocomplete="off">

  <div class="uid" id="uidLabel">UID: (chưa có)</div>

  <button id="save" disabled>Kết nối</button>
  <button type="button" class="ghost" id="check" style="width:100%;margin-top:10px;">Kiểm tra trạng thái</button>
  <div class="status" id="status"></div>
</div>

<script>
  var q = new URLSearchParams(location.search);
  var uid = q.get('uid') || '';
  // Charger only: one-time code from the app ("Thêm thiết bị") that adds the
  // charger to the user's account. The inverter ignores it.
  var claim = q.get('claim') || '';
  // Learnt from /connect-status: only the charger reports "acct".
  var isCharger = false;
  var hasAccount = false;
  document.getElementById('uidLabel').textContent = 'UID: ' + (uid || '(chưa có)');

  var ssidSel = document.getElementById('ssid');
  // NOTE: do NOT name this "status" — that collides with window.status (a
  // string-only property), which silently breaks all status updates.
  var statusEl = document.getElementById('status');
  var saveBtn = document.getElementById('save');

  function setStatus(cls, msg, spinning){
    statusEl.className = 'status ' + (cls || '');
    statusEl.innerHTML = '';
    if(spinning){
      var sp = document.createElement('span');
      sp.className = 'spin';
      statusEl.appendChild(sp);
    }
    statusEl.appendChild(document.createTextNode(msg));
  }

  // fetch with a timeout: when the phone drops off the setup WiFi a plain
  // fetch can hang for minutes and the page would show nothing.
  function req(url, opts, ms){
    opts = opts || {};
    opts.cache = 'no-store';
    var ctl = window.AbortController ? new AbortController() : null;
    var t = null;
    if(ctl){ opts.signal = ctl.signal; t = setTimeout(function(){ ctl.abort(); }, ms || 4000); }
    return fetch(url, opts).then(function(r){
      if(t) clearTimeout(t);
      if(!r.ok) throw new Error('HTTP ' + r.status);
      return r;
    }, function(e){ if(t) clearTimeout(t); throw e; });
  }
  function getJson(url, ms){ return req(url, {}, ms).then(function(r){ return r.json(); }); }

  function scan(){
    ssidSel.innerHTML = '<option value="">Đang quét...</option>';
    saveBtn.disabled = true;
    // Ask the device for ONE fresh scan, then poll the cached result.
    req('/scan?refresh=1').catch(function(){});
    var tries = 0;
    function poll(){
      getJson('/scan').then(function(list){
        if(list.length === 0 && tries < 10){ tries++; return setTimeout(poll, 1000); }
        ssidSel.innerHTML = '';
        if(list.length === 0){
          ssidSel.innerHTML = '<option value="">Không tìm thấy mạng nào</option>';
          return;
        }
        list.sort(function(a,b){return b.rssi - a.rssi;});
        var seen = {};
        list.forEach(function(n){
          if(!n.ssid || seen[n.ssid]) return;
          seen[n.ssid] = true;
          var o = document.createElement('option');
          o.value = n.ssid;
          o.textContent = n.ssid + (n.secure ? ' 🔒' : '');
          ssidSel.appendChild(o);
        });
        saveBtn.disabled = false;
      }).catch(function(){
        if(tries < 10){ tries++; return setTimeout(poll, 1000); }
        ssidSel.innerHTML = '<option value="">Không quét được, bấm ↻ để thử lại</option>';
      });
    }
    // Give the device ~1.8s to finish the scan before the first poll.
    setTimeout(poll, 1800);
  }
  document.getElementById('refresh').onclick = scan;

  // wifi_err_reason_t of the failed attempt -> what the user should do.
  function reasonText(r){
    if(r === 2 || r === 14 || r === 15 || r === 202 || r === 204)
      return 'Sai mật khẩu WiFi.';
    if(r === 201)
      return 'Không tìm thấy mạng: thiết bị ở quá xa router, hoặc mạng chỉ phát sóng 5GHz.';
    if(r === 4 || r === 200 || r === 203 || r === 205)
      return 'Sóng WiFi yếu: đặt thiết bị gần router hơn rồi thử lại.';
    return r ? 'Mã lỗi ' + r + '. Kiểm tra mật khẩu và thử lại.' : 'Kiểm tra mật khẩu và thử lại.';
  }

  // Device state (/connect-status) -> {cls, msg, spin, final, serverFail}.
  function describe(s){
    isCharger = ('acct' in s);
    hasAccount = s.acct === 1;
    var name = s.ssid || 'WiFi';
    if(s.busy === 1)
      return {cls:'info', spin:true, msg:'Đang kết nối tới ' + name + '...'};
    if(s.result === 0)
      return {cls:'err', final:true, msg:'❌ Không kết nối được ' + name + '. ' + reasonText(s.reason)};
    if(!(s.result === 1 || s.wifi === 3))
      return {cls:'', final:true, msg:'Sẵn sàng. Chọn mạng WiFi, nhập mật khẩu rồi bấm Kết nối.'};

    // WiFi is up: now the account (charger) and the server.
    if(isCharger){
      if(s.prov === -1)
        return {cls:'err', final:true, msg:'❌ Đã kết nối ' + name + ' nhưng không thêm được thiết bị vào tài khoản (mã thêm thiết bị đã hết hạn hoặc đã dùng). Hãy mở lại trang này từ ứng dụng.'};
      if(s.prov === 2)
        return {cls:'info', spin:true, msg:'🔐 Đã kết nối ' + name + '. Đang thêm thiết bị vào tài khoản...'};
      if(!hasAccount)
        return {cls:'warn', final:true, msg:'⚠️ Đã kết nối ' + name + ' nhưng thiết bị chưa được thêm vào tài khoản. Hãy mở trang này từ ứng dụng (mục Thêm thiết bị).'};
    }
    if(s.mqtt === 1)
      return {cls:'ok', final:true, msg:'✅ Đã kết nối ' + name + '. Thiết bị đang trực tuyến.'};
    if(s.mqtt === 0)
      return {cls:'warn', final:true, serverFail:true, msg:'⚠️ Đã kết nối ' + name + ' nhưng chưa vào được máy chủ. Mạng này có thể không có Internet; thiết bị sẽ tự thử lại.'};
    return {cls:'info', spin:true, msg:'📶 Đã kết nối ' + name + '. Đang kết nối máy chủ...'};
  }

  // Follow a connect attempt until it ends. The phone often drops off the
  // setup WiFi while the device joins the router (its radio moves to the
  // router's channel): keep polling, the page updates once the phone is back.
  var pollTimer = null;
  function follow(){
    if(pollTimer) clearTimeout(pollTimer);
    var started = Date.now();
    var serverFailSince = 0;
    function done(){ pollTimer = null; saveBtn.disabled = false; }
    function next(){
      if(Date.now() - started > 120000){
        setStatus('err', '⌛ Hết thời gian chờ. Kết nối lại WiFi của thiết bị rồi bấm "Kiểm tra trạng thái".');
        return done();
      }
      pollTimer = setTimeout(tick, 1500);
    }
    function tick(){
      getJson('/connect-status', 4000).then(function(s){
        var d = describe(s);
        // The first server connect can fail while DNS/NTP settle: only
        // report it after ~20 s.
        if(d.serverFail){
          if(!serverFailSince) serverFailSince = Date.now();
          if(Date.now() - serverFailSince < 20000){
            d = {cls:'info', spin:true, msg:'📶 Đã kết nối ' + (s.ssid || 'WiFi') + '. Đang kết nối máy chủ...'};
          }
        } else {
          serverFailSince = 0;
        }
        setStatus(d.cls, d.msg, d.spin);
        if(d.final && !d.spin) return done();
        next();
      }).catch(function(){
        setStatus('warn', '📡 Điện thoại đã rời WiFi của thiết bị (thường gặp khi thiết bị đang kết nối router). Hãy kết nối lại WiFi của thiết bị, trang sẽ tự cập nhật.', true);
        next();
      });
    }
    tick();
  }

  saveBtn.onclick = function(){
    var ssid = ssidSel.value;
    var password = document.getElementById('password').value;
    if(!ssid){ setStatus('err', 'Vui lòng chọn một mạng WiFi.'); return; }
    if(isCharger && !claim && !hasAccount){
      setStatus('err', 'Hãy mở trang này từ ứng dụng (mục Thêm thiết bị) để thêm thiết bị vào tài khoản.');
      return;
    }
    saveBtn.disabled = true;
    setStatus('info', 'Đang gửi thông tin tới thiết bị...', true);
    var url = '/wifi?ssid=' + encodeURIComponent(ssid) +
              '&password=' + encodeURIComponent(password) +
              '&uid=' + encodeURIComponent(uid) +
              (claim ? '&claim=' + encodeURIComponent(claim) : '');
    req(url, {method:'POST'}, 6000).then(function(r){ return r.text(); }).then(function(t){
      if(t.trim() === 'success'){
        follow();
      } else {
        setStatus('err', '❌ Thiết bị từ chối yêu cầu: ' + t);
        saveBtn.disabled = false;
      }
    }).catch(function(){
      setStatus('err', '❌ Không gửi được tới thiết bị. Hãy chắc chắn điện thoại đang kết nối WiFi của thiết bị rồi thử lại.');
      saveBtn.disabled = false;
    });
  };

  // Current state on load / on demand. After the phone rejoins the setup WiFi
  // this shows the result of the last attempt (and keeps following it if it
  // is still running).
  function showCurrentStatus(){
    setStatus('info', 'Đang kiểm tra trạng thái thiết bị...', true);
    getJson('/connect-status', 4000).then(function(s){
      var d = describe(s);
      setStatus(d.cls, d.msg, d.spin);
      if(d.spin){ saveBtn.disabled = true; follow(); }
    }).catch(function(){
      setStatus('err', '⚠️ Không kết nối được tới thiết bị. Hãy chắc chắn điện thoại đang kết nối WiFi của thiết bị.');
    });
  }
  document.getElementById('check').onclick = showCurrentStatus;

  showCurrentStatus();
  scan();
</script>
</body>
</html>
)rawliteral";

#endif  // WIFI_PAGE_H
