/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Builds one player in Module.rp6502.root from Module.rp6502.settings.
 * --pre-js puts this file at the top of the factory, so its top-level var is
 * in the scope of the instance.
 */

// `new AudioContext` in saudio_js_init resolves to this binding before the
// global one, so each instance uses the context created below.
var AudioContext;

(function () {
    const player = Module.rp6502;
    const {root, settings} = player;
    const ac = new AbortController();
    const {signal} = ac;
    player.signal = signal;
    const onAbort = (fn) => signal.aborted ? fn() : signal.addEventListener('abort', fn, {once: true});

    let failed = false, overlay = null, foot = null;
    let audio = null, resume = null, claimed = false, persisted = false;
    let sounding = null, starting = null;
    let saves = null, loading = null, flushing = null, unlock = null, released = false;

    const screen = document.createElement('div');
    screen.className = 'rp6502-screen';
    // The sokol_app selector #canvas resolves to Module.canvas through
    // specialHTMLTargets, so the canvas has no id.
    const canvas = document.createElement('canvas');
    canvas.className = 'rp6502-canvas';
    canvas.tabIndex = 0;
    Module.canvas = canvas;
    const msg = document.createElement('div');
    msg.className = 'rp6502-msg';
    screen.append(canvas, msg);

    const before = {
        class: root.getAttribute('class'),
        rp6502: root.classList.contains('rp6502'),
        tabindex: root.getAttribute('tabindex'),
        bgcolor: root.style.getPropertyValue('--rp6502-bgcolor'),
        border: root.style.getPropertyValue('--rp6502-border'),
    };

    function show(text) {
        msg.textContent = text;
        msg.style.display = 'grid';
    }
    function release() {
        JSEvents?.removeAllEventListeners();
        removeEventListener('paste', Module.sokol_paste);
        removeEventListener('beforeunload', Module.sokol_beforeunload);
    }
    function fail(text) {
        if (failed || signal.aborted)
            return;
        failed = true;
        console.error('rp6502:', text);
        overlay?.remove();
        audio?.close();
        release();
        show(text);
    }
    player.fail = fail;

    // After unlock(), another player or window loads the database, so no sync
    // runs after the last one.
    function store() {
        return Promise.all([loading, flushing]).then(() => saves && new Promise((resolve) => {
            Module.IDBFS.onAutoPersistStateChanged = (busy) => busy || resolve();
            Module.IDBFS.queuePersist(saves);
        })).then(() => {
            released = true;
            Module.IDBFS?.quit();
            unlock?.();
        });
    }
    player.destroy = () => {
        ac.abort();
        release();
        audio?.close();
        Module.noInitialRun = true;
        if (!failed)
            Module._web_quit?.();
        // Chromium loses the oldest WebGL context past a limit, and the context
        // of a removed canvas lasts until garbage collection.
        Module.ctx?.getExtension('WEBGL_lose_context')?.loseContext();
        screen.remove();
        foot?.remove();
        if (!before.rp6502)
            root.classList.remove('rp6502');
        if (before.class === null && !root.classList.length)
            root.removeAttribute('class');
        if (before.tabindex === null)
            root.removeAttribute('tabindex');
        root.style.setProperty('--rp6502-bgcolor', before.bgcolor);
        root.style.setProperty('--rp6502-border', before.border);
        return store();
    };

    root.classList.add('rp6502');
    if (before.tabindex === null)
        root.tabIndex = -1;
    root.append(screen);

    const fileName = (url) => url.split(/[?#]/)[0].split('/').pop();
    // The line of settings.footer, and under it links to the repository, the
    // ROM and the Picocomputer.
    function footer() {
        const div = document.createElement('div');
        div.className = 'rp6502-footer';
        const line = document.createElement('p');
        line.className = 'rp6502-line';
        line.innerHTML = settings.footer;
        const links = document.createElement('p');
        links.className = 'rp6502-links';
        const link = (href, text) => {
            const a = document.createElement('a');
            a.href = href;
            a.textContent = text;
            return a;
        };
        const parts = [];
        if (settings.github) {
            const a = link('https://github.com/' + settings.github, settings.github);
            a.target = '_blank';
            a.insertAdjacentHTML('afterbegin', '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 0c4.42 0 8 3.58 8 8a8.013 8.013 0 0 1-5.45 7.59c-.4.08-.55-.17-.55-.38 0-.27.01-1.13.01-2.2 0-.75-.25-1.23-.54-1.48 1.78-.2 3.65-.88 3.65-3.95 0-.88-.31-1.59-.82-2.15.08-.2.36-1.02-.08-2.12 0 0-.67-.22-2.2.82-.64-.18-1.32-.27-2-.27-.68 0-1.36.09-2 .27-1.53-1.03-2.2-.82-2.2-.82-.44 1.1-.16 1.92-.08 2.12-.51.56-.82 1.28-.82 2.15 0 3.06 1.86 3.75 3.64 3.95-.23.2-.44.55-.51 1.07-.46.21-1.61.55-2.33-.66-.15-.24-.6-.83-1.23-.82-.67.01-.27.38.01.53.34.19.73.9.82 1.13.16.45.68 1.31 2.69.94 0 .67.01 1.3.01 1.49 0 .21-.15.45-.55.38A7.995 7.995 0 0 1 0 8c0-4.42 3.58-8 8-8Z"/></svg>');
            parts.push(a);
        }
        const rom = link(settings.rom, 'Download ROM');
        rom.download = fileName(settings.rom);
        parts.push(rom);
        const site = link('https://picocomputer.github.io', 'Picocomputer 6502');
        site.target = '_blank';
        parts.push(site);
        parts.forEach((a, i) => links.append(...(i ? [' – ', a] : [a])));
        div.append(line, links);
        return div;
    }
    if (settings.footer) {
        foot = footer();
        root.append(foot);
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
        return;
    }

    const template = (id) => root.getRootNode().getElementById?.(id) ?? document.getElementById(id);
    const autoplay = settings.autoplay === undefined ? 'on' : settings.autoplay;
    const known = ['rom', 'install', 'title', 'args', 'db', 'bgcolor', 'border', 'filter',
                   'autoplay', 'overlay', 'footer', 'github'];
    for (const key in settings)
        if (!known.includes(key))
            console.warn('rp6502: unknown setting ' + key);
    const bad = settings.bgcolor && !/^#?[0-9a-f]{6}$/i.test(settings.bgcolor)
        ? 'bgcolor must be six hex digits, such as 000000 or #000000.'
        : settings.filter && !['nearest', 'linear', 'sharp'].includes(settings.filter)
        ? 'filter must be nearest, linear or sharp.'
        : !['on', 'auto', 'off', 'muted'].includes(autoplay)
        ? 'autoplay must be on, auto, off or muted.'
        : settings.overlay && !template(settings.overlay)?.content?.firstElementChild
        ? `overlay '${settings.overlay}' names no <template> that holds an element.`
        : '';
    if (bad) {
        Module.noInitialRun = true;
        fail(bad);
        return;
    }

    // The ROM and each install are written to /roms under the last part of
    // their URL.
    const install = [].concat(settings.install || []);
    Module.arguments = ['--save-dir', '/saves'];
    if (settings.bgcolor)
        Module.arguments.push('--bgcolor', settings.bgcolor);
    if (settings.filter)
        Module.arguments.push('--filter', settings.filter);
    for (const url of install)
        Module.arguments.push('--install', '/roms/' + fileName(url));
    Module.arguments.push('/roms/' + fileName(settings.rom));
    if (settings.args)
        Module.arguments.push('--', ...settings.args);

    // A failed fetch never settles, so the start waits and the message stays.
    const files = [settings.rom, ...install].map((url) => fetch(url, {signal}).then((r) => {
        if (!r.ok)
            throw new Error('HTTP ' + r.status);
        return r.arrayBuffer();
    }).then((buf) => [fileName(url), buf]).catch((e) => {
        fail(`Could not load ${url} (${e.message}).`);
        return new Promise(() => {});
    }));

    if (settings.bgcolor)
        root.style.setProperty('--rp6502-bgcolor', '#' + settings.bgcolor.replace(/^#/, ''));
    if (settings.border)
        root.style.setProperty('--rp6502-border', settings.border);
    if (settings.filter === 'nearest')
        canvas.style.imageRendering = 'pixelated';

    // The context starts suspended and resumes on a click on the player or a
    // key that the player handles. Chrome runs a new context after a click on
    // any page of the same site, so a running context at load is no sign of a
    // click on this player.
    if (typeof globalThis.AudioContext === 'function') {
        audio = new globalThis.AudioContext({sampleRate: 48000, latencyHint: 'interactive'});
        audio.suspend();
        resume = audio.resume.bind(audio);
        // sokol_audio calls resume() on a click or a key anywhere in the
        // document and on every statechange.
        audio.resume = () => Promise.resolve();
        // fail() and destroy() close the context before the sokol cleanup
        // does, and a second close() rejects.
        const close = audio.close.bind(audio);
        let closing = null;
        audio.close = () => closing ??= close().catch(() => {});
        AudioContext = function () {
            // saudio_js_init adds four document listeners right after this
            // returns and stores no reference to them, so the signal removes
            // them on destroy() and the instance can be freed.
            const own = Object.getOwnPropertyDescriptor(document, 'addEventListener');
            const add = document.addEventListener;
            document.addEventListener = (type, fn, opts) => add.call(document, type, fn,
                {...(typeof opts === 'object' ? opts : {capture: !!opts}), signal});
            queueMicrotask(() => own ? Object.defineProperty(document, 'addEventListener', own)
                                     : delete document.addEventListener);
            return audio;
        };
    }
    const silent = () => audio && (!claimed || ['suspended', 'interrupted'].includes(audio.state));

    let clicked = autoplay === 'on' || autoplay === 'auto';
    const started = autoplay === 'off' ? new Promise((r) => { starting = r; })
        : autoplay === 'auto' ? new Promise((r) => { sounding = r; })
        : Promise.resolve();

    // On a page that scrolls, the arrows, Space and PageDown scroll the page.
    function scrolls() {
        const s = document.scrollingElement;
        const fixed = (e) => ['hidden', 'clip'].includes(getComputedStyle(e).overflowY);
        return s.scrollHeight > s.clientHeight && !fixed(document.documentElement) && !fixed(document.body);
    }
    // In a shadow root, document.activeElement is the host element.
    function focused() {
        const a = root.getRootNode().activeElement;
        return a === root || a === canvas ||
            (document.activeElement === document.body && clicked && player.isActive() && !scrolls());
    }
    player.focused = focused;

    function sync() {
        if (!overlay)
            return;
        const up = !failed && (!clicked || silent());
        if (up && !overlay.isConnected)
            screen.append(overlay);
        else if (!up && overlay.isConnected) {
            const had = overlay.contains(root.getRootNode().activeElement);
            overlay.remove();
            if (had)
                canvas.focus({preventScroll: true});
        }
    }
    function changed() {
        sync();
        if (!silent())
            sounding?.();
    }
    function claim() {
        claimed = true;
        resume?.().catch(() => {});
        if (settings.db && !persisted) {
            persisted = true;
            navigator.storage?.persist().catch(console.error);
        }
        changed();
    }
    // The browser allows a resume only in a click or a key press.
    function engage() {
        if (failed || navigator.userActivation?.isActive === false)
            return;
        clicked = true;
        starting?.();
        claim();
    }
    audio?.addEventListener('statechange', changed, {signal});

    if (settings.overlay === undefined) {
        overlay = document.createElement('div');
        overlay.className = 'rp6502-overlay';
        overlay.innerHTML = '<div class="rp6502-play"><div class="rp6502-button"><svg viewBox="0 0 24 24">' +
            '<path d="M8.5 5.5v13l10-6.5z"/></svg></div><div class="rp6502-label">Click to play</div></div>';
    } else if (settings.overlay)
        overlay = template(settings.overlay).content.firstElementChild.cloneNode(true);
    changed();

    for (const type of ['pointerdown', 'pointerup', 'touchend'])
        screen.addEventListener(type, engage, {capture: true, signal});
    addEventListener('keydown', () => focused() && engage(), {capture: true, signal});
    root.addEventListener('focusin', player.activate, {signal});
    canvas.addEventListener('pointerdown', () => canvas.focus({preventScroll: true}), {signal});
    const load = () => {
        if (focused()) {
            player.activate();
            claim();
        }
    };
    if (document.readyState === 'complete')
        load();
    else
        addEventListener('load', load, {signal});

    // sokol_app reads the canvas size only on a window resize event, so the
    // resize handler of sokol_app is called directly when the canvas changes
    // size, with no resize event on the page.
    const resized = new ResizeObserver(() => {
        player.style();
        JSEvents?.eventHandlers.find((h) => h.target === window && h.eventTypeString === 'resize')
            ?.handlerFunc({target: window, preventDefault() {}});
    });
    resized.observe(canvas);
    onAbort(() => resized.disconnect());

    function wait(id, promise) {
        Module.addRunDependency(id);
        // main() must not start sokol on a destroyed player.
        promise.then(() => signal.aborted || Module.removeRunDependency(id),
                     (e) => fail('Emulator failed to start: ' + e.message));
    }

    // SAVE: opens files in /saves. With settings.db, /saves is IDBFS, stored
    // in the IndexedDB database named by settings.db. IDBFS names a database
    // after its mountpoint and keys a file by its full path, so the name is
    // swapped and each key is the file's path inside /saves, as every existing
    // database holds it.
    function mountSaves(done) {
        const {getDB, getRemoteSet, loadRemoteEntry, storeRemoteEntry, removeRemoteEntry, syncfs} = Module.IDBFS;
        const key = (path) => path.slice('/saves/'.length);
        Module.IDBFS.getDB = (name, cb) => getDB(name === '/saves' ? settings.db : name, cb);
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
        // autoPersist drops the error of each sync it starts. No sync runs
        // after destroy(), because another player or window may hold the lock.
        Module.IDBFS.syncfs = (mount, populate, cb) => released ? cb(null) : syncfs(mount, populate, (err) => {
            if (err)
                console.error('IDBFS:', err);
            cb(err);
        });
        // autoPersist queues a sync after a close that wrote and after a
        // create, unlink, rename or rmdir. A sync stores only entries whose
        // mtime changed, and MEMFS changes it only on a write, so a truncate
        // or a chmod marks the node modified for the close to store it.
        const node = Module.FS.mount(Module.IDBFS, {autoPersist: true}, '/saves');
        const ops = node.node_ops;
        const setattr = ops.setattr;
        ops.setattr = (node, attr) => {
            const dirty = (attr.size !== undefined && attr.size !== node.usedBytes) ||
                          (attr.mode !== undefined && attr.mode !== node.mode);
            setattr(node, attr);
            if (dirty) {
                node.mtime = Date.now();
                node.isModified = true;
            }
        };
        loading = new Promise((loaded) => Module.FS.syncfs(true, () => {
            loaded();
            if (signal.aborted)
                return;
            // Best effort when the tab is hidden or closed. A sync started
            // during the load above would store the empty folder over the
            // saves, so this waits until the load is done.
            const flush = () => {
                if (flushing)
                    return;
                flushing = new Promise((r) => Module.FS.syncfs(false, r)).then(() => { flushing = null; });
            };
            addEventListener('pagehide', flush, {signal});
            document.addEventListener('visibilitychange', () => {
                if (document.visibilityState === 'hidden')
                    flush();
            }, {signal});
            saves = node.mount;
            done();
        }));
    }

    // Each window loads a separate copy of the database, and a sync makes the
    // database match that copy, so two players on one database would undo
    // each other's saves. The lock is held until the player is destroyed or
    // the window closes.
    function lockSaves() {
        return new Promise((resolve) => {
            // After fail(), no lock is taken and the error message stays.
            const locked = () => {
                if (signal.aborted || failed)
                    return;
                mountSaves(resolve);
                return new Promise((r) => { unlock = r; });
            };
            if (!navigator.locks) {
                mountSaves(resolve);
                return;
            }
            // ifAvailable cannot be combined with a signal.
            navigator.locks.request(settings.db, {ifAvailable: true}, (lock) => {
                if (lock)
                    return locked();
                if (failed)
                    return;
                show('This game is running in another player or window.');
                return navigator.locks.request(settings.db, {signal}, () => {
                    if (failed)
                        return;
                    msg.style.display = 'none';
                    return locked();
                });
            }).catch((e) => {
                if (signal.aborted || failed)
                    return;
                // Where the browser blocks storage, the lock request fails
                // too, and the saves last until the page closes.
                console.error(e);
                mountSaves(resolve);
            });
        });
    }

    const errTail = [];
    Module.printErr = (text) => {
        console.error(text);
        errTail.push(text);
        if (errTail.length > 12)
            errTail.shift();
    };
    Module.preRun = () => {
        // This dependency is never removed, so a destroyed player never starts.
        if (signal.aborted)
            return Module.addRunDependency('destroyed');
        Module.FS.mkdir('/roms');
        Module.FS.mkdir('/saves');
        wait('roms', Promise.all(files).then((list) => {
            for (const [name, buf] of list)
                Module.FS.writeFile('/roms/' + name, new Uint8Array(buf));
        }));
        wait('hold', started);
        if (settings.db)
            wait('saves', lockSaves());
    };
    Module.postRun = () => {
        // sokol makes a WebGL context on the canvas once startup gets as far
        // as sapp_run, so a 2D context means startup failed, with the reason
        // on stderr.
        if (canvas.getContext('2d'))
            fail('Emulator failed to start: ' + (errTail.join(' · ') || 'see the browser console'));
    };
    // ABORT_ON_WASM_EXCEPTIONS reports a trap as 'unhandled exception: ' and
    // the error twice with its stack.
    Module.onAbort = (what) => {
        const text = String(what);
        const trap = text.match(/^unhandled exception: ([^,\n]*)/);
        fail('Emulator crashed: ' + (trap ? trap[1] : text.split('\n')[0]));
    };
    Module.onRuntimeInitialized = gamepads;

    // Nothing reads navigator.getGamepads() until the program maps the gamepad
    // report block into XRAM and the player handles the keys, and the polling
    // stops when either ends.
    function gamepads() {
        if (signal.aborted)
            return;
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
            raf = 0;
            if (failed)
                return;
            if (!mapped() || !focused()) {
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
            if (failed) {
                clearInterval(watch);
                watch = 0;
            } else if (mapped() && focused()) {
                clearInterval(watch);
                watch = 0;
                start();
            }
        }
        watch = setInterval(check, 250);
        onAbort(() => {
            clearInterval(watch);
            cancelAnimationFrame(raf);
        });
    }
})();
