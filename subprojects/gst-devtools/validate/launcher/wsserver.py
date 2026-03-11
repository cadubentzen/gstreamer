#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2025 Thibault Saunier <tsaunier@igalia.com>
#
# WebSocket server for gst-validate WASM test IPC.
# Implements RFC 6455 WebSocket handshake and text frame parsing
# to receive JSON messages from WASM validate runners.

import hashlib
import base64
import json
import struct
import socketserver
from launcher import loggable  # noqa: F401
from launcher.loggable import Loggable

WS_MAGIC = b"258EAFA5-E914-47DA-95CF-665B1E30B16B"


class GstValidateWebSocketServer(socketserver.ThreadingMixIn,
                                 socketserver.TCPServer):
    allow_reuse_address = True


class GstValidateWebSocketHandler(socketserver.BaseRequestHandler, Loggable):

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        Loggable.__init__(self, "GstValidateWebSocketHandler")

    def _do_handshake(self):
        """Perform the WebSocket HTTP upgrade handshake (RFC 6455)."""
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.request.recv(4096)
            if not chunk:
                return False
            data += chunk

        headers = {}
        lines = data.decode("utf-8", "ignore").split("\r\n")
        for line in lines[1:]:
            if ": " in line:
                key, value = line.split(": ", 1)
                headers[key.lower()] = value

        ws_key = headers.get("sec-websocket-key")
        if not ws_key:
            return False

        accept = base64.b64encode(
            hashlib.sha1(ws_key.encode() + WS_MAGIC).digest()
        ).decode()

        response = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n"
            "\r\n"
        )
        self.request.sendall(response.encode())
        return True

    def _recv_exactly(self, n):
        """Receive exactly n bytes."""
        data = b""
        while len(data) < n:
            chunk = self.request.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def _read_frame(self):
        """Read a single WebSocket frame. Returns (opcode, payload) or None."""
        header = self._recv_exactly(2)
        if not header:
            return None

        opcode = header[0] & 0x0F
        masked = (header[1] & 0x80) != 0
        payload_len = header[1] & 0x7F

        if payload_len == 126:
            ext = self._recv_exactly(2)
            if not ext:
                return None
            payload_len = struct.unpack(">H", ext)[0]
        elif payload_len == 127:
            ext = self._recv_exactly(8)
            if not ext:
                return None
            payload_len = struct.unpack(">Q", ext)[0]

        mask_key = None
        if masked:
            mask_key = self._recv_exactly(4)
            if not mask_key:
                return None

        payload = self._recv_exactly(payload_len)
        if payload is None:
            return None

        if masked and mask_key:
            payload = bytes(b ^ mask_key[i % 4]
                            for i, b in enumerate(payload))

        return (opcode, payload)

    def _dispatch_message(self, msg, test):
        """Process a JSON message, same logic as GstValidateListener."""
        try:
            obj = json.loads(msg)
        except json.JSONDecodeError:
            return test

        if test is None:
            uuid = obj.get("uuid")
            if uuid is None:
                return None
            for t in self.server.launcher.tests:
                if uuid == t.get_uuid():
                    test = t
                    break
            return test

        obj_type = obj.get("type", "")
        if obj_type == "position":
            test.set_position(obj["position"], obj["duration"],
                              obj["speed"])
        elif obj_type == "buffering":
            test.set_position(obj["position"], 100)
        elif obj_type == "action":
            test.add_action_execution(obj)
            test.position += 1
        elif obj_type == "action-done":
            test.position += 1
            if test.actions_infos:
                test.actions_infos[-1]["execution-duration"] = \
                    obj["execution-duration"]
        elif obj_type == "report":
            test.add_report(obj)
        elif obj_type == "skip-test":
            test.set_result("skipped")

        return test

    def handle(self):
        """Implements BaseRequestHandler handle method."""
        self.logCategory = "GstValidateWebSocketHandler"

        if not self._do_handshake():
            return

        test = None
        while True:
            frame = self._read_frame()
            if frame is None:
                return

            opcode, payload = frame

            if opcode == 0x8:  # Close frame
                return
            elif opcode == 0x9:  # Ping
                # Send pong
                self.request.sendall(bytes([0x8A, 0x00]))
                continue
            elif opcode != 0x1:  # Not a text frame
                continue

            msg = payload.decode("utf-8", "ignore")
            if not msg:
                return

            test = self._dispatch_message(msg, test)
            if test is None and msg:
                # First message should have set the test
                try:
                    obj = json.loads(msg)
                    if "uuid" in obj and not obj.get("started"):
                        return
                except json.JSONDecodeError:
                    return
