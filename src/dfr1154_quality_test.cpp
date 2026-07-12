#include "dfr1154_quality_test.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>

#include "dfr1154_config.h"
#include "dfr1154_pins.h"

namespace {

httpd_handle_t g_httpServer = nullptr;
uint32_t g_lastWifiAttempt = 0;

esp_err_t sendChunk(httpd_req_t *request, const char *data, size_t length) {
  return httpd_resp_send_chunk(request, data, length);
}

esp_err_t indexHandler(httpd_req_t *request) {
  static const char page[] = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DFR1154 Quality Test</title><style>
body{background:#111;color:#eee;font:15px sans-serif;margin:18px}img{display:block;max-width:100%;height:auto;border:1px solid #555}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:8px;max-width:1000px;margin:16px 0}
label{display:flex;justify-content:space-between;gap:12px;padding:8px;background:#222}input,select{width:110px}
button,a{padding:8px 12px;background:#285;color:#fff;border:0;text-decoration:none;cursor:pointer}#result{color:#9f9}
</style></head><body><h1>DFR1154 Direct Camera Test</h1>
<p>Direct MJPEG from the ESP32-S3. No MediaMTX or FFmpeg.</p><img src="/stream">
<p><a href="/capture" target="_blank">Single JPEG capture</a></p>
<div class="grid">
<label>Resolution<select data-var="framesize"><option value="0">QVGA</option><option value="1">VGA</option><option value="2">SVGA</option><option value="3">XGA</option><option value="4">HD</option><option value="5">SXGA</option><option value="6" selected>UXGA</option><option value="7">QXGA</option></select></label>
<label>JPEG quality (lower=better)<input data-var="quality" type="number" min="4" max="63" value="8"></label>
<label>Brightness<input data-var="brightness" type="number" min="-2" max="2" value="0"></label>
<label>Contrast<input data-var="contrast" type="number" min="-2" max="2" value="1"></label>
<label>Saturation<input data-var="saturation" type="number" min="-2" max="2" value="-2"></label>
<label>Sharpness<input data-var="sharpness" type="number" min="-2" max="2" value="0"></label>
<label>Auto white balance<select data-var="awb"><option value="1">on</option><option value="0">off</option></select></label>
<label>AWB gain<select data-var="awb_gain"><option value="1">on</option><option value="0">off</option></select></label>
<label>Auto gain<select data-var="agc"><option value="1">on</option><option value="0">off</option></select></label>
<label>Auto exposure<select data-var="aec"><option value="1">on</option><option value="0">off</option></select></label>
<label>AEC2<select data-var="aec2"><option value="1">on</option><option value="0">off</option></select></label>
<label>Lens correction<select data-var="lenc"><option value="1">on</option><option value="0">off</option></select></label>
<label>DCW<select data-var="dcw"><option value="1">on</option><option value="0">off</option></select></label>
<label>BPC<select data-var="bpc"><option value="0">off</option><option value="1">on</option></select></label>
<label>WPC<select data-var="wpc"><option value="1">on</option><option value="0">off</option></select></label>
<label>Raw gamma<select data-var="raw_gma"><option value="1">on</option><option value="0">off</option></select></label>
<label>AE level<input data-var="ae_level" type="number" min="-2" max="2" value="0"></label>
</div><button id="apply">Apply all settings</button> <span id="result"></span>
<script>
const result=document.querySelector('#result');
async function applyOne(el){
  result.textContent='applying '+el.dataset.var+'...';
  try{
    const r=await fetch('/control?var='+encodeURIComponent(el.dataset.var)+'&val='+encodeURIComponent(el.value),{cache:'no-store'});
    const text=await r.text();
    result.textContent=r.ok?text:'FAILED: '+text;
    return r.ok;
  }catch(error){result.textContent='FAILED: '+error.message;return false;}
}
const values=[...document.querySelectorAll('[data-var]')];
values.forEach(el=>el.addEventListener('change',()=>applyOne(el)));
document.querySelector('#apply').onclick=async()=>{
  result.textContent='applying all '+values.length+' settings...';
  const outcomes=await Promise.all(values.map(el=>applyOne(el)));
  const applied=outcomes.filter(Boolean).length;
  result.textContent='applied '+applied+'/'+values.length+' settings';
};
</script>
</body></html>)HTML";
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t controlHandler(httpd_req_t *request) {
  char query[160] = {};
  char variable[32] = {};
  char value[32] = {};
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "var", variable, sizeof(variable)) != ESP_OK ||
      httpd_query_key_value(query, "val", value, sizeof(value)) != ESP_OK) {
    return httpd_resp_send_404(request);
  }

  const int val = atoi(value);
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) return httpd_resp_send_500(request);
  int result = -1;
  if (!strcmp(variable, "framesize")) result = sensor->set_framesize(sensor, static_cast<framesize_t>(constrain(val, 0, 7)));
  else if (!strcmp(variable, "quality")) result = sensor->set_quality(sensor, constrain(val, 4, 63));
  else if (!strcmp(variable, "brightness")) result = sensor->set_brightness(sensor, constrain(val, -2, 2));
  else if (!strcmp(variable, "contrast")) result = sensor->set_contrast(sensor, constrain(val, -2, 2));
  else if (!strcmp(variable, "saturation")) result = sensor->set_saturation(sensor, constrain(val, -2, 2));
  else if (!strcmp(variable, "sharpness")) result = sensor->set_sharpness(sensor, constrain(val, -2, 2));
  else if (!strcmp(variable, "awb")) result = sensor->set_whitebal(sensor, val != 0);
  else if (!strcmp(variable, "awb_gain")) result = sensor->set_awb_gain(sensor, val != 0);
  else if (!strcmp(variable, "agc")) result = sensor->set_gain_ctrl(sensor, val != 0);
  else if (!strcmp(variable, "aec")) result = sensor->set_exposure_ctrl(sensor, val != 0);
  else if (!strcmp(variable, "aec2")) result = sensor->set_aec2(sensor, val != 0);
  else if (!strcmp(variable, "lenc")) result = sensor->set_lenc(sensor, val != 0);
  else if (!strcmp(variable, "dcw")) result = sensor->set_dcw(sensor, val != 0);
  else if (!strcmp(variable, "bpc")) result = sensor->set_bpc(sensor, val != 0);
  else if (!strcmp(variable, "wpc")) result = sensor->set_wpc(sensor, val != 0);
  else if (!strcmp(variable, "raw_gma")) result = sensor->set_raw_gma(sensor, val != 0);
  else if (!strcmp(variable, "ae_level")) result = sensor->set_ae_level(sensor, constrain(val, -2, 2));
  char response[96] = {};
  if (result < 0) {
    httpd_resp_set_status(request, "400 Bad Request");
    snprintf(response, sizeof(response), "rejected var=%s val=%d result=%d", variable, val, result);
  } else {
    snprintf(response, sizeof(response), "applied var=%s val=%d", variable, val);
  }
  httpd_resp_set_type(request, "text/plain");
  return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

esp_err_t captureHandler(httpd_req_t *request) {
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) return httpd_resp_send_500(request);
  httpd_resp_set_type(request, "image/jpeg");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  const esp_err_t result = httpd_resp_send(request, reinterpret_cast<const char *>(frame->buf), frame->len);
  esp_camera_fb_return(frame);
  return result;
}

esp_err_t streamHandler(httpd_req_t *request) {
  static const char *contentType = "multipart/x-mixed-replace;boundary=frame";
  static const char *boundary = "\r\n--frame\r\n";
  char header[96];
  httpd_resp_set_type(request, contentType);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");

  while (WiFi.status() == WL_CONNECTED) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == nullptr) break;
    const int headerLength = snprintf(header, sizeof(header), "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", frame->len);
    const esp_err_t headerResult = sendChunk(request, boundary, strlen(boundary));
    const esp_err_t imageResult = headerResult == ESP_OK ? sendChunk(request, header, headerLength) : headerResult;
    const esp_err_t dataResult = imageResult == ESP_OK
        ? sendChunk(request, reinterpret_cast<const char *>(frame->buf), frame->len)
        : imageResult;
    esp_camera_fb_return(frame);
    if (dataResult != ESP_OK) break;
    delay(1);
  }

  httpd_resp_send_chunk(request, nullptr, 0);
  return ESP_OK;
}

void startHttpServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;
  if (httpd_start(&g_httpServer, &config) != ESP_OK) return;

  const httpd_uri_t index = {"/", HTTP_GET, indexHandler, nullptr};
  const httpd_uri_t stream = {"/stream", HTTP_GET, streamHandler, nullptr};
  const httpd_uri_t capture = {"/capture", HTTP_GET, captureHandler, nullptr};
  const httpd_uri_t control = {"/control", HTTP_GET, controlHandler, nullptr};
  httpd_register_uri_handler(g_httpServer, &index);
  httpd_register_uri_handler(g_httpServer, &stream);
  httpd_register_uri_handler(g_httpServer, &capture);
  httpd_register_uri_handler(g_httpServer, &control);
}

bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = DFR_CAM_Y2;
  config.pin_d1 = DFR_CAM_Y3;
  config.pin_d2 = DFR_CAM_Y4;
  config.pin_d3 = DFR_CAM_Y5;
  config.pin_d4 = DFR_CAM_Y6;
  config.pin_d5 = DFR_CAM_Y7;
  config.pin_d6 = DFR_CAM_Y8;
  config.pin_d7 = DFR_CAM_Y9;
  config.pin_xclk = DFR_CAM_XCLK;
  config.pin_pclk = DFR_CAM_PCLK;
  config.pin_vsync = DFR_CAM_VSYNC;
  config.pin_href = DFR_CAM_HREF;
  config.pin_sccb_sda = DFR_CAM_SIOD;
  config.pin_sccb_scl = DFR_CAM_SIOC;
  config.pin_pwdn = DFR_CAM_PWDN;
  config.pin_reset = DFR_CAM_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 8;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&config) != ESP_OK) return false;
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) return false;
  sensor->set_whitebal(sensor, 1);
  sensor->set_awb_gain(sensor, 1);
  sensor->set_gain_ctrl(sensor, 1);
  sensor->set_exposure_ctrl(sensor, 1);
  sensor->set_aec2(sensor, 1);
  sensor->set_ae_level(sensor, 0);
  sensor->set_dcw(sensor, 1);
  sensor->set_lenc(sensor, 1);
  sensor->set_vflip(sensor, 1);
  sensor->set_brightness(sensor, 0);
  sensor->set_contrast(sensor, 1);
  sensor->set_saturation(sensor, -2);
  sensor->set_sharpness(sensor, 0);
  return true;
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - g_lastWifiAttempt < 5000) return;
  g_lastWifiAttempt = millis();
  WiFi.begin(dfrcfg::kWifiSsid, dfrcfg::kWifiPassword);
}

}  // namespace

