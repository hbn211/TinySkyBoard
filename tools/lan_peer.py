#!/usr/bin/env python3
"""A TinySkyBoard "panel" on the computer, for testing the LAN sharing (main/lan.c).

Speaks the same UDP protocol: announces itself, shows what the panels send, and sends messages or
schedules to them, timing each acknowledgment. Standard library only.

  lan_peer.py listen                    watch the network (announces itself too)
  lan_peer.py msg "text" [--from Ana]   send one message, time the acknowledgment
  lan_peer.py flood 50 [--gap 0.2]      many messages in a row: lost ones, latency
  lan_peer.py sched "text" [--in 2]     a one-time schedule starting in N minutes [--dur 15] minutes long

The password (the panel's pairing code) comes from --password or TSB_LAN_PASS.
"""
import argparse
import hashlib
import hmac
import os
import random
import socket
import statistics
import struct
import sys
import time

PORT = 47474
T_HELLO, T_MSG, T_SCHED, T_ACK = 1, 2, 3, 4
NAMES = {T_HELLO: "HELLO", T_MSG: "MSG", T_SCHED: "SCHED", T_ACK: "ACK"}
MSG_FROM = 24
MY_MAC = bytes([0x02, 0x54, 0x53, 0x42]) + random.randbytes(2)   # locally administered, random per run


class Peer:
    def __init__(self, password):
        # the pairing code as the panel reads it: XXXX-XXXX, look-alikes B=8, S=5, Z=2, V=U (main/lan.c)
        c = "".join(ch for ch in password.upper() if ch.isalnum()).translate(str.maketrans("BSZV", "852U"))
        c = f"{c[:4]}-{c[4:]}" if len(c) == 8 else password
        self.key = hashlib.sha256(c.encode()).digest()
        self.seq = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.bind(("", PORT))
        self.sock.settimeout(0.2)
        self.last_hello = 0.0

    def sign(self, data):
        return hmac.new(self.key, data, hashlib.sha256).digest()[:16]

    def send(self, ptype, payload, repeat=1):
        self.seq += 1
        pkt = b"TSB1" + bytes([ptype]) + struct.pack(">I", self.seq) + MY_MAC + payload
        pkt += self.sign(pkt)
        for _ in range(repeat):
            self.sock.sendto(pkt, ("255.255.255.255", PORT))

    def hello(self):
        if time.time() - self.last_hello >= 10:
            self.last_hello = time.time()
            self.send(T_HELLO, b"laptop")

    def recv(self):
        """(type, mac, payload, ip) of a valid packet from someone else, or None"""
        try:
            data, addr = self.sock.recvfrom(2048)
        except socket.timeout:
            return None
        if len(data) < 31 or data[:4] != b"TSB1" or data[9:15] == MY_MAC:
            return None
        if not hmac.compare_digest(self.sign(data[:-16]), data[-16:]):
            print(f"  bad signature from {addr[0]} (another password?)")
            return None
        return data[4], data[9:15], data[15:-16], addr[0]


def msg_payload(text, sender):
    msg_id = random.getrandbits(32)
    return msg_id, struct.pack(">I", msg_id) + sender.encode()[:MSG_FROM - 1].ljust(MSG_FROM, b"\0") + text.encode()


def wait_ack(peer, msg_id, timeout=3.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = peer.recv()
        if r and r[0] == T_ACK and r[2][0] == T_MSG and struct.unpack(">I", r[2][1:5])[0] == msg_id:
            return time.time() - t0
    return None


def cmd_listen(peer, args):
    print(f"listening on UDP {PORT} (Ctrl+C to stop)")
    while True:
        peer.hello()
        r = peer.recv()
        if not r:
            continue
        t, mac, p, ip = r
        who = f"{ip} ..{mac[4]:02x}:{mac[5]:02x}"
        if t == T_HELLO:
            print(f"{time.strftime('%H:%M:%S')} HELLO {who} v{p.decode(errors='replace')}")
        elif t == T_MSG:
            name = p[4:4 + MSG_FROM].split(b"\0")[0].decode(errors="replace")
            print(f"{time.strftime('%H:%M:%S')} MSG   {who} from '{name}': {p[4 + MSG_FROM:].decode(errors='replace')}")
        elif t == T_SCHED:
            text = p[8:128].split(b"\0")[0].decode(errors="replace")
            print(f"{time.strftime('%H:%M:%S')} SCHED {who}: {text}")
        else:
            print(f"{time.strftime('%H:%M:%S')} {NAMES.get(t, t)} {who}")


def cmd_msg(peer, args):
    msg_id, payload = msg_payload(args.text, args.sender)
    t0 = time.time()
    peer.send(T_MSG, payload, repeat=3)
    rtt = wait_ack(peer, msg_id)
    print(f"acknowledged in {rtt * 1000:.0f} ms" if rtt is not None else "no acknowledgment in 3 s")


def cmd_flood(peer, args):
    rtts, lost = [], 0
    for i in range(args.count):
        msg_id, payload = msg_payload(f"Teste {i + 1}/{args.count}", args.sender)
        peer.send(T_MSG, payload, repeat=3)
        rtt = wait_ack(peer, msg_id, timeout=2.0)
        if rtt is None:
            lost += 1
        else:
            rtts.append(rtt * 1000)
        time.sleep(args.gap)
        print(f"\r{i + 1}/{args.count} sent, {lost} without acknowledgment", end="", flush=True)
    print()
    if rtts:
        print(f"acknowledgment: median {statistics.median(rtts):.0f} ms, max {max(rtts):.0f} ms, "
              f"min {min(rtts):.0f} ms over {len(rtts)}")


def cmd_sched(peer, args):
    start = int(time.time()) + args.minutes * 60
    lt = time.localtime(start)
    # sched_t (main/schedule.h): id, text[120], who, (pad 3), start, hour, minute, repeat, flags, dur_min, (pad 2)
    s = struct.pack("<I120sB3xqBBBBH2x", 0, args.text.encode()[:119], 0, start, lt.tm_hour, lt.tm_min, 0, 0, args.dur)
    assert len(s) == 144
    peer.send(T_SCHED, struct.pack(">I", random.getrandbits(32)) + s, repeat=3)
    print(f"schedule sent: '{args.text}' at {time.strftime('%H:%M', lt)}, {args.dur} min")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--password", default=os.environ.get("TSB_LAN_PASS"))
    ap.add_argument("--from", dest="sender", default="Laptop")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("listen")
    p = sub.add_parser("msg")
    p.add_argument("text")
    p = sub.add_parser("flood")
    p.add_argument("count", type=int)
    p.add_argument("--gap", type=float, default=0.2)
    p = sub.add_parser("sched")
    p.add_argument("text")
    p.add_argument("--in", dest="minutes", type=int, default=2)
    p.add_argument("--dur", type=int, default=15)
    args = ap.parse_args()
    if not args.password:
        sys.exit("missing --password (or TSB_LAN_PASS)")
    peer = Peer(args.password)
    try:
        {"listen": cmd_listen, "msg": cmd_msg, "flood": cmd_flood, "sched": cmd_sched}[args.cmd](peer, args)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
