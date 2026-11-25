/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "ag_client.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ag_log.h"
#include "device_model.h"

#include "ag_server_cert.h"
#include "payload_serializer.h"

#ifndef TEST_HOST
#include "../backends/wifi_http_client.h"
#endif

namespace {
constexpr const char *TAG = "AgClient";

#if defined(CONFIG_AG_DEVICE_MODEL_GO)
constexpr size_t POST_BODY_BUFFER_SIZE = 512;
#else
constexpr size_t POST_BODY_BUFFER_SIZE = 1024;
#endif

[[noreturn]] void abort_unsupported(const char *method, const char *reason) {
  AG_LOGE(TAG, "%s: %s", method, reason);
  std::abort();
}

#ifndef TEST_HOST
HttpClient *make_wifi_http_client() {
  static WifiHttpClient instance; // lives for program lifetime
  return &instance;
}
#endif

} // namespace

bool AgClient::begin(const char *serial_number, NetworkType network, CellularModem *modem) {
  if (serial_number == nullptr) {
    AG_LOGE(TAG, "begin: serial_number is null");
    return false;
  }

  std::strncpy(_serial_number, serial_number, sizeof(_serial_number) - 1);
  _serial_number[sizeof(_serial_number) - 1] = '\0';

  _network = network;

  if (network == NetworkType::Cellular) {
    (void)modem;
    AG_LOGE(TAG, "begin: NetworkType::Cellular is not supported in this build");
    return false;
  }

  // Tests inject via AgClientTestAccess and skip this.
#ifndef TEST_HOST
  _http = make_wifi_http_client();
#endif

  AG_LOGI(TAG, "Initialised for WiFi, sn=%s", _serial_number);
  return true;
}

// -----------------------------------------------------------------------------
// MeasuresInput builders
// -----------------------------------------------------------------------------

MeasuresInput AgClient::_make_input(const Measures &m) {
  MeasuresInput input;
  input.temp_hum_a = &m.temp_hum_a;
  input.temp_hum_b = &m.temp_hum_b;
  input.pm_a = &m.pm_a;
  input.pm_b = &m.pm_b;
  input.co2 = &m.co2;
  input.tvoc_nox = &m.tvoc_nox;
  input.power = &m.power;
  input.electrode = &m.electrode;
  return input;
}

MeasuresInput AgClient::_make_input(const MeasuresBasic &m) {
  MeasuresInput input;
  input.temp_hum_a = &m.temp_hum_a;
  input.pm_a = &m.pm_a;
  input.co2 = &m.co2;
  input.tvoc_nox = &m.tvoc_nox;
  return input;
}

MeasuresInput AgClient::_make_input(const MeasuresAGo &m) {
  MeasuresInput input;
  input.temp_hum_a = &m.temp_hum_a;
  input.pm_a = &m.pm_a;
  input.co2 = &m.co2;
  input.tvoc_nox = &m.tvoc_nox;
  input.power = &m.power;
  return input;
}

// -----------------------------------------------------------------------------
// HTTP (WiFi)
// -----------------------------------------------------------------------------

AgClientResult AgClient::http_fetch_config(char *config_out, size_t config_size,
                                           size_t *bytes_written) {
  if (bytes_written != nullptr) {
    *bytes_written = 0;
  }

  if (_network != NetworkType::Wifi) {
    abort_unsupported("http_fetch_config", "called on non-WiFi network");
  }
  if (_http == nullptr) {
    AG_LOGE(TAG, "http_fetch_config: client not initialised");
    return AgClientResult::TransportError;
  }
  if (config_out == nullptr || config_size == 0) {
    AG_LOGE(TAG, "http_fetch_config: invalid output buffer");
    return AgClientResult::TransportError;
  }

  char url[URL_BUFFER_SIZE];
  if (!_build_fetch_config_url(url, sizeof(url))) {
    AG_LOGE(TAG, "http_fetch_config: URL build failed");
    return AgClientResult::TransportError;
  }

  int status = 0;
  bool truncated = false;
  size_t written = 0;
  const bool ok =
      _http->get(url, AG_SERVER_ROOT_CA, status, config_out, config_size, &written, &truncated);
  if (bytes_written != nullptr) {
    *bytes_written = written;
  }
  const AgClientResult result = _map_fetch_config_result(ok, status, truncated);
  AG_LOGI(TAG, "fetch_config: url=%s status=%d result=%d", url, status, static_cast<int>(result));
  return result;
}

AgClientResult AgClient::http_post_measures(const Measures &measures, int signal, uint32_t boot) {
  return _do_http_post_measures(_make_input(measures), signal, boot);
}

AgClientResult AgClient::http_post_measures(const MeasuresBasic &measures, int signal,
                                            uint32_t boot) {
  return _do_http_post_measures(_make_input(measures), signal, boot);
}

AgClientResult AgClient::http_post_measures(const MeasuresAGo &measures, int signal,
                                            uint32_t boot) {
  return _do_http_post_measures(_make_input(measures), signal, boot);
}

AgClientResult AgClient::_do_http_post_measures(const MeasuresInput &input, int signal,
                                                uint32_t boot) {
  if (_network != NetworkType::Wifi) {
    abort_unsupported("http_post_measures", "called on non-WiFi network");
  }
  if (_http == nullptr) {
    AG_LOGE(TAG, "http_post_measures: client not initialised");
    return AgClientResult::TransportError;
  }

  char url[URL_BUFFER_SIZE];
  if (!_build_post_measures_url(url, sizeof(url))) {
    AG_LOGE(TAG, "http_post_measures: URL build failed");
    return AgClientResult::TransportError;
  }

  char body[POST_BODY_BUFFER_SIZE];
  size_t body_len = 0;
  if (!serialize_measures_json(input, signal, boot, body, sizeof(body), &body_len)) {
    AG_LOGE(TAG, "http_post_measures: JSON serialisation failed");
    return AgClientResult::TransportError;
  }

  AG_LOGI(TAG, "post_measures: url=%s body=%.*s", url, static_cast<int>(body_len), body);

  int status = 0;
  const bool ok = _http->post(url, AG_SERVER_ROOT_CA, "application/json",
                              reinterpret_cast<const uint8_t *>(body), body_len, status);
  const AgClientResult result = _map_post_measures_result(ok, status);
  AG_LOGI(TAG, "post_measures: status=%d result=%d", status, static_cast<int>(result));
  return result;
}

