/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One player is two instances of the emulator (link.h): the window on the
 * page, in Module.rp6502.root from Module.rp6502.settings, and the machine in
 * a worker the window starts. --pre-js puts this file at the top of the
 * factory, so its top-level var is in the scope of the instance, and each
 * instance runs the half for its role.
 */

(function () {
    const player = Module.rp6502;

    // ---------------- the seam's batches, both halves ----------------

    // Each message the seam sends is a record in a batch, laid out as seam.c
    // lays out its queue: kind and length as 32-bit words, then the body,
    // padded to eight. The batch is posted once the script that added to it
    // is done, and the other half puts its records in its own queue.
    function linker(send) {
        let buf = new Uint8Array(4096), used = 0, queued = false;
        const flush = () => {
            queued = false;
            if (!used)
                return;
            const out = buf.slice(0, used).buffer;
            used = 0;
            send({seam: out}, [out]);
        };
        return (kind, body) => {
            const need = 8 + ((body.length + 7) & ~7);
            if (used + need > buf.length) {
                const grown = new Uint8Array(Math.max(buf.length * 2, used + need));
                grown.set(buf.subarray(0, used));
                buf = grown;
            }
            const view = new DataView(buf.buffer, used, 8);
            view.setUint32(0, kind, true);
            view.setUint32(4, body.length, true);
            buf.set(body, used + 8);
            used += need;
            if (!queued) {
                queued = true;
                queueMicrotask(flush);
            }
        };
    }
    // A batch the other half posted, into this half's queue.
    function deliver(batch) {
        const bytes = new Uint8Array(batch);
        const at = Module._link_inbox(bytes.length);
        HEAPU8.set(bytes, at);
        Module._link_deliver(bytes.length);
    }

    if (player.role === 'machine')
        inWorker();
    else
        onPage();

    // ---------------- the machine, in the worker ----------------

    function inWorker() {
        const {settings, origin} = player;
        let held = null, gone = false, failed = false, quitting = false;
        let saves = null, loading = null, flushing = null, unlock = null, released = false;
        const post = (msg, transfer) => postMessage(msg, transfer || []);
        const fail = (text) => {
            if (failed || quitting)
                return;
            failed = true;
            post({fail: text});
        };

        let timer = 0;
        player.link = {
            post: linker(post),
            frame(buf, w, h, cw, ch, last) {
                post({frame: buf, w, h, cw, ch, last}, [buf]);
            },
            // The next frame comes due in ms, unless something wakes the
            // machine sooner.
            arm(ms) {
                clearTimeout(timer);
                timer = setTimeout(wake, ms);
            },
        };
        const wake = () => player.running && !quitting && !failed && Module._link_wake();

        // The sound's player is on the page's audio thread, and its port comes
        // from the page. It says after each quantum how much it holds, which
        // also wakes the machine to make more. Each chunk's buffer comes back
        // empty, to be filled again, so the audio thread makes no garbage.
        const pool = [];
        const sound = player.sound = {
            rate: settings.rate,
            running: false,
            port: null,
            low: -1,
            least: -1,
            dry: 0,
            push(data, frames) {
                if (!this.port)
                    return;
                let d = pool.pop();
                if (!d || d.length < data.length)
                    d = new Float32Array(Math.max(data.length, 8192));
                d.set(data);
                this.port.postMessage({d, n: frames}, [d.buffer]);
            },
        };
        // A page that wants to watch the queue, as the developer's run page
        // does (.vscode/run.py), is told once a second: its least, how many
        // times it ran dry, its rate and the floor it is kept at.
        if (settings.stats)
            setInterval(() => {
                if (!sound.running)
                    return;
                post({stats: {least: sound.least, dry: sound.dry, rate: sound.rate,
                              floor: Module._web_audio_floor?.()}});
                sound.least = -1;
            }, 1000);
        function connect(port) {
            sound.port = port;
            port.onmessage = (e) => {
                const q = e.data;
                if (typeof q !== 'number') {
                    if (q.d)
                        pool.push(q.d);
                    else
                        sound.dry = q.dry;
                    return;
                }
                sound.low = sound.low < 0 ? q : Math.min(sound.low, q);
                sound.least = sound.least < 0 ? q : Math.min(sound.least, q);
                wake();
            };
        }

        // What the page sends before the machine's runtime is up waits for it.
        let ready = false;
        const early = [];
        const take = (e) => {
            const m = e.data;
            if (m.quit)
                quit();
            else if (m.go) {
                gone = true;
                held?.();
            }
            else if (!ready)
                early.push(e);
            else if (m.seam) {
                deliver(m.seam);
                wake();
            } else if (m.port)
                connect(m.port);
            else if (m.running !== undefined)
                sound.running = m.running;
            else if (m.flush)
                flush();
        };
        onmessage = take;
        for (const e of player.early.splice(0))
            take(e);

        const errTail = [];
        Module.print = (text) => console.log(text);
        Module.printErr = (text) => {
            console.error(text);
            errTail.push(text);
            if (errTail.length > 12)
                errTail.shift();
        };
        Module.onAbort = (what) => {
            const text = String(what);
            const trap = text.match(/^unhandled exception: ([^,\n]*)/);
            fail('Emulator crashed: ' + (trap ? trap[1] : text.split('\n')[0]));
        };

        // After quit, another player or window loads the database, so no sync
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
        function quit() {
            quitting = true;
            clearTimeout(timer);
            store().then(() => post({stored: true}));
        }
        // Best effort when the page is hidden or closed. A sync started during
        // the load would store the empty folder over the saves.
        function flush() {
            if (!saves || flushing)
                return;
            flushing = new Promise((r) => Module.FS.syncfs(false, r)).then(() => { flushing = null; });
        }

        // SAVE: opens files in /saves. With settings.db, /saves is IDBFS,
        // stored in the IndexedDB database named by settings.db. IDBFS names a
        // database after its mountpoint and keys a file by its full path, so
        // the name is swapped and each key is the file's path inside /saves,
        // as every existing database holds it.
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
            // after quit, because another player or window may hold the lock.
            Module.IDBFS.syncfs = (mount, populate, cb) => released ? cb(null) : syncfs(mount, populate, (err) => {
                if (err)
                    console.error('IDBFS:', err);
                cb(err);
            });
            // autoPersist queues a sync after a close that wrote and after a
            // create, unlink, rename or rmdir. A sync stores only entries
            // whose mtime changed, and MEMFS changes it only on a write, so a
            // truncate or a chmod marks the node modified for the close to
            // store it.
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
                saves = node.mount;
                done();
            }));
        }

        // Each window loads a separate copy of the database, and a sync makes
        // the database match that copy, so two players on one database would
        // undo each other's saves. The lock is held until the player is
        // destroyed or the window closes.
        function lockSaves() {
            return new Promise((resolve) => {
                const locked = () => {
                    if (quitting || failed)
                        return;
                    mountSaves(resolve);
                    return new Promise((r) => { unlock = r; });
                };
                if (!navigator.locks) {
                    mountSaves(resolve);
                    return;
                }
                navigator.locks.request(settings.db, {ifAvailable: true}, (lock) => {
                    if (lock)
                        return locked();
                    if (failed)
                        return;
                    post({waiting: true});
                    return navigator.locks.request(settings.db, () => {
                        if (failed || quitting)
                            return;
                        post({waiting: false});
                        return locked();
                    });
                }).catch((e) => {
                    if (quitting || failed)
                        return;
                    // Where the browser blocks storage, the lock request fails
                    // too, and the saves last until the page closes.
                    console.error(e);
                    mountSaves(resolve);
                });
            });
        }

        function wait(id, promise) {
            Module.addRunDependency(id);
            promise.then(() => quitting || Module.removeRunDependency(id),
                         (e) => fail('Emulator failed to start: ' + e.message));
        }

        // A failed fetch never settles, so the start waits and the message
        // stays.
        const files = settings.files.map(([name, url]) => fetch(url).then((r) => {
            if (!r.ok)
                throw new Error('HTTP ' + r.status);
            return r.arrayBuffer();
        }).then((buf) => [name, buf]).catch((e) => {
            fail(`Could not load ${url} (${e.message}).`);
            return new Promise(() => {});
        }));

        Module.arguments = settings.args;
        Module.preRun = () => {
            Module.FS.mkdir('/roms');
            Module.FS.mkdir('/saves');
            wait('roms', Promise.all(files).then((list) => {
                for (const [name, buf] of list)
                    Module.FS.writeFile('/roms/' + name, new Uint8Array(buf));
            }));
            if (settings.held && !gone)
                wait('hold', new Promise((r) => { held = r; }));
            if (settings.db)
                wait('saves', lockSaves());
        };
        // The page stamps its display callbacks on its own clock.
        Module.onRuntimeInitialized = () => {
            Module._link_clock(origin - performance.timeOrigin);
            ready = true;
            for (const e of early.splice(0))
                take(e);
        };
        // main() reaches link_run only with the machine booted; anything else
        // said why on stderr.
        Module.postRun = () => {
            if (!player.running)
                fail('Emulator failed to start: ' + (errTail.join(' · ') || 'see the browser console'));
        };
    }

    // ---------------- the window, on the page ----------------

    function onPage() {
        const {root, settings} = player;
        const ac = new AbortController();
        const {signal} = ac;
        player.signal = signal;
        const onAbort = (fn) => signal.aborted ? fn() : signal.addEventListener('abort', fn, {once: true});

        let failed = false, overlay = null, foot = null, worker = null;
        let audio = null, resume = null, claimed = false, persisted = false;
        let sounding = null, starting = null, stored = null;

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

        player.destroy = () => {
            ac.abort();
            release();
            audio?.close();
            Module.noInitialRun = true;
            if (!failed)
                Module._web_quit?.();
            // Chromium loses the oldest WebGL context past a limit, and the
            // context of a removed canvas lasts until garbage collection.
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
            // The worker stores the saves and lets go of their lock first,
            // even after a failure, since what was saved before it stands.
            if (!worker)
                return Promise.resolve();
            const done = new Promise((r) => { stored = r; }).then(() => worker?.terminate());
            worker.postMessage({quit: true});
            return done;
        };

        root.classList.add('rp6502');
        if (before.tabindex === null)
            root.tabIndex = -1;
        root.append(screen);

        const fileName = (url) => url.split(/[?#]/)[0].split('/').pop();
        // The line of settings.footer, and under it links to the repository,
        // the ROM and the Picocomputer.
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

        // With ?credits, the emulator prints the license notice of every
        // bundled component, and runs nothing else.
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
                       'aspect', 'autoplay', 'overlay', 'footer', 'github'];
        for (const key in settings)
            if (!known.includes(key))
                console.warn('rp6502: unknown setting ' + key);
        const bad = settings.bgcolor && !/^#?[0-9a-f]{6}$/i.test(settings.bgcolor)
            ? 'bgcolor must be six hex digits, such as 000000 or #000000.'
            : settings.filter && !['nearest', 'linear', 'sharp'].includes(settings.filter)
            ? 'filter must be nearest, linear or sharp.'
            : settings.aspect && !['4:3', '16:9', '16:10'].includes(settings.aspect)
            ? 'aspect must be 4:3, 16:9 or 16:10.'
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

        // The ROM and each install are written to /roms under the last part
        // of their URL, in the worker, which fetches them by their full URL.
        const install = [].concat(settings.install || []);
        const args = ['--save-dir', '/saves'];
        if (settings.bgcolor)
            args.push('--bgcolor', settings.bgcolor);
        if (settings.filter)
            args.push('--filter', settings.filter);
        if (settings.aspect)
            args.push('--aspect', settings.aspect);
        for (const url of install)
            args.push('--install', '/roms/' + fileName(url));
        args.push('/roms/' + fileName(settings.rom));
        if (settings.args)
            args.push('--', ...settings.args);
        Module.arguments = args;
        const files = [settings.rom, ...install].map((url) => [fileName(url), new URL(url, document.baseURI).href]);

        if (settings.bgcolor)
            root.style.setProperty('--rp6502-bgcolor', '#' + settings.bgcolor.replace(/^#/, ''));
        if (settings.border)
            root.style.setProperty('--rp6502-border', settings.border);
        if (settings.filter === 'nearest')
            canvas.style.imageRendering = 'pixelated';
        // With aspect, the emulator gives the box the console's shape once it
        // starts. The box takes that shape now, before the WebAssembly loads,
        // so it does not jump.
        if (settings.aspect)
            screen.style.aspectRatio = settings.aspect.replace(':', ' / ');

        // The context starts suspended and resumes on a click on the player or
        // a key that the player handles. Chrome runs a new context after a
        // click on any page of the same site, so a running context at load is
        // no sign of a click on this player. Without a secure context there is
        // no AudioWorklet, and so no sound.
        if (typeof globalThis.AudioContext === 'function' && globalThis.isSecureContext) {
            audio = new globalThis.AudioContext({sampleRate: 48000, latencyHint: 'interactive'});
            audio.suspend();
            resume = audio.resume.bind(audio);
            // fail() and destroy() close the context, and a second close()
            // rejects.
            const close = audio.close.bind(audio);
            let closing = null;
            audio.close = () => closing ??= close().catch(() => {});
        }
        const silent = () => audio && (!claimed || ['suspended', 'interrupted'].includes(audio.state));

        let clicked = autoplay === 'on' || autoplay === 'auto';
        const started = autoplay === 'off' ? new Promise((r) => { starting = r; })
            : autoplay === 'auto' ? new Promise((r) => { sounding = r; })
            : Promise.resolve();

        // On a page that scrolls, the arrows, Space and PageDown scroll the
        // page.
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
            worker?.postMessage({running: !!audio && audio.state === 'running'});
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

        // The browser tells the locks only inside an event, so they are kept
        // from the last key or click. Until the first one NumLock is taken as
        // on. An Apple keyboard has no NumLock or Scroll Lock: its keypad
        // always types digits, so NumLock stays on and Scroll Lock off.
        const apple = /Mac|iPhone|iPad/.test(navigator.platform);
        let locks = 1;
        const seeLocks = (e) => {
            locks = (apple || e.getModifierState('NumLock') ? 1 : 0) |
                (e.getModifierState('CapsLock') ? 2 : 0) |
                (!apple && e.getModifierState('ScrollLock') ? 4 : 0);
        };
        for (const type of ['keydown', 'keyup', 'pointerdown'])
            addEventListener(type, seeLocks, {capture: true, signal});
        player.locks = () => locks;
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

        // sokol_app reads the canvas size only on a window resize event, so
        // the resize handler of sokol_app is called directly when the canvas
        // changes size, with no resize event on the page.
        const fit = () => {
            player.style();
            JSEvents?.eventHandlers.find((h) => h.target === globalThis && h.eventTypeString === 'resize')
                ?.handlerFunc({target: globalThis, preventDefault() {}});
        };
        const resized = new ResizeObserver(fit);
        resized.observe(canvas);
        onAbort(() => resized.disconnect());
        // The emulator gives the box the shape of its frame. The observer
        // would see the new size only after the picture being drawn now, so
        // sokol_app is told at once.
        player.aspect = (w, h) => {
            screen.style.aspectRatio = w + ' / ' + h;
            fit();
        };

        // The controllers, read from the Gamepad API while the player has the
        // keys (gamepad.c asks only while a program has mapped them). Each is
        // a row of twelve numbers into the heap: its id, then the bits and
        // axes of the gamepad report in the RP6502-RIA docs, its face-button
        // labels and whether it has sticks.
        const clamp = (v, lo, hi) => v < lo ? lo : (v > hi ? hi : v);
        const axis = (v) => clamp(Math.round((v || 0) * 127), -128, 127);
        const trig = (btn) => btn ? clamp(Math.round(btn.value * 255), 0, 255) : 0;
        // Face button labels: 3 PlayStation, 2 Eastern BA, 1 Western AB, 0
        // when the id does not identify the pad or buttons[] is in the
        // device's order.
        function padType(gp) {
            if (gp.mapping !== 'standard')
                return 0;
            if (/054c|0ce6|dualshock|dualsense|playstation|sony/i.test(gp.id))
                return 3;
            // Pro Controller or Joy-Con pair; a lone Joy-Con held sideways
            // fits no row.
            if (/057e\D+20(09|0e)/i.test(gp.id))
                return 2;
            if (/045e|xbox|xinput/i.test(gp.id))
                return 1;
            return 0;
        }
        player.gamepads = (heap, at, max) => {
            if (failed || !focused() || !navigator.getGamepads)
                return 0;
            let n = 0;
            for (const gp of navigator.getGamepads()) {
                if (!gp || n >= max)
                    continue;
                const b = gp.buttons, a = gp.axes;
                const on = (i) => (b[i] && b[i].pressed) ? 1 : 0;
                const dpad = (on(12) ? 0x01 : 0) | (on(13) ? 0x02 : 0) |
                             (on(14) ? 0x04 : 0) | (on(15) ? 0x08 : 0);
                let b0 = (on(0) ? 0x01 : 0) | (on(1) ? 0x02 : 0) | (on(2) ? 0x08 : 0) |
                         (on(3) ? 0x10 : 0) | (on(4) ? 0x40 : 0) | (on(5) ? 0x80 : 0);
                const type = padType(gp);
                // The standard mapping places Nintendo face buttons by
                // position.
                if (type === 2)
                    b0 = (b0 & 0xE4) | (b0 & 0x09) << 1 | (b0 & 0x12) >> 1;
                const b1 = (on(6) ? 0x01 : 0) | (on(7) ? 0x02 : 0) | (on(8) ? 0x04 : 0) |
                           (on(9) ? 0x08 : 0) | (on(16) ? 0x10 : 0) | (on(10) ? 0x20 : 0) |
                           (on(11) ? 0x40 : 0);
                heap.set([gp.index + 1, dpad, b0, b1, axis(a[0]), axis(a[1]), axis(a[2]), axis(a[3]),
                          trig(b[6]), trig(b[7]), type, a.length >= 4 ? 1 : 0], at + n * 12);
                n++;
            }
            return n;
        };

        // ---------------- the machine's worker ----------------

        // The sound's player, on the browser's audio thread, plays from a
        // queue the worker fills through a port of their own (audio.c). After
        // each quantum it says how much it holds, and it hands each chunk's
        // buffer back once it has played it.
        const worklet = () => {
            class Out extends AudioWorkletProcessor {
                constructor(options) {
                    super();
                    this.chunks = [];
                    this.at = 0;
                    this.queued = 0;
                    this.floor = options.processorOptions.floor;
                    this.most = options.processorOptions.most;
                    this.playing = false;
                    this.rising = false;
                    this.last = [0, 0];
                    this.dry = 0;
                    this.port.onmessage = (e) => {
                        this.feed = e.data;
                        this.feed.onmessage = (m) => {
                            this.chunks.push(m.data);
                            this.queued += m.data.n;
                            // Past the most the oldest goes, so the sound is
                            // never far behind the machine.
                            while (this.queued - (this.chunks[0].n - this.at / 2) >= this.most)
                                this.done(this.chunks.shift());
                        };
                    };
                }
                done(c) {
                    this.queued -= c.n - this.at / 2;
                    this.at = 0;
                    this.feed.postMessage({d: c.d}, [c.d.buffer]);
                }
                process(inputs, outputs) {
                    const out = outputs[0];
                    const L = out[0], R = out[1] || out[0], n = L.length;
                    if (!this.playing && this.queued >= this.floor)
                        this.playing = this.rising = true;
                    let i = 0;
                    if (this.playing) {
                        while (i < n && this.chunks.length) {
                            const c = this.chunks[0], d = c.d, end = c.n * 2;
                            for (; i < n && this.at < end; this.at += 2, i++) {
                                L[i] = d[this.at];
                                R[i] = d[this.at + 1];
                            }
                            if (this.at >= end) {
                                this.chunks.shift();
                                this.at = 0;
                                this.feed.postMessage({d}, [d.buffer]);
                            }
                        }
                        if (this.rising)
                            for (let k = 0; k < i; k++) {
                                L[k] *= k / n;
                                R[k] *= k / n;
                            }
                        this.rising = false;
                        this.queued -= i;
                        if (i) {
                            this.last[0] = L[i - 1];
                            this.last[1] = R[i - 1];
                        }
                    }
                    if (i < n) {
                        if (this.playing)
                            this.feed?.postMessage({dry: ++this.dry});
                        const m = n - i;
                        for (let k = 0; k < m; k++) {
                            const g = (m - 1 - k) / m;
                            L[i + k] = this.last[0] * g;
                            R[i + k] = this.last[1] * g;
                        }
                        this.last[0] = this.last[1] = 0;
                        this.playing = false;
                    }
                    this.feed?.postMessage(this.queued);
                    return true;
                }
            }
            registerProcessor('rp6502-out', Out);
        };

        function connectSound() {
            if (!audio?.audioWorklet || typeof AudioWorkletNode !== 'function')
                return;
            const url = URL.createObjectURL(new Blob(['(' + worklet + ')()'], {type: 'text/javascript'}));
            audio.audioWorklet.addModule(url).then(() => {
                URL.revokeObjectURL(url);
                if (signal.aborted || failed)
                    return;
                const node = new AudioWorkletNode(audio, 'rp6502-out', {
                    numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
                    processorOptions: {floor: Module._web_audio_floor(), most: Module._web_audio_most()},
                });
                node.connect(audio.destination);
                onAbort(() => node.disconnect());
                const ch = new MessageChannel();
                node.port.postMessage(ch.port1, [ch.port1]);
                worker.postMessage({port: ch.port2}, [ch.port2]);
                changed();
            }).catch((err) => console.error('rp6502: no AudioWorklet:', err));
        }

        const linkPost = linker((m, t) => worker?.postMessage(m, t));
        player.link = {post: linkPost};
        function onWorker(e) {
            const m = e.data;
            if (m.frame) {
                const at = Module._link_frame_at();
                HEAPU8.set(new Uint8Array(m.frame, 0, m.w * m.h * 4), at);
                Module._link_frame_done(m.w, m.h, m.cw, m.ch, m.last ? 1 : 0);
            } else if (m.seam)
                deliver(m.seam);
            else if (m.fail)
                fail(m.fail);
            else if (m.stats)
                globalThis.rp6502_audio_stats(m.stats);
            else if (m.waiting !== undefined) {
                if (m.waiting)
                    show('This game is running in another player or window.');
                else
                    msg.style.display = 'none';
            } else if (m.up)
                Module._link_up();
            else if (m.stored)
                stored?.();
        }
        function startWorker() {
            worker = player.spawn();
            worker.onmessage = onWorker;
            // A worker that could not load rp6502.js answers nothing, so a
            // destroy() waiting on it is let go.
            worker.onerror = (e) => {
                worker.terminate();
                worker = null;
                stored?.();
                fail('Emulator failed to start: ' + (e.message || 'see the browser console'));
            };
            worker.postMessage({init: {
                settings: {args, files, db: settings.db || '', held: autoplay === 'off' || autoplay === 'auto',
                           rate: audio?.audioWorklet ? audio.sampleRate : 0,
                           stats: typeof globalThis.rp6502_audio_stats === 'function'},
                origin: performance.timeOrigin,
            }, module: player.module});
            started.then(() => worker.postMessage({go: true}));
            // Best effort when the tab is hidden or closed.
            const flush = () => worker.postMessage({flush: true});
            addEventListener('pagehide', flush, {signal});
            document.addEventListener('visibilitychange', () => {
                if (document.visibilityState === 'hidden')
                    flush();
            }, {signal});
            connectSound();
        }

        const errTail = [];
        Module.printErr = (text) => {
            console.error(text);
            errTail.push(text);
            if (errTail.length > 12)
                errTail.shift();
        };
        Module.preRun = () => {
            // This dependency is never removed, so a destroyed player never
            // starts.
            if (signal.aborted)
                Module.addRunDependency('destroyed');
        };
        Module.onRuntimeInitialized = () => signal.aborted || startWorker();
        Module.postRun = () => {
            // sokol makes a WebGL context on the canvas once startup gets as
            // far as sapp_run, so a 2D context means startup failed, with the
            // reason on stderr.
            if (canvas.getContext('2d'))
                fail('Emulator failed to start: ' + (errTail.join(' · ') || 'see the browser console'));
        };
        // ABORT_ON_WASM_EXCEPTIONS reports a trap as 'unhandled exception: '
        // and the error twice with its stack.
        Module.onAbort = (what) => {
            const text = String(what);
            const trap = text.match(/^unhandled exception: ([^,\n]*)/);
            fail('Emulator crashed: ' + (trap ? trap[1] : text.split('\n')[0]));
        };
    }
})();
