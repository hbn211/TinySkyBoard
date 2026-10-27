// Listens to the TinySkyBoard panels on the local network (main/lan.c). Receive only: it never
// sends a packet, so the computer is a mirror and never a part of what the panels agree on.
// Packet: "TSB1" | type (1) | seq (4) | sender MAC (6) | payload | HMAC-SHA256 (16), key = SHA-256(code)
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

export const PORT = 47474;
const T_STATE = 5;
const HDR = 15, TAG = 16;
const PEER_S = 30;      // a panel heard within this is "on the network" (they say hello every 10 s)
const STALE_S = 90;     // the shown state is old after this (sent every 30 s at least)

// "k7pm 3qxd", "K7PM3QXD" -> "K7PM-3QXD", as the setup page shows it; look-alikes as the panel reads
// them (B=8, S=5, Z=2, V=U: main/lan.c), so a code copied with one of them still pairs
export function normalizeCode(code) {
    const c = (code || '').toUpperCase().replace(/[^0-9A-Z]/g, '')
        .replace(/B/g, '8').replace(/S/g, '5').replace(/Z/g, '2').replace(/V/g, 'U');
    return c.length === 8 ? `${c.slice(0, 4)}-${c.slice(4)}` : c;
}

function hex(bytes) {
    return Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
}

function unhex(h) {
    const out = new Uint8Array(h.length / 2);
    for (let i = 0; i < out.length; i++)
        out[i] = parseInt(h.substr(2 * i, 2), 16);
    return out;
}

// the lines of a STATE (see main/lan.c)
export function parseState(text) {
    const st = {weather: null, sensors: [], vent: -1, unread: 0, messages: [], next: null};
    for (const line of text.split('\n')) {
        const f = line.split('\t');
        switch (f[0]) {
        case 'W':
            st.weather = {temp: f[1] / 10, code: +f[2], max: +f[3], min: +f[4], rain: +f[5], day: f[6] === '1'};
            break;
        case 'S':
            st.sensors.push({name: f[1], temp: f[2] / 10, rh: +f[3], trend: +f[4]});
            break;
        case 'V':
            st.vent = +f[1];
            break;
        case 'U':
            st.unread = +f[1];
            break;
        case 'M':
            st.messages.push({unread: f[1] === '1', when: +f[2], from: f[3], text: f.slice(4).join(' ')});
            break;
        case 'N':
            st.next = {when: +f[1], text: f.slice(2).join(' ')};
            break;
        }
    }
    return st;
}

export class Receiver {
    // onUpdate({state, panels, age}) on every new state, and every few seconds for the panel count
    constructor(code, onUpdate) {
        this._key = unhex(GLib.compute_checksum_for_string(GLib.ChecksumType.SHA256, normalizeCode(code), -1));
        this._onUpdate = onUpdate;
        this._peers = new Map();   // mac hex -> last heard (s)
        this._primary = null;      // the panel being mirrored: the first one heard, kept while it talks
        this._state = null;
        this._stateAt = 0;
        this.bad = 0;
    }

    start() {
        this._sock = Gio.Socket.new(Gio.SocketFamily.IPV4, Gio.SocketType.DATAGRAM, Gio.SocketProtocol.UDP);
        this._sock.bind(Gio.InetSocketAddress.new(Gio.InetAddress.new_any(Gio.SocketFamily.IPV4), PORT), true);
        this._sock.set_blocking(false);
        this._src = this._sock.create_source(GLib.IOCondition.IN, null);
        this._src.set_callback(() => {
            this._drain();
            return GLib.SOURCE_CONTINUE;
        });
        this._src.attach(null);
        this._tick = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 5, () => {
            this._emit();
            return GLib.SOURCE_CONTINUE;
        });
    }

    stop() {
        if (this._tick)
            GLib.source_remove(this._tick);
        this._tick = 0;
        this._src?.destroy();
        this._src = null;
        this._sock?.close();
        this._sock = null;
    }

    _now() {
        return GLib.get_monotonic_time() / 1e6;
    }

    _drain() {
        for (let i = 0; i < 16; i++) {
            let bytes;
            try {
                [bytes] = this._sock.receive_bytes_from(2048, 0, null);
            } catch (e) {
                return;   // nothing more to read
            }
            const data = bytes.toArray();
            if (data.length === 0)
                return;
            this._packet(data);
        }
    }

    _packet(d) {
        if (d.length < HDR + TAG || d[0] !== 0x54 || d[1] !== 0x53 || d[2] !== 0x42 || d[3] !== 0x31)
            return;   // not "TSB1"
        const h = GLib.Hmac.new(GLib.ChecksumType.SHA256, this._key);
        h.update(d.subarray(0, d.length - TAG));
        if (h.get_string().slice(0, 2 * TAG) !== hex(d.subarray(d.length - TAG))) {
            this.bad++;   // another code
            return;
        }
        const mac = hex(d.subarray(9, 15));
        const now = this._now();
        this._peers.set(mac, now);
        if (d[4] !== T_STATE)
            return;   // HELLO and the panels' own messages: only counted
        if (this._primary && this._primary !== mac && now - this._stateAt < STALE_S)
            return;
        this._primary = mac;
        this._state = parseState(new TextDecoder().decode(d.subarray(HDR, d.length - TAG)));
        this._stateAt = now;
        this._emit();
    }

    _emit() {
        const now = this._now();
        let panels = 0;
        for (const t of this._peers.values())
            panels += now - t <= PEER_S;
        this._onUpdate({state: this._state, panels, age: this._state ? now - this._stateAt : Infinity});
    }
}

