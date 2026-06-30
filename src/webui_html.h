#pragma once

static const char HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ProjektX</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: sans-serif;
      background: #1a1a1a;
      color: #e0e0e0;
      display: flex;
      flex-direction: column;
      height: 100dvh;
      max-width: 600px;
      margin: 0 auto;
      overflow-x: hidden;
    }
    #navbar {
      display: flex;
      align-items: center;
      padding: 6px 8px;
      background: #111;
      border-bottom: 1px solid #333;
      flex-shrink: 0;
    }
    #navbar > * + * { margin-left: 8px; }
    #btn-channels {
      background: #2a2a2a;
      color: #e0e0e0;
      border: 1px solid #444;
      border-radius: 6px;
      padding: 8px 10px;
      min-height: 44px;
      font-size: 13px;
      cursor: pointer;
      display: flex;
      align-items: center;
      white-space: nowrap;
      flex-shrink: 1;
      overflow: hidden;
    }
    #unread-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: #d44;
      display: none;
      flex-shrink: 0;
    }
    #nav-center {
      flex: 1;
      text-align: center;
      min-width: 0;
      overflow: hidden;
    }
    #active-channel {
      font-size: 15px;
      font-weight: bold;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    #dutyCycleInfo {
      font-size: 11px;
      color: #d4a45a;
    }
    #btn-config {
      background: #2a2a2a;
      color: #e0e0e0;
      border: 1px solid #444;
      border-radius: 6px;
      min-width: 44px;
      min-height: 44px;
      font-size: 20px;
      cursor: pointer;
      flex-shrink: 0;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    #channel-dropdown {
      background: #222;
      border-bottom: 1px solid #333;
      flex-shrink: 0;
    }
    .ch-item {
      padding: 12px 16px;
      min-height: 44px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      cursor: pointer;
      color: #e0e0e0;
      font-size: 15px;
      border-bottom: 1px solid #2a2a2a;
    }
    .ch-item.active {
      background: #1e3a5f;
      font-weight: bold;
    }
    .ch-badge {
      background: #d44;
      color: #fff;
      border-radius: 10px;
      padding: 2px 8px;
      font-size: 12px;
      font-weight: bold;
      flex-shrink: 0;
      margin-left: 8px;
    }
    #messages {
      flex: 1;
      overflow-y: auto;
      overflow-x: hidden;
      padding: 12px;
      display: flex;
      flex-direction: column;
    }
    #messages > * + * { margin-top: 8px; }
    #no-channel-msg {
      flex: 1;
      display: flex;
      align-items: center;
      justify-content: center;
      text-align: center;
      padding: 24px;
      color: #888;
      font-size: 16px;
    }
    .msg {
      background: #2a2a2a;
      border-radius: 8px;
      padding: 10px 14px;
      word-break: break-word;
    }
    .msg .sender {
      font-size: 12px;
      color: #888;
      margin-bottom: 4px;
    }
    .msg .text {
      font-size: 15px;
    }
    .msg .debug {
      font-size: 11px;
      color: #5a8a5a;
      margin-top: 4px;
      font-family: monospace;
    }
    .msg.own {
      background: #1e3a5f;
      align-self: flex-end;
      max-width: 85%;
    }
    .msg.own .sender { color: #5a9fd4; }
    #input-bar {
      display: flex;
      flex-direction: column;
      padding: 12px;
      background: #111;
      border-top: 1px solid #333;
      flex-shrink: 0;
    }
    #input-bar textarea {
      background: #2a2a2a;
      border: 1px solid #444;
      color: #e0e0e0;
      border-radius: 6px;
      padding: 10px 12px;
      font-size: 15px;
      font-family: sans-serif;
      width: 100%;
      resize: none;
    }
    #input-bar textarea:focus {
      outline: none;
      border-color: #5a9fd4;
    }
    #input-bar button {
      background: #5a9fd4;
      color: #fff;
      border: none;
      border-radius: 6px;
      padding: 12px;
      font-size: 15px;
      font-weight: bold;
      cursor: pointer;
      min-height: 44px;
    }
    #input-bar button:disabled {
      background: #444;
      cursor: default;
    }
    #status {
      font-size: 12px;
      color: #888;
      text-align: center;
      min-height: 16px;
    }
    #status.error { color: #e07a5a; }
  </style>