// -----------------------------------------------------------------------------
// CoAP stubs
// -----------------------------------------------------------------------------

AgClientResult AgClient::coap_fetch_config(char *, size_t, size_t *) {
  if (_network != NetworkType::Cellular) {
    abort_unsupported("coap_fetch_config", "called on non-Cellular network");
  }
  abort_unsupported("coap_fetch_config", "cellular CoAP backend not implemented");
}

AgClientResult AgClient::coap_post_measures(const Measures &, int, int) {
  abort_unsupported("coap_post_measures", "cellular CoAP backend not implemented");
}
AgClientResult AgClient::coap_post_measures(const MeasuresBasic &, int, int) {
  abort_unsupported("coap_post_measures", "cellular CoAP backend not implemented");
}
AgClientResult AgClient::coap_post_measures(const MeasuresAGo &, int, int) {
  abort_unsupported("coap_post_measures", "cellular CoAP backend not implemented");
}

AgClientResult AgClient::coap_post_measures(const Measures *, size_t, int, int) {
  abort_unsupported("coap_post_measures (batch)", "cellular CoAP backend not implemented");
}
AgClientResult AgClient::coap_post_measures(const MeasuresBasic *, size_t, int, int) {
  abort_unsupported("coap_post_measures (batch)", "cellular CoAP backend not implemented");
}
AgClientResult AgClient::coap_post_measures(const MeasuresAGo *, size_t, int, int) {
  abort_unsupported("coap_post_measures (batch)", "cellular CoAP backend not implemented");
}

// -----------------------------------------------------------------------------
// MQTT stubs
// -----------------------------------------------------------------------------

AgClientResult AgClient::mqtt_connect(const char *, int, const char *, const char *) {
  abort_unsupported("mqtt_connect", "MQTT backend not implemented");
}

AgClientResult AgClient::mqtt_disconnect() {
  abort_unsupported("mqtt_disconnect", "MQTT backend not implemented");
}

AgClientResult AgClient::mqtt_publish_measures(const Measures &, int, int) {
  abort_unsupported("mqtt_publish_measures", "MQTT backend not implemented");
}
AgClientResult AgClient::mqtt_publish_measures(const MeasuresBasic &, int, int) {
  abort_unsupported("mqtt_publish_measures", "MQTT backend not implemented");
}
AgClientResult AgClient::mqtt_publish_measures(const MeasuresAGo &, int, int) {
  abort_unsupported("mqtt_publish_measures", "MQTT backend not implemented");
}

// -----------------------------------------------------------------------------
// Domain overrides
// -----------------------------------------------------------------------------

void AgClient::set_http_domain(const char *domain) {
  if (domain != nullptr) {
    _http_domain = domain;
  }
}

void AgClient::reset_http_domain() { _http_domain = DEFAULT_HTTP_DOMAIN; }

void AgClient::set_coap_host(const char *host) {
  if (host != nullptr) {
    _coap_host = host;
  }
}

void AgClient::reset_coap_host() { _coap_host = DEFAULT_COAP_HOST; }

// -----------------------------------------------------------------------------
// URL building
// -----------------------------------------------------------------------------

bool AgClient::_build_fetch_config_url(char *buf, size_t size) const {
#if defined(CONFIG_AG_DEVICE_MODEL_GO)
  static constexpr const char *CONFIG_PATH = "go/config";
#elif defined(CONFIG_AG_DEVICE_MODEL_MAX)
  static constexpr const char *CONFIG_PATH = "one/config";
#elif defined(CONFIG_AG_DEVICE_MODEL_ONE_OPEN_AIR)
  static constexpr const char *CONFIG_PATH = "one/config";
#endif

  const int n = std::snprintf(buf, size, "https://%s/sensors/airgradient:%s/%s",
                              _http_domain.c_str(), _serial_number, CONFIG_PATH);
  return n > 0 && static_cast<size_t>(n) < size;
}

bool AgClient::_build_post_measures_url(char *buf, size_t size) const {
  const int n = std::snprintf(buf, size, "https://%s/sensors/airgradient:%s/measures",
                              _http_domain.c_str(), _serial_number);
  return n > 0 && static_cast<size_t>(n) < size;
}

// -----------------------------------------------------------------------------
// Response code mapping
// -----------------------------------------------------------------------------

AgClientResult AgClient::_map_fetch_config_result(bool transport_ok, int status, bool truncated) {
  if (!transport_ok) {
    return AgClientResult::TransportError;
  }
  if (truncated) {
    // Truncation beats status: a 200 with partial body is unusable.
    return AgClientResult::BufferTooSmall;
  }
  switch (status) {
  case 200:
    return AgClientResult::Ok;
  case 400:
    return AgClientResult::NotRegistered;
  default:
    return AgClientResult::ServerError;
  }
}

AgClientResult AgClient::_map_post_measures_result(bool transport_ok, int status) {
  if (!transport_ok) {
    return AgClientResult::TransportError;
  }
  // 429 = rate-limited but stored.
  if (status == 200 || status == 201 || status == 429) {
    return AgClientResult::Ok;
  }
  return AgClientResult::ServerError;
}
