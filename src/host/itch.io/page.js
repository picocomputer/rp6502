/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The page around the emulator, built from CONFIG in index.html. --pre-js
 * puts this file at the top of rp6502.js, where it shares the top-level scope,
 * so all of it is inside one function. It uses only Module fields, which also
 * lets it run as a plain script in front of an rp6502.js built without it.
 */

(function () {
    // A page from 0.34 or earlier sets up Module itself.
    if (Module.arguments)
        return;
    document.title = CONFIG.title || 'Picocomputer 6502';

    const canvas = document.createElement('canvas');
    canvas.id = 'canvas';
    canvas.tabIndex = 0;
    Module.canvas = canvas;
    const msg = document.createElement('div');
    msg.id = 'rp6502-msg';
    let overlay = null;

    function show(text) {
        msg.textContent = text;
        msg.style.display = 'grid';
    }
    function fail(text) {
        overlay?.remove();
        show(text);
    }
    addEventListener('error', (e) => fail('Emulator crashed: ' + e.message));
    addEventListener('unhandledrejection', (e) =>
        fail('Emulator crashed: ' + (e.reason?.message || e.reason)));

    // The <style> of index.html comes after this one, so its rules take
    // precedence.
    function frame() {
        const style = document.createElement('style');
        style.textContent = `
html, body { height: 100%; margin: 0; overflow: hidden; overscroll-behavior: none; background: #000; }
body { display: flex; flex-direction: column; }
#rp6502 { position: relative; flex: 1 1 0; min-height: 0;
  background: center / contain no-repeat; image-rendering: pixelated; }
#rp6502 > * { image-rendering: auto; }
#canvas { position: absolute; inset: 0; width: 100%; height: 100%; display: block;
  outline: none; touch-action: none; }
#rp6502-msg { position: absolute; inset: 0; z-index: 1; display: none; place-items: center;
  padding: 1em; text-align: center; color: #c7d0d9; background: rgba(0, 0, 0, .85);
  font: 14px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
#rp6502-msg pre { max-width: 92vw; max-height: 92vh; overflow: auto; margin: 0; text-align: left;
  font: 12px/1.35 ui-monospace, SFMono-Regular, Menlo, monospace; }`;
        document.head.prepend(style);
        const box = document.createElement('div');
        box.id = 'rp6502';
        box.append(canvas, msg);
        document.body.prepend(box);
        return box;
    }
    function parsed(build) {
        return new Promise((resolve) => {
            const ready = () => {
                build();
                resolve();
            };
            if (document.readyState === 'loading')
                document.addEventListener('DOMContentLoaded', ready);
            else
                ready();
        });
    }
    function template(id) {
        return document.getElementById(id).content.firstElementChild.cloneNode(true);
    }
    function wait(id, promise) {
        Module.addRunDependency(id);
        promise.then(() => Module.removeRunDependency(id));
    }

    // With ?credits, the emulator prints the license notice of every bundled
    // component.
    if (new URLSearchParams(location.search).has('credits')) {
        const pre = document.createElement('pre');
        Module.arguments = ['--credits'];
        Module.print = (text) => {
            pre.textContent += text + '\n';
            msg.replaceChildren(pre);
            msg.style.display = 'grid';
        };
        parsed(frame);
        return;
    }

    const bad = CONFIG.bg && !/^[0-9a-fA-F]{6}$/.test(CONFIG.bg)
        ? 'CONFIG.bg must be six hex digits, such as 000000.'
        : CONFIG.filter && !['nearest', 'linear', 'sharp'].includes(CONFIG.filter)
        ? 'CONFIG.filter must be nearest, linear or sharp.'
        : '';
    if (bad) {
        Module.noInitialRun = true;
        show(bad);
        parsed(frame);
        return;
    }

    // The ROM and each file in CONFIG.install are written to /roms under
    // the last part of their URL.
    const fileName = (url) => url.split(/[?#]/)[0].split('/').pop();
    const install = CONFIG.install || [];
    Module.arguments = ['--save-dir', '/saves'];
    if (CONFIG.bg)
        Module.arguments.push('--bgcolor', CONFIG.bg);
    if (CONFIG.filter)
        Module.arguments.push('--filter', CONFIG.filter);
    for (const url of install)
        Module.arguments.push('--install', '/roms/' + fileName(url));
    Module.arguments.push('/roms/' + fileName(CONFIG.rom));
    if (CONFIG.args)
        Module.arguments.push('--', ...CONFIG.args);

    // A failed fetch never settles, so the start waits and the message stays.
    const files = [CONFIG.rom, ...install].map((url) => fetch(url).then((r) => {
        if (!r.ok)
            throw new Error('HTTP ' + r.status);
        return r.arrayBuffer();
    }).then((buf) => [fileName(url), buf]).catch((e) => {
        fail(`Could not load ${url} (${e.message}).`);
        return new Promise(() => {});
    }));

    // Sound starts with the click on the overlay. sokol resumes a suspended
    // context by itself, so the connection to the speakers is held back
    // instead; sokol makes it once, at start.
    const connect = AudioNode.prototype.connect;
    const deferred = [];
    let play = null;
    let clicked = Promise.resolve();
    if (CONFIG.overlay) {
        AudioNode.prototype.connect = function (dest) {
            if (dest instanceof AudioDestinationNode) {
                deferred.push([this, arguments]);
                return dest;
            }
            return connect.apply(this, arguments);
        };
        clicked = new Promise((resolve) => {
            play = () => {
                removeEventListener('keydown', play);
                AudioNode.prototype.connect = connect;
                for (const [node, args] of deferred.splice(0))
                    connect.apply(node, args);
                if (Module._saudio_context)
                    Module._saudio_context.resume();
                else {
                    // WebKit leaves a context made outside a click suspended,
                    // so the context sokol makes later is made in the click.
                    const made = new AudioContext({sampleRate: 48000, latencyHint: 'interactive'});
                    const native = AudioContext;
                    window.AudioContext = function () {
                        window.AudioContext = native;
                        return made;
                    };
                }
                overlay.remove();
                canvas.focus({preventScroll: true});
                resolve();
            };
        });
    }

    const built = parsed(() => {
        const box = frame();
        if (CONFIG.bg)
            document.body.style.background = '#' + CONFIG.bg;
        if (CONFIG.image)
            box.style.backgroundImage = `url("${CONFIG.image}")`;
        if (CONFIG.filter === 'nearest')
            canvas.style.imageRendering = 'pixelated';
        if (CONFIG.overlay) {
            overlay = template(CONFIG.overlay);
            overlay.addEventListener('click', play);
            addEventListener('keydown', play);
            box.append(overlay);
        }
        if (CONFIG.footer)
            box.after(template(CONFIG.footer));
        // sokol measures the canvas only on a window resize, and the footer
        // can change the canvas height without one.
        new ResizeObserver(() => dispatchEvent(new Event('resize'))).observe(canvas);
    });
    canvas.addEventListener('pointerdown', () => canvas.focus({preventScroll: true}));

    // SAVE: opens files in /saves. With CONFIG.db, /saves is IDBFS, stored in
    // the IndexedDB database named by CONFIG.db. IDBFS names a database after
    // its mountpoint and keys a file by its full path, so the name is swapped
    // and each key is the file's path inside /saves, as every existing
    // database holds it.
    function mountSaves(done) {
        const {getDB, getRemoteSet, loadRemoteEntry, storeRemoteEntry, removeRemoteEntry, syncfs} = Module.IDBFS;
        const key = (path) => path.slice('/saves/'.length);
        Module.IDBFS.getDB = (name, cb) => getDB(name === '/saves' ? CONFIG.db : name, cb);
        Module.IDBFS.getRemoteSet = (mount, cb) => getRemoteSet(mount, (err, remote) => {
            if (remote?.entries) {
                const entries = {};
                for (const k in remote.entries)
                    entries['/saves/' + k] = remote.entries[k];
                remote.entries = entries;
            }
            cb(err, remote);
        });
        Module.IDBFS.loadRemoteEntry = (store, path, cb) => loadRemoteEntry(store, key(path), cb);
        Module.IDBFS.storeRemoteEntry = (store, path, entry, cb) => storeRemoteEntry(store, key(path), entry, cb);
        Module.IDBFS.removeRemoteEntry = (store, path, cb) => removeRemoteEntry(store, key(path), cb);
        // autoPersist drops the error of each sync it starts.
        Module.IDBFS.syncfs = (mount, populate, cb) => syncfs(mount, populate, (err) => {
            if (err)
                console.error('IDBFS:', err);
            cb(err);
        });
        // autoPersist queues a sync after a close that wrote and after a
        // create, unlink, rename or rmdir. A sync stores only entries whose
        // mtime changed, and MEMFS changes it only on a write, so a truncate
        // or a chmod marks the node modified for the close to store it.
        const ops = Module.FS.mount(Module.IDBFS, {autoPersist: true}, '/saves').node_ops;
        const setattr = ops.setattr;
        ops.setattr = (node, attr) => {
            const changed = (attr.size !== undefined && attr.size !== node.usedBytes) ||
                            (attr.mode !== undefined && attr.mode !== node.mode);
            setattr(node, attr);
            if (changed) {
                node.mtime = Date.now();
                node.isModified = true;
            }
        };
        Module.FS.syncfs(true, () => {
            // Best effort when the tab is hidden or closed. A sync started
            // during the load above would store the empty folder over the
            // saves, so this waits until the load is done.
            let syncing = false;
            const flush = () => {
                if (syncing)
                    return;
                syncing = true;
                Module.FS.syncfs(false, () => { syncing = false; });
            };
            addEventListener('pagehide', flush);
            document.addEventListener('visibilitychange', () => {
                if (document.visibilityState === 'hidden')
                    flush();
            });
            done();
        });
    }

    // Each window loads a separate copy of the database, and a sync makes the
    // database match that copy, so two windows on one database would undo
    // each other's saves. The lock is held until the window closes.
    function saves() {
        return new Promise((resolve) => {
            const locked = () => {
                mountSaves(resolve);
                return new Promise(() => {});
            };
            if (!navigator.locks) {
                mountSaves(resolve);
                return;
            }
            navigator.locks.request(CONFIG.db, {ifAvailable: true}, (lock) => {
                if (lock)
                    return locked();
                show('This game is running in another window.');
                return navigator.locks.request(CONFIG.db, () => {
                    msg.style.display = 'none';
                    return locked();
                });
            }).catch((e) => {
                // Where the browser blocks storage, the lock request fails
                // too, and the saves last until the page closes.
                console.error(e);
                mountSaves(resolve);
            });
        });
    }
    if (CONFIG.db)
        navigator.storage?.persist().catch(console.error);

    const errTail = [];
    Module.printErr = (text) => {
        console.error(text);
        errTail.push(text);
        if (errTail.length > 12)
            errTail.shift();
    };
    Module.preRun = () => {
        Module.FS.mkdir('/roms');
        Module.FS.mkdir('/saves');
        wait('dom', built);
        wait('roms', Promise.all(files).then((list) => {
            for (const [name, buf] of list)
                Module.FS.writeFile('/roms/' + name, new Uint8Array(buf));
        }));
        if (CONFIG.image)
            wait('hold', clicked);
        if (CONFIG.db)
            wait('saves', saves());
    };
    Module.postRun = () => {
        // sokol makes a WebGL context on the canvas once startup gets as far
        // as sapp_run, so a 2D context means startup failed, with the reason
        // on stderr.
        if (canvas.getContext('2d'))
            fail('Emulator failed to start: ' + (errTail.join(' · ') || 'see the browser console'));
    };
    Module.onAbort = (what) => fail(String(what));
    Module.onRuntimeInitialized = gamepads;

    // Nothing reads navigator.getGamepads() until the program maps the gamepad
    // report block into XRAM, and the polling stops when it unmaps it.
    function gamepads() {
        const mapped = Module._gamepad_mapped;
        const report = Module._gamepad_host;
        const disconnect = Module._gamepad_disconnect;
        let polling = false, raf = 0, watch = 0;
        const connected = [false, false, false, false];

        const clamp = (v, lo, hi) => v < lo ? lo : (v > hi ? hi : v);
        const axis = (v) => clamp(Math.round((v || 0) * 127), -128, 127);
        const trig = (btn) => btn ? clamp(Math.round(btn.value * 255), 0, 255) : 0;
        // Face button labels: 3 PlayStation, 2 Eastern BA, 1 Western AB, 0 when
        // the id does not identify the pad or buttons[] is in the device's order.
        function padType(gp) {
            if (gp.mapping !== 'standard')
                return 0;
            if (/054c|0ce6|dualshock|dualsense|playstation|sony/i.test(gp.id))
                return 3;
            // Pro Controller or Joy-Con pair; a lone Joy-Con held sideways fits no row.
            if (/057e\D+20(09|0e)/i.test(gp.id))
                return 2;
            if (/045e|xbox|xinput/i.test(gp.id))
                return 1;
            return 0;
        }

        function poll() {
            if (!mapped()) {
                stop();
                return;
            }
            const pads = navigator.getGamepads ? navigator.getGamepads() : [];
            for (let p = 0; p < 4; p++) {
                const gp = pads[p];
                if (!gp) {
                    if (connected[p]) {
                        disconnect(p);
                        connected[p] = false;
                    }
                    continue;
                }
                const b = gp.buttons, a = gp.axes;
                const on = (i) => (b[i] && b[i].pressed) ? 1 : 0;
                // The bits of the gamepad report in the RP6502-RIA docs.
                const dpad = (on(12) ? 0x01 : 0) | (on(13) ? 0x02 : 0) |
                             (on(14) ? 0x04 : 0) | (on(15) ? 0x08 : 0);
                let b0 = (on(0) ? 0x01 : 0) | (on(1) ? 0x02 : 0) | (on(2) ? 0x08 : 0) |
                         (on(3) ? 0x10 : 0) | (on(4) ? 0x40 : 0) | (on(5) ? 0x80 : 0);
                const type = padType(gp);
                // The standard mapping places Nintendo face buttons by position.
                if (type === 2)
                    b0 = (b0 & 0xE4) | (b0 & 0x09) << 1 | (b0 & 0x12) >> 1;
                const b1 = (on(6) ? 0x01 : 0) | (on(7) ? 0x02 : 0) | (on(8) ? 0x04 : 0) |
                           (on(9) ? 0x08 : 0) | (on(16) ? 0x10 : 0) | (on(10) ? 0x20 : 0) |
                           (on(11) ? 0x40 : 0);
                report(p, dpad, b0, b1, axis(a[0]), axis(a[1]), axis(a[2]), axis(a[3]),
                       trig(b[6]), trig(b[7]), type, a.length >= 4 ? 1 : 0);
                connected[p] = true;
            }
            raf = requestAnimationFrame(poll);
        }
        function start() {
            if (!polling) {
                polling = true;
                raf = requestAnimationFrame(poll);
            }
        }
        function stop() {
            polling = false;
            if (raf) {
                cancelAnimationFrame(raf);
                raf = 0;
            }
            for (let p = 0; p < 4; p++)
                if (connected[p]) {
                    disconnect(p);
                    connected[p] = false;
                }
            if (!watch)
                watch = setInterval(check, 250);
        }
        function check() {
            if (mapped()) {
                clearInterval(watch);
                watch = 0;
                start();
            }
        }
        watch = setInterval(check, 250);
    }
})();