namespace dfr1154_quality_test {

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("[QUALITY TEST] DFR1154 direct CameraWebServer test");
  Serial.printf("[QUALITY TEST] psram=%u\n", psramFound());
  pinMode(DFR_LED_PIN, OUTPUT);
  pinMode(DFR_IR_PIN, OUTPUT);
  digitalWrite(DFR_LED_PIN, LOW);
  digitalWrite(DFR_IR_PIN, LOW);
  if (!initCamera()) {
    Serial.println("[QUALITY TEST] camera init failed");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  ensureWifi();
  ArduinoOTA.setHostname(dfrcfg::kOtaHostname);
  ArduinoOTA.setPassword(dfrcfg::kOtaPassword);
  ArduinoOTA.begin();
  Serial.printf("[QUALITY TEST] OTA host=%s.local\n", dfrcfg::kOtaHostname);
}

void loop() {
  ArduinoOTA.handle();
  ensureWifi();
  if (WiFi.status() == WL_CONNECTED) {
    static bool started = false;
    if (!started) {
      MDNS.begin(dfrcfg::kMdnsHostname);
      startHttpServer();
      Serial.printf("[QUALITY TEST] open http://%s/\n", WiFi.localIP().toString().c_str());
      started = true;
    }
  }
  delay(1);
}

}  // namespace dfr1154_quality_test
