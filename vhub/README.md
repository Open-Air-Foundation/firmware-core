# VHUB Verification Templates

This directory contains manual release-verification templates consumed by
VHUB. These templates describe user-observable and hardware-observable firmware
behavior; they are not automated host tests.

## Scope

Each shipping product in this monorepo owns one product-specific
`*.vhub.json` template. New templates are added here alongside new product
application roots.

```text
vhub/
├── README.md
├── Go.vhub.json
└── FutureProduct.vhub.json
```

The reference product does not ship and therefore does not require a VHUB
template.

## Maintenance

Review the relevant product template before opening each pull request. Update it
only when firmware changes behavior that a tester can observe through the
device, hardware, serial logs, network interfaces, or server data.

`rev` is the 7 digit last commit head. Keep test IDs stable and update the template `rev` whenever its content changes.
