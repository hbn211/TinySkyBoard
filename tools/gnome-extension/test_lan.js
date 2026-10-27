// Tries the extension's receiver outside GNOME Shell: gjs -m test_lan.js CODE [seconds]
import GLib from 'gi://GLib';
import {Receiver} from './tinyskyboard@hbn211.github.io/lan.js';

const [code, secs = '45'] = ARGV;
const loop = new GLib.MainLoop(null, false);
const rx = new Receiver(code, ({state, panels, age}) => {
    print(`panels ${panels}, age ${age === Infinity ? '-' : age.toFixed(0)} s, bad ${rx.bad}`);
    if (state && age < 1)
        print(JSON.stringify(state, null, 1));
});
rx.start();
GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, +secs, () => loop.quit());
loop.run();
rx.stop();
