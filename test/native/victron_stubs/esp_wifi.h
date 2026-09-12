#pragma once
using esp_err_t = int;
enum wifi_ps_type_t { WIFI_PS_NONE, WIFI_PS_MIN_MODEM };
inline esp_err_t esp_wifi_get_ps(wifi_ps_type_t *value) {
  *value = WIFI_PS_MIN_MODEM;
  return 0;
}
inline esp_err_t esp_wifi_set_ps(wifi_ps_type_t) { return 0; }
