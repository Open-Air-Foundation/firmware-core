/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_CLIENT_H
#define AG_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "../clients/coap_client.h"
#include "../clients/http_client.h"
#include "../clients/mqtt_client.h"
#include "../types/client_types.h"

class CellularModem;

// Unified AirGradient server client.  Only WiFi HTTP is implemented;
// CoAP/MQTT/cellular methods abort.  Not ISR-safe, not thread-safe,
// blocks on network I/O.
class AgClient {
public:
  AgClient() = default;

  // serial_number: 12-char hex.  Cellular returns false until backends ship.
  bool begin(const char *serial_number, NetworkType network, CellularModem *modem = nullptr);

  // --- HTTP (WiFi) ---
  AgClientResult http_fetch_config(char *config_out, size_t config_size, size_t *bytes_written);

  AgClientResult http_post_measures(const Measures &measures, int signal, uint32_t boot);
  AgClientResult http_post_measures(const MeasuresBasic &measures, int signal, uint32_t boot);
  AgClientResult http_post_measures(const MeasuresAGo &measures, int signal, uint32_t boot);

  // --- CoAP (stubs, abort) ---
  AgClientResult coap_fetch_config(char *config_out, size_t config_size, size_t *bytes_written);

  AgClientResult coap_post_measures(const Measures &measures, int signal, int interval_seconds);
  AgClientResult coap_post_measures(const MeasuresBasic &measures, int signal,
                                    int interval_seconds);
  AgClientResult coap_post_measures(const MeasuresAGo &measures, int signal, int interval_seconds);

  AgClientResult coap_post_measures(const Measures *measures, size_t count, int signal,
                                    int interval_seconds);
  AgClientResult coap_post_measures(const MeasuresBasic *measures, size_t count, int signal,
                                    int interval_seconds);
  AgClientResult coap_post_measures(const MeasuresAGo *measures, size_t count, int signal,
                                    int interval_seconds);

  // --- MQTT (stubs, abort) ---
  AgClientResult mqtt_connect(const char *host, int port, const char *username = nullptr,
                              const char *password = nullptr);
  AgClientResult mqtt_disconnect();

  AgClientResult mqtt_publish_measures(const Measures &measures, int signal, int interval_seconds);
  AgClientResult mqtt_publish_measures(const MeasuresBasic &measures, int signal,
                                       int interval_seconds);
  AgClientResult mqtt_publish_measures(const MeasuresAGo &measures, int signal,
                                       int interval_seconds);

  // --- Domain overrides ---
  void set_http_domain(const char *domain);
  void reset_http_domain();
  void set_coap_host(const char *host);
  void reset_coap_host();

private:
  static constexpr const char *DEFAULT_HTTP_DOMAIN = "hw.airgradient.com";
  static constexpr const char *DEFAULT_COAP_HOST = "128.140.49.53";
  static constexpr int DEFAULT_COAP_PORT = 5683;
  static constexpr size_t URL_BUFFER_SIZE = 128;

  static MeasuresInput _make_input(const Measures &m);
  static MeasuresInput _make_input(const MeasuresBasic &m);
  static MeasuresInput _make_input(const MeasuresAGo &m);

  AgClientResult _do_http_post_measures(const MeasuresInput &input, int signal, uint32_t boot);

  bool _build_fetch_config_url(char *buf, size_t size) const;
  bool _build_post_measures_url(char *buf, size_t size) const;

  static AgClientResult _map_fetch_config_result(bool transport_ok, int status, bool truncated);
  static AgClientResult _map_post_measures_result(bool transport_ok, int status);

  NetworkType _network = NetworkType::Wifi;
  char _serial_number[13] = {}; // 12-char hex + NUL

  std::string _http_domain = DEFAULT_HTTP_DOMAIN;
  std::string _coap_host = DEFAULT_COAP_HOST;

  HttpClient *_http = nullptr;
  MqttClient *_mqtt = nullptr;
  CoapClient *_coap = nullptr;

#ifdef TEST_HOST
  friend class AgClientTestAccess;
#endif
};

#endif // AG_CLIENT_H
