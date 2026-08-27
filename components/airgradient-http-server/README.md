# airgradient-http-server

Generic HTTP server primitive: route registration and a request/response
abstraction backed by `esp_http_server`, so route handler logic stays
host-testable without a running server.

## Status

`Stable`.

## Scope

This component owns:

- HTTP server lifecycle (start on a configurable port, stop)
- route registration and unregistration (HTTP method + exact path +
  handler callback), supported both before and after server start
- request abstraction (method, URI, headers, query parameters, complete body)
- response abstraction (status, headers, body with owning and borrowing
  modes)
- static asset serving helper (registers GET handlers for flash-embedded
  files)
- a `TestHttpRequest` helper for consumer host tests

This component does not own:

- endpoints, payload schemas, or product-specific routes
- TLS / HTTPS
- authentication or authorization
- WebSockets or long-lived connections
- CORS headers or preflight handling
- path parameters, wildcards, or pattern-based route matching
- cloud connectivity

## Directory Layout

```text
components/airgradient-http-server/
  hal/
  types/
  drivers/
  tests/
  CMakeLists.txt
  Kconfig
  README.md
```

- `hal/` — public abstract interfaces (`HttpServer`, `HttpRequest`) and
  the concrete response value type (`HttpResponse`)
- `types/` — shared enums and type aliases (`HttpMethod`, `HttpStatus`,
  `HttpHandler`)
- `drivers/` — `esp_http_server`-backed concrete implementation
- `tests/` — host tests and the reusable `TestHttpRequest` helper

## Public Includes

```cpp
#include "hal/http_server.h"
#include "hal/http_request.h"
#include "hal/http_response.h"
#include "types/http_types.h"
#include "drivers/idf_http_server.h"
```

Guideline:

- include from `hal/` when depending on the server, request, or response
  abstractions
- include from `types/` when only the enums or handler type alias are
  needed
- include from `drivers/` only when instantiating the ESP-IDF-backed
  server

## Design

```text
caller -> HttpServer& -> IdfHttpServer -> esp_http_server -> TCP stack
```

Product code creates an `IdfHttpServer`, registers routes through the
abstract `HttpServer &` interface, and starts listening. Routes may be
registered or unregistered at any time — before or after start. Each
handler
receives a `const HttpRequest &` (abstract, backed by `httpd_req_t *` at
runtime) and an `HttpResponse &` (concrete value type). After the handler
returns the driver reads the populated response and sends it. ESP-IDF
headers are confined to `drivers/`; `hal/` and `types/` use only
standard C++.

`HttpResponse` defaults to `500 Internal Server Error`. A handler that
forgets to populate the response produces an error, not an accidental
`200 OK` with an empty body.

### Request Body Completeness

Request body buffering is lazy. `body()`, `body_length()`, and
`body_complete()` trigger one read of the declared HTTP entity. Bodies larger
than `CONFIG_AG_HTTP_MAX_BODY_SIZE`, socket short reads, and receive failures
set `body_complete()` to `false` and expose no partial bytes. This prevents a
valid-looking prefix from being parsed as the complete request.

Zero-length entities are complete but still return `nullptr` from `body()`.
Handlers remain responsible for deciding whether an empty body is valid.

### Response Body Modes

| Mode | Setter | Copies? | When To Use |
|---|---|---|---|
| Owning | `json`, `body` | yes | Stack-local handler buffers (dynamic JSON, etc.) |
| Borrowing | `body_static` | no | Flash-embedded assets or other data with static lifetime |

The owning mode copies into an internal `std::string` so the body
survives after the handler's stack frame is destroyed. The borrowing
mode stores a raw pointer and skips the copy.

`HttpStatus` includes `202 Accepted` and `503 Service Unavailable`.
`HttpResponse::empty(status)` creates a status-only response with no content
type or body.

## Public API

| Method | Returns | Purpose |
|---|---|---|
| `register_route(method, path, handler)` | `bool` | Register an exact `method`+`path` handler; works before or after `start()` |
| `unregister_route(method, path)` | `bool` | Remove a previously registered handler by `method`+`path` |
| `unregister_all()` | `void` | Remove all registered routes |
| `register_static(uri, data_start, data_end, content_type)` | `bool` | Convenience GET handler for flash-embedded assets |
| `start(port)` | `bool` | Bind and start listening |
| `stop()` | `void` | Stop the server; does not clear routes |

