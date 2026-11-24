# Documentation Guide

This guide explains how to create, organize, and maintain first-party
documentation in this repository, including writing style and validation.

Before creating or updating documentation, choose its purpose and authoritative
home using this guide. Use the [templates](templates) when applicable, adapting
them to the topic rather than filling sections that add no useful information.

## Purpose and Audience

Documentation should help a reader understand the firmware, make a change
safely, integrate with a device, or verify its behavior. Write for someone who
knows embedded development but has not followed the implementation discussion.

Start each document with its subject and purpose. Decide which reader it serves:

- New contributors need orientation, build instructions, and a reading path.
- Firmware maintainers need responsibilities, runtime interactions, constraints,
  and the reasons behind decisions.
- Client developers need protocol contracts, payloads, compatibility, and errors.
- Testers and operators need prerequisites, actions, and observable results.

A document should answer a concrete question. Add detail when it helps the
reader perform the task or avoid a mistake; length and API coverage are not
measures of completeness.

## Scope

These rules apply to:

- the root `README.md`
- contributor guides and development workflows
- `components/<name>/README.md` for first-party components
- `products/<name>/README.md` and `products/<name>/ARCHITECTURE.md`
- `products/<name>/docs/*.md` (service / module docs)
- `components/<name>/docs/*.md` when a component needs detailed topic docs
- `products/<name>/specs/*.md` (design specs)
- `tests/README.md` and other test-runner docs
- shared references under `docs/` and product integration references
- manual verification documentation under `vhub/`; its JSON templates follow
  the additional rules in [`vhub/README.md`](../vhub/README.md)

These rules do **not** apply to:

- third-party / vendor component docs (e.g. `components/esp-nimble-cpp/`,
  `components/embedded-i2c-scd4x/`, `components/libnmea-esp32/`); leave
  those untouched
- auto-generated content (e.g. `.pytest_cache/README.md`)

`AGENTS.md` and other agent-instruction files are not required to use the
document templates; keep their documentation pointers aligned with this guide.

## Structure and Ownership

Choose the narrowest location that owns the subject. Shared capabilities belong
with shared components; product wiring and policy belong with the product.

| Document | Question It Answers | Content It Owns |
|---|---|---|
| Root `README.md` | What is this repository, and where do I start? | Repository map, key architectural boundaries, build/test entrypoints, links to workflows and deeper docs |
| `components/README.md`, `products/README.md` | Which component or product should I look at? | Responsibility summaries and links |
| `components/<name>/README.md` | How do I use and maintain this shared capability? | Scope, interfaces, constraints, dependencies, minimal usage, tests |
| `products/<name>/README.md` | How do I get started with this product? | Product overview, sensors, essential setup notes, product-specific build requirements when needed, reading path |
| `products/<name>/ARCHITECTURE.md` | How does this product work as a system? | Boot composition, ownership, tasks/events, major state transitions, end-to-end data flow |
| Component or product `docs/<topic>.md` | How does this behavior or subsystem work? | Detailed lifecycle, contracts, timing, concurrency, failure handling, design rationale |
| Shared or product integration reference | How does an external client use this interface? | Protocol, payload schema, units, validation, compatibility, errors |
| `tests/README.md`, test-local docs, `vhub/README.md` | How do I verify a change? | Test setup, commands, coverage boundaries, manual verification workflow |
| `products/<name>/specs/<topic>.md` | What change are we proposing? | Problem, intended design, implementation and verification plan, unresolved decisions |

### One Authoritative Home per Topic

Keep the full explanation in one document. Other documents may give a short
summary and link to it. For example, Go's README can mention that PM sensing
depends on the power rail, while the power-management document owns detailed
PMID sequencing and its rationale.

Architecture documents explain how services cooperate. Service documents own
their internal behavior. Protocol references own wire contracts; service docs
link to those contracts and explain how the implementation satisfies them.
Preserve the distinctions between product policy and reusable driver behavior.

Before adding a document, search for an existing owner. Extend that document
unless the new topic has a distinct audience or substantial independent content.
There is no requirement for one document per class, source file, or service.
Split an oversized document by reader task or responsibility, then replace
moved content with links.

### Reading Path

Keep the root README as the repository entry point. Link to the relevant
product or component README, then to architecture and detailed topic documents.
Add new evergreen documents to the nearest useful index so readers can discover
them. Specifications are temporary implementation plans and do not need incoming
links from READMEs, indexes, or other documents. Link directly to a relevant
section when it helps the reader continue a task.

## Writing Useful Content

Explain the behavior and reasoning that are difficult to recover from individual
source files. Depending on the topic, cover:

