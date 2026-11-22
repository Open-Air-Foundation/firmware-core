#!/usr/bin/env python3

"""Tests for the CI build selector."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

from select_builds import BuildSelection, classify_paths, read_changed_paths


PRODUCTS = ("go", "reference")


class ClassifyPathsTest(unittest.TestCase):
    def assert_selection(
        self,
        paths: list[str],
        products: tuple[str, ...],
        host_tests_required: bool,
    ) -> None:
        self.assertEqual(
            classify_paths(paths, PRODUCTS),
            BuildSelection(
                products=products,
                host_tests_required=host_tests_required,
            ),
        )

    def test_documentation_only_selects_no_builds(self) -> None:
        self.assert_selection(
            ["README.md", "docs/GUIDE.md", "products/go/docs/ble.md"],
            (),
            False,
        )

    def test_product_source_selects_only_that_product(self) -> None:
        self.assert_selection(
            ["products/go/main/main.cpp"],
            ("go",),
            True,
        )

    def test_embedded_product_asset_selects_product(self) -> None:
        self.assert_selection(
            ["products/reference/main/web/index.html"],
            ("reference",),
            True,
        )

    def test_embedded_markdown_selects_product(self) -> None:
        self.assert_selection(
            ["products/go/main/help.md"],
            ("go",),
            True,
        )

    def test_component_source_under_docs_directory_selects_all(self) -> None:
        self.assert_selection(
            ["components/airgradient-common/docs/generated.cpp"],
            PRODUCTS,
            True,
        )

    def test_markdown_under_unknown_path_fails_safe(self) -> None:
        self.assert_selection(
            ["new-build-system/design.md"],
            PRODUCTS,
            True,
        )

    def test_shared_component_selects_every_product(self) -> None:
        self.assert_selection(
            ["components/airgradient-common/include/common.h"],
            PRODUCTS,
            True,
        )

    def test_test_only_change_selects_host_tests(self) -> None:
        self.assert_selection(
            ["components/airgradient-ota/tests/test_ota.cpp"],
            (),
            True,
        )

    def test_unknown_path_fails_safe(self) -> None:
        self.assert_selection(
            ["new-build-system/config.yaml"],
            PRODUCTS,
            True,
        )

    def test_unknown_product_fails_safe(self) -> None:
        self.assert_selection(
            ["products/unknown/main/main.cpp"],
            PRODUCTS,
            True,
        )

    def test_new_discovered_product_is_selected(self) -> None:
        selection = classify_paths(
            ["products/outdoor/main/main.cpp"],
            ("go", "outdoor", "reference"),
        )
        self.assertEqual(
            selection,
            BuildSelection(products=("outdoor",), host_tests_required=True),
        )

    def test_host_workflow_change_does_not_select_firmware(self) -> None:
        self.assert_selection(
            [".github/workflows/host-tests.yml"],
            (),
            True,
        )

    def test_selector_change_selects_all_validation(self) -> None:
        self.assert_selection(
            [".github/scripts/select_builds.py"],
            PRODUCTS,
            True,
        )

    def test_selector_test_change_selects_only_host_tests(self) -> None:
        self.assert_selection(
            [".github/scripts/test_select_builds.py"],
            (),
            True,
        )


class ReadChangedPathsTest(unittest.TestCase):
    def run_git(self, repository: Path, *args: str) -> str:
        result = subprocess.run(
            ["git", *args],
            cwd=repository,
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        )
        return result.stdout.strip()

    def test_merge_base_excludes_changes_made_only_on_base_branch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            self.run_git(repository, "init", "--quiet")
            self.run_git(repository, "config", "user.email", "ci@example.com")
            self.run_git(repository, "config", "user.name", "CI Test")

            product_file = repository / "products/go/main/main.cpp"
            component_file = repository / "components/common/common.cpp"
            product_file.parent.mkdir(parents=True)
            component_file.parent.mkdir(parents=True)
            product_file.write_text("root\n", encoding="utf-8")
            component_file.write_text("root\n", encoding="utf-8")
            self.run_git(repository, "add", ".")
            self.run_git(repository, "commit", "--quiet", "-m", "root")
            root_branch = self.run_git(repository, "branch", "--show-current")

            self.run_git(repository, "checkout", "--quiet", "-b", "feature")
            product_file.write_text("feature\n", encoding="utf-8")
            self.run_git(repository, "commit", "--quiet", "-am", "feature")
            feature_head = self.run_git(repository, "rev-parse", "HEAD")

            self.run_git(repository, "checkout", "--quiet", root_branch)
            component_file.write_text("base\n", encoding="utf-8")
            self.run_git(repository, "commit", "--quiet", "-am", "base")
            base_head = self.run_git(repository, "rev-parse", "HEAD")

            self.assertEqual(
                read_changed_paths(
                    repository,
                    base_head,
                    feature_head,
                    use_merge_base=True,
                ),
                ["products/go/main/main.cpp"],
            )


if __name__ == "__main__":
    unittest.main()
