// Listens to the TinySkyBoard panels on the local network (main/lan.c), like the GNOME extension's
// lan.js. Receive only: it never sends a packet.
//
// Packet: "TSB2" | type (1) | seq (4) | sender MAC (6) | nonce (8) | encrypted: time (4) | payload |
// HMAC-SHA256 (16) over everything before it. Keys from the pairing code: SHA-256("tsb-mac:" + code)
// signs, SHA-256("tsb-enc:" + code) encrypts (keystream HMAC(key, nonce | block number)).
//
// Replays: a packet older than the newest one already taken from that panel (by the panel's clock) is
// dropped. Not by this computer's clock: a dual-boot PC often starts Windows an hour or two off until it
// syncs, and the panels sync theirs over the internet anyway.
package main

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"net"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	lanPort  = 47474
	tState   = 5
	hdrLen   = 15
	nonceLen = 8
	timeLen  = 4
	bodyOff  = hdrLen + nonceLen + timeLen
	tagLen   = 16
	reorderS = 5  // a packet this much older than the newest from its panel still counts (copies, reordering)
	resyncN  = 6  // this many "older" packets in a row: the panel's clock went back (reset), follow it
	peerS    = 30 // a panel heard within this is "on the network" (hello every 10 s)
	StaleS   = 90 // the shown state is old after this (sent every 30 s at least)
)

var codeRe = regexp.MustCompile(`^[2-9ACDEFGHJKMNPQRTUWXY]{4}-[2-9ACDEFGHJKMNPQRTUWXY]{4}$`)

// NormalizeCode: "k7pm 3qxd" -> "K7PM-3QXD"; look-alikes as the panel reads them (B=8, S=5, Z=2, V=U)
func NormalizeCode(s string) string {
	var b strings.Builder
	for _, r := range strings.ToUpper(s) {
		if (r >= '0' && r <= '9') || (r >= 'A' && r <= 'Z') {
			b.WriteRune(r)
		}
	}
	c := strings.NewReplacer("B", "8", "S", "5", "Z", "2", "V", "U").Replace(b.String())
	if len(c) == 8 {
		return c[:4] + "-" + c[4:]
	}
	return c
}

func ValidCode(c string) bool { return codeRe.MatchString(c) }

type Weather struct {
	Temp           float64
	Code, Max, Min int
	Rain           int // chance this hour, -1 unknown
	Day            bool
}

type Sensor struct {
	Name      string
	Temp      float64
	RH, Trend int
}

type Message struct {
	Unread     bool
	When       int64
	From, Text string
}

type Next struct {
	When int64
	Text string
}

type State struct {
	Weather  *Weather
	Sensors  []Sensor
	Vent     int // 1 airing out helps, 2 it doesn't, else nothing to say (psy_vent_t)
	Unread   int
	Messages []Message
	Next     *Next
}

func atoi(s string) int     { n, _ := strconv.Atoi(s); return n }
func atoi64(s string) int64 { n, _ := strconv.ParseInt(s, 10, 64); return n }

// ParseState reads the text lines of a STATE (see main/lan.c)
func ParseState(text string) *State {
	st := &State{Vent: -1}
	for _, line := range strings.Split(text, "\n") {
		f := strings.Split(line, "\t")
		switch {
		case f[0] == "W" && len(f) >= 7:
			st.Weather = &Weather{float64(atoi(f[1])) / 10, atoi(f[2]), atoi(f[3]), atoi(f[4]), atoi(f[5]), f[6] == "1"}
		case f[0] == "S" && len(f) >= 5:
			st.Sensors = append(st.Sensors, Sensor{f[1], float64(atoi(f[2])) / 10, atoi(f[3]), atoi(f[4])})
		case f[0] == "V" && len(f) >= 2:
			st.Vent = atoi(f[1])
		case f[0] == "U" && len(f) >= 2:
			st.Unread = atoi(f[1])
		case f[0] == "M" && len(f) >= 5:
			st.Messages = append(st.Messages, Message{f[1] == "1", atoi64(f[2]), f[3], strings.Join(f[4:], " ")})
		case f[0] == "N" && len(f) >= 3:
			st.Next = &Next{atoi64(f[1]), strings.Join(f[2:], " ")}
		}
	}
	return st
}

