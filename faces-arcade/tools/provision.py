#!/usr/bin/env python3
"""Provision Wi-Fi and Tailscale over USB without echoing credentials."""
import argparse
import getpass
import sys
import warnings

import serial


def secret(prompt, maximum, allow_empty=False):
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            value = getpass.getpass(prompt).encode("utf-8")
    except getpass.GetPassWarning:
        raise RuntimeError("入力を隠せないため中止しました") from None
    if (not allow_empty and not value) or len(value) > maximum or any(c in value for c in b"\t\r\n"):
        raise ValueError("入力の長さか形式が不正です")
    return bytearray(value)


def command(port, payload, expected):
    port.reset_input_buffer()
    port.write(payload + b"\n")
    port.flush()
    while True:
        line = port.readline().strip()
        if line == expected:
            return
        if line in ("保存できません".encode(), "入力が長すぎます".encode()):
            raise RuntimeError("本体に保存できませんでした")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("operation", choices=("store", "clear"))
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    if not sys.stdin.isatty() or not sys.stderr.isatty():
        raise RuntimeError("隠して入力するため、対話端末で実行してください")
    port = serial.Serial(port=None, baudrate=460800, timeout=None)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    secrets = []
    try:
        if args.operation == "clear":
            command(port, b"X", "消去した".encode())
            print("通信設定を消去した")
            return
        ssid = secret("Wi-Fi SSID: ", 32)
        secrets.append(ssid)
        password = secret("Wi-Fi パスワード: ", 64, allow_empty=True)
        secrets.append(password)
        key = secret("Tailscale 認証キー: ", 95)
        secrets.append(key)
        wifi_command = bytearray(b"W") + ssid + b"\t" + password
        key_command = bytearray(b"K") + key
        secrets.extend((wifi_command, key_command))
        command(port, wifi_command, "保存した".encode())
        command(port, key_command, "保存した".encode())
        print("保存した")
    finally:
        for value in secrets:
            value[:] = b"\0" * len(value)
        port.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, serial.SerialException) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
