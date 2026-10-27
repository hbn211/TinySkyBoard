// TinySkyBoard mirror — a GNOME Shell top-bar indicator showing what a panel on the same network
// shows. Receive only (lan.js): it never sends anything back.
// Pairing code (setup page of the panel) in ~/.config/tinyskyboard/code
import GObject from 'gi://GObject';
import GLib from 'gi://GLib';
import St from 'gi://St';
import Clutter from 'gi://Clutter';
import Pango from 'gi://Pango';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {Receiver} from './lan.js';

const DIM = 170;        // opacity of secondary text: follows the shell's light or dark style
const STALE_S = 90;     // no state for this long: "no signal"
const CODE_FILE = GLib.build_filenamev([GLib.get_user_config_dir(), 'tinyskyboard', 'code']);
const VENT_YES = 1, VENT_NO = 2;   // psy_vent_t (main/psychro.h)

// the panel's languages; any other system language gets English
const TEXTS = {
    pt: {
        clear: 'Céu limpo', mostly: 'Quase limpo', partly: 'Parcialmente nublado', overcast: 'Nublado',
        fog: 'Neblina', drizzle: 'Garoa', rain: 'Chuva', showers: 'Pancadas de chuva', snow: 'Neve',
        storm: 'Tempestade', max: 'máx', min: 'mín', chance: 'chuva', now: 'agora', today: 'hoje',
        tomorrow: 'amanhã', ventYes: 'Vale arejar: o ar lá fora está mais seco',
        ventNo: 'Melhor não arejar: lá fora está mais úmido', messages: 'Mensagens', next: 'Próximo',
        mirror: 'Espelho', panel: 'painel', panels: 'painéis', noCode: 'Sem código de pareamento',
        noCodeHint: f => `Copie o código da página de configuração do painel para ${f} e saia e entre de novo na sessão.`,
        noSignal: 'Sem sinal dos painéis', noSignalHint: 'Mesma rede Wi-Fi e mesmo código de pareamento?',
    },
    en: {
        clear: 'Clear sky', mostly: 'Mostly clear', partly: 'Partly cloudy', overcast: 'Overcast',
        fog: 'Fog', drizzle: 'Drizzle', rain: 'Rain', showers: 'Rain showers', snow: 'Snow',
        storm: 'Thunderstorm', max: 'high', min: 'low', chance: 'rain', now: 'now', today: 'today',
        tomorrow: 'tomorrow', ventYes: 'Worth airing out: the air outside is drier',
        ventNo: 'Better not air out: it is more humid outside', messages: 'Messages', next: 'Next',
        mirror: 'Mirror', panel: 'panel', panels: 'panels', noCode: 'No pairing code',
        noCodeHint: f => `Copy the code from the panel's setup page to ${f}, then log out and back in.`,
        noSignal: 'No signal from the panels', noSignalHint: 'Same Wi-Fi network and same pairing code?',
    },
    es: {
        clear: 'Despejado', mostly: 'Casi despejado', partly: 'Parcialmente nublado', overcast: 'Nublado',
        fog: 'Niebla', drizzle: 'Llovizna', rain: 'Lluvia', showers: 'Chubascos', snow: 'Nieve',
        storm: 'Tormenta', max: 'máx', min: 'mín', chance: 'lluvia', now: 'ahora', today: 'hoy',
        tomorrow: 'mañana', ventYes: 'Conviene ventilar: el aire de fuera está más seco',
        ventNo: 'Mejor no ventilar: fuera hay más humedad', messages: 'Mensajes', next: 'Próximo',
        mirror: 'Espejo', panel: 'panel', panels: 'paneles', noCode: 'Sin código de emparejamiento',
        noCodeHint: f => `Copia el código de la página de configuración del panel en ${f} y vuelve a iniciar sesión.`,
        noSignal: 'Sin señal de los paneles', noSignalHint: '¿Misma red Wi-Fi y mismo código de emparejamiento?',
    },
};
const LANG = GLib.get_language_names().map(l => l.slice(0, 2)).find(l => l in TEXTS) ?? 'en';
const L = TEXTS[LANG];

