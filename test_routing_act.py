#!/usr/bin/env python3
import socket, struct, sys

TESTER_LA = 0x0E00  # Tester Logical Address
ECU_LA = 0x1003     # ECU Logical Address (or 0x0E00 for default)

def send_doip_diagnostic(sock, uds_payload):
    """Send UDS request wrapped in DoIP Diagnostic Message"""
    # Build payload: Source LA (2B) + Target LA (2B) + UDS Data
    payload = struct.pack('!HH', TESTER_LA, ECU_LA) + uds_payload
    
    # Send as Diagnostic Request (0x8001)
    header = struct.pack('!BBHI', 0x02, 0xFD, 0x8001, len(payload))
    sock.sendall(header + payload)
    
    # Receive response
    resp_hdr = sock.recv(8)
    ver, inv, ptype, plen = struct.unpack('!BBHI', resp_hdr)
    
    if ptype == 0x8002 and plen > 0:
        resp_payload = sock.recv(plen)
        # Parse response: Source LA (2B) + Target LA (2B) + UDS Data
        if len(resp_payload) >= 4:
            src_la, tgt_la = struct.unpack('!HH', resp_payload[:4])
            uds_data = resp_payload[4:]
            return src_la, tgt_la, uds_data
    return None, None, None

def test_uds_services():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    sock.connect(('127.0.0.1', 13400))
    
    # 1. Routing Activation (required first)
    print("1️⃣ Routing Activation...")
    req = struct.pack('!HBI', TESTER_LA, 0x00, 0x00)
    header = struct.pack('!BBHI', 0x02, 0xFD, 0x0005, len(req))
    sock.sendall(header + req)
    resp = sock.recv(8)
    ver, inv, ptype, plen = struct.unpack('!BBHI', resp)
    if plen > 0:
        act_resp = sock.recv(plen)
        print(f"   ✅ Activated: {act_resp.hex()}")
    
    # 2. Diagnostic Session Control (0x10 0x03 -> Extended)
    print("\n2️⃣ Session Control (Extended)...")
    uds_req = bytes([0x10, 0x03])
    src, tgt, uds_res = send_doip_diagnostic(sock, uds_req)
    print(f"   Request:  {uds_req.hex()}")
    print(f"   Response: {uds_res.hex() if uds_res else 'None'}")
    if uds_res and uds_res[0] == 0x50:
        print(f"   ✅ Session changed to Extended")
    
    # 3. Read VIN (0x22 F1 90)
    print("\n3️⃣ Read VIN (DID 0xF190)...")
    uds_req = bytes([0x22, 0xF1, 0x90])
    src, tgt, uds_res = send_doip_diagnostic(sock, uds_req)
    print(f"   Request:  {uds_req.hex()}")
    print(f"   Response: {uds_res.hex() if uds_res else 'None'}")
    if uds_res and len(uds_res) > 3:
        vin = uds_res[3:].decode('ascii', errors='ignore')
        print(f"   🚗 VIN: {vin}")
    
    # 4. Read Software Version (0x22 F1 82)
    print("\n4️⃣ Read Software Version (DID 0xF182)...")
    uds_req = bytes([0x22, 0xF1, 0x82])
    src, tgt, uds_res = send_doip_diagnostic(sock, uds_req)
    print(f"   Request:  {uds_req.hex()}")
    print(f"   Response: {uds_res.hex() if uds_res else 'None'}")
    if uds_res and len(uds_res) > 3:
        sw = uds_res[3:].decode('ascii', errors='ignore')
        print(f"   💾 SW: {sw}")
    
    # 5. Tester Present (0x3E 0x00)
    print("\n5️⃣ Tester Present...")
    uds_req = bytes([0x3E, 0x00])
    src, tgt, uds_res = send_doip_diagnostic(sock, uds_req)
    print(f"   Request:  {uds_req.hex()}")
    print(f"   Response: {uds_res.hex() if uds_res else 'None'}")
    
    # 6. ECU Reset (0x11 0x01)
    print("\n6️⃣ ECU Reset (Hard)...")
    uds_req = bytes([0x11, 0x01])
    src, tgt, uds_res = send_doip_diagnostic(sock, uds_req)
    print(f"   Request:  {uds_req.hex()}")
    print(f"   Response: {uds_res.hex() if uds_res else 'None'}")
    
    sock.close()
    print("\n✅ All UDS tests completed!")

if __name__ == '__main__':
    test_uds_services()