- Who creates, owns, calls, and stops an object or service.
- Which task or callback executes work, how events move, and where state changes.
- Initialization and shutdown order, timing requirements, and retained state.
- Data validity, units, partial sensor failures, and unavailable peripherals.
- Error outcomes, retries, recovery, and what users or callers can observe.
- Hardware constraints and why a design decision is necessary.
- Where relevant tests and production entrypoints can be found.

Use a short end-to-end example or diagram when interactions are the subject.
For an architecture overview, trace a measurement from acquisition through
events to its consumers; leave per-service algorithms in their owning docs.

Link to source for full signatures, member lists, and implementation details.
Document an API's contract when callers need to know side effects, ownership,
blocking behavior, or failure semantics. Do not reproduce every method merely
because it is public. Keep local algorithm explanations near the code and
cross-module reasoning in the relevant documentation.

Write current behavior precisely. Distinguish supported behavior, limitations,
and proposals. Verify claims against the implementation and tests; do not
present an unimplemented spec as existing behavior. If code contradicts an
intended requirement, investigate the discrepancy rather than silently treating
a bug as the new contract.

## Maintenance Workflow

1. Identify the reader, question, and existing authoritative document.
2. Read the relevant implementation and tests before describing behavior.
3. Update the owning document with the implementation change, after the design
   has settled and before final verification.
4. Check summaries, diagrams, examples, and links in related documents. Replace
   duplicate explanations with links while preserving useful rationale.
5. If manually observable behavior changes, review the relevant VHUB template
   using [`vhub/README.md`](../vhub/README.md). Internal-only and editorial
   changes do not need new manual verification cases.
