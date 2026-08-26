#!/usr/bin/env python3
"""Send one AirGradient Go manufacturing command over USB Serial/JTAG.

The Go serial command protocol shares its transport with normal firmware logs.
This client ignores ordinary log lines and prints the first ``#AG`` response.
The device must be un-onboarded or already in manufacturing mode.

Requirements:
    pip install pyserial

Usage:
    # Confirm the connection and read the board serial.
    python scripts/ago_serial_command.py /dev/ttyACM0 GET_SERIAL

    # Read or set a correction.
    python scripts/ago_serial_command.py /dev/ttyACM0 GET_SLR TEMP
    python scripts/ago_serial_command.py /dev/ttyACM0 SET_SLR TEMP 1.1 -0.3

    # Show interleaved logs while waiting for a response.
    python scripts/ago_serial_command.py --show-logs /dev/ttyACM0 HELP
"""

from __future__ import annotations

import argparse
import sys
import time
from typing import Sequence

PROTOCOL_PREFIX = "#AG "
LINE_FEED = b"\n"
DEFAULT_BAUD_RATE = 115200
DEFAULT_TIMEOUT_SECONDS = 5.0
READ_TIMEOUT_SECONDS = 0.1
DEVICE_ERROR_EXIT_CODE = 2


def _build_request(command_tokens: Sequence[str]) -> bytes:
    """Validate CLI tokens and return one newline-delimited protocol request."""
    tokens = list(command_tokens)
    if tokens and tokens[0] == "#AG":
        tokens = tokens[1:]
    if not tokens:
        raise ValueError("a command is required")
    if any(not token or any(char.isspace() for char in token) for token in tokens):
        raise ValueError("each command field must be one non-whitespace argument")

    try:
        return (PROTOCOL_PREFIX + " ".join(tokens)).encode("ascii") + LINE_FEED
    except UnicodeEncodeError as exc:
        raise ValueError("command fields must use ASCII protocol tokens") from exc


def _read_response(serial_port: object, timeout_seconds: float, show_logs: bool) -> str:
    """Return the first complete protocol response, ignoring ordinary logs."""
    deadline = time.monotonic() + timeout_seconds
    pending = bytearray()

    while time.monotonic() < deadline:
        chunk = serial_port.read(256)
        if not chunk:
            continue
        pending.extend(chunk)

        while True:
            line_end = pending.find(LINE_FEED)
            if line_end < 0:
                break
            raw_line = bytes(pending[:line_end])
            del pending[: line_end + 1]
            line = raw_line.rstrip(b"\r").decode("utf-8", errors="replace")
            if line.startswith(PROTOCOL_PREFIX):
                return line
            if show_logs:
                print(line, file=sys.stderr)

    raise TimeoutError(f"no #AG response within {timeout_seconds:g} seconds")


def _run(args: argparse.Namespace) -> int:
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError("pyserial is required; install it with: pip install pyserial") from exc

    request = _build_request(args.command)
    try:
        with serial.Serial(
            port=args.port,
            baudrate=args.baud_rate,
            timeout=READ_TIMEOUT_SECONDS,
            write_timeout=args.timeout,
        ) as serial_port:
            serial_port.reset_input_buffer()
            serial_port.write(request)
            serial_port.flush()
            response = _read_response(serial_port, args.timeout, args.show_logs)
    except serial.SerialException as exc:
        raise RuntimeError(f"serial transport failed: {exc}") from exc

    print(response)
    return DEVICE_ERROR_EXIT_CODE if response.startswith("#AG ERROR ") else 0


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Send one #AG manufacturing command to an AirGradient Go USB "
            "Serial/JTAG device and print its response."
        ),
    )
    parser.add_argument("port", help="USB Serial/JTAG device path, for example /dev/ttyACM0.")
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="Protocol command and arguments, for example: SET_SLR TEMP 1.1 -0.3.",
    )
    parser.add_argument(
        "--baud-rate",
        type=int,
        default=DEFAULT_BAUD_RATE,
        help="Host serial baud rate required by pyserial (default: 115200; USB ignores it).",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="Seconds to wait for one #AG response (default: 5).",
    )
    parser.add_argument(
        "--show-logs",
        action="store_true",
        help="Write interleaved non-protocol USB logs to stderr while waiting.",
    )
    args = parser.parse_args()

    if args.baud_rate <= 0:
        parser.error("--baud-rate must be positive")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    try:
        exit_code = _run(args)
    except (RuntimeError, TimeoutError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
