/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <cstring>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <cJSON.h>

#include "fake_providers.h"
#include "services/local_server.h"
#include "test_http_request.h"

namespace {

constexpr const char *MEASURES = "/api/v1/measures";
constexpr const char *CONFIG = "/api/v1/config";
constexpr const char *CALIBRATE_CO2 = "/api/v1/actions/calibrate-co2";
constexpr const char *TEST_LEDS = "/api/v1/actions/test-leds";
constexpr const char *TEST_GPS = "/api/v1/actions/test-gps";

std::string body_string(const HttpResponse &resp) {
  return std::string(static_cast<const char *>(resp.body_data()), resp.body_size());
}

// Extract error.code; returns "" if absent.
std::string error_code(const HttpResponse &resp) {
  cJSON *root = cJSON_Parse(body_string(resp).c_str());
  if (root == nullptr) {
    return "";
  }
  cJSON *err = cJSON_GetObjectItem(root, "error");
  cJSON *code = cJSON_GetObjectItem(err, "code");
  std::string out = (cJSON_IsString(code)) ? code->valuestring : "";
  cJSON_Delete(root);
  return out;
}

std::string error_field(const HttpResponse &resp) {
  cJSON *root = cJSON_Parse(body_string(resp).c_str());
  if (root == nullptr) {
    return "";
  }
  cJSON *err = cJSON_GetObjectItem(root, "error");
  cJSON *field = cJSON_GetObjectItem(err, "field");
  std::string out = (cJSON_IsString(field)) ? field->valuestring : "";
  cJSON_Delete(root);
  return out;
}

std::string error_message(const HttpResponse &resp) {
  cJSON *root = cJSON_Parse(body_string(resp).c_str());
  if (root == nullptr) {
    return "";
  }
  cJSON *err = cJSON_GetObjectItem(root, "error");
  cJSON *message = cJSON_GetObjectItem(err, "message");
  std::string out = (cJSON_IsString(message)) ? message->valuestring : "";
  cJSON_Delete(root);
  return out;
}

} // namespace

TEST_CASE("GET measures returns a zero-copy JSON body", "[handler][measures]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  std::strncpy(measures.info.model, "O-1PST", sizeof(measures.info.model) - 1);
  measures.measures.co2.co2 = 612;

  LocalServer ls(server, {measures});
  REQUIRE(ls.begin());
  REQUIRE(server.has_route(HttpMethod::Get, MEASURES));

  TestHttpRequest req(HttpMethod::Get, MEASURES);
  HttpResponse resp;
  REQUIRE(server.invoke(HttpMethod::Get, MEASURES, req, resp));

  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(std::string(resp.content_type) == "application/json");
  REQUIRE(resp.is_body_static());
  cJSON *root = cJSON_Parse(body_string(resp).c_str());
  REQUIRE(root != nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "co2")->valueint == 612);
  cJSON_Delete(root);
}

TEST_CASE("ConfigAccess controls config route presence", "[handler][config][lifecycle]") {
  FakeMeasuresProvider measures;
  FakeConfigProvider config;

  SECTION("Disabled exposes no config route") {
    RecordingHttpServer server;
    LocalServer ls(server, {measures, &config, ConfigAccess::Disabled});
    REQUIRE(ls.begin());
    REQUIRE_FALSE(server.has_route(HttpMethod::Get, CONFIG));
    REQUIRE_FALSE(server.has_route(HttpMethod::Put, CONFIG));
  }

  SECTION("ReadOnly exposes GET but not PUT") {
    RecordingHttpServer server;
    LocalServer ls(server, {measures, &config, ConfigAccess::ReadOnly});
    REQUIRE(ls.begin());
    REQUIRE(server.has_route(HttpMethod::Get, CONFIG));
    REQUIRE_FALSE(server.has_route(HttpMethod::Put, CONFIG));
  }

  SECTION("ReadWrite exposes GET and PUT") {
    RecordingHttpServer server;
    LocalServer ls(server, {measures, &config, ConfigAccess::ReadWrite});
    REQUIRE(ls.begin());
    REQUIRE(server.has_route(HttpMethod::Get, CONFIG));
    REQUIRE(server.has_route(HttpMethod::Put, CONFIG));
  }
}

