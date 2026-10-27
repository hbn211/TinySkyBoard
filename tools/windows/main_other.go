//go:build !windows

// Test harness for Linux: the same receiver and menu lines as the Windows tray app, on the console,
// and the tray icon as PNG previews. go run . -code XXXX-XXXX [-secs 60] [-icons dir]
package main

import (
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"time"
)

var version = "dev"

func main() {
	code := flag.String("code", "", "pairing code")
	secs := flag.Int("secs", 60, "how long to listen")
	icons := flag.String("icons", "", "write icon previews (PNG) to this folder and exit")
	flag.Parse()
	if *icons != "" {
		writePreviews(*icons)
		return
	}
	c := NormalizeCode(*code)
	if !ValidCode(c) {
		fmt.Println("invalid code")
		os.Exit(1)
	}
	t := texts[langFrom(os.Getenv("LANG"))]
	r := NewReceiver(c)
	r.OnUpdate = func() {
		st, panels, s := r.Snapshot()
		l := t.Lines(st, panels)
		fmt.Printf("--- %s  packets %d (taken %d, other code %d, replayed %d, clock skew %d s)  icon %q\n",
			time.Now().Format("15:04:05"), s.Raw, s.Good, s.Bad, s.Old, s.Skew, iconText(st))
		for _, s := range append(append(append([]string{l.Weather, l.WeatherInfo}, l.Sensors...), l.Vent), append(l.Messages, l.Next, l.Footer, "tooltip: "+l.Tooltip)...) {
			if s != "" {
				fmt.Println("  " + s)
			}
		}
	}
	if err := r.Start(); err != nil {
		fmt.Println("listen:", err)
		os.Exit(1)
	}
	time.Sleep(time.Duration(*secs) * time.Second)
	_, _, s := r.Snapshot()
	fmt.Printf("done: packets %d, taken %d, other code %d, replayed %d\n", s.Raw, s.Good, s.Bad, s.Old)
}

func langFrom(s string) string {
	s = strings.ToLower(s)
	for _, l := range []string{"pt", "es", "en"} {
		if strings.HasPrefix(s, l) {
			return l
		}
	}
	return "en"
}

// every size of a few icons, enlarged 4x, on a dark and a light taskbar color
func writePreviews(dir string) {
	os.MkdirAll(dir, 0o755)
	cases := []*State{
		{Weather: &Weather{Temp: 14}},
		{Weather: &Weather{Temp: 8.2}, Unread: 2},
		{Weather: &Weather{Temp: -3}},
		{Weather: &Weather{Temp: -12.4}, Unread: 1},
		{Weather: &Weather{Temp: 31}},
		nil,
	}
	const zoom = 4
	cell := 32*zoom + 8
	out := image.NewRGBA(image.Rect(0, 0, cell*len(iconSizes)*2, cell*len(cases)))
	for row, st := range cases {
		for bgi, bg := range []color.RGBA{{32, 32, 32, 255}, {243, 243, 243, 255}} {
			for col, s := range iconSizes {
				x0 := (bgi*len(iconSizes) + col) * cell
				y0 := row * cell
				draw.Draw(out, image.Rect(x0, y0, x0+cell, y0+cell), image.NewUniform(bg), image.Point{}, draw.Src)
				fg := []color.RGBA{colOnDark, colOnLight}[bgi]
				if st == nil {
					fg = colNoData
				}
				ic := renderIcon(s, iconText(st), fg, st != nil && st.Unread > 0)
				for y := 0; y < s; y++ {
					for x := 0; x < s; x++ {
						c := ic.RGBAAt(x, y)
						if c.A == 0 {
							continue
						}
						r := image.Rect(x0+4+x*zoom, y0+4+y*zoom, x0+4+(x+1)*zoom, y0+4+(y+1)*zoom)
						draw.Draw(out, r, image.NewUniform(c), image.Point{}, draw.Over)
					}
				}
			}
		}
	}
	f, _ := os.Create(filepath.Join(dir, "icons.png"))
	png.Encode(f, out)
	f.Close()
	ico := makeIcon(cases[1], false)
	os.WriteFile(filepath.Join(dir, "sample.ico"), ico, 0o644)
	fmt.Println("wrote", filepath.Join(dir, "icons.png"), "and sample.ico", len(ico), "bytes")
}
