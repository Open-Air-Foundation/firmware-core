---
name: update-vhub
description: Assess and update product VHUB manual release-verification cases for firmware changes, PRs, or commit ranges. Use for VHUB maintenance and coverage reviews, not automated firmware test implementation.
---

# Update VHUB

Read and apply [vhub/README.md](../../../vhub/README.md) before reviewing or
editing a template. It owns the QA scope, tooling rules, case format, authoring
criteria, revision convention, and validation requirements. This skill defines
the agent workflow for investigating changes and applying that guidance.

## Understand the Change

Establish the requested scope from the user's PR, commit range, local diff,
described behavior, or template review. A review-only request produces findings
without edits.

Inspect the actual diff, relevant implementation, product wiring, and local
documentation. Use the PR description as context, then establish which shipping
products and observable behaviors are affected. Group overlapping changes from
multiple commits or PRs before assessing their coverage.

Identify the before-and-after behavior and its evidence. If implementation and
requirements disagree, report the mismatch rather than turning a likely bug
into an expected result.

## Assess Coverage

Read the affected templates and inspect relevant host tests, device integration
tests, and script assertions. For each affected behavior, identify existing case
IDs across categories, what automation actually asserts, and any agreed future
coverage. Apply the guide to decide whether to update, add, merge, remove, defer,
or leave a scenario unchanged.

Resolve case-specific questions about behavior and available tools from the
product documentation and implementation. Ask for missing information only when
it affects the decision or procedure. Before editing, briefly state the affected
products, proposed case changes, and unresolved gaps.

## Update Cases

Apply the selected changes to the affected product templates using the guide.
Preserve unrelated work. Keep a record of coverage decisions and supporting
evidence for the final summary, including where removed or merged cases went.

## Validate

Run the guide's template and documentation checks. Review each changed procedure
as QA, then inspect the final diff against the coverage decisions for omissions
or changes outside the requested scope.

If the task also changes firmware, complete the repository's required firmware
verification separately. This maintenance workflow does not authorize flashing
devices or recording a release as passed.

## Report

Return findings for a review or a change summary for an update. Include affected
files and case IDs, the evidence behind coverage decisions, validation results,
and remaining gaps or deferred work. Explain when no template change is needed.
Report template validation separately from any tests actually executed.
