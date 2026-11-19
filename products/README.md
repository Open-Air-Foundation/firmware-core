# Products

AirGradient product-specific ESP-IDF application roots.

Each product folder is a thin application layer that composes shared
components from `components/` into a buildable firmware project.

## Product Folders

| Product | Description | Status |
|---|---|---|
| [`go`](go/README.md) | AirGradient Go portable air quality monitor: PM, CO2, TVOC/NOx, GPS, e-paper display, BLE, battery | Active |
| [`reference`](reference/README.md) | Component integration and hardware smoke-test application | Developer test bed |

## Documentation

Start with the product README linked above, then follow its links to architecture,
topic documentation, or smoke-test sources as needed. For where documentation
belongs and how to maintain it, follow the [documentation guide](../docs/GUIDE.md).

## Typical Product Layout

```text
products/<product>/
  main/                # ESP-IDF app entrypoint
  tests/               # native product tests, when present
  docs/                # optional detailed topic documents
  specs/               # optional proposed-change specifications
  sdkconfig.defaults   # product-specific default configuration
  CMakeLists.txt       # ESP-IDF project root
  README.md            # product overview, sensors, build
```

Board support belongs with the product; its directory placement follows that
product's implementation. For example, Go keeps board composition under
`main/` rather than a mandatory top-level `bsp/` directory.

## Guidelines

- Keep product folders thin; reusable capabilities live in `components/`.
- Keep product-specific wiring, BSP, and composition here.
