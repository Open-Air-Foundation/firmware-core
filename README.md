# AirGradient Firmware

This repository contains the AirGradient ESP-IDF firmware monorepo for
AirGradient product models.

The project is organized around shared reusable components and thin
product-specific application roots. The goal is to keep firmware logic
testable, readable, and reusable across multiple AirGradient devices.

## Current Status

Today it includes:

- shared firmware components under `components/`
- the **AirGradient Go** portable monitor under `products/go/`
- a thin reference ESP-IDF product under `products/reference/`
- host-side test entrypoints under `tests/`

## Repository Layout

- `components/` - shared AirGradient ESP-IDF components, including shared
  drivers, HALs, services, and third-party code when it belongs to a shared
  component responsibility
- `products/` - AirGradient product-specific ESP-IDF application roots
- `tests/` - top-level host-test entrypoint
- `vhub/` - product-specific manual release-verification templates

## Key Ideas

- shared capability code lives in `components/`
- product-specific wiring and BSP live in `products/`
- application logic should stay host-testable where practical
- product application roots should stay thin

## Build Firmware

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"

# AirGradient Go
idf.py -C products/go build

# Reference product
idf.py -C products/reference build
```

## Release Firmware

Firmware releases use product-prefixed Git tags and the repository-level
[`release.yml`](.github/workflows/release.yml) workflow. Each releasable product
owns a tag prefix and a product-specific release job within that workflow.

| Tag Pattern | Product | Published Output |
|---|---|---|
| `go-vMAJOR.MINOR.PATCH` | AirGradient Go | Versioned firmware bundle ZIP attached to a GitHub Release |

Reference is a smoke-test product and has no release tag. Future shipping
products extend the same workflow with their own tag trigger and release job.

## Run Host Tests

```sh
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

## Continuous Integration

Pull requests and pushes to `main` use the shared build selector in
[`select_builds.py`](.github/scripts/select_builds.py). Firmware products are
discovered from product directories containing a `CMakeLists.txt` file.

| Changed Files | Firmware Builds | Host Tests |
|---|---|---|
| Documentation and known non-build tooling only | Skipped | Skipped |
| One product's production files | Changed product | Run |
| Shared component production files | All products | Run |
| Host-test files only | Skipped | Run |
| Unknown or unclassified files | All products | Run |

[`firmware-build.yml`](.github/workflows/firmware-build.yml) builds the selected
products with ESP-IDF v5.5.6. The workflow caches managed components using the
dependency lockfiles and manifests, but does not cache build directories or
upload firmware binaries. [`host-tests.yml`](.github/workflows/host-tests.yml)
uses the same ESP-IDF version and managed-component cache.

Both workflows retain an always-reporting result job when compilation is
intentionally skipped. Dependency resolution must not modify the committed
`dependencies.lock` file.

## Editor Compile Database

For clangd or Neovim LSP, this repo can generate compile databases in multiple
build roots:

- `tests/build/compile_commands.json` for native host tests
- `products/go/build/compile_commands.json` for the AGo ESP-IDF build
- `products/reference/build/compile_commands.json` for the reference ESP-IDF build

To create a single root-level `compile_commands.json` for editor discovery,
build whichever targets you need, then merge them:

```sh
python scripts/merge_compile_commands.py
```

The merge prefers host-test entries for shared files that appear in both
databases, then includes the remaining firmware-only entries.

## Documentation

Start at the layer that matches your task:

- [`components/README.md`](components/README.md) — shared component layout;
  each component carries its own `README.md`
- [`products/README.md`](products/README.md) — product application roots;
  start with its `README.md`, then follow links to architecture and topic docs
- [`tests/README.md`](tests/README.md) — host-test workflow
- [`vhub/README.md`](vhub/README.md) — manual release-verification templates
- [`docs/local_http_api.md`](docs/local_http_api.md) — product-neutral local
  HTTP API and mDNS discovery, with links to product support

Before creating or updating documentation, read the documentation guide:

- [`docs/GUIDE.md`](docs/GUIDE.md) — purpose, ownership, writing, maintenance,
  and style rules for first-party documentation
- [`docs/templates/`](docs/templates) — copy-pasteable templates per doc type

## Contributing

Before opening a PR, verify the relevant firmware build succeeds and all
relevant host tests pass:

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/<product> build
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

Update related documentation after the implementation changes are complete and
before final verification. For Markdown changes, run the documentation lint or
the full pre-commit suite.

Before opening a PR, review the relevant product template under [`vhub/`](vhub).
Update it when the change affects behavior that manual QA can observe through
the device, hardware, serial logs, network interfaces, or server data.
Internal-only changes do not require a template update.

Install the pre-commit hook once per clone so staged Markdown is checked and
staged C/C++ files are formatted locally before each commit:

```sh
pip install pre-commit
pre-commit install
```

The same pre-commit hooks run on every pull request via
[`pre-commit.yml`](.github/workflows/pre-commit.yml), including `clang-format`
and Markdown lint. PRs that fail formatting or lint checks are blocked.

GitHub Actions applies the change-aware firmware and host-test policy described
in [Continuous Integration](#continuous-integration) on every pull request and
push to `main`.

To run the hooks on the currently staged files before committing:

```sh
pre-commit run
```