6. Validate changed documentation using the [checklist](#contributor-checklist).

When moving or renaming a file, update incoming links, contributor instructions,
templates, and tooling references in the same change. Before removing a
document, preserve still-useful contracts and rationale in the authoritative
home. Git history retains implementation history; evergreen docs should explain
the current system rather than accumulate chronological change notes.

Apply this guide to new and materially revised content. Existing documentation
can be consolidated in focused follow-ups; an unrelated code change does not
require a repository-wide documentation rewrite.

## Doc Lifecycle

The repository keeps two kinds of Markdown files. They have different
intents and different lifecycles.

| Kind | Path | Tense | Audience | Lifetime |
|---|---|---|---|---|
| Doc | `README.md`, `components/<name>/README.md`, `products/<name>/README.md`, `products/<name>/ARCHITECTURE.md`, `products/<name>/docs/*.md`, `tests/README.md` | Present — describes what currently exists | Anyone reading the codebase | Evergreen — kept in sync with the code |
| Spec | `products/<name>/specs/*.md` | Future / imperative — describes how a feature will be built | The implementer | Temporary — usually deleted once shipped |

Rules that follow from this:

- A doc describes the **implemented behavior at the current revision**.
  Identify version-specific behavior explicitly in release or integration docs.
  Investigate disagreements with the code and correct the implementation or
  documentation as appropriate.
- A spec must capture **intent before implementation**. Once the feature
  ships, update the corresponding doc to describe what was actually built.
- After a spec is fully implemented and the corresponding doc reflects
  reality, the spec is **typically deleted**. Deletion is manual and at the
  author's discretion — there is no archive folder. If retained, label its
  status and link to the current documentation so it is not mistaken for a
  current behavior reference. Preserve useful design rationale in that reference.
- When implementing a spec, prefer focused commits that update the spec,
  the code, and the corresponding doc together so they don't drift.

## Document Outlines and Templates

Use the matching template under [`docs/templates/`](templates) as a starting
point. Keep the order below for sections that apply, omit irrelevant sections,
and add topic-specific sections when readers need them. Remove placeholder
text and adjust example paths for the destination. Do not invent APIs,
configuration, or directories to fill a template.

### Component README (`components/<name>/README.md`)

1. Title matching the component directory name and one-line summary
2. `## Status` — `Stable`, `Experimental`, or `Scaffold`
3. `## Scope` — what the component owns / does not own
4. `## Directory Layout` — `text` tree plus per-directory bullets
5. `## Public Includes` — include-by-role examples
6. `## Design` — responsibilities, interactions, and rationale; diagram if useful
7. `## Usage` — minimal `cpp` example
8. `## Configuration` — Kconfig table (`Symbol` / `Default` / `Purpose`),
   omit the section if the component has none
9. `## Dependencies` — bullet list of other components / managed components
10. `## Tests` — link to `tests/README.md` and component-local test path
11. `## Notes` — optional caveats, gotchas, follow-up work

### Product README (`products/<name>/README.md`)

1. Title and one-line product description
2. `## Sensors` — bullet list with sensor model and quantity
3. `## Hardware Notes` — essential setup constraints and links to detailed
   hardware and power behavior
4. `## Build` — optional; include only for product-specific requirements not
   covered by the root README. Omit when the shared instructions are sufficient.
5. `## Documentation` — links to `ARCHITECTURE.md`, `docs/`, board config

### Architecture Doc (`products/<name>/ARCHITECTURE.md`)

Keep this focused on the system rather than a catalog of service internals.
Use the following outline; a separate template is not required:

1. Title and short explanation of the product's architecture
2. `## System Structure` — major responsibilities and hardware boundaries
3. `## Boot and Lifecycle` — composition, startup, shutdown, and wake paths
4. `## Runtime Interactions` — tasks, events, state ownership, and coordination
5. `## Data Flow` — representative end-to-end paths and their consumers
6. `## Constraints and Decisions` — system-wide constraints and rationale
7. `## Detailed Documentation` — links to authoritative topic docs

### Service or Topic Doc (`docs/<topic>.md` Within a Product or Component)

1. Title and one-paragraph summary
2. `## Files` — table (`File` / `Purpose`)
3. `## Dependencies` — table (`Dependency` / `Source` / `Usage`)
4. `## Public API` — important caller contracts and links to headers
5. `## Behavior` — state machines, lifecycle, timing
6. `## Edge Cases / Errors`
7. `## Verification` — links to tests and important coverage boundaries

For topics spanning several services, organize behavior by the flow or contract
being explained. A method table is unnecessary when there is no useful public
API to describe. Link to relevant tests or verification instructions.

### Other Documentation

Root and directory READMEs, integration references, test instructions, and
contributor guides use headings appropriate to their purpose. They follow the
same ownership, lifecycle, and style rules without having to fit a service
template. For protocol references, cover discovery, operations, payloads,
units, errors, and compatibility as applicable. For procedures, state
prerequisites, commands or actions, expected results, and recovery steps.

### Spec Doc (`products/<name>/specs/<topic>.md`)

1. Title and one-paragraph problem statement
2. `## Problem`
3. `## Goals`
4. `## Non-Goals`
5. `## Design`
6. `## Implementation Plan`
7. `## Testing Strategy`
8. `## Open Questions`

## Brand and Casing

- Always **AirGradient** (never "Airgradient", "airgradient" outside of
  identifiers, "AIRGRADIENT", etc.)
- First-party component H1: the directory name exactly, usually
  **`# airgradient-<name>`**. No `Component` suffix in the H1.
- Section headings: **Title Case** (`Directory Layout`, not
  `Directory layout`)
- File names for new topic docs: **`snake_case.md`**. Keep established entrypoint
  names such as `README.md`, `ARCHITECTURE.md`, and this `GUIDE.md`.

## Required Heading Layout

Every Markdown file:

- has exactly **one** H1 at the top
- uses **ATX-style** headings (`#`, `##`, `###`) — no underline (`====`)
  headings
- never skips a heading level (no `#` then `###`)
- uses sentence-style prose under each heading; do not nest the H1's title
  inside the body

## Code Fences

Always include a language hint:

| Content | Fence Language |
|---|---|
| Shell commands | ```` ```sh ```` |
| C / C++ code | ```` ```cpp ```` |
| Python code | ```` ```python ```` |
| ASCII trees, register maps, plain output | ```` ```text ```` |
| Mermaid diagrams | ```` ```mermaid ```` |
| JSON, YAML, TOML, etc. | matching language hint |

Do not use ```` ```bash ```` — use ```` ```sh ```` for portability.

Do not use untagged fences in first-party docs.

## Code in Documentation

Docs describe what the code does; they are not a place to **copy** the code.
Readers should read the source for full implementations.

Rules for non-spec docs (READMEs, service docs, ARCHITECTURE):

- **Soft cap of ≤ 15 lines per code block.** Longer examples need a clear
  reader benefit; prefer a source link when the implementation already serves
  as the example.
- For longer examples, replace the block with a short preview plus a link
  to the source file. Use a relative path:

  For example, see [`go_ble.cpp`](../products/go/main/go_ble.cpp) for the BLE
  implementation. Adjust the relative path for the document's location.

- Prefer **method or function signatures** over working call examples when
  the goal is to enumerate the API.
- Prefer **a 5–10 line minimal call site** plus a pointer to a real example
  when the goal is to show how something is wired up.
- Never paste an entire `init()` or `start()` body into a doc — link to it.

Specs are **exempt** from this cap. Specs guide implementation and may
include longer code sketches, interface drafts, and pseudo-code.

## Diagrams

Use the right tool for the shape of the information. Both ASCII and
Mermaid render on GitHub; Mermaid produces clearer output for non-trivial
flows and is preferred whenever the content has state, branching, or
multiple actors.

| Diagram Type | Use | Fence |
|---|---|---|
| State machine | Mermaid `stateDiagram-v2` | ```` ```mermaid ```` |
| Multi-actor sequence (more than 3 steps) | Mermaid `sequenceDiagram` | ```` ```mermaid ```` |
| Branched / decision flow (any branch, or more than 4 nodes) | Mermaid `flowchart TD` | ```` ```mermaid ```` |
| Class / module relationship | Mermaid `classDiagram` | ```` ```mermaid ```` |
| Linear flow with no branches, ≤ 4 steps | ASCII (`A -> B -> C`) | ```` ```text ```` |
| Directory tree | ASCII | ```` ```text ```` |
| Register address map | ASCII or table | ```` ```text ```` or table |

Notes:

- GitHub renders Mermaid natively; no extra tooling or build step is
  required.
- Keep diagram source readable in the raw file — break long Mermaid blocks
  with comments or whitespace.
- If a diagram needs more than ~30 nodes, the doc is too dense. Split it.

### Mermaid Gotchas

The Mermaid parser interprets a few characters as syntax inside `Note over`
text and some other contexts. Avoid them in message / note text or rephrase:

| Avoid in Note / Message Text | Why | Use Instead |
|---|---|---|
| `;` | Statement separator inside `Note over` | em-dash `—` or comma |
| `\|` | Pipe is reserved in some contexts (e.g. flowchart edge labels) | `+`, `or`, `and` |
| Trailing `()` on bare names (`foo()`) | Usually fine, but render quirks vary | drop empty parens (`foo`) |

When in doubt, render the diagram once with `mermaid-cli`:

```sh
npx --yes -p @mermaid-js/mermaid-cli@10.9.1 mmdc -i diagram.mmd -o diagram.svg
```

## Subdirectory READMEs

Avoid `README.md` files inside component subdirectories (`hal/`, `drivers/`,
`backends/`, `tests/`, etc.) unless they carry **substantial unique content**
beyond what the parent component README already says.

If a subdirectory genuinely needs its own README, follow the same heading
discipline as a component README in miniature: a single H1, Title Case
headings, language-tagged fences.

## Tables

- Use Title Case for header cells
- Keep tables compact; if a table grows beyond ~10 rows of dense prose,
  consider switching to a bulleted list or splitting it
- Surround tables with blank lines

## Cross-References

- Use **relative** links between repo docs (`../components/airgradient-ble/README.md`),
  not absolute paths or full GitHub URLs
- Link to anchors with kebab-case slugs (`#directory-layout`)
- Verify links still resolve when files move; broken cross-refs are bugs

## Configuration Tables

For Kconfig, settings fields, and similar enumerations, use:

| Symbol | Default | Purpose |
|---|---|---|
| `CONFIG_FOO_BAR_MS` | `1000` | One-line intent |

Never document configuration purely in prose.

## Lint Enforcement

The repository runs `markdownlint-cli2` via `pre-commit` on staged Markdown,
subject to vendor exclusions. Configuration lives in `.markdownlint.json`,
`.markdownlint-cli2.jsonc`, and `.pre-commit-config.yaml`. Lint checks formatting;
review must also check ownership, accuracy, usefulness, and links.

From the repository root, run the hooks on changed documentation, for example:

```sh
pre-commit run --files docs/GUIDE.md README.md
```

To lint all tracked Markdown:

```sh
pre-commit run markdownlint-cli2 --all-files
```

## Contributor Checklist

Before opening a PR that touches docs:

- [ ] Reader and purpose are clear; evergreen documents are linked from a useful
      index (specifications do not require incoming links)
- [ ] Each topic has one authoritative home; other docs summarize and link
- [ ] Behavior and examples agree with the current code and relevant tests
- [ ] Important constraints, errors, and design rationale are preserved
- [ ] Single H1, ATX style, Title Case section headings
- [ ] Every code fence has a language hint
- [ ] Applicable template sections are used; irrelevant sections and placeholders
      are removed, and paths are adjusted for the destination
- [ ] No `Airgradient` typos (always `AirGradient`)
- [ ] Cross-links resolve
- [ ] Code blocks in non-spec docs are short; longer examples have a clear
      reader benefit or are replaced with a source link
- [ ] State machines, sequences, and branched flows use Mermaid; ASCII is
      reserved for ≤ 4-step linear flows and directory trees
- [ ] Doc tense matches its kind: docs describe what _exists_ (present),
      specs describe what _will be built_ (future)
- [ ] If this PR implements a spec, current docs are updated and the spec is
      deleted or clearly labeled with its status and a link to those docs
- [ ] Manually observable changes are reflected in the relevant VHUB template,
      or the change is confirmed to have no manual QA impact
- [ ] Pre-commit checks pass for changed documentation