TEST_CASE("GET config serializes the provider config", "[handler][config]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  FakeConfigProvider config;
  config.config.temperature_unit = "f";
  config.config.altitude_unit = "ft";

  LocalServer ls(server, {measures, &config, ConfigAccess::ReadOnly});
  REQUIRE(ls.begin());

  TestHttpRequest req(HttpMethod::Get, CONFIG);
  HttpResponse resp;
  REQUIRE(server.invoke(HttpMethod::Get, CONFIG, req, resp));
  REQUIRE(resp.status == HttpStatus::Ok);
  cJSON *root = cJSON_Parse(body_string(resp).c_str());
  REQUIRE(std::strcmp(cJSON_GetObjectItem(root, "temperatureUnit")->valuestring, "f") == 0);
  REQUIRE(std::strcmp(cJSON_GetObjectItem(root, "altitudeUnit")->valuestring, "ft") == 0);
  cJSON_Delete(root);
}

TEST_CASE("GET config rejects incomplete correction snapshots", "[handler][config]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  FakeConfigProvider config;
  Corrections corrections;
  CorrectionEntry pm25;
  pm25.algorithm = "custom_via_pm25_raw";
  SlrParams slr;
  slr.scaling_factor = 1.0;
  pm25.slr = slr;
  corrections.pm25 = pm25;
  config.config.corrections = corrections;

  LocalServer ls(server, {measures, &config, ConfigAccess::ReadOnly});
  REQUIRE(ls.begin());

  TestHttpRequest req(HttpMethod::Get, CONFIG);
  HttpResponse resp;
  REQUIRE(server.invoke(HttpMethod::Get, CONFIG, req, resp));
  REQUIRE(resp.status == HttpStatus::InternalServerError);
  REQUIRE(error_code(resp) == "internal");
}

