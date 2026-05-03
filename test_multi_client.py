import threading, socket, time, struct

def client_thread(client_id):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    sock.connect(('127.0.0.1', 13400))
    
    # Routing Activation
    req = struct.pack('!HBI', 0x0E00 + client_id, 0x00, 0x00)
    hdr = struct.pack('!BBHI', 0x02, 0xFD, 0x0005, len(req))
    sock.sendall(hdr + req)
    print(f"[Client {client_id}] ✅ Connected")
    
    # Session Control
    def send_diag(sock, uds_data):
        payload = struct.pack('!HH', 0x0E00 + client_id, 0x1003) + uds_data
        hdr = struct.pack('!BBHI', 0x02, 0xFD, 0x8001, len(payload))
        sock.sendall(hdr + payload)
        # Skip ACK, just get response
        sock.recv(8) # ACK
        sock.recv(1024) # Drain
        sock.recv(8) # Response header
        resp = sock.recv(1024)
        return resp
    
    for i in range(3):
        resp = send_diag(sock, bytes([0x10, 0x03]))
        print(f"[Client {client_id}] Session Response: {resp[:5].hex()}")
        time.sleep(1)
    
    sock.close()
    print(f"[Client {client_id}] Closed")

# Start 3 clients concurrently
threads = []
for i in range(3):
    t = threading.Thread(target=client_thread, args=(i,))
    threads.append(t)
    t.start()

for t in threads:
    t.join()