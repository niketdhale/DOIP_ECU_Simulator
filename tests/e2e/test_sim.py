#!/usr/bin/env python3
"""End-to-end tests: launch the real simulator binary and talk DoIP to it.

Unlike tests/integration (which link the library in-process), these cover
main.c: command-line flags, start-up, and clean shutdown. Standard library
only.

    make test-e2e                              # builds ./test-sim and runs this
    DOIP_SIM=./doip_ecu_sim python3 tests/e2e/test_sim.py
"""
import os
import re
import signal
import socket
import struct
import subprocess
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BINARY = os.environ.get("DOIP_SIM", os.path.join(ROOT, "test-sim"))
PORT = int(os.environ.get("DOIP_E2E_PORT", "24400"))

VIN = b"E2EVIN00000000001"
ECU, FUNCTIONAL, TESTER = 0x2002, 0xE4F0, 0x0E00

# Seed/key algorithm constant, read from the config so the two cannot drift apart
with open(os.path.join(ROOT, "config", "doip_uds_config.h")) as _f:
    KEY_MASK = int(re.search(r"UDS_SECURITY_KEY_MASK\s+(0x[0-9A-Fa-f]+)", _f.read()).group(1), 16)

PT_VEHICLE_ID_REQ, PT_VEHICLE_ANNOUNCE = 0x0001, 0x0004
PT_ROUTING_REQ, PT_ROUTING_RES = 0x0005, 0x0006
PT_DIAG, PT_DIAG_ACK, PT_DIAG_NACK = 0x8001, 0x8002, 0x8003


def frame(version, ptype, payload=b""):
    return struct.pack("!BBHI", version, version ^ 0xFF, ptype, len(payload)) + payload


class DiagNack(Exception):
    def __init__(self, code):
        super().__init__(f"diagnostic NACK 0x{code:02X}")
        self.code = code


class Tester:
    """Minimal DoIP tester on one TCP connection."""

    def __init__(self, version, address=TESTER):
        self.version, self.address = version, address
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=2)

    def close(self):
        self.sock.close()

    def _recv_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("simulator closed the connection")
            data += chunk
        return data

    def recv_frame(self):
        _ver, _inv, ptype, length = struct.unpack("!BBHI", self._recv_exact(8))
        return ptype, self._recv_exact(length)

    def activate(self):
        self.sock.sendall(frame(self.version, PT_ROUTING_REQ, struct.pack("!HBI", self.address, 0, 0)))
        ptype, payload = self.recv_frame()
        assert ptype == PT_ROUTING_RES, hex(ptype)
        return payload[4]  # response code

    def send_uds(self, uds, target=ECU):
        self.sock.sendall(frame(self.version, PT_DIAG, struct.pack("!HH", self.address, target) + uds))

    def recv_uds(self, timeout=2.0, keep_pending=False):
        """Next UDS response as (source_address, bytes), or None on timeout.
        DoIP ACKs are skipped; 7F xx 78 is skipped unless keep_pending."""
        self.sock.settimeout(timeout)
        try:
            while True:
                ptype, payload = self.recv_frame()
                if ptype == PT_DIAG_ACK:
                    continue
                if ptype == PT_DIAG_NACK:
                    raise DiagNack(payload[4])
                assert ptype == PT_DIAG, hex(ptype)
                source, uds = struct.unpack("!H", payload[:2])[0], payload[4:]
                if not keep_pending and len(uds) == 3 and uds[0] == 0x7F and uds[2] == 0x78:
                    continue
                return source, uds
        except socket.timeout:
            return None

    def uds(self, request, target=ECU, timeout=2.0):
        """Send a request and return the UDS response bytes (None on timeout)."""
        self.send_uds(bytes(request), target)
        answer = self.recv_uds(timeout)
        return None if answer is None else answer[1]

    def unlock(self):
        assert self.uds([0x10, 0x03])[0] == 0x50
        seed = self.uds([0x27, 0x01])
        assert seed[:2] == b"\x67\x01", seed.hex()
        key = struct.unpack("!I", seed[2:6])[0] ^ KEY_MASK
        assert self.uds(b"\x27\x02" + struct.pack("!I", key)) == b"\x67\x02"