// WMO code (Open-Meteo) -> symbolic icon and text
function wmo(code, day) {
    const n = day ? '' : '-night';
    if (code === 0)
        return [`weather-clear${n}-symbolic`, L.clear];
    if (code <= 2)
        return [`weather-few-clouds${n}-symbolic`, code === 1 ? L.mostly : L.partly];
    if (code === 3)
        return ['weather-overcast-symbolic', L.overcast];
    if (code <= 48)
        return ['weather-fog-symbolic', L.fog];
    if (code <= 57)
        return ['weather-showers-scattered-symbolic', L.drizzle];
    if (code <= 67 || (code >= 80 && code <= 82))
        return ['weather-showers-symbolic', code >= 80 ? L.showers : L.rain];
    if (code <= 86)
        return ['weather-snow-symbolic', L.snow];
    return ['weather-storm-symbolic', L.storm];
}

function hhmm(d) {
    return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}

// "now", "today 08:30", "tomorrow 08:30", "28 Oct 08:30" (in the system language)
function when(epoch) {
    const d = new Date(epoch * 1000), now = new Date();
    if (d <= now)
        return L.now;
    const days = Math.round((new Date(d).setHours(0, 0, 0, 0) - new Date(now).setHours(0, 0, 0, 0)) / 86400000);
    const day = days === 0 ? L.today : days === 1 ? L.tomorrow
        : d.toLocaleDateString(LANG, {day: 'numeric', month: 'short'});
    return `${day} ${hhmm(d)}`;
}

function readCode() {
    try {
        const [ok, bytes] = GLib.file_get_contents(CODE_FILE);
        return ok ? new TextDecoder().decode(bytes).trim() : '';
    } catch (e) {
        return '';
    }
}

function box(styleClass, vertical = false, props = {}) {
    return new St.BoxLayout({
        style_class: styleClass,
        orientation: vertical ? Clutter.Orientation.VERTICAL : Clutter.Orientation.HORIZONTAL,
        ...props,
    });
}

function label(text, styleClass, props = {}) {
    return new St.Label({text, style_class: styleClass, y_align: Clutter.ActorAlign.CENTER, ...props});
}

function dim(actor) {
    actor.opacity = DIM;
    return actor;
}

