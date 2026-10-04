    // The end of the function that player-pre.js opens.
    const create = rp6502;
    const nonce = document.currentScript?.nonce || '';
    const roots = new WeakSet();
    const styles = new WeakMap();
    let active = null, title = null;

    const CSS = `
@layer rp6502 {
.rp6502 { display: flex; flex-direction: column; box-sizing: border-box; width: 100%; min-width: 0;
  background: var(--rp6502-bgcolor, #000); outline: none; -webkit-tap-highlight-color: transparent; }
.rp6502[hidden] { display: none; }
}
.rp6502-screen { position: relative; flex: 1 1 auto; min-height: 0; aspect-ratio: 4 / 3; container-type: size; }
.rp6502-canvas { position: absolute; inset: var(--rp6502-border, 0px); display: block; outline: none;
  width: calc(100% - 2 * var(--rp6502-border, 0px)); height: calc(100% - 2 * var(--rp6502-border, 0px));
  user-select: none; -webkit-user-select: none; }
.rp6502-msg { position: absolute; inset: 0; z-index: 1; display: none; place-items: center;
  padding: 1em; text-align: center; color: #c7d0d9; background: rgba(0, 0, 0, .85);
  font: 14px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
.rp6502-msg pre { max-width: 100%; max-height: 100%; overflow: auto; margin: 0; text-align: left;
  font: 12px/1.35 ui-monospace, SFMono-Regular, Menlo, monospace; }
.rp6502-footer { padding: 8px 16px; border-top: 1px solid #303335; text-align: center;
  color: #9ca0a5; font: 13px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
.rp6502-line { margin: 0; color: inherit; font: inherit; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.rp6502-links { margin: 0; font: inherit; font-size: 12px; color: #6b7075; }
.rp6502-footer a { color: #5ca5ff; text-decoration: none; }
.rp6502-links svg { display: inline; width: 1em; height: 1em; margin-right: .3em; vertical-align: -.15em; fill: currentColor; }
.rp6502-overlay { position: absolute; inset: 0; cursor: pointer;
  background: var(--rp6502-overlay-background, rgba(0, 0, 0, .45));
  color: #fff; font: 600 15px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
.rp6502-play { position: absolute; left: var(--rp6502-play-x, 50%); top: var(--rp6502-play-y, 50%);
  transform: translate(-50%, -50%); display: flex; flex-direction: column; align-items: center; gap: 12px; }
.rp6502-button { width: var(--rp6502-button-size, clamp(56px, 18cqmin, 96px));
  height: var(--rp6502-button-size, clamp(56px, 18cqmin, 96px)); border-radius: 50%;
  display: grid; place-items: center; box-sizing: border-box;
  border: 3px solid #fff; background: rgba(0, 0, 0, .3); backdrop-filter: blur(2px);
  box-shadow: 0 6px 24px rgba(0, 0, 0, .5); transition: transform .15s ease; }
.rp6502-overlay:hover .rp6502-button { transform: scale(1.06); }
.rp6502-button svg { width: 51%; fill: currentColor; stroke: currentColor; stroke-width: 2; stroke-linejoin: round; }
.rp6502-label { padding: 4px 14px; border-radius: 999px; background: rgba(0, 0, 0, .6); }`;

    function style(root) {
        const tree = root.getRootNode();
        const parent = tree === document ? document.head : tree instanceof ShadowRoot ? tree : null;
        const s = styles.get(tree);
        if (!parent || (s?.isConnected && s.getRootNode() === tree))
            return;
        const added = document.createElement('style');
        added.dataset.rp6502 = '';
        added.nonce = nonce;
        added.textContent = CSS;
        parent.prepend(added);
        styles.set(tree, added);
    }

    globalThis.rp6502 = (container, rom, options = {}) => {
        const root = typeof container === 'string' ? document.getElementById(container) : container;
        if (root?.nodeType !== 1)
            throw new TypeError(typeof container === 'string'
                ? `rp6502: no element has the id '${container}'`
                : "rp6502: container must be the element for the player, or its id, such as 'game'");
        if (root.localName === 'canvas')
            throw new TypeError('rp6502: container must be an element such as a <div>, not a <canvas>');
        if (roots.has(root))
            throw new Error('rp6502: the container is already a player; call destroy() on that player first');
        if (typeof rom !== 'string' || !rom)
            throw new TypeError("rp6502: rom must be the program file, such as 'game.rp6502'");
        const settings = {...options, rom};
        roots.add(root);
        style(root);
        const player = {
            root,
            settings,
            style: () => style(root),
            isActive: () => active === player && root.isConnected,
            activate() {
                active = player;
                title ??= document.title;
                document.title = settings.title || title;
            },
        };
        if (!active?.isActive())
            active = player;
        // A throw in preRun, or a trap in main() before sokol starts, rejects
        // the factory promise with no call to onAbort.
        create({rp6502: player}).catch((e) => player.fail?.('Emulator crashed: ' + e.message));
        let held = player, done = null;
        return {
            element: root,
            destroy() {
                if (!done) {
                    roots.delete(root);
                    if (active === held) {
                        active = null;
                        if (title !== null)
                            document.title = title;
                    }
                    done = held.destroy();
                    // The returned object holds no reference to the instance
                    // after destroy(), so the instance can be freed.
                    held = null;
                }
                return done;
            },
        };
    };
})();