class SimTestCase(unittest.TestCase):
    EXTRA_ARGS = []

    def setUp(self):
        self.sim = subprocess.Popen(
            [BINARY, "--vin", VIN.decode(), "--address", hex(ECU),
             "--functional-address", hex(FUNCTIONAL), "--port", str(PORT),
             "--log-level", "1", *self.EXTRA_ARGS],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.addCleanup(self._stop_sim)
        self._testers = []
        deadline = time.monotonic() + 5
        while True:  # wait until the TCP port accepts connections
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                break
            except OSError:
                if self.sim.poll() is not None or time.monotonic() > deadline:
                    self.fail("simulator did not start: " + self.sim.stderr.read().decode(errors="replace"))
                time.sleep(0.02)
        self.version = self.vehicle_id()[0]

    def _stop_sim(self):
        for t in self._testers:
            t.close()
        if self.sim.poll() is None:
            self.sim.send_signal(signal.SIGTERM)
        try:
            code = self.sim.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.sim.kill()
            self.fail("simulator did not exit on SIGTERM")
        stderr = self.sim.stderr.read().decode(errors="replace")
        self.sim.stderr.close()
        # Also catches sanitizer / leak reports when the binary is instrumented
        self.assertEqual(code, 0, "simulator exit code; stderr:\n" + stderr)

    def connect(self, address=TESTER, activate=True):
        t = Tester(self.version, address)
        self._testers.append(t)
        if activate:
            self.assertEqual(t.activate(), 0x10)
        return t

    def vehicle_id(self):
        """Vehicle identification over UDP → (protocol_version, payload_type, payload)."""
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(2)
            # 0xFF is the default version a tester uses before it knows the entity's
            udp.sendto(frame(0xFF, PT_VEHICLE_ID_REQ), ("127.0.0.1", PORT))
            data, _ = udp.recvfrom(256)
        version, _inv, ptype, length = struct.unpack("!BBHI", data[:8])
        self.assertEqual(length, len(data) - 8)
        return version, ptype, data[8:]


class CommandLineTest(SimTestCase):
    def test_identity_address_and_port_come_from_flags(self):
        version, ptype, payload = self.vehicle_id()
        self.assertIn(version, (0x02, 0x03))
        self.assertEqual(ptype, PT_VEHICLE_ANNOUNCE)
        self.assertEqual(len(payload), 33)
        self.assertEqual(payload[:17], VIN)
        self.assertEqual(struct.unpack("!H", payload[17:19])[0], ECU)

        t = self.connect()
        self.assertEqual(t.uds([0x22, 0xF1, 0x90]), b"\x62\xF1\x90" + VIN)
        # the compiled-in default address is not this ECU any more
        with self.assertRaises(DiagNack) as nack:
            t.uds([0x3E, 0x00], target=0x1003)
        self.assertEqual(nack.exception.code, 0x03)

    def test_invalid_arguments_are_rejected(self):
        for args in (["--vin", "SHORT"], ["--port", "70000"], ["--address", "zero"], ["--no-such-flag"], ["--log-level", "5"]):
            result = subprocess.run([BINARY, *args], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0, args)
        # --log-level 0 (ERROR only) is valid: the server starts and keeps running
        with self.assertRaises(subprocess.TimeoutExpired):
            subprocess.run([BINARY, "--log-level", "0", "--port", "24499"], capture_output=True, timeout=1)
        self.assertEqual(subprocess.run([BINARY, "--help"], capture_output=True, timeout=5).returncode, 0)


class DiagnosticsTest(SimTestCase):
    def test_multiple_dids_in_one_request(self):
        response = self.connect().uds([0x22, 0xF1, 0x90, 0xF1, 0x8C])
        self.assertTrue(response.startswith(b"\x62\xF1\x90" + VIN + b"\xF1\x8C"), response.hex())

    def test_functional_addressing(self):
        t = self.connect()
        t.send_uds(b"\x3E\x00", target=FUNCTIONAL)
        self.assertEqual(t.recv_uds(), (ECU, b"\x7E\x00"))       # answered from the physical address
        self.assertIsNone(t.uds([0x3E, 0x80], target=FUNCTIONAL, timeout=0.3))  # suppressed
        self.assertIsNone(t.uds([0x99], target=FUNCTIONAL, timeout=0.3))        # NRC 0x11 stays silent
        self.assertEqual(t.uds([0x99]), b"\x7F\x99\x11")                        # ...but not physically

    def test_secured_vin_write_is_announced(self):
        new_vin = b"WRITTENBYE2ETEST1"
        t = self.connect()
        self.assertEqual(t.uds(b"\x2E\xF1\x90" + new_vin), b"\x7F\x2E\x7F")   # default session
        self.assertEqual(t.uds([0x10, 0x03])[:2], b"\x50\x03")
        self.assertEqual(t.uds(b"\x2E\xF1\x90" + new_vin), b"\x7F\x2E\x33")   # still locked
        t.unlock()
        self.assertEqual(t.uds(b"\x2E\xF1\x90" + new_vin), b"\x6E\xF1\x90")
        self.assertEqual(self.vehicle_id()[2][:17], new_vin)

    def test_dtc_read_and_clear(self):
        t = self.connect()
        confirmed = t.uds([0x19, 0x02, 0x08])
        self.assertEqual(confirmed[:3], b"\x59\x02\xFF")
        self.assertEqual((len(confirmed) - 3) // 4, 2)
        self.assertEqual(t.uds([0x14, 0xFF, 0xFF, 0xFF]), b"\x54")
        self.assertEqual(t.uds([0x19, 0x01, 0xFF]), b"\x59\x01\xFF\x01\x00\x00")

    def test_testers_have_independent_sessions(self):
        testers = [self.connect(address=TESTER + i) for i in range(3)]
        self.assertEqual(testers[1].uds([0x10, 0x03])[:2], b"\x50\x03")
        routine = [0x31, 0x01, 0xFF, 0x00]
        self.assertEqual(testers[0].uds(routine), b"\x7F\x31\x7F")
        self.assertEqual(testers[1].uds(routine)[0], 0x71)
        self.assertEqual(testers[2].uds(routine), b"\x7F\x31\x7F")

    def test_routing_activation_rejects_non_tester_address(self):
        t = self.connect(address=0x1234, activate=False)
        self.assertEqual(t.activate(), 0x00)


class ExtraDidTest(SimTestCase):
    EXTRA_ARGS = ["--did", "F187=PART-NO-0042", "--did-rw", "2000=0x0A0B0C"]

    def test_registered_dids_from_the_command_line(self):
        t = self.connect()
        self.assertEqual(t.uds([0x22, 0xF1, 0x87]), b"\x62\xF1\x87PART-NO-0042")
        self.assertEqual(t.uds([0x22, 0x20, 0x00]), b"\x62\x20\x00\x0A\x0B\x0C")
        t.unlock()
        self.assertEqual(t.uds(b"\x2E\xF1\x87X"), b"\x7F\x2E\x31")          # read-only
        self.assertEqual(t.uds(b"\x2E\x20\x00\xFF\xEE"), b"\x6E\x20\x00")
        self.assertEqual(t.uds([0x22, 0x20, 0x00]), b"\x62\x20\x00\xFF\xEE")

    def test_bad_did_arguments_are_rejected(self):
        for value in ("F187", "F187=", "XYZ=1", "F190=NOTALLOWED", "2000=0x0A0", "2000=0xZZ", "12345=1"):
            result = subprocess.run([BINARY, "--did", value], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0, value)


class ResponseDelayTest(SimTestCase):
    EXTRA_ARGS = ["--response-delay", "300"]

    def test_response_pending_precedes_delayed_answer(self):
        t = self.connect()
        started = time.monotonic()
        t.send_uds(b"\x3E\x00")
        self.assertEqual(t.recv_uds(keep_pending=True)[1], b"\x7F\x3E\x78")
        self.assertEqual(t.recv_uds(keep_pending=True)[1], b"\x7E\x00")
        self.assertGreaterEqual(time.monotonic() - started, 0.28)


class ForcedNackTest(SimTestCase):
    EXTRA_ARGS = ["--force-nack", "0x05"]

    def test_every_request_is_nacked(self):
        with self.assertRaises(DiagNack) as nack:
            self.connect().uds([0x3E, 0x00])
        self.assertEqual(nack.exception.code, 0x05)


if __name__ == "__main__":
    if not os.access(BINARY, os.X_OK):
        raise SystemExit(f"simulator binary not found: {BINARY} (run `make test-e2e`)")
    unittest.main(verbosity=2)
