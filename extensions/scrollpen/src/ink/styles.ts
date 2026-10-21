// Styles for ScrollPen's shadow root. The page's CSS can't reach inside it, and none of
// this leaks out.
export const HOST_STYLE = [
  'all: initial',
  'position: absolute !important',
  'left: 0 !important',
  'top: 0 !important',
  'width: 0 !important',
  'height: 0 !important',
  'margin: 0 !important',
  'padding: 0 !important',
  'border: 0 !important',
  'transform: none !important',
  'z-index: 2147483647 !important',
  'pointer-events: none !important',
  'display: block !important',
  'visibility: visible !important',
  'opacity: 1 !important',
].join('; ');

export const SHADOW_CSS = `
:host { all: initial; }
* { box-sizing: border-box; }
.doc { position: absolute; left: 0; top: 0; width: 0; height: 0; overflow: visible; pointer-events: none; }
.doc > svg { position: absolute; left: 0; top: 0; overflow: visible; pointer-events: none; }
.doc > svg.el { overflow: hidden; }
.fixed { position: fixed; inset: 0; pointer-events: none; overflow: hidden; }
.fixed > svg { position: absolute; left: 0; top: 0; width: 100%; height: 100%; overflow: visible; pointer-events: none; }
.board { position: fixed; inset: 0; pointer-events: none; display: none; }
.board.white { display: block; background: #fbfbf8; }
.board.black { display: block; background: #1d2a24; }
.board > svg, .screen > svg { position: absolute; left: 0; top: 0; width: 100%; height: 100%; overflow: visible; }
.screen { position: fixed; inset: 0; pointer-events: none; overflow: hidden; }
.capture { position: fixed; inset: 0; pointer-events: auto; touch-action: none; cursor: crosshair; outline: none; display: none; }
.capture.on { display: block; }
.capture.through { pointer-events: none; }
.capture.eraser { cursor: none; }
.capture.text { cursor: text; }
.capture.laser { cursor: none; }
.laser path { fill: none; stroke: #ff2d2d; stroke-linecap: round; stroke-linejoin: round; filter: drop-shadow(0 0 4px #ff2d2dcc); }
.laser circle { fill: #ff4d4d; filter: drop-shadow(0 0 6px #ff2d2d); }
.cursor circle { fill: #ffffff55; stroke: #333; stroke-width: 1; }
textarea.text-edit {
  position: fixed; margin: 0; padding: 0; border: 0; outline: 1.5px dashed #2563eb; outline-offset: 2px;
  background: transparent; resize: none; overflow: hidden; pointer-events: auto; white-space: pre;
  field-sizing: content; min-width: 1ch; min-height: 1em; font-family: system-ui, "Segoe UI", Roboto, sans-serif;
  line-height: 1.25;
}

.toolbar {
  --bg: #f7f7f8; --fg: #1b1b1f; --muted: #6b6b76; --border: #e1e1e6; --hover: #e8e8ee; --accent: #2563eb; --accent-fg: #fff;
  position: fixed; display: none; flex-direction: column; align-items: center; gap: 2px; padding: 4px;
  width: 44px; max-height: calc(100vh - 16px); overflow-y: auto; scrollbar-width: none;
  background: var(--bg); color: var(--fg); border: 1px solid var(--border); border-radius: 12px;
  box-shadow: 0 6px 24px rgba(0,0,0,.18), 0 1px 3px rgba(0,0,0,.12); pointer-events: auto;
  font: 13px/1.3 system-ui, "Segoe UI", Roboto, sans-serif; user-select: none;
}
.toolbar.open { display: flex; }
@media (prefers-color-scheme: dark) {
  .toolbar, .popover, .toast {
    --bg: #1f1f23; --fg: #f2f2f5; --muted: #a0a0ab; --border: #3a3a42; --hover: #2f2f36; --accent: #5b8def; --accent-fg: #0b0b0d;
  }
}
.toolbar button {
  all: unset; box-sizing: border-box; width: 34px; height: 32px; flex: none; display: grid; place-items: center;
  border-radius: 8px; color: var(--fg); cursor: pointer;
}
.toolbar button:hover { background: var(--hover); }
.toolbar button:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; }
.toolbar button[aria-pressed="true"] { background: var(--accent); color: var(--accent-fg); }
.toolbar button:disabled { opacity: .35; cursor: default; background: none; }
.toolbar .grip { cursor: grab; height: 18px; color: var(--muted); }
.toolbar .grip svg { width: 16px; height: 16px; }
.toolbar .sep { flex: none; width: 24px; height: 1px; background: var(--border); margin: 3px 0; }
.toolbar .swatch { width: 20px; height: 20px; border-radius: 50%; border: 2px solid var(--border); }
.toolbar .restore { display: none; }
.toolbar.mini > :not(.grip):not(.restore) { display: none; }
.toolbar.mini .restore { display: grid; }

.popover {
  --bg: #f7f7f8; --fg: #1b1b1f; --muted: #6b6b76; --border: #e1e1e6; --hover: #e8e8ee; --accent: #2563eb;
  position: fixed; display: none; width: 176px; padding: 10px; pointer-events: auto;
  background: var(--bg); color: var(--fg); border: 1px solid var(--border); border-radius: 12px;
  box-shadow: 0 6px 24px rgba(0,0,0,.18); font: 12px/1.3 system-ui, "Segoe UI", Roboto, sans-serif; user-select: none;
}
.popover.open { display: block; }
.popover .swatches { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; margin-bottom: 10px; }
.popover .swatches button {
  all: unset; box-sizing: border-box; width: 30px; height: 30px; border-radius: 50%; cursor: pointer;
  border: 2px solid var(--border); justify-self: center;
}
.popover .swatches button[aria-pressed="true"] { outline: 2px solid var(--accent); outline-offset: 2px; }
.popover label { display: flex; justify-content: space-between; color: var(--muted); margin-bottom: 4px; }
.popover input[type="range"] { width: 100%; accent-color: var(--accent); margin: 0; }

.toast {
  --bg: #1f1f23; --fg: #f2f2f5;
  position: fixed; left: 50%; bottom: 24px; transform: translateX(-50%); max-width: min(480px, calc(100vw - 32px));
  padding: 8px 14px; border-radius: 10px; background: #1f1f23ee; color: #f2f2f5; pointer-events: none;
  font: 13px/1.4 system-ui, "Segoe UI", Roboto, sans-serif; opacity: 0; transition: opacity .2s; text-align: center;
}
.toast.show { opacity: 1; }
@media print { .toolbar, .popover, .toast, .capture, .cursor, .laser { display: none !important; } }
`;