</head>
<body>
  <div id="navbar">
    <button id="btn-channels" onclick="toggleDD()">
      <span id="btn-ch-text"></span>
      <span id="unread-dot"></span>
    </button>
    <div id="nav-center">
      <div id="active-channel"></div>
      <div id="dutyCycleInfo"></div>
    </div>
    <button id="btn-config" onclick="location.href='/config.html'">&#9881;</button>
  </div>
  <div id="channel-dropdown" style="display:none"></div>
  <div id="no-channel-msg" style="display:none">Kein Kanal konfiguriert.<br>Bitte Konfiguration &#9881; oeffnen.</div>
  <div id="messages"></div>
  <div id="input-bar">
    <textarea id="text" rows="2" placeholder="Nachricht..." maxlength="182"></textarea>
    <button id="send" onclick="sendMsg()">Senden</button>
    <div id="status"></div>
  </div>

  <script>
    let activeChannel = '';
    let channels = [];
    let lastCount = 0;
    let myName = '';
    let debugMode = false;
    let lastDisplayedTimestamp = 0;
    let ddOpen = false;
    let pollInFlight = false;

    window.onload = function() {
      debugMode = localStorage.getItem('px_debug') === '1';
      document.getElementById('text').addEventListener('keydown', function(e) {
        if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); sendMsg(); }
      });
      document.addEventListener('click', function(e) {
        if (ddOpen) {
          var dd = document.getElementById('channel-dropdown');
          var btn = document.getElementById('btn-channels');
          if (!dd.contains(e.target) && !btn.contains(e.target)) {
            ddOpen = false;
            renderDD();
          }
        }
      });
      init();
    };

    function getLS(ch) { return parseInt(localStorage.getItem('px_ls_' + ch) || '0'); }
    function setLS(ch, v) { localStorage.setItem('px_ls_' + ch, '' + v); }

    async function init() {
      await fetchChannels();
      if (channels.length === 0) {
        showNoChannel();
        setInterval(function() { fetchChannels().then(function() { if (channels.length > 0) location.reload(); }); }, 10000);
        return;
      }
      var saved = localStorage.getItem('px_active');
      var found = false;
      for (var i = 0; i < channels.length; i++) {
        if (channels[i].name === saved) { found = true; break; }
      }
      activeChannel = found ? saved : channels[0].name;
      localStorage.setItem('px_active', activeChannel);
      lastCount = 0;
      updateNav();
      await pollMessages();
      for (var i = 0; i < channels.length; i++) {
        if (channels[i].name === activeChannel) continue;
        if (!localStorage.getItem('px_ls_' + channels[i].name)) {
          setLS(channels[i].name, lastCount);
        }
      }
      setInterval(pollMessages, 2000);
      setInterval(pollInactive, 10000);
      setInterval(function() { fetchChannels().then(updateNav); }, 30000);
    }

    function showNoChannel() {
      document.getElementById('no-channel-msg').style.display = 'flex';
      document.getElementById('messages').style.display = 'none';
      document.getElementById('input-bar').style.display = 'none';
      document.getElementById('active-channel').textContent = 'Kein Kanal';
      document.getElementById('btn-ch-text').textContent = '≡ Kanäle';
    }

    async function fetchChannels() {
      try {
        var res = await fetch('/channels');
        if (!res.ok) return;
        var data = await res.json();
        var names = data.channels || [];
        var old = {};
        for (var i = 0; i < channels.length; i++) old[channels[i].name] = channels[i].unread || 0;
        channels = [];
        for (var i = 0; i < names.length; i++) {
          channels.push({name: names[i], unread: old[names[i]] || 0});
        }
      } catch(e) {}
    }

    function updateNav() {
      document.getElementById('active-channel').textContent = activeChannel || 'ProjektX';
      var arrow = ddOpen ? ' ▲' : ' ▼';
      document.getElementById('btn-ch-text').textContent = '≡ Kanäle' + arrow;
      var hasUnread = false;
      for (var i = 0; i < channels.length; i++) {
        if (channels[i].name !== activeChannel && channels[i].unread > 0) { hasUnread = true; break; }
      }
      document.getElementById('unread-dot').style.display = hasUnread ? 'inline-block' : 'none';
    }

    function toggleDD() {
      ddOpen = !ddOpen;
      renderDD();
      updateNav();
    }

    function renderDD() {
      var dd = document.getElementById('channel-dropdown');
      if (!ddOpen) { dd.style.display = 'none'; return; }
      dd.style.display = 'block';
      dd.innerHTML = '';
      for (var i = 0; i < channels.length; i++) {
        (function(ch) {
          var div = document.createElement('div');
          div.className = 'ch-item' + (ch.name === activeChannel ? ' active' : '');
          var label = document.createElement('span');
          label.textContent = (ch.name === activeChannel ? '● ' : '  ') + ch.name;
          div.appendChild(label);
          if (ch.unread > 0 && ch.name !== activeChannel) {
            var badge = document.createElement('span');
            badge.className = 'ch-badge';
            badge.textContent = ch.unread;
            div.appendChild(badge);
          }
          div.onclick = function() { switchChannel(ch.name); };
          dd.appendChild(div);
        })(channels[i]);
      }
    }

    function switchChannel(name) {
      if (name === activeChannel) { ddOpen = false; renderDD(); updateNav(); return; }
      setLS(activeChannel, lastCount);
      activeChannel = name;
      localStorage.setItem('px_active', activeChannel);
      lastCount = 0;
      lastDisplayedTimestamp = 0;
      document.getElementById('messages').innerHTML = '';
      for (var i = 0; i < channels.length; i++) {
        if (channels[i].name === name) { channels[i].unread = 0; break; }
      }
      ddOpen = false;
      renderDD();
      updateNav();
      pollMessages();
    }

    function setStatus(msg, isError) {
      var el = document.getElementById('status');
      el.textContent = msg;
      el.className = isError ? 'error' : '';
    }

    async function sendMsg() {
      var textEl = document.getElementById('text');
      var btn = document.getElementById('send');
      var text = textEl.value.trim();
      if (!text) { setStatus('Nachricht eingeben', false); return; }
      if (!activeChannel) { setStatus('Kein Kanal', true); return; }
      btn.disabled = true;
      setStatus('Wird gesendet...', false);
      try {
        var body = 'text=' + encodeURIComponent(text) + '&ts=' + Date.now() + '&channel=' + encodeURIComponent(activeChannel);
        var res = await fetch('/send', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          body: body
        });
        if (res.ok) {
          textEl.value = '';
        } else if (res.status === 503) {
          setStatus('Vorherige Nachricht noch nicht abgeschlossen', true);
        } else {
          setStatus('Fehler beim Senden', true);
        }
      } catch(e) {
        setStatus('Verbindungsfehler', true);
      }
      btn.disabled = false;
    }

    async function pollMessages() {
      if (pollInFlight || !activeChannel) return;
      pollInFlight = true;
      try {
        var url = '/messages?since=' + lastCount + '&channel=' + encodeURIComponent(activeChannel);
        var res = await fetch(url);
        if (!res.ok) return;
        var data = await res.json();
        if (data.device) {
          myName = data.device;
        }
        if (data.sendResult !== undefined && data.sendResult !== 0) {
          switch (data.sendResult) {
            case 1: setStatus('', false); break;
            case 2: setStatus('', false); break;
            case 3: setStatus('Kanal belegt', true); break;
            case 4: setStatus('Senden fehlgeschlagen', true); break;
            case 5: setStatus('Kein Kanal konfiguriert', true); break;
          }
        }
        if (data.dutyCycleMs !== undefined) {
          var dc = document.getElementById('dutyCycleInfo');
          if (data.dutyCycleMs > 0) {
            var reason = '';
            if (data.dutyCycleReason === 1) reason = ' · eigene Nachricht';
            else if (data.dutyCycleReason === 2) reason = ' · Weiterleitung';
            dc.textContent = '⏳ Pause: ' + (data.dutyCycleMs / 1000).toFixed(0) + 's' + reason;
          } else {
            dc.textContent = '';
          }
        }
        var prevTotal = lastCount;
        if (data.messages && data.messages.length > 0) {
          var container = document.getElementById('messages');
          data.messages.forEach(function(msg) {
            var div = document.createElement('div');
            div.className = 'msg' + (msg.isOwn ? ' own' : '');
            var senderMeta = escHtml(msg.sender);
            if (msg.timestamp) {
              var isLate = lastDisplayedTimestamp > 0 && msg.timestamp < lastDisplayedTimestamp;
              senderMeta += ' · ' + fmtTime(msg.timestamp);
              if (isLate) senderMeta += ' · früher gesendet';
              lastDisplayedTimestamp = msg.timestamp;
            }
            var html = '<div class="sender">' + senderMeta + '</div>' +
                       '<div class="text">' + escHtml(msg.text) + '</div>';
            if (debugMode) {
              if (!msg.isOwn && msg.rssi !== undefined && msg.rssi !== 0) {
                var snr = (msg.snr !== undefined) ? msg.snr : null;
                var col = rssiSnrColor(msg.rssi, snr);
                var snrStr = snr !== null ? ' / ' + (snr >= 0 ? '+' : '') + snr + ' dB' : '';
                html += '<div class="debug"><span style="color:' + col + '">&#9679;</span> ' + msg.rssi + ' dBm' + snrStr + '</div>';
              }
              if (msg.isOwn && msg.airtimeMs !== undefined && msg.airtimeMs > 0) {
                html += '<div class="debug">Airtime: ' + msg.airtimeMs + 'ms</div>';
              }
            }
            div.innerHTML = html;
            container.appendChild(div);
          });
          lastCount = data.total;
          setLS(activeChannel, lastCount);
          container.scrollTop = container.scrollHeight;
        } else if (data.total !== undefined) {
          lastCount = data.total;
          setLS(activeChannel, lastCount);
        }
        if (data.total !== undefined && data.total > prevTotal) {
          var newForActive = data.messages ? data.messages.length : 0;
          var newTotal = data.total - prevTotal;
          if (newTotal > newForActive) {
            pollInactive();
          }
        }
      } catch(e) {}
      finally { pollInFlight = false; }
    }

    async function pollInactive() {
      for (var i = 0; i < channels.length; i++) {
        var ch = channels[i];
        if (ch.name === activeChannel) continue;
        try {
          var since = getLS(ch.name);
          var url = '/messages?since=' + since + '&channel=' + encodeURIComponent(ch.name);
          var res = await fetch(url);
          if (!res.ok) continue;
          var data = await res.json();
          if (data.messages && data.messages.length > 0) {
            ch.unread += data.messages.length;
          }
          if (data.total !== undefined) {
            setLS(ch.name, data.total);
          }
        } catch(e) {}
      }
      updateNav();
      if (ddOpen) renderDD();
    }

    function rssiSnrColor(rssi, snr) {
      var rl = rssi > -90 ? 0 : rssi > -105 ? 1 : rssi > -115 ? 2 : 3;
      var sl = snr === null ? 0 : snr > 7 ? 0 : snr >= 0 ? 1 : snr >= -10 ? 2 : 3;
      return ['#4caf50','#ffc107','#ff9800','#f44336'][Math.max(rl, sl)];
    }
    function fmtTime(unixSec) {
      var d = new Date(unixSec * 1000);
      return d.toLocaleTimeString('de-DE', {hour:'2-digit', minute:'2-digit', second:'2-digit'});
    }
    function escHtml(s) {
      return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
    }
  </script>
</body>
</html>
)rawhtml";
