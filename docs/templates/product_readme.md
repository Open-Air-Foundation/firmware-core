# AirGradient \<Product\>

Firmware for the AirGradient \<Product\> (one-line description and a link to
the product page if applicable).

Template instructions: keep applicable sections, remove placeholders, and
adjust paths for the destination. Link only to documents that exist. Remove
this paragraph when using the template.

## Sensors

- **PM** — vendor model (PM1.0, PM2.5, PM10)
- **CO2** — vendor model
- **TVOC / NOx** — vendor model
- **Temp / Humidity** — vendor model or fallback source
- **Pressure** — vendor model
- **Battery** — vendor PMIC

## Hardware Notes

Summarize essential setup constraints and hardware variants. Link to the
authoritative hardware or power document for initialization sequences, timing,
failure handling, and design rationale. Keep this section short enough for a
reader getting started with the product.

## Build

Optional: document only product-specific requirements not covered by the root
README. Remove this section when the shared build instructions are sufficient.

## Documentation

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — boot paths, event model, module
  structure
- [`docs/`](docs) — current behavior, service contracts, and design rationale
- [`specs/`](specs) — proposed changes and implementation plans
- `main/board_config.h` — pin assignments and I2C addresses
