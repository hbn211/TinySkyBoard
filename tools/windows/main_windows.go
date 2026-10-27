//go:build windows

// TinySkyBoard in the Windows notification area: the temperature as the icon, the panel in its menu.
// A mirror: it only listens to the panels on the same network (lan.go). Settings and the log in
// %APPDATA%\TinySkyBoard.
package main

import (
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"fyne.io/systray"
	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

var version = "dev"

const (
	maxSensors  = 4
	maxMessages = 2
	runKey      = `Software\Microsoft\Windows\CurrentVersion\Run`
	runValue    = "TinySkyBoard"
	themeKey    = `Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`
)

var (
	t       Texts
	dataDir string
	rx      *Receiver
	started = time.Now()
	logged  Stats

	mWeather, mWeatherInfo, mVent, mNext, mFooter, mHint *systray.MenuItem
	mSensors                                             [maxSensors]*systray.MenuItem
	mMessages                                            [maxMessages]*systray.MenuItem
	mCode, mAuto, mLog, mQuit                            *systray.MenuItem
	lastIcon                                             string
	refreshMu                                            sync.Mutex
	firstRun                                             bool
)

func main() {
	t = texts[uiLanguage()]
	name, _ := windows.UTF16PtrFromString(`Local\TinySkyBoardTray`)
	h, err := windows.CreateMutex(nil, false, name)
	if err == windows.ERROR_ALREADY_EXISTS {
		msgBox(t.AlreadyRunning, false)
		return
	}
	defer windows.CloseHandle(h)
	base, _ := os.UserConfigDir()
	dataDir = filepath.Join(base, "TinySkyBoard")
	os.MkdirAll(dataDir, 0o700)
	_, err = os.Stat(filepath.Join(dataDir, "log.txt"))
	firstRun = err != nil
	openLog()
	log.Printf("start, version %s, %s, language %s", version, exePath(), uiLanguage())
	systray.Run(onReady, func() { log.Print("quit") })
}

func onReady() {
	systray.SetIcon(makeIcon(nil, lightTaskbar()))
	systray.SetTooltip("TinySkyBoard")
	mWeather = systray.AddMenuItem("", "")
	mWeatherInfo = systray.AddMenuItem("", "")
	for i := range mSensors {
		mSensors[i] = systray.AddMenuItem("", "")
	}
	mVent = systray.AddMenuItem("", "")
	for i := range mMessages {
		mMessages[i] = systray.AddMenuItem("", "")
	}
	mNext = systray.AddMenuItem("", "")
	mHint = systray.AddMenuItem("", "")
	mHint.Disable()
	mFooter = systray.AddMenuItem("", "")
	mFooter.Disable()
	systray.AddSeparator()
	mCode = systray.AddMenuItem(t.MenuCode, "")
	mAuto = systray.AddMenuItemCheckbox(t.MenuAutostart, "", autostartOn())
	mLog = systray.AddMenuItem(t.MenuLog, "")
	mQuit = systray.AddMenuItem(t.MenuQuit, "")

	code := readCode()
	if code == "" {
		code = askCode("")
	}
	if code != "" {
		startReceiver(code)
	}
	refresh()
	if firstRun {
		msgBox(t.Welcome, false)
	}

	go func() {
		for {
			select {
			case <-mCode.ClickedCh:
				if c := askCode(readCode()); c != "" {
					startReceiver(c)
					refresh()
				}
			case <-mAuto.ClickedCh:
				setAutostart(!mAuto.Checked())
			case <-mLog.ClickedCh:
				exec.Command("notepad.exe", filepath.Join(dataDir, "log.txt")).Start()
			case <-mQuit.ClickedCh:
				if rx != nil {
					rx.Stop()
				}
				systray.Quit()
				return
			}
		}
	}()
	go func() {
		for range time.Tick(5 * time.Second) {
			refresh()
		}
	}()
}

func startReceiver(code string) {
	if rx != nil {
		rx.Stop()
	}
	rx = NewReceiver(code)
	rx.OnUpdate = refresh
	started = time.Now()
	if err := rx.Start(); err != nil {
		log.Printf("can't listen on UDP %d: %v", lanPort, err)
		msgBox(t.PortBusy+err.Error(), true)
		return
	}
	log.Printf("listening on UDP %d", lanPort)
}

func show(m *systray.MenuItem, text string) {
	if text == "" {
		m.Hide()
		return
	}
	m.SetTitle(strings.ReplaceAll(text, "&", "&&"))
	m.Show()
}

// menu, tooltip and icon from the last state; and why nothing arrives, if that's the case
func refresh() {
	refreshMu.Lock()
	defer refreshMu.Unlock()
	var st *State
	var panels int
	var s Stats
	if rx != nil {
		st, panels, s = rx.Snapshot()
	}
	l := t.Lines(st, panels)
	if rx == nil {
		l.Weather, l.Tooltip = t.NoCode, "TinySkyBoard — "+t.NoCode
	}
	show(mWeather, l.Weather)
	show(mWeatherInfo, l.WeatherInfo)
	for i, m := range mSensors {
		s := ""
		if i < len(l.Sensors) {
			s = l.Sensors[i]
		}
		show(m, s)
	}
	show(mVent, l.Vent)
	for i, m := range mMessages {
		s := ""
		if i < len(l.Messages) {
			s = l.Messages[i]
		}
		show(m, s)
	}
	show(mNext, l.Next)
	hint := ""
	if rx != nil && st == nil {
		switch {
		case s.Good == 0 && s.Bad > 0:
			hint = t.HintWrongCode
		case s.Raw == 0 && time.Since(started) > 45*time.Second:
			hint = t.HintNoPackets
		case s.Good == 0:
			hint = t.HintWait
		}
	}
	show(mHint, clip(hint, 110))
	show(mFooter, l.Footer)
	systray.SetTooltip(l.Tooltip)
	key := fmt.Sprintf("%s|%v|%v|%v", iconText(st), st == nil, st != nil && st.Unread > 0, lightTaskbar())
	if key != lastIcon {
		lastIcon = key
		systray.SetIcon(makeIcon(st, lightTaskbar()))
	}
	if s.Raw != logged.Raw && (logged.Raw == 0 || s.Raw-logged.Raw >= 100) || s.Bad > 0 && logged.Bad == 0 ||
		s.Old > logged.Old {
		log.Printf("packets %d: taken %d, another code %d, replayed %d; panels %d; this clock %+d s from the panel's",
			s.Raw, s.Good, s.Bad, s.Old, panels, s.Skew)
		logged = s
	}
}

// ---- settings

func readCode() string {
	b, err := os.ReadFile(filepath.Join(dataDir, "code.txt"))
	if err != nil {
		return ""
	}
	return NormalizeCode(string(b))
}

// asks until it gets a valid code (saved) or the person cancels ("")
func askCode(prev string) string {
	for {
		in, ok := inputBox(t.AskCode, prev)
		if !ok || strings.TrimSpace(in) == "" {
			return ""
		}
		c := NormalizeCode(in)
		if ValidCode(c) {
			os.WriteFile(filepath.Join(dataDir, "code.txt"), []byte(c+"\r\n"), 0o600)
			log.Print("pairing code saved")
			return c
		}
		msgBox(t.BadCode, true)
		prev = in
	}
}

// a text box from .NET through PowerShell (on every Windows 10 and 11): no GUI toolkit needed
func inputBox(prompt, def string) (string, bool) {
	q := func(s string) string { return "'" + strings.ReplaceAll(s, "'", "''") + "'" }
	script := "Add-Type -AssemblyName Microsoft.VisualBasic;" +
		"$r=[Microsoft.VisualBasic.Interaction]::InputBox(" + q(prompt) + ",'TinySkyBoard'," + q(def) + ");" +
		"[Console]::Out.Write($r)"
	cmd := exec.Command("powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000} // CREATE_NO_WINDOW
	out, err := cmd.Output()
	if err != nil {
		log.Printf("input box: %v", err)
		return "", false
	}
	return strings.TrimSpace(string(out)), true
}

func msgBox(text string, warn bool) {
	flags := uint32(windows.MB_OK | windows.MB_ICONINFORMATION | windows.MB_SETFOREGROUND)
	if warn {
		flags = windows.MB_OK | windows.MB_ICONWARNING | windows.MB_SETFOREGROUND
	}
	tp, _ := windows.UTF16PtrFromString(text)
	cp, _ := windows.UTF16PtrFromString("TinySkyBoard")
	windows.MessageBox(0, tp, cp, flags)
}

func exePath() string {
	p, _ := os.Executable()
	return p
}

func autostartOn() bool {
	k, err := registry.OpenKey(registry.CURRENT_USER, runKey, registry.QUERY_VALUE)
	if err != nil {
		return false
	}
	defer k.Close()
	v, _, err := k.GetStringValue(runValue)
	return err == nil && strings.EqualFold(strings.Trim(v, `"`), exePath())
}

func setAutostart(on bool) {
	k, err := registry.OpenKey(registry.CURRENT_USER, runKey, registry.SET_VALUE)
	if err != nil {
		log.Printf("autostart: %v", err)
		return
	}
	defer k.Close()
	if on {
		err = k.SetStringValue(runValue, `"`+exePath()+`"`)
	} else {
		err = k.DeleteValue(runValue)
	}
	if err != nil {
		log.Printf("autostart %v: %v", on, err)
	}
	if autostartOn() {
		mAuto.Check()
	} else {
		mAuto.Uncheck()
	}
	log.Printf("start with Windows: %v", autostartOn())
}

// the taskbar follows "system" mode (light or dark), not the apps' mode
func lightTaskbar() bool {
	k, err := registry.OpenKey(registry.CURRENT_USER, themeKey, registry.QUERY_VALUE)
	if err != nil {
		return false
	}
	defer k.Close()
	v, _, err := k.GetIntegerValue("SystemUsesLightTheme")
	return err == nil && v == 1
}

func uiLanguage() string {
	langs, err := windows.GetUserPreferredUILanguages(windows.MUI_LANGUAGE_NAME)
	if err == nil {
		for _, l := range langs {
			l = strings.ToLower(l)
			for _, c := range []string{"pt", "es", "en"} {
				if strings.HasPrefix(l, c) {
					return c
				}
			}
		}
	}
	return "en"
}

// log.txt, started over when it passes 256 KB
func openLog() {
	p := filepath.Join(dataDir, "log.txt")
	if fi, err := os.Stat(p); err == nil && fi.Size() > 256*1024 {
		os.Remove(p)
	}
	f, err := os.OpenFile(p, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err == nil {
		log.SetOutput(f)
	}
}
