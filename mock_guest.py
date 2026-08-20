#!/usr/bin/env python3
import sys
import os
import fcntl
import struct
import time

# Ioctl constants
VHOST_SET_OWNER = 0xaf01
VHOST_VSOCK_SET_GUEST_CID = 0x4008af60
VHOST_VSOCK_SET_RUNNING = 0x4004af61

def main():
    if len(sys.argv) < 2:
        print("Usage: mock_guest.py <cid>")
        sys.exit(1)
        
    cid = int(sys.argv[1])
    
    print(f"[*] Opening /dev/vhost-vsock...")
    fd = os.open("/dev/vhost-vsock", os.O_RDWR)
    
    try:
        print(f"[*] Setting owner...")
        fcntl.ioctl(fd, VHOST_SET_OWNER, 0)
        
        print(f"[*] Setting guest CID to {cid}...")
        cid_buf = struct.pack("Q", cid)
        fcntl.ioctl(fd, VHOST_VSOCK_SET_GUEST_CID, cid_buf)
        
        print(f"[*] Setting running status...")
        running_buf = struct.pack("I", 1)
        fcntl.ioctl(fd, VHOST_VSOCK_SET_RUNNING, running_buf)
        
        print(f"[+] Mock guest VM (CID={cid}) is now running in host kernel.")
        print("[*] Press Ctrl+C to terminate...")
        
        while True:
            time.sleep(1)
            
    except KeyboardInterrupt:
        print("\n[*] Terminating mock guest VM...")
    except Exception as e:
        print(f"[-] Error: {e}")
    finally:
        os.close(fd)
        print("[*] Closed /dev/vhost-vsock.")

if __name__ == "__main__":
    main()
