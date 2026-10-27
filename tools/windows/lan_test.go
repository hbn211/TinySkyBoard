package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"testing"
	"time"
)

// seal builds a packet the way a panel does (main/lan.c)
func seal(r *Receiver, typ byte, mac [6]byte, sent uint32, payload string) []byte {
	pk := []byte("TSB2")
	pk = append(pk, typ, 0, 0, 0, 1)
	pk = append(pk, mac[:]...)
	nonce := make([]byte, nonceLen)
	rand.Read(nonce)
	pk = append(pk, nonce...)
	body := binary.BigEndian.AppendUint32(nil, sent)
	body = append(body, payload...)
	r.keystreamXor(nonce, body)
	pk = append(pk, body...)
	h := hmac.New(sha256.New, r.keyMac)
	h.Write(pk)
	return append(pk, h.Sum(nil)[:tagLen]...)
}

func TestReceiver(t *testing.T) {
	r := NewReceiver("k7pm 3qxd")
	panel := [6]byte{1, 2, 3, 4, 5, 6}
	now := uint32(time.Now().Unix())
	state := "W\t104\t1\t17\t6\t0\t1\nU\t0\n"

	// this computer's clock two hours off (dual boot): still taken
	r.packet(seal(r, tState, panel, now-7200, state))
	if _, _, s := r.Snapshot(); s.Good != 1 || s.Old != 0 {
		t.Fatalf("skewed clock: %+v", s)
	}
	r.packet(seal(r, tState, panel, now-7100, state))
	// a recording of the first packet played back: dropped
	r.packet(seal(r, tState, panel, now-7200, state))
	if _, _, s := r.Snapshot(); s.Good != 2 || s.Old != 1 {
		t.Fatalf("replay: %+v", s)
	}
	// copies and reordering within reorderS: taken
	r.packet(seal(r, tState, panel, now-7103, state))
	if _, _, s := r.Snapshot(); s.Good != 3 {
		t.Fatalf("reordered copy: %+v", s)
	}
	// the panel's clock went back (reset before its clock synced): followed after resyncN in a row
	for i := 0; i < resyncN; i++ {
		r.packet(seal(r, tState, panel, now-90000+uint32(i), state))
	}
	if _, _, s := r.Snapshot(); s.Good != 4 || s.Old != uint64(1+resyncN-1) {
		t.Fatalf("resync: %+v", s)
	}
	r.packet(seal(r, tState, panel, now-89990, state))
	if _, _, s := r.Snapshot(); s.Good != 5 {
		t.Fatalf("after resync: %+v", s)
	}
	// another code: counted apart
	other := NewReceiver("K7PM-3QXE")
	r.packet(seal(other, tState, panel, now, state))
	st, panels, s := r.Snapshot()
	if s.Bad != 1 || panels != 1 || st == nil || st.Weather == nil || st.Weather.Temp != 10.4 {
		t.Fatalf("other code / state: %+v %+v %d", s, st, panels)
	}
}

func TestNormalizeCode(t *testing.T) {
	for in, want := range map[string]string{"k7pm 3qxd": "K7PM-3QXD", "K7PB-3QXD": "K7P8-3QXD", "s2zv-aaaa": "522U-AAAA"} {
		if got := NormalizeCode(in); got != want {
			t.Errorf("%q: %q, want %q", in, got, want)
		}
	}
}