const Indicator = GObject.registerClass(
class Indicator extends PanelMenu.Button {
    _init() {
        super._init(0.5, 'TinySkyBoard');
        const top = box('tsb-panel');
        this._icon = new St.Icon({icon_name: 'network-offline-symbolic', style_class: 'system-status-icon'});
        this._temp = label('--°', 'tsb-panel-temp');
        this._dot = label('●', 'tsb-unread-dot tsb-panel-dot', {visible: false});
        top.add_child(this._icon);
        top.add_child(this._temp);
        top.add_child(this._dot);
        this.add_child(top);
        this._last = {state: null, panels: 0, age: Infinity};

        const code = readCode();
        if (code) {
            this._rx = new Receiver(code, u => this._update(u));
            this._rx.start();
        }
        this._update(this._last);
    }

    destroy() {
        this._rx?.stop();
        this._rx = null;
        super.destroy();
    }

    _update(u) {
        this._last = u;
        const st = u.age < STALE_S ? u.state : null;
        if (st?.weather) {
            const [icon] = wmo(st.weather.code, st.weather.day);
            this._icon.icon_name = icon;
            this._temp.text = `${Math.round(st.weather.temp)}°`;
        } else {
            this._icon.icon_name = 'network-offline-symbolic';
            this._temp.text = '--°';
        }
        this._icon.opacity = this._temp.opacity = st ? 255 : DIM;
        this._dot.visible = !!st && st.unread > 0;
        this._rebuild(st, u.panels);
    }

    _item(vertical = true) {
        const item = new PopupMenu.PopupBaseMenuItem({reactive: false, can_focus: false});
        const b = box('tsb-section', vertical, {x_expand: true});
        item.add_child(b);
        this.menu.addMenuItem(item);
        return b;
    }

    _rebuild(st, panels) {
        this.menu.removeAll();
        if (!this._rx) {
            const b = this._item();
            b.add_child(label(L.noCode, 'tsb-heading'));
            const t = dim(label(L.noCodeHint(CODE_FILE), 'tsb-small'));
            t.clutter_text.line_wrap = true;
            b.add_child(t);
            return;
        }
        if (!st) {
            const b = this._item();
            b.add_child(label(L.noSignal, 'tsb-heading'));
            const t = dim(label(L.noSignalHint, 'tsb-small'));
            t.clutter_text.line_wrap = true;
            b.add_child(t);
            this._footer(panels);
            return;
        }
        let first = true;
        const sep = () => {
            if (!first)
                this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
            first = false;
        };
        if (st.weather) {
            sep();
            this._weather(st.weather);
        }
        if (st.sensors.length) {
            sep();
            this._sensors(st);
        }
        if (st.messages.length) {
            sep();
            this._messages(st.messages);
        }
        if (st.next) {
            sep();
            this._next(st.next);
        }
        this._footer(panels);
    }

    _weather(w) {
        const [icon, desc] = wmo(w.code, w.day);
        const b = this._item(false);
        b.add_child(new St.Icon({icon_name: icon, icon_size: 48, style_class: 'tsb-wx-icon'}));
        const col = box('', true, {y_align: Clutter.ActorAlign.CENTER});
        col.add_child(label(`${Math.round(w.temp)}°  ${desc}`, 'tsb-wx-temp'));
        const rain = w.rain >= 0 ? `  ·  ${L.chance} ${w.rain}%` : '';
        col.add_child(dim(label(`${L.max} ${w.max}°  ·  ${L.min} ${w.min}°${rain}`, 'tsb-small')));
        b.add_child(col);
    }

    _sensors(st) {
        const b = this._item();
        for (const s of st.sensors) {
            const row = box('tsb-row', false, {x_expand: true});
            row.add_child(label(s.name, 'tsb-name', {x_expand: true}));
            row.add_child(label(`${s.temp.toFixed(1)}°`, 'tsb-value'));
            row.add_child(dim(label(s.rh >= 0 ? `${s.rh}%` : '', 'tsb-value tsb-rh')));
            const arrow = label(s.trend > 0 ? '↑' : s.trend < 0 ? '↓' : '', 'tsb-trend');
            if (s.trend)
                arrow.add_style_class_name(s.trend > 0 ? 'tsb-warm' : 'tsb-cool');
            row.add_child(arrow);
            b.add_child(row);
        }
        if (st.vent === VENT_YES || st.vent === VENT_NO) {
            const hint = box(st.vent === VENT_YES ? 'tsb-vent' : 'tsb-vent tsb-vent-no', false, {x_expand: true});
            hint.add_child(label(st.vent === VENT_YES ? L.ventYes : L.ventNo, 'tsb-small'));
            b.add_child(hint);
        }
    }

    _messages(msgs) {
        const b = this._item();
        b.add_child(dim(label(L.messages, 'tsb-heading')));
        for (const m of msgs) {
            const row = box('tsb-msg', false, {x_expand: true});
            row.add_child(label(m.unread ? '●' : '', 'tsb-unread-dot tsb-msg-dot', {y_align: Clutter.ActorAlign.START}));
            const col = box('', true, {x_expand: true});
            const head = box('', false, {x_expand: true});
            head.add_child(label(m.from, m.unread ? 'tsb-from tsb-bold' : 'tsb-from', {x_expand: true}));
            head.add_child(dim(label(hhmm(new Date(m.when * 1000)), 'tsb-small')));
            col.add_child(head);
            const text = label(m.text, 'tsb-text');
            text.clutter_text.line_wrap = true;
            text.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;
            col.add_child(text);
            row.add_child(col);
            b.add_child(row);
        }
    }

    _next(n) {
        const b = this._item();
        b.add_child(dim(label(L.next, 'tsb-heading')));
        const row = box('tsb-row', false, {x_expand: true});
        const what = label(n.text, 'tsb-name', {x_expand: true});
        what.clutter_text.ellipsize = Pango.EllipsizeMode.END;
        row.add_child(what);
        row.add_child(label(when(n.when), 'tsb-when'));
        b.add_child(row);
    }

    _footer(panels) {
        const b = this._item(false);
        b.add_child(dim(label(`${L.mirror} · ${panels} ${panels === 1 ? L.panel : L.panels}`, 'tsb-footer',
            {x_expand: true})));
    }
});

export default class TinySkyBoardExtension extends Extension {
    enable() {
        this._indicator = new Indicator();
        Main.panel.addToStatusArea(this.uuid, this._indicator);
    }

    disable() {
        this._indicator?.destroy();
        this._indicator = null;
    }
}