TEST_CASE("PUT config maps submit results to status codes", "[handler][config]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  FakeConfigProvider config;
  LocalServer ls(server, {measures, &config, ConfigAccess::ReadWrite});
  REQUIRE(ls.begin());

  auto put = [&](const std::string &body, HttpResponse &resp) {
    TestHttpRequest req(HttpMethod::Put, CONFIG);
    req.set_body(body);
    REQUIRE(server.invoke(HttpMethod::Put, CONFIG, req, resp));
  };

  SECTION("Accepted -> empty 202 and the partial reaches the provider") {
    config.submit_result = {ConfigSubmitStatus::Accepted, ConfigFieldId::None};
    HttpResponse resp;
    put(R"({"altitudeUnit":"m"})", resp);
    REQUIRE(resp.status == HttpStatus::Accepted);
    REQUIRE(resp.content_type == nullptr);
    REQUIRE(resp.body_size() == 0);
    REQUIRE(config.submit_called);
    REQUIRE(*config.last_submitted.altitude_unit == "m");
  }

  SECTION("parse invalid_body -> 400, provider not called") {
    HttpResponse resp;
    put(R"({bad)", resp);
    REQUIRE(resp.status == HttpStatus::BadRequest);
    REQUIRE(error_code(resp) == "invalid_body");
    REQUIRE_FALSE(config.submit_called);
  }

  SECTION("unknown key -> 400 unknown_field with field echoed") {
    HttpResponse resp;
    put(R"({"temperatureUnits":"c"})", resp);
    REQUIRE(resp.status == HttpStatus::BadRequest);
    REQUIRE(error_code(resp) == "unknown_field");
    REQUIRE(error_field(resp) == "temperatureUnits");
    REQUIRE_FALSE(config.submit_called);
  }

  SECTION("unknown key is JSON escaped in the error field") {
    HttpResponse resp;
    put(R"({"bad\"key":1})", resp);
    REQUIRE(resp.status == HttpStatus::BadRequest);
    REQUIRE(error_code(resp) == "unknown_field");
    REQUIRE(error_field(resp) == "bad\"key");
    REQUIRE_FALSE(config.submit_called);
  }

  SECTION("bad enum -> 400 invalid_value with field") {
    HttpResponse resp;
    put(R"({"temperatureUnit":"k"})", resp);
    REQUIRE(resp.status == HttpStatus::BadRequest);
    REQUIRE(error_code(resp) == "invalid_value");
    REQUIRE(error_field(resp) == "temperatureUnit");
    REQUIRE_FALSE(config.submit_called);
  }

  SECTION("nested corrections error reports a dotted field") {
    HttpResponse resp;
    put(R"({"corrections":{"pm25":42}})", resp);
    REQUIRE(resp.status == HttpStatus::BadRequest);
    REQUIRE(error_code(resp) == "invalid_value");
    REQUIRE(error_field(resp) == "corrections.pm25");
    REQUIRE_FALSE(config.submit_called);
  }

  SECTION("submit InvalidValue -> 400 invalid_value with mapped field") {
    config.submit_result = {ConfigSubmitStatus::InvalidValue, ConfigFieldId::Co2AbcDays};
    HttpResponse resp;
    put(R"({"co2AbcDays":99})", resp);
    REQUIRE(resp.status == HttpStatus::BadRequest);
    REQUIRE(error_code(resp) == "invalid_value");
    REQUIRE(error_field(resp) == "co2AbcDays");
  }

  SECTION("submit Forbidden -> 403 forbidden (no field)") {
    config.submit_result = {ConfigSubmitStatus::Forbidden, ConfigFieldId::None};
    HttpResponse resp;
    put(R"({"temperatureUnit":"c"})", resp);
    REQUIRE(resp.status == HttpStatus::Forbidden);
    REQUIRE(error_code(resp) == "forbidden");
    REQUIRE(error_field(resp).empty());
  }

  SECTION("submit NotSupported -> 404 not_found with existing mapped field") {
    config.submit_result = {ConfigSubmitStatus::NotSupported, ConfigFieldId::LedMode};
    HttpResponse resp;
    put(R"({"ledMode":"pm"})", resp);
    REQUIRE(resp.status == HttpStatus::NotFound);
    REQUIRE(error_code(resp) == "not_found");
    REQUIRE(error_field(resp) == "ledMode");
  }

  SECTION("submit NotSupported -> 404 not_found with altitude field") {
    config.submit_result = {ConfigSubmitStatus::NotSupported, ConfigFieldId::AltitudeUnit};
    HttpResponse resp;
    put(R"({"altitudeUnit":"ft"})", resp);
    REQUIRE(resp.status == HttpStatus::NotFound);
    REQUIRE(error_code(resp) == "not_found");
    REQUIRE(error_field(resp) == "altitudeUnit");
  }

  SECTION("submit Busy -> 503 busy without retry metadata") {
    config.submit_result = {ConfigSubmitStatus::Busy, ConfigFieldId::None};
    HttpResponse resp;
    put(R"({"temperatureUnit":"c"})", resp);
    REQUIRE(resp.status == HttpStatus::ServiceUnavailable);
    REQUIRE(error_code(resp) == "busy");
    REQUIRE(error_message(resp) == "busy");
    REQUIRE(error_field(resp).empty());
    REQUIRE(resp.header_count == 0);
  }

  SECTION("submit Internal -> 500 internal") {
    config.submit_result = {ConfigSubmitStatus::Internal, ConfigFieldId::None};
    HttpResponse resp;
    put(R"({"temperatureUnit":"c"})", resp);
    REQUIRE(resp.status == HttpStatus::InternalServerError);
    REQUIRE(error_code(resp) == "internal");
  }
}

TEST_CASE("PUT config rejects incomplete bodies before provider policy", "[handler][config]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  FakeConfigProvider config;
  config.submit_result = {ConfigSubmitStatus::Forbidden, ConfigFieldId::None};
  LocalServer ls(server, {measures, &config, ConfigAccess::ReadWrite});
  REQUIRE(ls.begin());

  TestHttpRequest req(HttpMethod::Put, CONFIG);
  req.set_body(R"({"temperatureUnit":"c"})");
  req.set_body_complete(false);
  HttpResponse resp;
  REQUIRE(server.invoke(HttpMethod::Put, CONFIG, req, resp));

  REQUIRE(resp.status == HttpStatus::BadRequest);
  REQUIRE(error_code(resp) == "invalid_body");
  REQUIRE_FALSE(config.submit_called);
}