See [`hal/http_server.h`](hal/http_server.h),
[`hal/http_request.h`](hal/http_request.h), and
[`hal/http_response.h`](hal/http_response.h) for the full interfaces.

## Usage

### Basic

```cpp
IdfHttpServer server;
server.register_route(HttpMethod::Get, "/api/status", handle_status);
server.register_route(HttpMethod::Post, "/api/config", handle_config);
server.register_static("/", index_html_start, index_html_end, "text/html");
server.start(CONFIG_AG_HTTP_PORT);
```

### Dynamic route swap

Routes can be registered and unregistered on a running server:

```cpp
// Phase 1 — provisioning routes
server.start(CONFIG_AG_HTTP_PORT);
server.register_route(HttpMethod::Post, "/api/provision", handle_provision);

// Phase 2 — swap to measurement routes
server.unregister_all();
server.register_route(HttpMethod::Get, "/api/measures", handle_measures);
```

### Full teardown and restart

Call `unregister_all()` after `stop()` for a clean slate before the
next `start()` cycle:

```cpp
server.stop();
server.unregister_all();

// Re-configure with different routes
server.register_route(HttpMethod::Get, "/api/status", handle_status);
server.start(CONFIG_AG_HTTP_PORT);
```

### Handler example

A handler is any function matching `HttpHandler`:

```cpp
void handle_status(const HttpRequest &req, HttpResponse &resp) {
  char buf[128];
  const int n = std::snprintf(buf, sizeof(buf), R"({"uptime_ms":%lld})",
                              static_cast<long long>(esp_timer_get_time() / 1000));
  resp.json(HttpStatus::Ok, buf, static_cast<size_t>(n));
}
```

Flash-embedded assets come from ESP-IDF's `EMBED_FILES`; the
`_binary_<name>_start` / `_binary_<name>_end` linker symbols feed
straight into `register_static`.

For a complete working example, see
[`products/reference/main/test_http_server.cpp`](../../products/reference/main/test_http_server.cpp).

## Configuration

Configurable through Kconfig under **AirGradient HTTP Server**:

| Symbol | Default | Purpose |
|---|---|---|
| `CONFIG_AG_HTTP_PORT` | `80` | Default listen port |
| `CONFIG_AG_HTTP_MAX_CONNECTIONS` | `4` | Maximum concurrent connections |
| `CONFIG_AG_HTTP_MAX_BODY_SIZE` | `4096` | Maximum complete request body size in bytes; oversized bodies are rejected |
| `CONFIG_AG_HTTP_MAX_ROUTES` | `24` | Maximum number of registered URI handlers (passed to `esp_http_server` as `max_uri_handlers`); default sized for the provisioning captive portal |

The HTTP server task stack is fixed at 6144 bytes to provide sufficient
headroom for nested JSON route handlers. Other `httpd_config_t` fields retain
their `HTTPD_DEFAULT_CONFIG()` values unless listed above.

## Dependencies

- `components/airgradient-common/` — shared types and RTOS abstraction
- `esp_http_server` (ESP-IDF) — underlying HTTP server implementation

## Tests

Host tests live under `components/airgradient-http-server/tests/` and run
through the [tests runner](../../tests/README.md). The driver itself
(`drivers/idf_http_server.cpp`) depends on `esp_http_server` and is
covered by the firmware build rather than host tests. Platform-neutral tests
cover status phrases and the body reader's complete, oversized, short-read,
and receive-failure outcomes.

The component provides a `TestHttpRequest` helper for handler-level
tests. Because `HttpResponse` is a concrete value type it needs no mock —
tests construct one, call the handler, and inspect the fields directly:

```cpp
TEST_CASE("status handler returns JSON", "[http]") {
  TestHttpRequest req(HttpMethod::Get, "/api/status");
  HttpResponse resp;

  handle_status(req, resp);

  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(std::string(resp.content_type) == "application/json");
  REQUIRE(resp.body_size() > 0);
}
```