// Stats: TinySkyBoard packets seen, taken, signed with another code, and older than their panel's newest;
// Skew = this computer's clock minus the panel's, at the last packet taken (seconds)
type Stats struct {
	Raw, Good, Bad, Old uint64
	Skew                int64
}

type Receiver struct {
	keyMac, keyEnc []byte
	mu             sync.Mutex
	peers          map[string]time.Time
	primary        string
	state          *State
	stateAt        time.Time
	latest         map[string]uint32 // per panel: the newest clock taken
	olderRun       map[string]int
	stats          Stats
	OnUpdate       func() // called on every new state (from the receiving goroutine)
	conn           net.PacketConn
}

func NewReceiver(code string) *Receiver {
	c := NormalizeCode(code)
	km := sha256.Sum256([]byte("tsb-mac:" + c))
	ke := sha256.Sum256([]byte("tsb-enc:" + c))
	return &Receiver{keyMac: km[:], keyEnc: ke[:], peers: map[string]time.Time{}, latest: map[string]uint32{},
		olderRun: map[string]int{}}
}

func (r *Receiver) Start() error {
	lc := net.ListenConfig{Control: reuseAddr}
	conn, err := lc.ListenPacket(context.Background(), "udp4", ":"+strconv.Itoa(lanPort))
	if err != nil {
		return err
	}
	r.conn = conn
	go func() {
		buf := make([]byte, 2048)
		for {
			n, _, err := conn.ReadFrom(buf)
			if err != nil {
				return // closed
			}
			r.packet(append([]byte(nil), buf[:n]...))
		}
	}()
	return nil
}

func (r *Receiver) Stop() {
	if r.conn != nil {
		r.conn.Close()
	}
}

func (r *Receiver) mac(data []byte) []byte {
	h := hmac.New(sha256.New, r.keyMac)
	h.Write(data)
	return h.Sum(nil)[:tagLen]
}

// decrypts in place: XOR with HMAC(enc key, nonce | block number), 32 bytes a block
func (r *Receiver) keystreamXor(nonce, data []byte) {
	ctr := make([]byte, nonceLen+4)
	copy(ctr, nonce)
	for off := 0; off < len(data); off += 32 {
		binary.BigEndian.PutUint32(ctr[nonceLen:], uint32(off/32))
		h := hmac.New(sha256.New, r.keyEnc)
		h.Write(ctr)
		ks := h.Sum(nil)
		for i := 0; i < 32 && off+i < len(data); i++ {
			data[off+i] ^= ks[i]
		}
	}
}

func (r *Receiver) packet(d []byte) {
	if len(d) < bodyOff+tagLen || string(d[:4]) != "TSB2" {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.stats.Raw++
	if !hmac.Equal(r.mac(d[:len(d)-tagLen]), d[len(d)-tagLen:]) {
		r.stats.Bad++ // another code
		return
	}
	body := d[hdrLen+nonceLen : len(d)-tagLen]
	r.keystreamXor(d[hdrLen:hdrLen+nonceLen], body)
	sent := binary.BigEndian.Uint32(body)
	mac := hex.EncodeToString(d[9:15])
	if last, ok := r.latest[mac]; ok && int64(sent)+reorderS < int64(last) {
		if r.olderRun[mac]++; r.olderRun[mac] < resyncN {
			r.stats.Old++ // a recording played back
			return
		}
	}
	r.olderRun[mac] = 0
	if sent > r.latest[mac] || r.latest[mac]-sent > reorderS {
		r.latest[mac] = sent
	}
	r.stats.Good++
	r.stats.Skew = time.Now().Unix() - int64(sent)
	r.peers[mac] = time.Now()
	if d[4] != tState {
		return // HELLO and the panels' own messages: only counted
	}
	if r.primary != "" && r.primary != mac && time.Since(r.stateAt) < StaleS*time.Second {
		return // mirror the first panel heard while it talks
	}
	r.primary = mac
	r.state = ParseState(string(body[timeLen:]))
	r.stateAt = time.Now()
	if r.OnUpdate != nil {
		go r.OnUpdate()
	}
}

// Snapshot: the last state (nil if none or old), panels heard lately, and the packet counters
func (r *Receiver) Snapshot() (st *State, panels int, stats Stats) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, t := range r.peers {
		if time.Since(t) <= peerS*time.Second {
			panels++
		}
	}
	if r.state != nil && time.Since(r.stateAt) < StaleS*time.Second {
		st = r.state
	}
	return st, panels, r.stats
}
