#!/usr/bin/env python3
import socket
import argparse
import struct
import fcntl

def get_local_cid():
    # 0x7b03 is IOCTL_VM_SOCKETS_GET_LOCAL_CID
    # Let's try it first
    try:
        s = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
        buf = struct.pack("Q", 0)
        r = fcntl.ioctl(s.fileno(), 0x7b03, buf)
        return struct.unpack("Q", r)[0]
    except Exception:
        # Fallback to local vsock loopback or default host CID
        return 2

def main():
    parser = argparse.ArgumentParser(description="VSOCK Listener")
    parser.add_argument("--port", type=int, required=True, help="Port to listen on")
    parser.add_argument("--cid", type=int, default=socket.VMADDR_CID_ANY, help="CID to bind to")
    args = parser.parse_args()

    s = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
    try:
        s.bind((args.cid, args.port))
        s.listen(1)
        print(f"[*] Listening on VSOCK (CID={args.cid}, Port={args.port})...")
        
        conn, addr = s.accept()
        print(f"[+] Accepted connection from remote peer CID={addr[0]}, Port={addr[1]}")
        
        data = conn.recv(1024)
        print(f"[<] Received: {data.decode('utf-8', errors='ignore')}")
        
        response = f"Hello from Listener (CID={get_local_cid()})"
        conn.sendall(response.encode('utf-8'))
        print(f"[>] Sent: {response}")
        
        conn.close()
    except Exception as e:
        print(f"[-] Error: {e}")
    finally:
        s.close()

if __name__ == "__main__":
    main()
