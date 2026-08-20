#!/usr/bin/env python3
import socket
import argparse

def main():
    parser = argparse.ArgumentParser(description="VSOCK Client")
    parser.add_argument("--cid", type=int, required=True, help="Target CID to connect to")
    parser.add_argument("--port", type=int, required=True, help="Target Port to connect to")
    parser.add_argument("--msg", type=str, default="Hello via VSOCK", help="Message to send")
    args = parser.parse_args()

    s = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
    try:
        print(f"[*] Connecting to VSOCK CID={args.cid}, Port={args.port}...")
        s.connect((args.cid, args.port))
        print("[+] Connected!")
        
        print(f"[>] Sending: {args.msg}")
        s.sendall(args.msg.encode('utf-8'))
        
        data = s.recv(1024)
        print(f"[<] Received response: {data.decode('utf-8', errors='ignore')}")
    except Exception as e:
        print(f"[-] Error: {e}")
    finally:
        s.close()

if __name__ == "__main__":
    main()
