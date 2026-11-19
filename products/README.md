# Products

AirGradient product-specific ESP-IDF application roots.

Each product folder is a thin application layer that composes shared
components from `components/` into a buildable firmware project.

## Product Folders

| Product | Description | Status |
|---|---|---|
| [`go`](go/README.md) | AirGradient Go portable air quality monitor: PM, CO2, TVOC/NOx, GPS, e-paper display, BLE, battery | Active |
| [`reference`](reference/README.md) | Thin reference ESP-IDF entrypoint used to validate the shared component layout | Scaffold |

## Per-Product Documentation

Each product carries its own:

- `README.md` — sensors, hardware notes, build command
- `ARCHITECTURE.md` (when applicable) — boot paths, event model, module
  structure
- `docs/` — service-level documentation (one file per service)
- `specs/` — design specs and refactor plans (temporary; see
  [`docs/GUIDE.md`](../docs/GUIDE.md) → "Doc Lifecycle")

## Typical Product Layout

```text
products/<product>/
  main/                # ESP-IDF app entrypoint
  bsp/                 # product-specific board support and hardware wiring
  docs/                # per-service docs (active products)
  specs/               # design specs (active products)
  sdkconfig.defaults   # product-specific default configuration
  CMakeLists.txt       # ESP-IDF project root
  README.md            # product overview, sensors, build
```

## Guidelines

- Keep product folders thin; reusable capabilities live in `components/`.
- Keep product-specific wiring, BSP, and composition here.
- Each product README follows the
  [product README template](../docs/templates/product_readme.md).
