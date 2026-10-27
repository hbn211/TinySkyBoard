package main

import (
	"fmt"
	"math"
	"time"
)

// Texts in the panel's languages; any other system language gets English
type Texts struct {
	Clear, Mostly, Partly, Overcast, Fog, Drizzle, Rain, Showers, Snow, Storm string
	Max, Min, Chance, Now, Today, Tomorrow                                    string
	VentYes, VentNo, Unread, Next, Mirror, Panel, Panels                      string
	NoCode, NoSignal, HintNoPackets, HintWrongCode, HintWait                  string
	MenuCode, MenuAutostart, MenuLog, MenuQuit                                string
	AskCode, BadCode, AlreadyRunning, PortBusy, Welcome                       string
}

var texts = map[string]Texts{
	"pt": {
		Clear: "Céu limpo", Mostly: "Quase limpo", Partly: "Parcialmente nublado", Overcast: "Nublado",
		Fog: "Neblina", Drizzle: "Garoa", Rain: "Chuva", Showers: "Pancadas de chuva", Snow: "Neve",
		Storm: "Tempestade", Max: "máx", Min: "mín", Chance: "chuva", Now: "agora", Today: "hoje",
		Tomorrow: "amanhã", VentYes: "Vale arejar: o ar lá fora está mais seco",
		VentNo: "Melhor não arejar: lá fora está mais úmido", Unread: "não lida(s)", Next: "Próximo",
		Mirror: "Espelho", Panel: "painel", Panels: "painéis",
		NoCode:        "Sem código de pareamento",
		NoSignal:      "Sem sinal dos painéis",
		HintNoPackets: "Nada chega da rede: mesma rede Wi-Fi? Firewall do Windows: permita o TinySkyBoard em redes privadas, e a rede Wi-Fi deve estar como Privada",
		HintWrongCode: "O painel está na rede, mas com outro código: confira o código de pareamento",
		HintWait:      "Esperando o painel (até 30 s)…",
		MenuCode:      "Trocar o código de pareamento…", MenuAutostart: "Iniciar com o Windows",
		MenuLog: "Abrir o registro", MenuQuit: "Sair",
		AskCode:        "Código de pareamento do painel: está na página de configuração do painel, seção Computador e outros painéis. Só precisa trocar se você gerou um código novo lá.",
		BadCode:        "Isso não é um código de pareamento (8 letras e números, como K7PM-3QXD).",
		AlreadyRunning: "O TinySkyBoard já está aberto: veja o ícone perto do relógio (talvez na setinha ^).",
		PortBusy:       "A porta de rede 47474 está ocupada por outro programa. Detalhe: ",
		Welcome:        "O TinySkyBoard fica perto do relógio da barra de tarefas (talvez na setinha ^). Clique no ícone para ver o painel.\n\nSe o Windows perguntar sobre o firewall, permita em redes privadas.",
	},
	"en": {
		Clear: "Clear sky", Mostly: "Mostly clear", Partly: "Partly cloudy", Overcast: "Overcast",
		Fog: "Fog", Drizzle: "Drizzle", Rain: "Rain", Showers: "Rain showers", Snow: "Snow",
		Storm: "Thunderstorm", Max: "high", Min: "low", Chance: "rain", Now: "now", Today: "today",
		Tomorrow: "tomorrow", VentYes: "Worth airing out: the air outside is drier",
		VentNo: "Better not air out: it is more humid outside", Unread: "unread", Next: "Next",
		Mirror: "Mirror", Panel: "panel", Panels: "panels",
		NoCode:        "No pairing code",
		NoSignal:      "No signal from the panels",
		HintNoPackets: "Nothing arrives from the network: same Wi-Fi network? Windows Firewall: allow TinySkyBoard on private networks, and the Wi-Fi network must be set to Private",
		HintWrongCode: "The panel is on the network, but with another code: check the pairing code",
		HintWait:      "Waiting for the panel (up to 30 s)…",
		MenuCode:      "Change the pairing code…", MenuAutostart: "Start with Windows",
		MenuLog: "Open the log", MenuQuit: "Quit",
		AskCode:        "The panel's pairing code: it is on the panel's setup page, Computer and other panels section. Only needed again if you made a new code there.",
		BadCode:        "That isn't a pairing code (8 letters and digits, like K7PM-3QXD).",
		AlreadyRunning: "TinySkyBoard is already open: look for the icon by the clock (maybe under the ^ arrow).",
		PortBusy:       "Network port 47474 is taken by another program. Details: ",
		Welcome:        "TinySkyBoard sits by the taskbar clock (maybe under the ^ arrow). Click the icon to see the panel.\n\nIf Windows asks about the firewall, allow it on private networks.",
	},
	"es": {
		Clear: "Despejado", Mostly: "Casi despejado", Partly: "Parcialmente nublado", Overcast: "Nublado",
		Fog: "Niebla", Drizzle: "Llovizna", Rain: "Lluvia", Showers: "Chubascos", Snow: "Nieve",
		Storm: "Tormenta", Max: "máx", Min: "mín", Chance: "lluvia", Now: "ahora", Today: "hoy",
		Tomorrow: "mañana", VentYes: "Conviene ventilar: el aire de fuera está más seco",
		VentNo: "Mejor no ventilar: fuera hay más humedad", Unread: "sin leer", Next: "Próximo",
		Mirror: "Espejo", Panel: "panel", Panels: "paneles",
		NoCode:        "Sin código de emparejamiento",
		NoSignal:      "Sin señal de los paneles",
		HintNoPackets: "No llega nada de la red: ¿misma red Wi-Fi? Firewall de Windows: permite TinySkyBoard en redes privadas, y la red Wi-Fi debe estar como Privada",
		HintWrongCode: "El panel está en la red, pero con otro código: revisa el código de emparejamiento",
		HintWait:      "Esperando el panel (hasta 30 s)…",
		MenuCode:      "Cambiar el código de emparejamiento…", MenuAutostart: "Iniciar con Windows",
		MenuLog: "Abrir el registro", MenuQuit: "Salir",
		AskCode:        "Código de emparejamiento del panel: está en la página de configuración del panel, sección Ordenador y otros paneles. Solo hace falta cambiarlo si generaste un código nuevo allí.",
		BadCode:        "Eso no es un código de emparejamiento (8 letras y números, como K7PM-3QXD).",
		AlreadyRunning: "TinySkyBoard ya está abierto: busca el icono junto al reloj (quizá bajo la flecha ^).",
		PortBusy:       "El puerto de red 47474 está ocupado por otro programa. Detalle: ",
		Welcome:        "TinySkyBoard está junto al reloj de la barra de tareas (quizá bajo la flecha ^). Haz clic en el icono para ver el panel.\n\nSi Windows pregunta por el firewall, permítelo en redes privadas.",
	},
}

