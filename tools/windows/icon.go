package main

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"math"
	"strconv"

	"golang.org/x/image/font"
	"golang.org/x/image/font/gofont/gobold"
	"golang.org/x/image/font/opentype"
	"golang.org/x/image/math/fixed"
)

var boldFont, _ = opentype.Parse(gobold.TTF)

var (
	colOnDark  = color.RGBA{255, 255, 255, 255} // taskbar in dark mode
	colOnLight = color.RGBA{28, 28, 28, 255}    // taskbar in light mode
	colNoData  = color.RGBA{138, 138, 138, 255}
	colUnread  = color.RGBA{255, 140, 0, 255}
)

// iconText: just the number, as large as it fits ("14", "-3", "-12"); "--" without data. The degree
// sign and the rest are in the menu and the tooltip
func iconText(st *State) string {
	if st == nil || st.Weather == nil {
		return "--"
	}
	return strconv.Itoa(int(math.Round(st.Weather.Temp)))
}

// the text as large as the icon allows (by the glyphs' own box, not the line height), centered, in one
// flat color like Windows' own tray icons; a dot in the corner for unread messages
func renderIcon(size int, text string, fg color.RGBA, unread bool) *image.RGBA {
	img := image.NewRGBA(image.Rect(0, 0, size, size))
	var face font.Face
	var box fixed.Rectangle26_6
	for pt := float64(size) * 1.5; pt > 4; pt -= 0.5 {
		f, err := opentype.NewFace(boldFont, &opentype.FaceOptions{Size: pt, DPI: 72, Hinting: font.HintingFull})
		if err != nil {
			return img
		}
		b, _ := font.BoundString(f, text)
		if (b.Max.X-b.Min.X).Ceil() <= size && (b.Max.Y-b.Min.Y).Ceil() <= size*7/8 {
			face, box = f, b
			break
		}
	}
	if face == nil {
		return img
	}
	bw, bh := box.Max.X-box.Min.X, box.Max.Y-box.Min.Y
	d := &font.Drawer{Dst: img, Face: face, Src: image.NewUniform(fg)}
	d.Dot = fixed.Point26_6{X: (fixed.I(size)-bw)/2 - box.Min.X, Y: (fixed.I(size)-bh)/2 - box.Min.Y}
	d.DrawString(text)
	if unread {
		r := float64(size) / 7
		if r < 2 {
			r = 2
		}
		cx, cy := float64(size)-r-0.5, r+0.5
		for py := 0; py < size; py++ {
			for px := 0; px < size; px++ {
				dd := math.Hypot(float64(px)+0.5-cx, float64(py)+0.5-cy)
				switch {
				case dd <= r:
					img.SetRGBA(px, py, colUnread)
				case dd <= r+1:
					img.SetRGBA(px, py, color.RGBA{}) // a clear ring keeps it apart from the digits
				}
			}
		}
	}
	return img
}

var iconSizes = []int{16, 20, 24, 32}

// an .ico with one 32-bit bitmap per size (Windows picks the one for the screen's scaling). Bitmaps,
// not PNG: every Windows icon API reads them at any size
func makeIcon(st *State, lightTaskbar bool) []byte {
	text := iconText(st)
	fg := colOnDark
	if lightTaskbar {
		fg = colOnLight
	}
	if st == nil {
		fg = colNoData
	}
	unread := st != nil && st.Unread > 0
	var imgs [][]byte
	for _, s := range iconSizes {
		imgs = append(imgs, dib(renderIcon(s, text, fg, unread)))
	}
	var out bytes.Buffer
	binary.Write(&out, binary.LittleEndian, []uint16{0, 1, uint16(len(imgs))})
	offset := 6 + 16*len(imgs)
	for i, p := range imgs {
		s := iconSizes[i]
		out.Write([]byte{byte(s), byte(s), 0, 0})
		binary.Write(&out, binary.LittleEndian, []uint16{1, 32})
		binary.Write(&out, binary.LittleEndian, []uint32{uint32(len(p)), uint32(offset)})
		offset += len(p)
	}
	for _, p := range imgs {
		out.Write(p)
	}
	return out.Bytes()
}

// icon image as a DIB: BITMAPINFOHEADER (height doubled for the mask), BGRA rows bottom-up, then the
// 1-bit AND mask (all zero: the alpha channel does the transparency)
func dib(img *image.RGBA) []byte {
	s := img.Bounds().Dx()
	var b bytes.Buffer
	binary.Write(&b, binary.LittleEndian, struct {
		Size                  uint32
		Width, Height         int32
		Planes, BitCount      uint16
		Compression, SizeImg  uint32
		XPels, YPels          int32
		ClrUsed, ClrImportant uint32
	}{40, int32(s), int32(2 * s), 1, 32, 0, 0, 0, 0, 0, 0})
	for y := s - 1; y >= 0; y-- {
		for x := 0; x < s; x++ {
			c := img.RGBAAt(x, y) // image.RGBA is premultiplied: undo it, icons want straight alpha
			r, g, bl := c.R, c.G, c.B
			if c.A > 0 && c.A < 255 {
				r, g, bl = uint8(int(r)*255/int(c.A)), uint8(int(g)*255/int(c.A)), uint8(int(bl)*255/int(c.A))
			}
			b.Write([]byte{bl, g, r, c.A})
		}
	}
	b.Write(make([]byte, ((s+31)/32*4)*s))
	return b.Bytes()
}
