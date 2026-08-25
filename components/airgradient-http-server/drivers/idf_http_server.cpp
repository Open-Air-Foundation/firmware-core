/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "idf_http_server.h"

#include <algorithm>
#include <cstddef>

#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>

#include "hal/http_request.h"
#include "hal/http_response.h"
#include "idf_http_request.h"
#include "types/http_types.h"

namespace {

static constexpr const char *TAG = "IdfHttpServer";
static constexpr size_t HTTP_SERVER_TASK_STACK_SIZE_BYTES = 6144;

httpd_method_t to_httpd_method(HttpMethod m) {
  switch (m) {
  case HttpMethod::Get:
    return HTTP_GET;
  case HttpMethod::Post:
    return HTTP_POST;
  case HttpMethod::Put:
    return HTTP_PUT;
  case HttpMethod::Delete:
    return HTTP_DELETE;
  }
  return HTTP_GET;
}

HttpMethod from_httpd_method(int m) {
  switch (m) {
  case HTTP_GET:
    return HttpMethod::Get;
  case HTTP_POST:
    return HttpMethod::Post;
  case HTTP_PUT:
    return HttpMethod::Put;
  case HTTP_DELETE:
    return HttpMethod::Delete;
  default:
    return HttpMethod::Get;
  }
}

const char *method_str(HttpMethod m) {
  switch (m) {
  case HttpMethod::Get:
    return "GET";
  case HttpMethod::Post:
    return "POST";
  case HttpMethod::Put:
    return "PUT";
  case HttpMethod::Delete:
    return "DELETE";
  }
  return "GET";
}

} // namespace

IdfHttpServer::IdfHttpServer() = default;

IdfHttpServer::~IdfHttpServer() { stop(); }

bool IdfHttpServer::register_route(HttpMethod method, const char *path, HttpHandler handler) {
  if (path == nullptr || handler == nullptr) {
    return false;
  }
  _routes.emplace_back(
      std::make_unique<Route>(Route{method, std::string(path), std::move(handler)}));

  if (_started) {
    Route *route = _routes.back().get();
    httpd_uri_t uri = {};
    uri.uri = route->path.c_str();
    uri.method = to_httpd_method(route->method);
    uri.handler = &IdfHttpServer::_trampoline;
    uri.user_ctx = route;
    if (httpd_register_uri_handler(_handle, &uri) != ESP_OK) {
      ESP_LOGE(TAG, "failed to register route %s %s on running server", method_str(method), path);
      _routes.pop_back();
      return false;
    }
  }
  ESP_LOGI(TAG, "route registered: %s %s", method_str(method), path);
  return true;
}

bool IdfHttpServer::start(uint16_t port) {
  if (_started) {
    return true;
  }

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = HTTP_SERVER_TASK_STACK_SIZE_BYTES;
  cfg.server_port = port;
  cfg.max_open_sockets = CONFIG_AG_HTTP_MAX_CONNECTIONS;
  cfg.max_uri_handlers = CONFIG_AG_HTTP_MAX_ROUTES;
  // Default uri matcher is exact match — keep that behaviour.

  if (httpd_start(&_handle, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed on port %u", static_cast<unsigned>(port));
    _handle = nullptr;
    return false;
  }

  for (const auto &route : _routes) {
    httpd_uri_t uri = {};
    uri.uri = route->path.c_str();
    uri.method = to_httpd_method(route->method);
    uri.handler = &IdfHttpServer::_trampoline;
    uri.user_ctx = route.get();
    if (httpd_register_uri_handler(_handle, &uri) != ESP_OK) {
      ESP_LOGE(TAG, "failed to register route %s", route->path.c_str());
      httpd_stop(_handle);
      _handle = nullptr;
      return false;
    }
  }

  _started = true;
  ESP_LOGI(TAG, "server started on port %u", static_cast<unsigned>(port));
  return true;
}

void IdfHttpServer::stop() {
  if (!_started) {
    return;
  }
  if (_handle != nullptr) {
    httpd_stop(_handle);
    _handle = nullptr;
  }
  _started = false;
  ESP_LOGI(TAG, "server stopped");
}

bool IdfHttpServer::unregister_route(HttpMethod method, const char *path) {
  if (path == nullptr) {
    return false;
  }
  auto it = std::find_if(_routes.begin(), _routes.end(), [method, path](const auto &r) {
    return r->method == method && r->path == path;
  });
  if (it == _routes.end()) {
    return false;
  }
  if (_started) {
    if (httpd_unregister_uri_handler(_handle, path, to_httpd_method(method)) != ESP_OK) {
      ESP_LOGE(TAG, "failed to unregister route %s from running server", path);
      return false;
    }
  }
  ESP_LOGI(TAG, "route unregistered: %s %s", method_str(method), path);
  _routes.erase(it);
  return true;
}

void IdfHttpServer::unregister_all() {
  if (_started) {
    for (const auto &route : _routes) {
      if (httpd_unregister_uri_handler(_handle, route->path.c_str(),
                                       to_httpd_method(route->method)) != ESP_OK) {
        ESP_LOGW(TAG, "failed to unregister route %s during unregister_all", route->path.c_str());
      }
    }
  }
  const size_t count = _routes.size();
  _routes.clear();
  ESP_LOGI(TAG, "all routes unregistered (%u removed)", static_cast<unsigned>(count));
}

esp_err_t IdfHttpServer::_trampoline(httpd_req_t *req) {
  Route *route = static_cast<Route *>(req->user_ctx);
  if (route == nullptr || route->handler == nullptr) {
    httpd_resp_set_status(req, http_status_phrase(HttpStatus::InternalServerError));
    httpd_resp_send(req, nullptr, 0);
    return ESP_FAIL;
  }

  IdfHttpRequest request(req, from_httpd_method(req->method));
  HttpResponse response;

  route->handler(request, response);

  httpd_resp_set_status(req, http_status_phrase(response.status));
  if (response.content_type != nullptr) {
    httpd_resp_set_type(req, response.content_type);
  }
  for (size_t i = 0; i < response.header_count; ++i) {
    const HttpHeader &h = response.headers[i];
    if (h.name != nullptr && h.value != nullptr) {
      httpd_resp_set_hdr(req, h.name, h.value);
    }
  }

  const char *body = static_cast<const char *>(response.body_data());
  const size_t len = response.body_size();
  // httpd_resp_send treats HTTPD_RESP_USE_STRLEN (-1) as "use strlen"; we
  // always pass an explicit length to support binary payloads.
  const esp_err_t send_result =
      httpd_resp_send(req, body == nullptr ? "" : body, static_cast<ssize_t>(len));
  // Close a connection whose declared body was not consumed completely. This
  // prevents unread or failed body bytes from corrupting a subsequent request.
  return request.has_incomplete_body() ? ESP_FAIL : send_result;
}