// WMO code (Open-Meteo) -> text
func (t Texts) Weather(code int) string {
	switch {
	case code == 0:
		return t.Clear
	case code == 1:
		return t.Mostly
	case code == 2:
		return t.Partly
	case code == 3:
		return t.Overcast
	case code <= 48:
		return t.Fog
	case code <= 57:
		return t.Drizzle
	case code <= 67:
		return t.Rain
	case code >= 80 && code <= 82:
		return t.Showers
	case code <= 86:
		return t.Snow
	}
	return t.Storm
}

// "now", "today 08:30", "tomorrow 08:30", "28/10 08:30"
func (t Texts) When(epoch int64) string {
	d, now := time.Unix(epoch, 0), time.Now()
	if !d.After(now) {
		return t.Now
	}
	y1, m1, d1 := now.Date()
	y2, m2, d2 := d.Date()
	days := int(time.Date(y2, m2, d2, 0, 0, 0, 0, time.Local).Sub(time.Date(y1, m1, d1, 0, 0, 0, 0, time.Local)).Hours()/24 + 0.5)
	day := fmt.Sprintf("%02d/%02d", d.Day(), int(d.Month()))
	if days == 0 {
		day = t.Today
	} else if days == 1 {
		day = t.Tomorrow
	}
	return day + " " + d.Format("15:04")
}

// trims to n characters (runes) with an ellipsis
func clip(s string, n int) string {
	r := []rune(s)
	if len(r) <= n {
		return s
	}
	return string(r[:n-1]) + "…"
}

// the menu's lines for a state; the same for Windows and the Linux test
type Lines struct {
	Weather, WeatherInfo string
	Sensors              []string
	Vent                 string
	Messages             []string
	Next                 string
	Footer               string
	Tooltip              string
}

func (t Texts) Lines(st *State, panels int) Lines {
	var l Lines
	n := t.Panels
	if panels == 1 {
		n = t.Panel
	}
	l.Footer = fmt.Sprintf("%s · %d %s", t.Mirror, panels, n)
	if st == nil {
		l.Weather = t.NoSignal
		l.Tooltip = "TinySkyBoard — " + t.NoSignal
		return l
	}
	l.Tooltip = "TinySkyBoard"
	if w := st.Weather; w != nil {
		l.Weather = fmt.Sprintf("%d°  %s", int(math.Round(w.Temp)), t.Weather(w.Code))
		l.WeatherInfo = fmt.Sprintf("%s %d°  ·  %s %d°", t.Max, w.Max, t.Min, w.Min)
		if w.Rain >= 0 {
			l.WeatherInfo += fmt.Sprintf("  ·  %s %d%%", t.Chance, w.Rain)
		}
		l.Tooltip = fmt.Sprintf("TinySkyBoard — %s", l.Weather)
	}
	for _, s := range st.Sensors {
		arrow := map[int]string{1: " ↑", -1: " ↓"}[s.Trend]
		line := fmt.Sprintf("%s:  %.1f°", s.Name, s.Temp)
		if s.RH >= 0 {
			line += fmt.Sprintf("  %d%%", s.RH)
		}
		l.Sensors = append(l.Sensors, clip(line+arrow, 60))
	}
	switch st.Vent {
	case 1:
		l.Vent = t.VentYes
	case 2:
		l.Vent = t.VentNo
	}
	for _, m := range st.Messages {
		dot := "    "
		if m.Unread {
			dot = "●  "
		}
		l.Messages = append(l.Messages, clip(fmt.Sprintf("%s%s %s — %s", dot, m.From, time.Unix(m.When, 0).Format("15:04"), m.Text), 70))
	}
	if st.Next != nil {
		l.Next = clip(fmt.Sprintf("%s: %s — %s", t.Next, st.Next.Text, t.When(st.Next.When)), 70)
	}
	if st.Unread > 0 {
		l.Tooltip += fmt.Sprintf(" · %d %s", st.Unread, t.Unread)
	}
	if len([]rune(l.Tooltip)) > 120 {
		l.Tooltip = clip(l.Tooltip, 120)
	}
	return l
}
