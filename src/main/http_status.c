#include "http_status.h"
#include "usb_cdc_host.h"
#include "tcp_server.h"
#include "wifi.h"
#include "ota.h"
#include "ws_serial.h"
#include "tls_cert.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"

static const char *TAG = "http";

// Favicon (the Betaflight mark), embedded via EMBED_TXTFILES in CMakeLists.
extern const char icon_svg_start[] asm("_binary_icon_svg_start");
extern const char icon_svg_end[]   asm("_binary_icon_svg_end");

#define MAX_SCAN_APS  20

// Shown in the web UI tagline. Calendar-versioned (<year>.<month>.X).
#define BRIDGE_VERSION  "2026.6.X"

// Single-page UI. Status fields and the network list are filled by JS polling
// /status and /scan, so the page itself is static and cacheable.
static const char PAGE[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>betaflight-bridge</title>"
    "<link rel=\"icon\" type=\"image/svg+xml\" href=\"/icon.svg\">"
    "<style>"
    "*{box-sizing:border-box}"
    "body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:#0f1115;color:#e6e8ea;margin:0;padding:1.7rem 1rem;-webkit-font-smoothing:antialiased}"
    ".wrap{max-width:30rem;margin:0 auto}"
    ".logo{text-align:center;margin:.2rem 0 .4rem}"
    ".logo svg{height:56px;width:auto}"
    ".tag{text-align:center;color:#8b9199;font-size:.83rem;letter-spacing:.04em;margin:0 0 1.6rem}"
    ".card{background:#181b20;border:1px solid #262b32;border-radius:14px;padding:1.05rem 1.2rem;margin-bottom:1.1rem}"
    "h2{font-size:.73rem;text-transform:uppercase;letter-spacing:.1em;color:#FFBB00;margin:0 0 .75rem;font-weight:700}"
    "table{border-collapse:collapse;width:100%;font-size:.96rem}"
    "td{padding:.52rem .15rem;border-bottom:1px solid #23272e}"
    "tr:last-child td{border-bottom:0}"
    ".k{color:#8b9199;width:46%}"
    ".up{color:#46c66d;font-weight:600}"
    ".down{color:#6b7178}"
    ".warn{color:#FFBB00}"
    "code{color:#7fc7ff;font-family:ui-monospace,Menlo,monospace;font-size:.9rem}"
    "label{display:block;margin:.7rem 0 .25rem;color:#8b9199;font-size:.8rem}"
    "select,input{width:100%;padding:.6rem .65rem;background:#0f1115;color:#e6e8ea;border:1px solid #2d333b;border-radius:9px;font-size:.96rem}"
    "select:focus,input:focus{outline:none;border-color:#FFBB00}"
    "input[type=file]{padding:.45rem}"
    "input[type=file]::file-selector-button{background:#222831;color:#e6e8ea;border:1px solid #2d333b;border-radius:7px;padding:.35rem .7rem;margin-right:.6rem;cursor:pointer}"
    ".btns{display:flex;flex-wrap:wrap;justify-content:center;gap:.5rem;margin-top:.95rem}"
    "button{padding:.6rem 1.15rem;background:#FFBB00;color:#15140e;border:0;border-radius:9px;font-size:.94rem;font-weight:700;cursor:pointer}"
    "button:hover{filter:brightness(1.08)}"
    "button.sec{background:#222831;color:#e6e8ea;border:1px solid #2d333b}"
    ".msg{margin-top:.7rem;min-height:1.2rem;font-size:.9rem;color:#9aa0a8}"
    "</style></head><body><div class=\"wrap\">"
    "<div class=\"logo\"><svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1294.4 308.4\"><g><path fill=\"#FFBB00\" d=\"M453.2,141h36.3c17.5,0,17.7,9.9,15.3,16.6c-2.4,6.7-6.9,10.3-12.8,13c5.2,2,9.1,6.8,5.7,16.5 c-4.7,13.3-18.7,20.2-30.4,20.2h-37.6L453.2,141z M455.8,191.5h12c4.3,0,8.4-1.2,10.2-6.3c1.4-3.9-0.2-6-5-6h-12.8L455.8,191.5z M464.5,166.8h10.8c4.3,0,7.4-1.2,8.9-5.5c1.2-3.3-1.2-4.6-5.4-4.6h-10.8L464.5,166.8z\"/><path fill=\"#FFBB00\" d=\"M523.1,141H578l-6,17h-34.5l-2.9,8.3h31.3l-5.6,15.8H529l-2.9,8.3h35.5l-6,17h-55.9L523.1,141z\"/><path fill=\"#FFBB00\" d=\"M597.4,158h-18.6l6-17h57.6l-6,17h-18.6l-17.5,49.3h-20.4L597.4,158z\"/><path fill=\"#FFBB00\" d=\"M661.3,141h20l0.7,66.3h-21.2l0.6-9.5h-21.1l-6.3,9.5h-20.5L661.3,141z M662.2,183.6l1.3-20.4h-0.2l-13.7,20.4 H662.2z\"/><path fill=\"#fff\" d=\"M711,141h51.3l-6,17h-30.9l-2.9,8.3h26.5l-5.6,15.8h-26.5l-9,25.3h-20.4L711,141z\"/><path fill=\"#fff\" d=\"M770,141h20.4L773,190.3h29.4l-6,17h-49.8L770,141z\"/><path fill=\"#fff\" d=\"M827.2,141h20.4l-23.5,66.3h-20.4L827.2,141z\"/><path fill=\"#fff\" d=\"M886.3,200.6c-6.6,5.9-14.6,8.4-21.8,8.4c-19.6,0-26.3-15.2-19.6-34.1c9-25.3,30.3-35.4,45.3-35.4 c16.9,0,24.3,9.2,21.8,24.8h-19.6c1-4.8-1.9-8.3-6.7-8.3c-14,0-18.8,14.8-20.4,19.3c-2.2,6.2-3.5,17.1,8.9,17.1 c4.7,0,10.4-2.4,13.2-7.3h-9l5-14.2h27.1l-12.9,36.5h-12.9L886.3,200.6z\"/><path fill=\"#fff\" d=\"M932.5,141h20.4l-8.2,23h19.1l8.2-23h20.4L969,207.3h-20.4l9.3-26.3h-19.1l-9.3,26.3h-20.4L932.5,141z\"/><path fill=\"#fff\" d=\"M1011.9,158h-18.6l6-17h57.6l-6,17h-18.6l-17.5,49.3h-20.4L1011.9,158z\"/></g><path fill=\"#FFBB00\" d=\"M363.6,193.1c-15.2-6.4-26-15.9-36.9-20c5.7-2.5,11-6.1,14.3-9.2c3.6-3.4,4.7-7.7,4.5-11.8 c11.3,7.7,24.7,8.2,31.5,8.6c7.1,0.4,13.8,1.8,15.2,5.2c0,0,2.2-38.4-76.9-63.9c0,0,23.6,10.7,31,19.3c0,0-8.4-0.5-11.9,4.9 c-1.8,2.7-2.2,6.9-0.6,11.5c-1.3-0.2-2.8-0.4-4.4-0.5L203.8,67.7L217,96.1l-12.4-4.3l3.7,7c0.1,0.1,5.2,10,17.3,31.1 c9.5,16.6,26.5,24.9,49,23.9c0.8,0,1.8-0.1,2.8-0.2c5.7-0.4,14.5-1.2,23-2c-0.6,3.1-0.5,6.8,0.3,11.3c1,5.2,3.1,8.5,5.8,10.6 c-6.7,3.2-12.8,9.4-15.5,16.8c0,0,0,0,0,0c-1.9,4.6-2.4,10.2-0.1,16.7c0,0,0,0.1,0,0.1c0.5,1.3,1,2.6,1.8,4c0.7,1.3,1.6,2.6,2.7,3.7 c14,16.6,48,13.9,69.4,4.9c0.3-0.1,0.6-0.2,0.9-0.4c1.7-0.7,3.4-1.5,5.1-2.3c0.3-0.2,0.7-0.3,1-0.5c1.8-0.9,3.6-1.8,5.4-2.9 c21.3-12.4,28.3-26.9,28.3-26.9S384.2,201.8,363.6,193.1z M369,136.1c6,2.6,15.3,17.6,15.3,17.6c-7.7-1.2-23.1-6.2-24.7-12 S363,133.4,369,136.1z M349.2,210.5c0,0,1.6,5-0.3,11.5c-2.1,0.6-4.4,1.1-6.7,1.5C345.9,220.7,349.2,216.5,349.2,210.5z M245.5,114.4l54.5,31c-10,1-20.6,1.9-25.9,2.2c-8.1,0.4-15.4-0.6-21.8-2.8c-9.3-3.3-16.5-9.3-21.5-17.9 c-6.2-10.8-10.5-18.7-13.3-23.8l11.3,3.9l-11.3-24.3l99.1,55c-2.7,0.4-5.4,1.2-7.8,2.4L245.5,114.4z M303.4,153.1 c3.9-18.4,27.5-12.6,27.5-12.6c-8.6-0.2-13.6,2.2-16.1,5.6c-3.7,5.1,0.9,9,0.9,9c-1.3-0.2-2.4-0.6-3.2-1.2c0,0.1,0,0.2,0,0.2 c2,7.1,7.8,9,7.8,9c-2.2,0.3-4.1,0.2-5.7-0.2c1.3,3,3,5.2,4.2,5.7c-5.2,0-7.9,1.2-9.9,2C304.8,167.8,301.7,161.1,303.4,153.1z M319.6,193.4c0,0-8.6,10.7-27,10.7c-1.1-4.2-0.7-8.4,0.3-12.1C298.7,195.8,307.5,198,319.6,193.4z M304,218.3 c7.9-1.8,18-6.2,27.6-17.1c0,0-1.1,11.8-10,23c-4-0.5-8.1-1.5-12-3.1C307.6,220.3,305.7,219.3,304,218.3z M378.4,208.7 c-1.1-2.9-1.6-8.7-1.6-8.7c6.6,0.6,14.6-2.1,14.6-2.1C386.1,204.4,378.4,208.7,378.4,208.7z\"/></svg></div>"
    "<div class=\"tag\">USB-host &#8596; WiFi bridge for Betaflight [" BRIDGE_VERSION "]</div>"
    "<div class=\"card\"><table>"
    "<tr><td class=\"k\">FC (USB VCP)</td><td id=\"usb\">…</td></tr>"
    "<tr><td class=\"k\">Configurator (TCP)</td><td id=\"tcp\">…</td></tr>"
    "<tr><td class=\"k\">WiFi network</td><td id=\"sta\">…</td></tr>"
    "<tr><td class=\"k\">Signal</td><td id=\"rssi\">…</td></tr>"
    "<tr><td class=\"k\">IP address</td><td id=\"ip\">…</td></tr>"
    "<tr><td class=\"k\">Browser connect</td><td id=\"url\">…</td></tr>"
    "<tr><td class=\"k\">Gateway</td><td id=\"gw\">…</td></tr>"
    "<tr><td class=\"k\">Netmask</td><td id=\"mask\">…</td></tr>"
    "<tr><td class=\"k\">Access point</td><td id=\"ap\">…</td></tr>"
    "<tr><td class=\"k\">Board</td><td id=\"board\">…</td></tr>"
    "<tr><td class=\"k\">Firmware slot</td><td id=\"slot\">…</td></tr>"
    "</table></div>"
    "<div class=\"card\"><h2>Join a WiFi network</h2>"
    "<label for=\"ssid\">Network</label>"
    "<select id=\"ssid\" onchange=\"pick()\"><option value=\"\">— scanning… —</option>"
    "<option value=\"__manual__\">Other (enter manually)…</option></select>"
    "<div id=\"manualwrap\" style=\"display:none\">"
    "<label for=\"manual\">SSID</label><input id=\"manual\" autocapitalize=\"none\"></div>"
    "<label for=\"pass\">Password</label>"
    "<input id=\"pass\" type=\"password\" placeholder=\"(blank for open networks)\">"
    "<div class=\"btns\"><button onclick=\"join()\">Join network</button>"
    "<button class=\"sec\" onclick=\"scan()\">Rescan</button>"
    "<button class=\"sec\" onclick=\"forget()\">Forget</button></div>"
    "<div class=\"msg\" id=\"msg\"></div></div>"
    "<div class=\"card\"><h2>Firmware update</h2>"
    "<input type=\"file\" id=\"fw\" accept=\".bin\">"
    "<div class=\"btns\"><button onclick=\"upload()\">Upload &amp; reboot</button></div>"
    "<div class=\"msg\" id=\"up\"></div></div>"
    "<script>"
    "var $=function(i){return document.getElementById(i)};"
    "function pick(){$('manualwrap').style.display=$('ssid').value=='__manual__'?'block':'none'}"
    "function bars(r){return r>=-55?'\\u2588':r>=-67?'\\u2586':r>=-78?'\\u2584':'\\u2582'}"
    "function status(){fetch('/status').then(function(r){return r.json()}).then(function(s){"
    "$('usb').innerHTML=s.usb.up?('<span class=\\\"up\\\">connected</span> <code>'+s.usb.id+'</code>'):'<span class=\\\"down\\\">waiting…</span>';"
    "$('tcp').innerHTML=s.tcp.up?'<span class=\\\"up\\\">connected</span> :'+s.tcp.port:'<span class=\\\"down\\\">none</span> :'+s.tcp.port;"
    "var w=s.wifi,h;"
    "if(w.state=='connected')h='<span class=\\\"up\\\">'+w.ssid+'</span>';"
    "else if(w.state=='connecting')h='<span class=\\\"warn\\\">connecting to '+w.ssid+'…</span>';"
    "else if(w.state=='failed')h='<span class=\\\"warn\\\">failed: '+w.ssid+'</span>';"
    "else h='<span class=\\\"down\\\">none</span>';"
    "$('sta').innerHTML=h;"
    "var rs=w.rssi;"
    "if(w.state=='connected'&&rs){"
    "var q=rs>=-60?'<span class=\\\"up\\\">good</span>':rs>=-72?'<span class=\\\"warn\\\">fair</span>':'<span class=\\\"warn\\\">weak</span>';"
    "$('rssi').innerHTML='<code>'+bars(rs)+'</code> '+q+' <code>'+rs+' dBm</code>';"
    "}else $('rssi').innerHTML='<span class=\\\"down\\\">—</span>';"
    "$('ip').innerHTML=w.ip?'<code>'+w.ip+'</code>':'<span class=\\\"down\\\">—</span>';"
    "$('url').innerHTML=w.ip?'<code>wss://'+w.ip+'/serial</code>':'<span class=\\\"down\\\">—</span>';"
    "$('gw').innerHTML=w.gw?'<code>'+w.gw+'</code>':'<span class=\\\"down\\\">—</span>';"
    "$('mask').innerHTML=w.netmask?'<code>'+w.netmask+'</code>':'<span class=\\\"down\\\">—</span>';"
    "$('ap').innerHTML=w.ap?'<span class=\\\"warn\\\">broadcasting (setup mode)</span>':'<span class=\\\"down\\\">off</span>';"
    "$('board').innerHTML='<code>'+s.ota.board+'</code>';"
    "$('slot').innerHTML='<code>'+s.ota.slot+'</code> '+(s.ota.valid?'<span class=\\\"up\\\">valid</span>':'<span class=\\\"warn\\\">pending verify</span>');"
    "}).catch(function(){})}"
    "function scan(){var s=$('ssid');s.innerHTML='<option>— scanning… —</option>';"
    "fetch('/scan').then(function(r){return r.json()}).then(function(l){"
    "var o='<option value=\\\"\\\">— select —</option>';"
    "l.forEach(function(a){o+='<option value=\\\"'+a.ssid.replace(/\"/g,'&quot;')+'\\\">'+bars(a.rssi)+' '+a.ssid+(a.secure?' \\uD83D\\uDD12':'')+'</option>'});"
    "o+='<option value=\\\"__manual__\\\">Other (enter manually)…</option>';"
    "s.innerHTML=o}).catch(function(){s.innerHTML='<option>scan failed</option>'})}"
    "function join(){var v=$('ssid').value;if(v=='__manual__')v=$('manual').value;"
    "if(!v){$('msg').textContent='Pick or enter a network.';return}"
    "$('msg').textContent='Saving & connecting…';"
    "fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'ssid='+encodeURIComponent(v)+'&pass='+encodeURIComponent($('pass').value)})"
    ".then(function(r){$('msg').textContent=r.ok?'Saved. Connecting — watch the status above.':'Error saving.'})"
    ".catch(function(){$('msg').textContent='Request failed.'})}"
    "function forget(){$('msg').textContent='Clearing…';"
    "fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='})"
    ".then(function(){$('msg').textContent='Stored network cleared.'})}"
    "function upload(){var f=$('fw').files[0];if(!f){$('up').textContent='Pick a .bin file.';return}"
    "var x=new XMLHttpRequest();x.open('POST','/update');"
    "x.upload.onprogress=function(e){if(e.lengthComputable)$('up').textContent='Uploading '+Math.round(e.loaded/e.total*100)+'%…'};"
    "x.onload=function(){$('up').textContent=x.status==200?'Update OK — rebooting, reconnect in ~10s.':'Update failed: '+x.responseText};"
    "x.onerror=function(){$('up').textContent='Upload connection lost.'};"
    "x.send(f)}"
    "status();setInterval(status,2000);scan();"
    "</script></div></body></html>";

// Append src to dst as a JSON string body (no surrounding quotes), escaping the
// characters JSON requires. Caller supplies the quotes. Truncates safely.
static void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t o = 0;
    for (; *src && o + 2 < dst_len; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = c;
        } else if (c < 0x20) {
            if (o + 6 >= dst_len) break;
            o += snprintf(dst + o, dst_len - o, "\\u%04x", c);
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
}

// In-place URL-decode (%xx and '+'); used on form values.
static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else if (*r == '+') {
            *w++ = ' ';
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

static esp_err_t icon_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    // EMBED_TXTFILES appends a NUL terminator; don't send it.
    return httpd_resp_send(req, icon_svg_start, icon_svg_end - icon_svg_start - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    uint16_t vid = 0, pid = 0;
    bool usb = usb_cdc_host_status(&vid, &pid);
    bool tcp = tcp_server_client_connected();

    wifi_status_t w;
    wifi_sta_status(&w);
    const char *state = w.state == WIFI_STA_CONNECTED  ? "connected"
                      : w.state == WIFI_STA_CONNECTING ? "connecting"
                      : w.state == WIFI_STA_FAILED     ? "failed" : "idle";

    char ssid_esc[200];
    json_escape(ssid_esc, sizeof(ssid_esc), w.ssid);

    char slot[16];
    bool img_valid = true;
    ota_running_info(slot, sizeof(slot), &img_valid);

    char body[700];
    int n = snprintf(body, sizeof(body),
        "{\"usb\":{\"up\":%s,\"id\":\"%04x:%04x\"},"
        "\"tcp\":{\"up\":%s,\"port\":%d},"
        "\"wifi\":{\"state\":\"%s\",\"ap\":%s,\"ssid\":\"%s\","
        "\"ip\":\"%s\",\"gw\":\"%s\",\"netmask\":\"%s\",\"rssi\":%d},"
        "\"ota\":{\"board\":\"%s\",\"slot\":\"%s\",\"valid\":%s}}",
        usb ? "true" : "false", vid, pid,
        tcp ? "true" : "false", TCP_SERVER_PORT,
        state, w.ap_active ? "true" : "false", ssid_esc,
        w.ip, w.gw, w.netmask, w.rssi,
        ota_board_id(), slot, img_valid ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    wifi_scan_ap_t aps[MAX_SCAN_APS];
    int count = wifi_scan(aps, MAX_SCAN_APS);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < count; i++) {
        char ssid_esc[200];
        json_escape(ssid_esc, sizeof(ssid_esc), aps[i].ssid);
        char item[256];
        int n = snprintf(item, sizeof(item),
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
            i ? "," : "", ssid_esc, aps[i].rssi, aps[i].secure ? "true" : "false");
        httpd_resp_send_chunk(req, item, n);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_send_chunk(req, NULL, 0);   // end response
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char buf[256];
    int len = req->content_len < (int)sizeof(buf) - 1 ? req->content_len : (int)sizeof(buf) - 1;
    int got = httpd_req_recv(req, buf, len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[got] = '\0';

    char ssid[64] = {0};
    char pass[128] = {0};
    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);

    ESP_LOGI(TAG, "web request to join '%s'", ssid[0] ? ssid : "(clear)");
    wifi_set_station(ssid, pass);   // persists to NVS + applies live

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Attach the web UI + serial endpoints to a server (used for both the plain
// HTTP and the TLS server, so the page and the ws/wss serial bridge are
// reachable on either).
static void register_routes(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        { .uri = "/",         .method = HTTP_GET,  .handler = root_get   },
        { .uri = "/icon.svg", .method = HTTP_GET,  .handler = icon_get   },
        { .uri = "/status",   .method = HTTP_GET,  .handler = status_get },
        { .uri = "/scan",     .method = HTTP_GET,  .handler = scan_get   },
        { .uri = "/wifi",     .method = HTTP_POST, .handler = wifi_post  },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ota_register(server);       // POST /update
    ws_serial_register(server); // GET /serial (WebSocket)
}

// Start the plain HTTP server on port 80 (web UI + ws:// serial).
static void start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;   // free idle sockets so the page stays reachable
    cfg.stack_size = 8192;         // headroom for the blocking scan + JSON
    cfg.max_uri_handlers = 12;     // routes + /update + /serial

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return;
    }
    register_routes(server);
    ESP_LOGI(TAG, "web UI on http://%s/ (port 80)", WIFI_AP_IP);
}

// Start the TLS server on 443 (web UI + wss:// serial) so the HTTPS Configurator
// PWA can connect. Self-signed cert from tls_cert (generated once, kept in NVS).
static void start_https(void)
{
    const unsigned char *cert, *key;
    size_t cert_len, key_len;
    if (tls_cert_get(&cert, &cert_len, &key, &key_len) != ESP_OK) {
        ESP_LOGE(TAG, "no TLS cert; HTTPS/WSS disabled");
        return;
    }

    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert = cert;
    cfg.servercert_len = cert_len;
    cfg.prvtkey_pem = key;
    cfg.prvtkey_len = key_len;
    cfg.port_secure = 443;
    cfg.httpd.lru_purge_enable = true;
    cfg.httpd.stack_size = 10240;     // TLS handshake needs more headroom
    cfg.httpd.max_uri_handlers = 12;

    httpd_handle_t server = NULL;
    if (httpd_ssl_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTPS server");
        return;
    }
    register_routes(server);
    ESP_LOGI(TAG, "web UI on https://%s/ (port 443); serial at wss://<ip>/serial", WIFI_AP_IP);
}

void http_status_start(void)
{
    start_http();
    start_https();
}