TEST_CASE("PUT config parses before provider policy", "[handler][config]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  FakeConfigProvider config;
  config.submit_result = {ConfigSubmitStatus::Forbidden, ConfigFieldId::None};
  LocalServer ls(server, {measures, &config, ConfigAccess::ReadWrite});
  REQUIRE(ls.begin());

  TestHttpRequest malformed(HttpMethod::Put, CONFIG);
  malformed.set_body(R"({bad)");
  HttpResponse malformed_resp;
  REQUIRE(server.invoke(HttpMethod::Put, CONFIG, malformed, malformed_resp));
  REQUIRE(malformed_resp.status == HttpStatus::BadRequest);
  REQUIRE(error_code(malformed_resp) == "invalid_body");
  REQUIRE_FALSE(config.submit_called);

  TestHttpRequest valid(HttpMethod::Put, CONFIG);
  valid.set_body(R"({"temperatureUnit":"c"})");
  HttpResponse valid_resp;
  REQUIRE(server.invoke(HttpMethod::Put, CONFIG, valid, valid_resp));
  REQUIRE(valid_resp.status == HttpStatus::Forbidden);
  REQUIRE(config.submit_called);
}

TEST_CASE("PUT empty config still evaluates provider policy", "[handler][config]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  FakeConfigProvider config;
  LocalServer ls(server, {measures, &config, ConfigAccess::ReadWrite});
  REQUIRE(ls.begin());

  auto put_empty = [&](HttpResponse &resp) {
    TestHttpRequest req(HttpMethod::Put, CONFIG);
    req.set_body("{}");
    REQUIRE(server.invoke(HttpMethod::Put, CONFIG, req, resp));
  };

  SECTION("accepted no-op returns 202") {
    config.submit_result = {ConfigSubmitStatus::Accepted, ConfigFieldId::None};
    HttpResponse resp;
    put_empty(resp);
    REQUIRE(resp.status == HttpStatus::Accepted);
    REQUIRE(config.submit_called);
    REQUIRE_FALSE(config.last_submitted.temperature_unit.has_value());
  }

  SECTION("write gate can reject the no-op") {
    config.submit_result = {ConfigSubmitStatus::Forbidden, ConfigFieldId::None};
    HttpResponse resp;
    put_empty(resp);
    REQUIRE(resp.status == HttpStatus::Forbidden);
    REQUIRE(config.submit_called);
  }
}

