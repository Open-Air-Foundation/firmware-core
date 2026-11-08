#!/usr/bin/env python3
"""Read or provision the Go board ID using the active ESP-IDF Python environment."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


EFUSE_TABLE = (
    Path(__file__).resolve().parents[1]
    / "products/go/main/efuse/esp_efuse_custom_table.csv"
)
BOARD_FIELD = "USER_DATA_GO_BOARD_ID"
USER_BLOCK = "BLOCK_USR_DATA"
BOARD_IDS = {"v1": 1, "v2": 2}


def read_summary(command):
    with tempfile.TemporaryDirectory(prefix="go-board-") as directory:
        output = Path(directory) / "summary.json"
        subprocess.run(
            command
            + ["summary", "--format", "json", "--file", str(output), BOARD_FIELD, USER_BLOCK],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        )
        summary = json.loads(output.read_text())
    if not summary[BOARD_FIELD]["readable"] or not summary[USER_BLOCK]["readable"]:
        raise RuntimeError("Cannot read the Go board ID and user-data block")
    return summary


def print_board(summary):
    board_id = summary[BOARD_FIELD]["value"]
    if board_id == 0:
        description = "V1 (unprogrammed)"
    elif board_id in BOARD_IDS.values():
        description = f"V{board_id} (programmed)"
    else:
        description = "V1 (unknown-ID fallback)"
    print(f"Go board: {description}; eFuse ID: 0x{board_id:02x}", flush=True)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Setting an ID is permanent. ESP32-C5 user-data blocks can only be programmed once.",
    )
    transport = parser.add_mutually_exclusive_group(required=True)
    transport.add_argument("--port", "-p", help="ESP32-C5 serial port")
    transport.add_argument(
        "--virtual-file", type=Path,
        help="Simulate eFuses in this file; no hardware access",
    )
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("check", help="Read the ID and show the firmware's board selection")
    setter = actions.add_parser("set", help="Program an unprogrammed user-data block")
    setter.add_argument("board", choices=BOARD_IDS)
    args = parser.parse_args()

    command = [sys.executable, "-m", "espefuse", "--chip", "esp32c5"]
    if args.virtual_file is not None:
        command += ["--virt", "--path-efuse-file", str(args.virtual_file)]
    else:
        command += ["--port", args.port]
    command += ["--extend-efuse-table", str(EFUSE_TABLE)]

    summary = read_summary(command)
    print_board(summary)
    if args.action == "check":
        return

    desired = BOARD_IDS[args.board]
    if summary[BOARD_FIELD]["value"] == desired:
        print("Already set; no eFuses changed.")
        return
    # RS coding covers the whole block, including the optional custom MAC.
    if any(bytes.fromhex(summary[USER_BLOCK]["value"])):
        raise RuntimeError(
            "User-data block is already programmed; cannot set or change the board ID"
        )
    if not summary[BOARD_FIELD]["writeable"] or not summary[USER_BLOCK]["writeable"]:
        raise RuntimeError("User-data block is write-protected")

    # Retain espefuse's interactive BURN confirmation; never use --force.
    subprocess.run(command + ["burn_efuse", BOARD_FIELD, str(desired)], check=True)
    summary = read_summary(command)
    if summary[BOARD_FIELD]["value"] != desired:
        raise RuntimeError("Board ID readback does not match the requested value")
    print_board(summary)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout, file=sys.stderr, end="")
        print(
            "eFuse operation failed; use the ESP-IDF Python environment "
            "and check the device connection.",
            file=sys.stderr,
        )
        sys.exit(error.returncode)
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
