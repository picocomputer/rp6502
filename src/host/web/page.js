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
    let failed = false;

    function show(text) {
        msg.textContent = text;
        msg.style.display = 'grid';
    }
    function fail(text) {
        failed = true;
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
#rp6502 { --border: 0px; position: relative; flex: 1 1 0; min-height: 0; }
#canvas { position: absolute; inset: var(--border); display: block; outline: none; touch-action: none;
  width: calc(100% - 2 * var(--border)); height: calc(100% - 2 * var(--border)); }
#rp6502-msg { position: absolute; inset: 0; z-index: 1; display: none; place-items: center;
  padding: 1em; text-align: center; color: #c7d0d9; background: rgba(0, 0, 0, .85);
  font: 14px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
#rp6502-msg pre { max-width: 92vw; max-height: 92vh; overflow: auto; margin: 0; text-align: left;
  font: 12px/1.35 ui-monospace, SFMono-Regular, Menlo, monospace; }
#rp6502-footer { padding: 8px 16px; border-top: 1px solid #303335; text-align: center;
  color: #9ca0a5; font: 13px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
#rp6502-footer p { margin: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
#rp6502-footer .links { font-size: 12px; color: #6b7075; white-space: normal; }
#rp6502-footer a { color: #5ca5ff; text-decoration: none; }
#rp6502-footer svg { width: 1em; height: 1em; margin-right: .3em; vertical-align: -.15em; fill: currentColor; }`;
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
    // The line of CONFIG.footer, and under it links to the repository, the
    // ROM and the Picocomputer.
    function footer() {
        const div = document.createElement('div');
        div.id = 'rp6502-footer';
        const line = document.createElement('p');
        line.innerHTML = CONFIG.footer;
        const links = document.createElement('p');
        links.className = 'links';
        const link = (href, text) => {
            const a = document.createElement('a');
            a.href = href;
            a.textContent = text;
            return a;
        };
        const parts = [];
        if (CONFIG.github) {
            const a = link('https://github.com/' + CONFIG.github, CONFIG.github);
            a.target = '_blank';
            a.insertAdjacentHTML('afterbegin', '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 0c4.42 0 8 3.58 8 8a8.013 8.013 0 0 1-5.45 7.59c-.4.08-.55-.17-.55-.38 0-.27.01-1.13.01-2.2 0-.75-.25-1.23-.54-1.48 1.78-.2 3.65-.88 3.65-3.95 0-.88-.31-1.59-.82-2.15.08-.2.36-1.02-.08-2.12 0 0-.67-.22-2.2.82-.64-.18-1.32-.27-2-.27-.68 0-1.36.09-2 .27-1.53-1.03-2.2-.82-2.2-.82-.44 1.1-.16 1.92-.08 2.12-.51.56-.82 1.28-.82 2.15 0 3.06 1.86 3.75 3.64 3.95-.23.2-.44.55-.51 1.07-.46.21-1.61.55-2.33-.66-.15-.24-.6-.83-1.23-.82-.67.01-.27.38.01.53.34.19.73.9.82 1.13.16.45.68 1.31 2.69.94 0 .67.01 1.3.01 1.49 0 .21-.15.45-.55.38A7.995 7.995 0 0 1 0 8c0-4.42 3.58-8 8-8Z"/></svg>');
            parts.push(a);
        }
        const rom = link(CONFIG.rom, 'Download ROM');
        rom.download = fileName(CONFIG.rom);
        parts.push(rom);
        const site = link('https://picocomputer.github.io', 'Picocomputer 6502');
        site.target = '_blank';
        parts.push(site);
        parts.forEach((a, i) => links.append(...(i ? [' \u2013 ', a] : [a])));
        div.append(line, links);
        return div;
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

    const bad = CONFIG.bgcolor && !/^[0-9a-fA-F]{6}$/.test(CONFIG.bgcolor)
        ? 'CONFIG.bgcolor must be six hex digits, such as 000000.'
        : CONFIG.filter && !['nearest', 'linear', 'sharp'].includes(CONFIG.filter)
        ? 'CONFIG.filter must be nearest, linear or sharp.'
        : CONFIG.run && !['always', 'onaudio', 'onclick'].includes(CONFIG.run)
        ? 'CONFIG.run must be always, onaudio or onclick.'
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
    if (CONFIG.bgcolor)
        Module.arguments.push('--bgcolor', CONFIG.bgcolor);
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

    // Made here rather than by sokol at start, the context exists before the
    // emulator runs, so its state sets the overlay and the hold, and a click
    // can resume it: WebKit resumes a context only inside a gesture.
    let audio = null;
    if (typeof AudioContext === 'function') {
        audio = new AudioContext({sampleRate: 48000, latencyHint: 'interactive'});
        const native = AudioContext;
        window.AudioContext = function () {
            window.AudioContext = native;
            return audio;
        };
    }
    const silent = () => audio && ['suspended', 'interrupted'].includes(audio.state);

    // sokol tries a resume only on the first click, touch and key press, so a
    // first key press that is not a user activation, such as Shift, leaves
    // the sound off. The catch keeps a refused resume from showing as a
    // crash.
    const unmute = () => {
        if (silent() && navigator.userActivation?.isActive !== false)
            audio.resume().catch(() => {});
    };
    for (const type of ['keydown', 'pointerdown', 'pointerup', 'touchend'])
        addEventListener(type, unmute, true);

    const run = CONFIG.run || 'always';
    let clicked = run !== 'onclick';
    let click = null;
    const started = run === 'onclick'
        ? new Promise((resolve) => {
            click = () => {
                removeEventListener('keydown', click);
                removeEventListener('pointerdown', click);
                clicked = true;
                sync();
                resolve();
            };
        })
        : run === 'onaudio'
        ? new Promise((resolve) => {
            const check = () => {
                if (!silent()) {
                    audio?.removeEventListener('statechange', check);
                    resolve();
                }
            };
            audio?.addEventListener('statechange', check);
            check();
        })
        : Promise.resolve();

    function sync() {
        if (!overlay)
            return;
        const up = !failed && (!clicked || silent());
        if (up && !overlay.isConnected)
            box.append(overlay);
        else if (!up && overlay.isConnected) {
            overlay.remove();
            canvas.focus({preventScroll: true});
        }
    }

    let box = null;
    const built = parsed(() => {
        box = frame();
        if (CONFIG.bgcolor)
            document.body.style.background = '#' + CONFIG.bgcolor;
        if (CONFIG.border)
            box.style.setProperty('--border', CONFIG.border);
        if (CONFIG.filter === 'nearest')
            canvas.style.imageRendering = 'pixelated';
        if (CONFIG.overlay) {
            overlay = template(CONFIG.overlay);
            if (click)
                overlay.addEventListener('click', click);
        }
        if (click) {
            addEventListener('keydown', click);
            if (!overlay)
                addEventListener('pointerdown', click);
        }
        audio?.addEventListener('statechange', sync);
        sync();
        if (CONFIG.footer)
            box.after(footer());
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
        wait('hold', started);
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