TEST_CASE("actions register all catalog routes and map results", "[handler][actions]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  FakeActionHandler actions;
  LocalServer ls(server, {measures, nullptr, ConfigAccess::Disabled, &actions});
  REQUIRE(ls.begin());

  // Every catalog action gets a route, regardless of model support.
  REQUIRE(server.has_route(HttpMethod::Post, CALIBRATE_CO2));
  REQUIRE(server.has_route(HttpMethod::Post, TEST_LEDS));
  REQUIRE(server.has_route(HttpMethod::Post, TEST_GPS));

  SECTION("Dispatched -> 200 empty body") {
    actions.result_calibrate = {ActionStatus::Dispatched};
    TestHttpRequest req(HttpMethod::Post, CALIBRATE_CO2);
    HttpResponse resp;
    REQUIRE(server.invoke(HttpMethod::Post, CALIBRATE_CO2, req, resp));
    REQUIRE(resp.status == HttpStatus::Ok);
    REQUIRE(resp.content_type == nullptr);
    REQUIRE(resp.body_size() == 0);
    REQUIRE(actions.triggered);
    REQUIRE(actions.last_action == ActionId::CalibrateCo2);
  }

  SECTION("Rejected -> 403 forbidden") {
    actions.result_test_leds = {ActionStatus::Rejected};
    TestHttpRequest req(HttpMethod::Post, TEST_LEDS);
    HttpResponse resp;
    REQUIRE(server.invoke(HttpMethod::Post, TEST_LEDS, req, resp));
    REQUIRE(resp.status == HttpStatus::Forbidden);
    REQUIRE(error_code(resp) == "forbidden");
  }

  SECTION("GPS test dispatches the catalog action") {
    actions.result_test_gps = {ActionStatus::Dispatched};
    TestHttpRequest req(HttpMethod::Post, TEST_GPS);
    HttpResponse resp;
    REQUIRE(server.invoke(HttpMethod::Post, TEST_GPS, req, resp));
    REQUIRE(resp.status == HttpStatus::Ok);
    REQUIRE(resp.body_size() == 0);
    REQUIRE(actions.last_action == ActionId::TestGps);
  }

  SECTION("NotSupported -> 404 not_found") {
    actions.result_test_leds = {ActionStatus::NotSupported};
    TestHttpRequest req(HttpMethod::Post, TEST_LEDS);
    HttpResponse resp;
    REQUIRE(server.invoke(HttpMethod::Post, TEST_LEDS, req, resp));
    REQUIRE(resp.status == HttpStatus::NotFound);
    REQUIRE(error_code(resp) == "not_found");
  }

  SECTION("Busy -> 503 busy without retry metadata") {
    actions.result_test_leds = {ActionStatus::Busy};
    TestHttpRequest req(HttpMethod::Post, TEST_LEDS);
    HttpResponse resp;
    REQUIRE(server.invoke(HttpMethod::Post, TEST_LEDS, req, resp));
    REQUIRE(resp.status == HttpStatus::ServiceUnavailable);
    REQUIRE(error_code(resp) == "busy");
    REQUIRE(error_message(resp) == "busy");
    REQUIRE(resp.header_count == 0);
  }
}

TEST_CASE("no action handler leaves action routes unregistered", "[handler][actions]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  LocalServer ls(server, {measures});
  REQUIRE(ls.begin());
  REQUIRE_FALSE(server.has_route(HttpMethod::Post, CALIBRATE_CO2));
  REQUIRE_FALSE(server.has_route(HttpMethod::Post, TEST_LEDS));
  REQUIRE_FALSE(server.has_route(HttpMethod::Post, TEST_GPS));
}

TEST_CASE("begin is idempotent", "[lifecycle]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  LocalServer ls(server, {measures});
  REQUIRE(ls.begin());
  const size_t count = server.routes.size();
  REQUIRE(ls.begin()); // no-op true
  REQUIRE(server.routes.size() == count);
}

TEST_CASE("begin rolls back on mid-registration failure", "[lifecycle]") {
  RecordingHttpServer server;
  server.fail_path = CONFIG; // GET /config registration will fail
  FakeMeasuresProvider measures;
  FakeConfigProvider config;
  LocalServer ls(server, {measures, &config, ConfigAccess::ReadWrite});

  REQUIRE_FALSE(ls.begin());
  // measures was registered first then rolled back; nothing remains.
  REQUIRE(server.routes.empty());
}

TEST_CASE("end unregisters only this server's routes", "[lifecycle]") {
  RecordingHttpServer server;
  // A foreign route registered by some other owner on the same server.
  server.register_route(HttpMethod::Get, "/foreign", [](const HttpRequest &, HttpResponse &) {});
  FakeMeasuresProvider measures;
  LocalServer ls(server, {measures});
  REQUIRE(ls.begin());
  REQUIRE(server.has_route(HttpMethod::Get, MEASURES));

  ls.end();
  REQUIRE_FALSE(server.has_route(HttpMethod::Get, MEASURES));
  REQUIRE(server.has_route(HttpMethod::Get, "/foreign"));
}

TEST_CASE("destruction unregisters remaining routes", "[lifecycle]") {
  RecordingHttpServer server;
  FakeMeasuresProvider measures;
  {
    LocalServer ls(server, {measures});
    REQUIRE(ls.begin());
    REQUIRE(server.has_route(HttpMethod::Get, MEASURES));
  }
  REQUIRE_FALSE(server.has_route(HttpMethod::Get, MEASURES));
}
