#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <FastLED.h>
#include <Preferences.h>
#include "esp_wifi.h"  // 引入 ESP32 底层 Wi-Fi 控制库
#include "esp_event.h"

#if !defined(ARDUINO_EVENT_WIFI_STA_CONNECTED) && defined(SYSTEM_EVENT_STA_CONNECTED)
#define ARDUINO_EVENT_WIFI_STA_CONNECTED SYSTEM_EVENT_STA_CONNECTED
#endif
#if !defined(ARDUINO_EVENT_WIFI_STA_GOT_IP) && defined(SYSTEM_EVENT_STA_GOT_IP)
#define ARDUINO_EVENT_WIFI_STA_GOT_IP SYSTEM_EVENT_STA_GOT_IP
#endif
#if !defined(ARDUINO_EVENT_WIFI_STA_DISCONNECTED) && defined(SYSTEM_EVENT_STA_DISCONNECTED)
#define ARDUINO_EVENT_WIFI_STA_DISCONNECTED SYSTEM_EVENT_STA_DISCONNECTED
#endif

// ================= 1. 引脚定义 =================
#define IR_SEND_PIN 4
#define RGB_LED_PIN 48     // S3 内置 RGB 引脚
#define NUM_LEDS 1

// ================= 2. 引入红外库 =================
// 必须在定义 IR_SEND_PIN 之后引入
#include <IRremote.hpp>

// ================= 3. 多 Wi-Fi 备用配置 =================
struct WifiConfig {
  const char* ssid;
  const char* password;
};

WifiConfig wifiList[] = {
  {"YOUR_WIFI_2G", "YOUR_PASSWORD"},
  {"YOUR_BACKUP_WIFI", "YOUR_PASSWORD"}
};
const int wifiCount = sizeof(wifiList) / sizeof(wifiList[0]);

WebServer server(80);
CRGB leds[NUM_LEDS];
Preferences prefs;

// ================= 4. 全局状态变量 =================
unsigned long previousLedMillis = 0;

bool ledEnabled = true;
CRGB standbyColor = CRGB(0, 100, 255); // 默认待机颜色

const unsigned long wifiAttemptTimeout = 9000;      // 单个 SSID 连接超时
const unsigned long wifiRetryInterval = 2500;       // 下一次重试间隔
const uint8_t maxStaFailuresBeforeAP = 3;           // 失败到阈值后开启 AP 兜底

const char* fallbackApSsid = "RT4K-Remote";
const char* fallbackApPassword = "ChangeMe123";
const uint8_t fallbackApChannel = 6;
const bool forceStaTxPower = true;
const int8_t staTxPowerQdbm = 68; // 17.0 dBm, 稳定优先

bool staConnected = false;
bool staConnecting = false;
bool apFallbackActive = false;
bool mdnsReady = false;

int wifiIndex = 0;
uint8_t staFailureCount = 0;
unsigned long staAttemptStartMs = 0;
unsigned long lastStaActionMs = 0;
esp_event_handler_instance_t wifiDisconnectHandlerInstance = nullptr;

char savedWifiSsid[33] = {0};
char savedWifiPassword[65] = {0};
bool hasSavedWifi = false;

const char* wifiPrefNamespace = "rt4k_wifi";
const char* wifiPrefSsidKey = "ssid";
const char* wifiPrefPassKey = "pass";

String wifiUiState = "Booting";
String wifiUiReason = "None";
int wifiUiReasonCode = 0;
String wifiUiSsid = "";

// Forward declarations
void beginStaAttempt(int index);
void applyWifiTxPower();
int getTotalWifiCount();
const char* getWifiSsidByIndex(int index);
const char* getWifiPassByIndex(int index);
void loadSavedWifiCredential();
bool saveWifiCredential(const String& ssid, const String& password);
void clearSavedWifiCredential();
String jsonEscape(const String& input);
String wifiReasonToText(int reasonCode);
void setWifiReason(int reasonCode, const String& reasonText);
void setWifiReasonByCode(int reasonCode);
void onStaDisconnectedEspEvent(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

// ================= 5. HTML 前端 UI =================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <link rel="apple-touch-icon" href="https://raw.githubusercontent.com/mikechi2/RetroTINK-4K/main/Logo.png">
  <title>RT4K Pro</title>
  <style>
    :root {
      --bg: #1c1c1e; 
      --app-bg: #09090b;
      --surface: #1c1c1e;
      --text: #f5f5f7;
      --text-muted: #86868b;
      --accent-red: #ff453a;
      --accent-blue: #0a84ff;
      --accent-purple: #bf5af2;
      --accent-orange: #ff9f0a;
      --border: rgba(255,255,255,0.08);
      --safe-bottom: env(safe-area-inset-bottom, 20px);
      --app-height: 100vh;
      --tab-height: 60px;
    }
    @supports (height: 100dvh) {
      :root { --app-height: 100dvh; }
    }
    * { box-sizing: border-box; }
    html, body { width: 100%; height: var(--app-height); overflow: hidden; touch-action: manipulation; }
    body { 
      background: var(--bg); color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, "SF Pro Display", sans-serif; 
      margin: 0; padding: 0; user-select: none; 
      -webkit-user-select: none; -webkit-tap-highlight-color: transparent;
      height: var(--app-height); min-height: var(--app-height); display: flex; justify-content: center; overflow: hidden;
    }
    #app-container {
      width: 100%; max-width: 430px; height: var(--app-height); min-height: var(--app-height); background: var(--app-bg);
      position: relative; display: flex; flex-direction: column;
      border-left: 1px solid var(--border); border-right: 1px solid var(--border);
      box-shadow: 0 0 40px rgba(0,0,0,0.4);
      touch-action: manipulation;
    }
    .header {
      padding: 15px 20px 10px; display: flex; justify-content: space-between;
      align-items: center; background: rgba(9, 9, 11, 0.85);
      backdrop-filter: blur(10px); -webkit-backdrop-filter: blur(10px);
      z-index: 10; border-bottom: 1px solid var(--border);
    }
    .title-group { display: flex; flex-direction: column; }
    h2 { margin: 0; font-size: 18px; font-weight: 600; letter-spacing: 0.3px; }
    .status-wrap { display: flex; flex-direction: column; gap: 4px; margin-top: 4px; }
    .status-pill {
      display: inline-flex; align-items: center; width: fit-content; gap: 6px;
      font-size: 11px; line-height: 1; padding: 4px 9px; border-radius: 999px;
      border: 1px solid rgba(255,255,255,0.1); color: #d9d9de; background: rgba(255,255,255,0.04);
    }
    .status-dot { width: 7px; height: 7px; border-radius: 50%; background: #86868b; }
    .status-pill.connected { color: #b4f7c0; border-color: rgba(50,215,75,0.35); background: rgba(50,215,75,0.12); }
    .status-pill.connected .status-dot { background: #32d74b; }
    .status-pill.connecting { color: #ffd7a1; border-color: rgba(255,159,10,0.35); background: rgba(255,159,10,0.12); }
    .status-pill.connecting .status-dot { background: #ff9f0a; animation: statusPulse 1.05s ease-in-out infinite; }
    .status-pill.ap { color: #add5ff; border-color: rgba(10,132,255,0.35); background: rgba(10,132,255,0.14); }
    .status-pill.ap .status-dot { background: #0a84ff; }
    .status-pill.offline { color: #ffb3ad; border-color: rgba(255,69,58,0.35); background: rgba(255,69,58,0.13); }
    .status-pill.offline .status-dot { background: #ff453a; }
    .status-reason {
      font-size: 11px; color: var(--text-muted); max-width: 280px;
      white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
    }
    @keyframes statusPulse { 0%,100% { opacity: 1; } 50% { opacity: 0.35; } }
    .led { width: 8px; height: 8px; background-color: #333; border-radius: 50%;
           box-shadow: inset 0 1px 3px rgba(0,0,0,0.5); transition: 0.2s; }
    .led.active { background-color: #32d74b;
                  box-shadow: 0 0 10px #32d74b, inset 0 1px 2px rgba(255,255,255,0.5); }
    .scroll-container {
      flex: 1; overflow-y: auto; -webkit-overflow-scrolling: touch;
      scroll-behavior: smooth; padding: 15px; scrollbar-width: none;
    }
    .scroll-container::-webkit-scrollbar { display: none; }
    .bottom-spacer { height: calc(var(--tab-height) + 10px + var(--safe-bottom)); width: 100%; }
    .card { background: var(--surface); border-radius: 18px; margin-bottom: 15px; overflow: hidden; border: 1px solid var(--border); }
    .card-header { padding: 14px 18px; font-size: 14px; font-weight: 600; display: flex; justify-content: space-between; align-items: center; }
    .card-content { padding: 0 14px 14px 14px; }
    .grid { display: grid; gap: 8px; }
    .grid-2 { grid-template-columns: repeat(2, 1fr); }
    .grid-3 { grid-template-columns: repeat(3, 1fr); }
    .grid-4 { grid-template-columns: repeat(4, 1fr); }
    .d-pad { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; background: rgba(0,0,0,0.2); padding: 10px; border-radius: 16px; }
    button { background: rgba(255,255,255,0.06); color: var(--text); border: none; border-radius: 12px; padding: 14px 0; font-size: 13px; font-weight: 500; cursor: pointer; transition: 0.1s; touch-action: manipulation; }
    button:active { background: rgba(255,255,255,0.15); transform: scale(0.94); }
    .btn-red { color: var(--accent-red); font-weight: 600; background: rgba(255,69,58,0.1); }
    .btn-red:active { background: rgba(255,69,58,0.2); }
    .btn-system { font-weight: 600; }
    .btn-red-strong { color: #ff6b62; font-weight: 600; background: rgba(255,69,58,0.26); }
    .btn-red-strong:active { background: rgba(255,69,58,0.36); }
    .btn-blue { color: var(--accent-blue); font-weight: 600; background: rgba(10,132,255,0.1); }
    .btn-blue:active { background: rgba(10,132,255,0.2); }
    .empty { background: transparent; pointer-events: none; }
    .tab-bar {
      position: absolute; bottom: 0; left: 0; right: 0; height: calc(var(--tab-height) + var(--safe-bottom));
      background: #1c1c1e; backdrop-filter: blur(20px); -webkit-backdrop-filter: blur(20px);
      border-top: 1px solid var(--border); display: flex; justify-content: space-around;
      padding: 10px 20px var(--safe-bottom); z-index: 100;
    }
    .tab-item { display: flex; flex-direction: column; align-items: center; justify-content: center; color: var(--text-muted); font-size: 10px; font-weight: 500; cursor: pointer; width: 60px; transition: 0.2s; touch-action: manipulation; border-radius: 12px; }
    .tab-item.active { color: var(--accent-blue); }
    .press-feedback {
      transform: scale(0.90) translateY(1px) !important;
      filter: brightness(1.2);
      box-shadow: 0 0 0 1px rgba(10,132,255,0.38), 0 0 16px rgba(10,132,255,0.28);
      transition: transform 0.08s ease, filter 0.08s ease, box-shadow 0.08s ease;
    }
    .tab-icon { font-size: 20px; margin-bottom: 4px; }
    .page { display: none; animation: fadeIn 0.2s ease-out; }
    .page.active { display: block; }
    @keyframes fadeIn { from { opacity: 0; transform: translateY(10px); } to { opacity: 1; transform: translateY(0); } }
    .pro-card { background: linear-gradient(180deg, #1c1c1e 0%, #151517 100%); border: 1px solid rgba(255,255,255,0.05); }
    .pro-btn { display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 14px 0; gap: 6px; background: rgba(0,0,0,0.3); border: 1px solid rgba(255,255,255,0.03); }
    .pro-btn:active { background: rgba(0,0,0,0.5); }
    .pro-icon { font-size: 18px; line-height: 1; }
    .pro-label { font-size: 11px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; }
    .c-purple { color: var(--accent-purple); } .c-orange { color: var(--accent-orange); } .c-red { color: var(--accent-red); }
    .color-picker { -webkit-appearance: none; border: none; width: 100%; height: 44px; border-radius: 12px; cursor: pointer; padding: 0; background: none; }
    .color-picker::-webkit-color-swatch-wrapper { padding: 0; }
    .color-picker::-webkit-color-swatch { border: 1px solid var(--border); border-radius: 12px; }
    .dash-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1px; background: var(--border); border-radius: 12px; overflow: hidden; }
    .dash-item { background: var(--surface); padding: 12px; }
    .dash-label { font-size: 10px; color: var(--text-muted); text-transform: uppercase; margin-bottom: 4px; }
    .dash-val { font-size: 15px; font-family: monospace; color: #32d74b; }
    #toast-container { position: absolute; top: 15px; left: 0; right: 0; display: flex; flex-direction: column; align-items: center; gap: 8px; z-index: 9999; pointer-events: none; }
    .toast { background: var(--text); color: var(--app-bg); padding: 8px 16px; border-radius: 20px; font-size: 13px; font-weight: 600; box-shadow: 0 4px 12px rgba(0,0,0,0.3); opacity: 0; transform: translateY(-20px); transition: all 0.2s; }
    .toast.show { opacity: 1; transform: translateY(0); }
    .modal-overlay {
      position: absolute; inset: 0; z-index: 10010;
      background: rgba(0,0,0,0.55); backdrop-filter: blur(6px); -webkit-backdrop-filter: blur(6px);
      display: flex; align-items: center; justify-content: center; padding: 18px;
      opacity: 0; visibility: hidden; pointer-events: none;
      transition: opacity 0.2s ease;
    }
    .modal-overlay.show { opacity: 1; visibility: visible; pointer-events: auto; }
    .modal-card {
      width: 100%; max-width: 330px; border-radius: 16px;
      background: linear-gradient(180deg, #222226 0%, #17171a 100%);
      border: 1px solid var(--border); box-shadow: 0 20px 40px rgba(0,0,0,0.45);
      padding: 16px;
      opacity: 0;
      transform: translateY(22px) scale(0.98);
      transition: opacity 0.24s ease, transform 0.24s ease;
    }
    .modal-overlay.show .modal-card { opacity: 1; transform: translateY(0) scale(1); }
    .modal-title { font-size: 16px; font-weight: 700; margin-bottom: 8px; }
    .modal-message { font-size: 13px; color: var(--text-muted); line-height: 1.5; margin-bottom: 14px; }
    .modal-actions { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
    .modal-btn { border-radius: 10px; padding: 10px 0; font-weight: 600; }
    .modal-btn-cancel { background: rgba(255,255,255,0.08); color: var(--text); }
    .modal-btn-confirm { background: rgba(255,69,58,0.2); color: var(--accent-red); }
    .wifi-modal-overlay {
      align-items: center; justify-content: center; overflow: hidden;
      padding-top: max(12px, env(safe-area-inset-top, 0px));
      padding-bottom: max(12px, var(--safe-bottom));
    }
    .wifi-modal-card {
      max-width: 360px;
      max-height: calc(var(--app-height) - 24px - var(--safe-bottom));
      overflow-y: auto;
      -webkit-overflow-scrolling: touch;
    }
    .wifi-modal-overlay.keyboard-open {
      align-items: flex-start;
      overflow-y: auto;
      padding-top: max(12px, env(safe-area-inset-top, 0px));
      padding-bottom: calc(var(--safe-bottom) + 12px);
    }
    .wifi-label { font-size: 11px; color: var(--text-muted); margin: 8px 0 6px; text-transform: uppercase; }
    .wifi-input, .wifi-select {
      width: 100%; border: 1px solid var(--border); border-radius: 10px;
      background: rgba(255,255,255,0.06); color: var(--text); font-size: 13px;
      padding: 10px 12px; outline: none;
    }
    .wifi-input::placeholder { color: #9a9aa0; }
    .wifi-helper { font-size: 11px; color: var(--text-muted); margin-top: 8px; line-height: 1.4; }
    .wifi-actions { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-top: 12px; }
    .wifi-actions-3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; margin-top: 8px; }
  </style>
  <script>
    const AP_POPUP_ACK_KEY = "rt4k_ap_popup_ack_v1";
    let modalConfirmAction = null;
    let apPopupShown = false;
    let wifiScanItems = [];
    let viewportLockedForInput = false;
    let lockedViewportHeight = 0;
    let lastTouchEndTs = 0;
    try {
      apPopupShown = localStorage.getItem(AP_POPUP_ACK_KEY) === "1";
    } catch (e) {
      apPopupShown = false;
    }

    function haptic(ms) {
      if (navigator.vibrate) navigator.vibrate(ms || 16);
    }

    function isTextInputElement(el) {
      if (!el || !el.tagName) return false;
      const tag = el.tagName.toLowerCase();
      return tag === 'input' || tag === 'textarea' || tag === 'select';
    }

    function lockViewportForInput() {
      const current = parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--app-height')) || window.innerHeight;
      viewportLockedForInput = true;
      lockedViewportHeight = current > 0 ? current : window.innerHeight;
      document.documentElement.style.setProperty('--app-height', lockedViewportHeight + 'px');
      const wifiOverlay = document.getElementById('wifi-modal-overlay');
      if (wifiOverlay) wifiOverlay.classList.add('keyboard-open');
    }

    function unlockViewportForInput() {
      viewportLockedForInput = false;
      lockedViewportHeight = 0;
      const wifiOverlay = document.getElementById('wifi-modal-overlay');
      if (wifiOverlay) wifiOverlay.classList.remove('keyboard-open');
      syncViewportHeight();
    }

    function syncViewportHeight() {
      if (viewportLockedForInput) return;
      const h = (window.visualViewport && window.visualViewport.height) ? window.visualViewport.height : window.innerHeight;
      if (h && h > 0) {
        document.documentElement.style.setProperty('--app-height', h + 'px');
      }
    }

    function bindMobileInteractionGuards() {
      document.addEventListener('click', (e) => {
        if (e.target.closest('button, .tab-item')) {
          haptic(16);
        }
      }, { passive: true });

      const applyPressFeedback = (el) => {
        if (!el) return;
        el.classList.add('press-feedback');
        window.setTimeout(() => el.classList.remove('press-feedback'), 120);
      };
      document.addEventListener('pointerdown', (e) => {
        const target = e.target.closest('button, .tab-item');
        if (target) applyPressFeedback(target);
      }, { passive: true });

      // iOS: prevent double-tap zoom on non-input areas while keeping input behavior normal.
      document.addEventListener('touchend', (e) => {
        const now = Date.now();
        const tag = (e.target && e.target.tagName) ? e.target.tagName.toLowerCase() : "";
        const isInput = tag === "input" || tag === "textarea" || tag === "select";
        if (!isInput && now - lastTouchEndTs <= 320) {
          e.preventDefault();
        }
        lastTouchEndTs = now;
      }, { passive: false });

      // Safari pinch/double gesture guard
      document.addEventListener('gesturestart', (e) => e.preventDefault(), { passive: false });
    }

    function flashLED() {
      const led = document.getElementById('status-led');
      led.classList.add('active'); setTimeout(() => led.classList.remove('active'), 150);
    }
    function showToast(msg) {
      let tc = document.getElementById('toast-container');
      while (tc.children.length >= 3) {
        let first = tc.firstChild; first.classList.remove('show');
        setTimeout(() => first.remove(), 200); tc.removeChild(first);
      }
      let toast = document.createElement('div'); toast.className = 'toast'; toast.innerText = msg;
      tc.appendChild(toast); requestAnimationFrame(() => toast.classList.add('show'));
      setTimeout(() => { 
        if (toast.parentElement) { toast.classList.remove('show'); setTimeout(() => toast.remove(), 200); }
      }, 1000);
    }

    function showModal(title, message, confirmText, onConfirm, cancelText) {
      const overlay = document.getElementById('modal-overlay');
      const cancelBtn = document.getElementById('modal-cancel');
      const confirmBtn = document.getElementById('modal-confirm');
      document.getElementById('modal-title').innerText = title;
      document.getElementById('modal-message').innerText = message;
      confirmBtn.innerText = confirmText || "OK";
      modalConfirmAction = onConfirm || null;
      if (onConfirm) {
        cancelBtn.style.display = "block";
        cancelBtn.innerText = cancelText || "Cancel";
        confirmBtn.style.gridColumn = "auto";
      } else {
        cancelBtn.style.display = "none";
        confirmBtn.style.gridColumn = "1 / -1";
      }
      overlay.classList.add('show');
    }

    function closeModal() {
      document.getElementById('modal-overlay').classList.remove('show');
      modalConfirmAction = null;
    }

    function confirmModalAction() {
      const fn = modalConfirmAction;
      closeModal();
      if (typeof fn === 'function') fn();
    }

    function openWifiManager() {
      unlockViewportForInput();
      document.getElementById('wifi-modal-overlay').classList.add('show');
      document.getElementById('wifi-helper').innerText = "Tap Scan to load nearby Wi-Fi.";
    }

    function closeWifiManager() {
      const active = document.activeElement;
      if (active && typeof active.blur === 'function') active.blur();
      document.getElementById('wifi-modal-overlay').classList.remove('show');
      unlockViewportForInput();
    }

    function renderWifiSelect(items, savedSsid) {
      const select = document.getElementById('wifi-select');
      select.innerHTML = "";

      const placeholder = document.createElement('option');
      placeholder.value = "";
      placeholder.textContent = items.length > 0 ? "Select Wi-Fi from scan..." : "No Wi-Fi found";
      select.appendChild(placeholder);

      items.forEach(item => {
        const opt = document.createElement('option');
        opt.value = item.ssid;
        const signal = typeof item.rssi === 'number' ? ` (${item.rssi}dBm)` : "";
        opt.textContent = item.ssid + signal;
        if (savedSsid && item.ssid === savedSsid) opt.selected = true;
        select.appendChild(opt);
      });

      if (savedSsid && !items.find(i => i.ssid === savedSsid)) {
        const saved = document.createElement('option');
        saved.value = savedSsid;
        saved.textContent = savedSsid + " (Saved)";
        saved.selected = true;
        select.appendChild(saved);
      }
    }

    function scanWifiList() {
      const helper = document.getElementById('wifi-helper');
      helper.innerText = "Scanning nearby Wi-Fi...";

      if (window.location.protocol === 'file:') {
        wifiScanItems = [
          { ssid: "Home_2.4G", rssi: -48 },
          { ssid: "Office_2.4G", rssi: -61 },
          { ssid: "GameRoom", rssi: -72 }
        ];
        renderWifiSelect(wifiScanItems, "Home_2.4G");
        helper.innerText = "Preview mode: sample Wi-Fi list loaded.";
        return;
      }

      fetch('/api/wifi/scan')
        .then(r => r.json())
        .then(data => {
          wifiScanItems = Array.isArray(data.items) ? data.items : [];
          const savedSsid = data.savedSsid || "";
          renderWifiSelect(wifiScanItems, savedSsid);
          helper.innerText = wifiScanItems.length > 0
            ? `Found ${wifiScanItems.length} Wi-Fi network(s).`
            : "No Wi-Fi found. You can type SSID manually below.";
          document.getElementById('wifi-manual-ssid').value = savedSsid;
        })
        .catch(() => {
          helper.innerText = "Scan failed. Try manual SSID input.";
          showToast("Wi-Fi Scan Failed");
        });
    }

    function saveWifiFromManager() {
      const selected = document.getElementById('wifi-select').value.trim();
      const manual = document.getElementById('wifi-manual-ssid').value.trim();
      const password = document.getElementById('wifi-password').value;
      const ssid = manual.length > 0 ? manual : selected;
      if (!ssid) {
        showToast("Please select or input SSID");
        return;
      }

      if (window.location.protocol === 'file:') {
        showToast("Saved: " + ssid);
        closeWifiManager();
        return;
      }

      const url = '/api/wifi/save?ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(password);
      fetch(url)
        .then(r => r.json())
        .then(resp => {
          if (resp.ok) {
            showToast("Saved and reconnecting...");
            closeWifiManager();
          } else {
            showToast("Save Failed");
          }
        })
        .catch(() => showToast("Save Failed"));
    }

    function forgetSavedWifi() {
      if (window.location.protocol === 'file:') {
        showToast("Saved Wi-Fi cleared");
        return;
      }
      fetch('/api/wifi/forget')
        .then(() => {
          document.getElementById('wifi-manual-ssid').value = "";
          document.getElementById('wifi-password').value = "";
          showToast("Saved Wi-Fi cleared");
        })
        .catch(() => showToast("Forget Failed"));
    }
    
    function sendCmd(cmd) {
      flashLED();
      if (window.location.protocol === 'file:') {
        showToast("Sent: " + cmd);
      } else {
        fetch('/ir?cmd=' + cmd)
          .then(() => showToast("Sent: " + cmd))
          .catch(e => {
            console.error(e);
            showToast("Send Failed");
          });
      }
    }
    
    function switchTab(tabId) {
      const scroller = document.querySelector('.scroll-container');
      scroller.style.scrollBehavior = 'auto'; scroller.scrollTop = 0; scroller.style.scrollBehavior = 'smooth';
      document.querySelectorAll('.tab-item').forEach(el => el.classList.remove('active'));
      document.querySelectorAll('.page').forEach(el => el.classList.remove('active'));
      document.getElementById('tab-' + tabId).classList.add('active');
      document.getElementById('page-' + tabId).classList.add('active');
    }
    
    function toggleEspLed() {
      if (window.location.protocol === 'file:') showToast("LED Toggled");
      else fetch('/api/led?toggle=1').then(r=>r.text()).then(t=>showToast("LED " + t));
    }
    
    function setEspColor(hex) {
      const hexColor = hex.replace('#', '');
      if (window.location.protocol === 'file:') showToast("Color: #" + hexColor);
      else fetch('/api/led?color=' + hexColor);
    }
    
    function rebootEsp() {
      showModal("Reboot ESP32", "Are you sure you want to reboot now?", "Reboot", () => {
        showToast("Rebooting...");
        if (window.location.protocol !== 'file:') fetch('/api/reboot');
      }, "Cancel");
    }

    function retryWifiSta() {
      showToast("Retrying Wi-Fi...");
      if (window.location.protocol !== 'file:') {
        fetch('/api/wifi/retry').catch(()=>{});
      }
    }
    
    // 【防塞车修复】改用链式 setTimeout 替代 setInterval
    function updateDashboard() {
      if (window.location.protocol === 'file:') {
        document.getElementById('d-ip').innerText = "192.168.1.10";
        document.getElementById('d-rssi').innerText = "-45 dBm";
        document.getElementById('d-time').innerText = "2h 15m";
        document.getElementById('d-temp').innerText = "42.0°C";
        const statusPill = document.getElementById('status-pill');
        const statusMain = document.getElementById('status-main');
        const statusReason = document.getElementById('status-reason');
        if (statusPill && statusMain && statusReason) {
          statusPill.className = "status-pill connected";
          statusMain.innerText = "Connected";
          statusReason.innerText = "Preview mode";
        }
        setTimeout(updateDashboard, 5000);
        return;
      }
      
      fetch('/status')
        .then(r => r.json())
        .then(d => {
          document.getElementById('d-ip').innerText = d.ip;
          document.getElementById('d-rssi').innerText = d.rssi + " dBm";
          document.getElementById('d-time').innerText = d.uptime;
          document.getElementById('d-temp').innerText = d.temp + "°C";
          const statusPill = document.getElementById('status-pill');
          const statusMain = document.getElementById('status-main');
          const statusReason = document.getElementById('status-reason');
          if (statusPill && statusMain && statusReason) {
            let pillClass = "offline";
            let mainText = d.state || "Offline";
            let reasonText = "";

            if (d.mode === "AP") {
              pillClass = "ap";
              mainText = "AP Fallback";
              reasonText = "AP IP: " + (d.ip || "192.168.4.1");
            } else if (d.mode === "STA" && d.state === "Connected") {
              pillClass = "connected";
              mainText = "Connected";
              reasonText = d.ssid ? ("SSID: " + d.ssid + " (" + d.rssi + " dBm)") : ("RSSI: " + d.rssi + " dBm");
            } else if ((d.state || "").toLowerCase().includes("connect")) {
              pillClass = "connecting";
              mainText = d.state || "Connecting";
            }

            if ((d.reason || "None") !== "None") {
              reasonText = d.reasonCode ? (d.reason + " (code " + d.reasonCode + ")") : d.reason;
            }

            statusPill.className = "status-pill " + pillClass;
            statusMain.innerText = mainText;
            statusReason.innerText = reasonText || "Ready";
          }

          if (d.mode === "STA") {
            apPopupShown = false;
            try { localStorage.removeItem(AP_POPUP_ACK_KEY); } catch (e) {}
          }
          if (!apPopupShown && d.mode === "AP") {
            apPopupShown = true;
            try { localStorage.setItem(AP_POPUP_ACK_KEY, "1"); } catch (e) {}
            showModal("AP Fallback Enabled", "Wi-Fi unavailable. Connect hotspot \"RT4K-Remote\" to keep controlling the device.", "Got it");
          }
        })
        .catch(e => { console.log("Status fetch error", e); })
        .finally(() => {
          // 上次请求彻底结束后，再开启下一个 5 秒轮询
          setTimeout(updateDashboard, 5000); 
        });
    }
    
    window.addEventListener('resize', syncViewportHeight);
    window.addEventListener('orientationchange', syncViewportHeight);
    if (window.visualViewport) {
      window.visualViewport.addEventListener('resize', syncViewportHeight);
    }
    document.addEventListener('focusin', (e) => {
      const wifiModal = document.getElementById('wifi-modal-overlay');
      if (wifiModal && wifiModal.classList.contains('show') && isTextInputElement(e.target)) {
        lockViewportForInput();
      }
    }, true);
    document.addEventListener('focusout', () => {
      setTimeout(() => {
        const ae = document.activeElement;
        if (!isTextInputElement(ae)) {
          unlockViewportForInput();
        }
      }, 120);
    }, true);

    // 页面加载完毕后启动第一次查询
    window.onload = () => {
      bindMobileInteractionGuards();
      syncViewportHeight();
      updateDashboard();
    };
  </script>
</head>
<body>
<div id="app-container">
  <div id="toast-container"></div>
  <div id="wifi-modal-overlay" class="modal-overlay wifi-modal-overlay" onclick="if(event.target===this) closeWifiManager()">
    <div class="modal-card wifi-modal-card">
      <div class="modal-title">Wi-Fi Manager</div>
      <div class="wifi-label">Nearby Wi-Fi</div>
      <select id="wifi-select" class="wifi-select"></select>
      <div class="wifi-actions-3">
        <button class="modal-btn" onclick="scanWifiList()">Scan</button>
        <button class="modal-btn btn-blue" onclick="retryWifiSta()">Retry STA</button>
        <button class="modal-btn btn-red" onclick="forgetSavedWifi()">Forget</button>
      </div>
      <div class="wifi-label">Manual SSID (optional)</div>
      <input id="wifi-manual-ssid" class="wifi-input" type="text" maxlength="32" placeholder="Type SSID if hidden">
      <div class="wifi-label">Password</div>
      <input id="wifi-password" class="wifi-input" type="password" maxlength="64" placeholder="Wi-Fi password">
      <div id="wifi-helper" class="wifi-helper">Tap Scan to load nearby Wi-Fi, then save to connect.</div>
      <div class="wifi-actions">
        <button class="modal-btn modal-btn-cancel" onclick="closeWifiManager()">Close</button>
        <button class="modal-btn btn-blue" onclick="saveWifiFromManager()">Save & Connect</button>
      </div>
    </div>
  </div>
  <div id="modal-overlay" class="modal-overlay" onclick="if(event.target===this) closeModal()">
    <div class="modal-card">
      <div id="modal-title" class="modal-title">Notice</div>
      <div id="modal-message" class="modal-message">Message</div>
      <div class="modal-actions">
        <button id="modal-cancel" class="modal-btn modal-btn-cancel" onclick="closeModal()">Cancel</button>
        <button id="modal-confirm" class="modal-btn modal-btn-confirm" onclick="confirmModalAction()">OK</button>
      </div>
    </div>
  </div>
  <div class="header">
    <div class="title-group">
      <h2>RetroTINK-4K</h2>
      <div class="status-wrap">
        <div id="status-pill" class="status-pill connecting"><span class="status-dot"></span><span id="status-main">Booting</span></div>
        <div id="status-reason" class="status-reason">Initializing Wi-Fi...</div>
      </div>
    </div>
    <div class="led" id="status-led"></div>
  </div>
  <div class="scroll-container">
    <div id="page-main" class="page active">
      <div class="card"><div class="card-content" style="padding-top: 14px;">
          <div class="grid grid-3" style="margin-bottom: 12px;">
            <button class="btn-red" onclick="sendCmd(220)">POWER</button><button onclick="sendCmd(185)">STAT</button><button onclick="sendCmd(184)">DIAG</button>
          </div>
          <div class="d-pad">
            <button onclick="sendCmd(226)">MENU</button><button onclick="sendCmd(202)">UP</button><button onclick="sendCmd(197)">BACK</button>
            <button onclick="sendCmd(153)">LEFT</button><button class="btn-blue" style="background: rgba(10,132,255,0.2);" onclick="sendCmd(206)">OK</button><button onclick="sendCmd(193)">RIGHT</button>
            <div class="empty"></div><button onclick="sendCmd(210)">DOWN</button><div class="empty"></div>
          </div>
      </div></div>
      <div class="card"><div class="card-header">Video Processing</div><div class="card-content">
          <div class="grid grid-3">
            <button onclick="sendCmd(176)">INPUT</button><button onclick="sendCmd(177)">OUTPUT</button><button onclick="sendCmd(178)">SCALER</button>
            <button class="btn-blue" onclick="sendCmd(181)">PROFILE</button><button onclick="sendCmd(180)">ADC</button><button onclick="sendCmd(191)">BUFFER</button> 
          </div>
      </div></div>
      <div class="card"><div class="card-header">Resolutions</div><div class="card-content">
          <div class="grid grid-4">
            <button class="btn-blue" onclick="sendCmd(208)">4K</button><button onclick="sendCmd(211)">1440P</button>
            <button onclick="sendCmd(209)">1080P</button><button onclick="sendCmd(212)">480P</button>
            <button onclick="sendCmd(213)">RES 1</button><button onclick="sendCmd(214)">RES 2</button><button onclick="sendCmd(215)">RES 3</button><button onclick="sendCmd(216)">RES 4</button>
          </div>
      </div></div>
      <div class="card pro-card"><div class="card-header" style="color: var(--text-muted); font-size: 12px; letter-spacing: 1px;">PRO FEATURES</div><div class="card-content">
          <div class="grid grid-3">
            <button class="pro-btn" onclick="sendCmd(187)"><div class="pro-icon c-purple">⏸️</div><div class="pro-label c-purple">Freeze</div></button>
            <button class="pro-btn" onclick="sendCmd(179)"><div class="pro-icon c-orange">📺</div><div class="pro-label c-orange">CRT FX</div></button>
            <button class="pro-btn" onclick="sendCmd(190)"><div class="pro-icon c-red">🛡️</div><div class="pro-label c-red">Safe Mode</div></button>
          </div>
      </div></div>
      <div class="bottom-spacer"></div>
    </div>
    <div id="page-settings" class="page">
      <div class="card"><div class="card-header">Advanced Tweaks</div><div class="card-content">
          <div class="grid grid-3"><button onclick="sendCmd(186)">GAIN</button><button onclick="sendCmd(189)">PHASE</button><button onclick="sendCmd(188)">GENLOCK</button></div>
      </div></div>
      <div class="card"><div class="card-header">Profiles (1-12)</div><div class="card-content">
          <div class="grid grid-4">
            <button onclick="sendCmd(146)">P 1</button><button onclick="sendCmd(147)">P 2</button><button onclick="sendCmd(204)">P 3</button><button onclick="sendCmd(142)">P 4</button>
            <button onclick="sendCmd(143)">P 5</button><button onclick="sendCmd(200)">P 6</button><button onclick="sendCmd(138)">P 7</button><button onclick="sendCmd(139)">P 8</button>
            <button onclick="sendCmd(196)">P 9</button><button onclick="sendCmd(135)">P 10</button><button onclick="sendCmd(182)">P 11</button><button onclick="sendCmd(183)">P 12</button>
          </div>
      </div></div>
      <div class="card"><div class="card-header">Custom AUX Keys</div><div class="card-content">
          <div class="grid grid-4">
            <button onclick="sendCmd(225)">AUX 1</button><button onclick="sendCmd(229)">AUX 2</button><button onclick="sendCmd(227)">AUX 3</button><button onclick="sendCmd(228)">AUX 4</button>
            <button onclick="sendCmd(221)">AUX 5</button><button onclick="sendCmd(222)">AUX 6</button><button onclick="sendCmd(223)">AUX 7</button><button onclick="sendCmd(224)">AUX 8</button>
          </div>
      </div></div>
      <div class="card"><div class="card-header" style="color: #0a84ff;">Wi-Fi Manager</div><div class="card-content">
          <button class="btn-blue" style="width: 100%;" onclick="openWifiManager()">Configure Wi-Fi</button>
          <div style="font-size: 12px; color: var(--text-muted); margin-top: 8px;">Scan nearby Wi-Fi, input password, and save for auto reconnect.</div>
      </div></div>
      <div class="card"><div class="card-header" style="color: #0a84ff;">ESP32 System Control</div><div class="card-content">
          <div class="grid grid-2" style="margin-bottom: 8px;"><button class="btn-system" onclick="toggleEspLed()">Toggle LED</button><button class="btn-blue btn-system" onclick="retryWifiSta()">Retry Wi-Fi STA</button></div>
          <div class="grid grid-2" style="margin-bottom: 8px;"><button class="btn-red btn-red-strong btn-system" style="grid-column: 1 / -1;" onclick="rebootEsp()">Reboot ESP32</button></div>
          <div style="font-size: 12px; color: var(--text-muted); margin-bottom: 6px;">LED Color (Standby)</div>
          <input type="color" class="color-picker" id="led-color" value="#0064ff" onchange="setEspColor(this.value)">
      </div></div>
      <div class="card"><div class="card-header" style="color: #32d74b;">Telemetry Dashboard</div><div class="card-content">
          <div class="dash-grid">
            <div class="dash-item"><div class="dash-label">IP Address</div><div class="dash-val" id="d-ip">--</div></div>
            <div class="dash-item"><div class="dash-label">Signal (RSSI)</div><div class="dash-val" id="d-rssi">--</div></div>
            <div class="dash-item"><div class="dash-label">Core Temp</div><div class="dash-val" id="d-temp">--</div></div>
            <div class="dash-item"><div class="dash-label">Uptime</div><div class="dash-val" id="d-time">--</div></div>
          </div>
      </div></div>
      <div class="bottom-spacer"></div>
    </div>
  </div>
  <div class="tab-bar">
    <div class="tab-item active" id="tab-main" onclick="switchTab('main')"><div class="tab-icon">🎮</div><div>Control</div></div>
    <div class="tab-item" id="tab-settings" onclick="switchTab('settings')"><div class="tab-icon">⚙️</div><div>Settings</div></div>
  </div>
</div>
</body>
</html>
)rawliteral";

// ================= 6. 辅助控制函数 =================
void ledStandby() {
  if (!ledEnabled) return;
  float breath = (exp(sin(millis() / 2000.0 * PI)) - 0.36787944) * 108.0;
  int brightness = map(breath, 0, 255, 2, 15);
  leds[0] = standbyColor;
  FastLED.setBrightness(brightness);
  FastLED.show();
}

void ledFlash(CRGB color) {
  if (!ledEnabled) return;
  FastLED.setBrightness(255);
  leds[0] = color;
  FastLED.show();
  delay(50);
  leds[0] = CRGB::Black;
  FastLED.show();
}

String getUptime() {
  unsigned long seconds = millis() / 1000;
  return String(seconds / 3600) + "h " + String((seconds / 60) % 60) + "m";
}

float getCpuTemp() {
  return temperatureRead(); 
}

CRGB hexToCRGB(String hex) {
  long number = strtol(&hex[0], NULL, 16);
  long r = number >> 16;
  long g = number >> 8 & 0xFF;
  long b = number & 0xFF;
  return CRGB(r, g, b);
}

int getTotalWifiCount() {
  return wifiCount + (hasSavedWifi ? 1 : 0);
}

const char* getWifiSsidByIndex(int index) {
  if (hasSavedWifi) {
    if (index == 0) return savedWifiSsid;
    index--;
  }
  if (index < 0 || index >= wifiCount) return "";
  return wifiList[index].ssid;
}

const char* getWifiPassByIndex(int index) {
  if (hasSavedWifi) {
    if (index == 0) return savedWifiPassword;
    index--;
  }
  if (index < 0 || index >= wifiCount) return "";
  return wifiList[index].password;
}

void loadSavedWifiCredential() {
  prefs.begin(wifiPrefNamespace, true);
  String ssid = prefs.getString(wifiPrefSsidKey, "");
  String pass = prefs.getString(wifiPrefPassKey, "");
  prefs.end();

  if (ssid.length() > 0 && ssid.length() <= 32 && pass.length() <= 64) {
    ssid.toCharArray(savedWifiSsid, sizeof(savedWifiSsid));
    pass.toCharArray(savedWifiPassword, sizeof(savedWifiPassword));
    hasSavedWifi = true;
    Serial.print("[WiFi] Loaded saved SSID: ");
    Serial.println(savedWifiSsid);
  } else {
    hasSavedWifi = false;
    savedWifiSsid[0] = '\0';
    savedWifiPassword[0] = '\0';
  }
}

bool saveWifiCredential(const String& ssid, const String& password) {
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) {
    return false;
  }

  prefs.begin(wifiPrefNamespace, false);
  prefs.putString(wifiPrefSsidKey, ssid);
  prefs.putString(wifiPrefPassKey, password);
  prefs.end();

  ssid.toCharArray(savedWifiSsid, sizeof(savedWifiSsid));
  password.toCharArray(savedWifiPassword, sizeof(savedWifiPassword));
  hasSavedWifi = true;

  Serial.print("[WiFi] Saved SSID from manager: ");
  Serial.println(savedWifiSsid);
  return true;
}

void clearSavedWifiCredential() {
  prefs.begin(wifiPrefNamespace, false);
  prefs.remove(wifiPrefSsidKey);
  prefs.remove(wifiPrefPassKey);
  prefs.end();

  hasSavedWifi = false;
  savedWifiSsid[0] = '\0';
  savedWifiPassword[0] = '\0';
  Serial.println("[WiFi] Cleared saved Wi-Fi credential.");
}

String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

String wifiReasonToText(int reasonCode) {
  switch (reasonCode) {
    case 0: return "None";
    case -100: return "Connect timeout";
    case 2: return "Auth expired";
    case 3: return "Auth leave";
    case 4: return "Association expired";
    case 5: return "Too many clients";
    case 6: return "Not authenticated";
    case 7: return "Not associated";
    case 8: return "Association leave";
    case 9: return "Assoc not authed";
    case 15: return "4-way handshake timeout";
    case 16: return "Group key timeout";
    case 23: return "802.1X auth failed";
    case 200: return "Beacon timeout";
    case 201: return "No AP found";
    case 202: return "Authentication failed";
    case 203: return "Association failed";
    case 204: return "Handshake timeout";
    default: return "Disconnect reason " + String(reasonCode);
  }
}

void setWifiReason(int reasonCode, const String& reasonText) {
  wifiUiReasonCode = reasonCode;
  wifiUiReason = reasonText;
}

void setWifiReasonByCode(int reasonCode) {
  setWifiReason(reasonCode, wifiReasonToText(reasonCode));
}

void onStaDisconnectedEspEvent(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED && event_data != nullptr) {
    wifi_event_sta_disconnected_t* disc = reinterpret_cast<wifi_event_sta_disconnected_t*>(event_data);
    setWifiReasonByCode((int)disc->reason);
    Serial.print("[WiFi] Disconnect reason code: ");
    Serial.print((int)disc->reason);
    Serial.print(" (");
    Serial.print(wifiUiReason);
    Serial.println(")");
  }
}

// ================= 7. 核心 Wi-Fi 连接器 =================
void startApFallback() {
  if (apFallbackActive) return;

  Serial.println("[WiFi] Starting fallback AP...");
  wifiUiState = "AP Fallback";
  wifiUiSsid = fallbackApSsid;
  // 进入 AP-only，避免 STA 频繁扫网切信道导致手机无法稳定加入热点
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(fallbackApSsid, fallbackApPassword, fallbackApChannel, 0, 4);
  if (ok) {
    apFallbackActive = true;
    staConnecting = false;
    staConnected = false;
    Serial.print("[WiFi] Fallback AP SSID: ");
    Serial.println(fallbackApSsid);
    Serial.print("[WiFi] Fallback AP IP: ");
    Serial.println(WiFi.softAPIP());
    ledFlash(CRGB::Orange);
  } else {
    Serial.println("[WiFi] Failed to start fallback AP.");
  }
}

void stopApFallback() {
  if (!apFallbackActive) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apFallbackActive = false;
  wifiUiState = "Reconnecting";
  Serial.println("[WiFi] Fallback AP stopped.");
}

void retryStaFromFallback() {
  Serial.println("[WiFi] Manual retry from fallback AP.");
  stopApFallback();
  staFailureCount = 0;
  wifiIndex = 0;
  lastStaActionMs = 0;
  wifiUiState = "Reconnecting";
  setWifiReason(0, "Manual retry");
  beginStaAttempt(wifiIndex);
}

void ensureMdnsStarted() {
  if (!mdnsReady && staConnected) {
    if (MDNS.begin("rt4k")) {
      mdnsReady = true;
      Serial.println("[mDNS] Ready at rt4k.local");
    } else {
      Serial.println("[mDNS] Start failed, will retry after reconnect.");
    }
  }
}

void applyWifiTxPower() {
  if (!forceStaTxPower) return;

  // 68 quarter-dBm = 17.0 dBm，通常比拉满功率更稳
  esp_err_t err = esp_wifi_set_max_tx_power(staTxPowerQdbm);
  if (err == ESP_OK) {
    int8_t qdbm = 0;
    if (esp_wifi_get_max_tx_power(&qdbm) == ESP_OK) {
      Serial.print("[WiFi] TX power set: ");
      Serial.print((float)qdbm / 4.0f, 1);
      Serial.println(" dBm");
    } else {
      Serial.println("[WiFi] TX power set (readback failed).");
    }
  } else {
    Serial.print("[WiFi] TX power set failed, err=");
    Serial.println((int)err);
  }
}

void configureWifiRadio() {
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
}

void markStaAttemptFailed(const char* reason) {
  int totalWifiCount = getTotalWifiCount();
  if (!staConnecting || totalWifiCount == 0) return;

  staConnecting = false;
  staConnected = false;
  staFailureCount++;
  wifiIndex = (wifiIndex + 1) % totalWifiCount;
  lastStaActionMs = millis();
  wifiUiState = "Reconnecting";
  if (strcmp(reason, "timeout") == 0) {
    setWifiReasonByCode(-100);
  } else {
    setWifiReason(-1, String(reason));
  }

  Serial.print("[WiFi] Attempt failed (");
  Serial.print(reason);
  Serial.print("), failure=");
  Serial.print(staFailureCount);
  Serial.print(", next SSID: ");
  Serial.println(getWifiSsidByIndex(wifiIndex));

  if (!apFallbackActive && staFailureCount >= maxStaFailuresBeforeAP) {
    startApFallback();
  }
}

void beginStaAttempt(int index) {
  int totalWifiCount = getTotalWifiCount();
  if (totalWifiCount == 0) return;

  if (index < 0 || index >= totalWifiCount) {
    index = 0;
  }
  wifiIndex = index;

  WiFi.mode(WIFI_STA);
  applyWifiTxPower();
  const char* attemptSsid = getWifiSsidByIndex(wifiIndex);
  const char* attemptPass = getWifiPassByIndex(wifiIndex);
  WiFi.begin(attemptSsid, attemptPass);
  staConnecting = true;
  staAttemptStartMs = millis();
  lastStaActionMs = staAttemptStartMs;
  wifiUiState = "Connecting";
  wifiUiSsid = String(attemptSsid);

  Serial.print("[WiFi] Trying SSID: ");
  Serial.println(getWifiSsidByIndex(wifiIndex));
}

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi] STA associated.");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      staConnected = true;
      staConnecting = false;
      staFailureCount = 0;
      lastStaActionMs = millis();
      wifiUiState = "Connected";
      wifiUiSsid = WiFi.SSID();
      setWifiReason(0, "None");
      Serial.print("[WiFi] Connected, IP: ");
      Serial.println(WiFi.localIP());
      stopApFallback();
      ensureMdnsStarted();
      ledFlash(CRGB::Green);
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (staConnecting) {
        // 仅在“连接尝试中”把本次断连记为失败，避免空闲断连事件反复重置重试计时器
        markStaAttemptFailed("disconnect event");
      } else if (staConnected) {
        // 已连接后掉线，尽快启动下一次重连
        staConnected = false;
        lastStaActionMs = 0;
        wifiUiState = "Disconnected";
        Serial.println("[WiFi] STA disconnected.");
      } else {
        // 未连接且未在尝试中，忽略噪声断连事件，不重置重试计时
      }
      staConnected = false;
      break;

    default:
      break;
  }
}

void handleWiFiStateMachine() {
  unsigned long now = millis();
  int totalWifiCount = getTotalWifiCount();

  if (staConnected || totalWifiCount == 0) {
    return;
  }

  // AP 兜底模式下暂停 STA 扫网，保证热点稳定可连接
  if (apFallbackActive) {
    return;
  }

  if (staConnecting) {
    if (now - staAttemptStartMs >= wifiAttemptTimeout) {
      Serial.print("[WiFi] Timeout on SSID: ");
      Serial.println(getWifiSsidByIndex(wifiIndex));
      markStaAttemptFailed("timeout");
      WiFi.disconnect(false, false);
    }
    return;
  }

  if (now - lastStaActionMs >= wifiRetryInterval) {
    beginStaAttempt(wifiIndex);
  }
}

// ================= 8. SETUP 初始化 =================
void setup() {
  Serial.begin(115200);
  
  // LED 初始化
  FastLED.addLeds<WS2812, RGB_LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(20);
  leds[0] = CRGB::Orange;
  FastLED.show();

  // IR 初始化
  IrSender.begin(IR_SEND_PIN);

  // WiFi 初始化（非阻塞重连状态机）
  WiFi.onEvent(onWiFiEvent);
  esp_err_t evtRegErr = esp_event_handler_instance_register(
    WIFI_EVENT,
    WIFI_EVENT_STA_DISCONNECTED,
    &onStaDisconnectedEspEvent,
    nullptr,
    &wifiDisconnectHandlerInstance
  );
  if (evtRegErr != ESP_OK) {
    Serial.print("[WiFi] Failed to register disconnect reason handler, err=");
    Serial.println((int)evtRegErr);
  }
  configureWifiRadio();
  loadSavedWifiCredential();
  WiFi.mode(WIFI_STA);
  applyWifiTxPower();
  WiFi.disconnect(false, false);
  beginStaAttempt(0);

  // 同步时间戳防越界
  previousLedMillis = millis();

  // OTA 服务
  ArduinoOTA.setHostname("rt4k-remote");
  ArduinoOTA.onStart([]() { leds[0] = CRGB::Purple; FastLED.setBrightness(100); FastLED.show(); });
  ArduinoOTA.onEnd([]() { ledFlash(CRGB::Green); });
  ArduinoOTA.onError([](ota_error_t error) { ledFlash(CRGB::Red); });
  ArduinoOTA.begin();

  // ================= Web 服务器 API 路由 =================
  server.on("/", []() { 
    // 发送网页时也添加 Connection: close 避免长连接积压
    server.sendHeader("Connection", "close");
    // Use PROGMEM response to avoid large RAM copy when serving HTML.
    server.send_P(200, "text/html", index_html);
  });
  
  server.on("/ir", []() {
    if (server.hasArg("cmd")) {
      int cmd = server.arg("cmd").toInt();
      ledFlash(CRGB::LimeGreen);
      IrSender.sendNEC(179, cmd, 0); 
      
      // 【修复】：强行关闭 TCP 连接，释放 Socket 槽位
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", "OK");
    } else {
      server.sendHeader("Connection", "close");
      server.send(400, "text/plain", "Bad Request");
    }
  });

  server.on("/status", []() {
    String ip = "offline";
    int rssi = 0;
    String mode = "OFFLINE";
    if (staConnected && WiFi.status() == WL_CONNECTED) {
      ip = WiFi.localIP().toString();
      rssi = WiFi.RSSI();
      mode = "STA";
    } else if (apFallbackActive) {
      ip = WiFi.softAPIP().toString();
      mode = "AP";
    }

    String json = "{\"ip\":\"" + ip + "\",\"rssi\":\"" + String(rssi) + "\",\"uptime\":\"" + getUptime() + "\",\"temp\":\"" + String(getCpuTemp(), 1) + "\",\"mode\":\"" + mode + "\",\"state\":\"" + jsonEscape(wifiUiState) + "\",\"reason\":\"" + jsonEscape(wifiUiReason) + "\",\"reasonCode\":" + String(wifiUiReasonCode) + ",\"ssid\":\"" + jsonEscape(wifiUiSsid) + "\"}";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    
    // 【修复】：强行关闭 TCP 连接
    server.sendHeader("Connection", "close");
    server.send(200, "application/json", json);
  });

  server.on("/api/led", []() {
    server.sendHeader("Connection", "close");
    if (server.hasArg("toggle")) {
      ledEnabled = !ledEnabled;
      if (!ledEnabled) { leds[0] = CRGB::Black; FastLED.show(); }
      server.send(200, "text/plain", ledEnabled ? "ON" : "OFF");
    } 
    else if (server.hasArg("color")) {
      String hexColor = server.arg("color");
      standbyColor = hexToCRGB(hexColor);
      ledFlash(standbyColor); 
      server.send(200, "text/plain", "Color Updated");
    }
    else {
      server.send(400, "text/plain", "Bad Request");
    }
  });

  server.on("/api/reboot", []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "Rebooting...");
    delay(500);
    ESP.restart();
  });

  server.on("/api/wifi/retry", []() {
    server.sendHeader("Connection", "close");
    retryStaFromFallback();
    server.send(200, "text/plain", "Retrying STA...");
  });

  server.on("/api/wifi/scan", []() {
    server.sendHeader("Connection", "close");

    bool restoreApOnly = false;
    if (apFallbackActive) {
      WiFi.mode(WIFI_AP_STA);
      restoreApOnly = true;
      delay(120);
    }

    int n = WiFi.scanNetworks(false, true);
    String json = "{\"ok\":true,\"savedSsid\":\"";
    json += hasSavedWifi ? jsonEscape(String(savedWifiSsid)) : "";
    json += "\",\"items\":[";

    if (n > 0) {
      for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"";
        json += jsonEscape(WiFi.SSID(i));
        json += "\",\"rssi\":";
        json += String(WiFi.RSSI(i));
        json += "}";
      }
    }
    json += "]}";
    WiFi.scanDelete();

    if (restoreApOnly) {
      WiFi.mode(WIFI_AP);
    }

    server.send(200, "application/json", json);
  });

  server.on("/api/wifi/save", []() {
    server.sendHeader("Connection", "close");

    if (!server.hasArg("ssid")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_ssid\"}");
      return;
    }

    String ssid = server.arg("ssid");
    String password = server.hasArg("password") ? server.arg("password") : "";
    if (!saveWifiCredential(ssid, password)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_credential\"}");
      return;
    }

    retryStaFromFallback();
    String json = "{\"ok\":true,\"savedSsid\":\"";
    json += jsonEscape(ssid);
    json += "\",\"connecting\":true}";
    server.send(200, "application/json", json);
  });

  server.on("/api/wifi/forget", []() {
    server.sendHeader("Connection", "close");
    clearSavedWifiCredential();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
  Serial.println("Web Server operational.");
}

// ================= 9. LOOP 主循环 =================
void loop() {
  handleWiFiStateMachine();
  server.handleClient();
  if (staConnected || apFallbackActive) {
    ArduinoOTA.handle();
  }

  unsigned long currentMillis = millis();

  // LED 呼吸动画
  if (currentMillis - previousLedMillis > 50) {
    previousLedMillis = currentMillis; 
    ledStandby();
  }
}
