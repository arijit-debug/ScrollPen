// ScrollPen's settings page. Built with DOM calls only: page titles and addresses come
// from the web and are shown as text, never as markup.
import { INK_PREFIX, loadSettings, sanitizeSaved, saveSettings, SETTINGS_KEY } from '../ink/storage';
import {
  CLICK_THROUGH_KEYS,
  DEFAULT_SETTINGS,
  LASER_FADE_RANGE,
  SIZE_RANGE,
  type Settings,
  type SizeKey,
} from '../ink/types';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
let settings: Settings = DEFAULT_SETTINGS;

async function update(patch: Partial<Settings>): Promise<void> {
  settings = { ...settings, ...patch };
  await saveSettings(settings);
}

function el<K extends keyof HTMLElementTagNameMap>(tag: K, className?: string, text?: string): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  if (className) e.className = className;
  if (text !== undefined) e.textContent = text;
  return e;
}

function renderSettings(): void {
  for (const id of ['documentMode', 'fill', 'startActive', 'openPdfs'] as const) $<HTMLInputElement>(id).checked = settings[id];

  const fade = $<HTMLInputElement>('laserFadeSeconds');
  fade.min = String(LASER_FADE_RANGE[0]);
  fade.max = String(LASER_FADE_RANGE[1]);
  fade.value = String(settings.laserFadeSeconds);
  $('laserFadeSecondsValue').textContent = `${settings.laserFadeSeconds.toFixed(1)} s`;

  for (const key of Object.keys(SIZE_RANGE) as SizeKey[]) {
    const input = $<HTMLInputElement>(`size-${key}`);
    [input.min, input.max] = SIZE_RANGE[key].map(String);
    input.value = String(settings.sizes[key]);
    $(`size-${key}Value`).textContent = String(Math.round(settings.sizes[key]));
  }

  const chips = $('clickThroughKey');
  chips.replaceChildren();
  for (const k of CLICK_THROUGH_KEYS) {
    const b = el('button', undefined, k || 'Off');
    b.setAttribute('aria-pressed', String(settings.clickThroughKey === k));
    b.addEventListener('click', () => void update({ clickThroughKey: k }).then(renderSettings));
    chips.append(b);
  }
}

function wireSettings(): void {
  for (const id of ['documentMode', 'fill', 'startActive', 'openPdfs'] as const) {
    $<HTMLInputElement>(id).addEventListener('change', (e) => void update({ [id]: (e.target as HTMLInputElement).checked }));
  }
  $<HTMLInputElement>('laserFadeSeconds').addEventListener('input', (e) => {
    const v = Number((e.target as HTMLInputElement).value);
    $('laserFadeSecondsValue').textContent = `${v.toFixed(1)} s`;
    void update({ laserFadeSeconds: v });
  });
  for (const key of Object.keys(SIZE_RANGE) as SizeKey[]) {
    $<HTMLInputElement>(`size-${key}`).addEventListener('input', (e) => {
      const v = Number((e.target as HTMLInputElement).value);
      $(`size-${key}Value`).textContent = String(v);
      void update({ sizes: { ...settings.sizes, [key]: v } });
    });
  }
  $('resetToolbar').addEventListener('click', () => void update({ toolbar: null, toolbarMinimized: false }));
  $('resetSettings').addEventListener('click', async () => {
    if (!confirm('Reset all ScrollPen settings? Your saved ink is kept.')) return;
    settings = DEFAULT_SETTINGS;
    await saveSettings(settings);
    renderSettings();
  });
  $('editShortcuts').addEventListener('click', () => void chrome.tabs.create({ url: 'chrome://extensions/shortcuts' }));
}

async function renderCommands(): Promise<void> {
  const box = $('commands');
  box.replaceChildren();
  for (const c of await chrome.commands.getAll()) {
    const row = el('div', 'row');
    row.append(el('div', 'text', c.description || 'Start / stop drawing'), el('span', 'value', c.shortcut || 'Not set'));
    box.append(row);
  }
}

async function renderPages(): Promise<void> {
  const all = await chrome.storage.local.get(null);
  const pages = Object.entries(all)
    .filter(([k]) => k.startsWith(INK_PREFIX))
    .map(([k, v]) => ({ key: k, saved: sanitizeSaved(v, k.slice(INK_PREFIX.length)) }))
    .filter((p): p is { key: string; saved: NonNullable<typeof p.saved> } => !!p.saved)
    .sort((a, b) => b.saved.updated - a.saved.updated);

  const box = $('pages');
  box.replaceChildren();
  $('pageCount').textContent = pages.length === 1 ? '1 page' : `${pages.length} pages`;
  $<HTMLButtonElement>('deleteAll').disabled = !pages.length;
  if (!pages.length) {
    box.append(el('div', 'empty', 'No saved ink yet.'));
    return;
  }
  for (const { key, saved } of pages) {
    const row = el('div', 'page');
    const address = saved.url.startsWith('pdf:') ? saved.url.slice(4) : saved.url;
    const when = saved.updated ? new Date(saved.updated).toLocaleString() : '';
    row.append(
      el('div', 'title', saved.title || address),
      el('div', 'url', `${address} · ${saved.items.length} ink ${saved.items.length === 1 ? 'mark' : 'marks'}${when ? ` · ${when}` : ''}`),
    );
    const actions = el('div', 'actions');
    if (/^(https?|file):/.test(address)) {
      const open = el('button', undefined, 'Open');
      open.addEventListener('click', () => void chrome.tabs.create({ url: address }));
      actions.append(open);
    }
    const del = el('button', 'danger', 'Delete');
    del.addEventListener('click', () => void chrome.storage.local.remove(key));
    actions.append(del);
    row.append(actions);
    box.append(row);
  }
}

async function main(): Promise<void> {
  $('version').textContent = `ScrollPen ${chrome.runtime.getManifest().version}`;
  settings = await loadSettings();
  renderSettings();
  wireSettings();
  $('deleteAll').addEventListener('click', async () => {
    if (!confirm('Delete the saved ink on every page? This cannot be undone.')) return;
    const all = await chrome.storage.local.get(null);
    await chrome.storage.local.remove(Object.keys(all).filter((k) => k.startsWith(INK_PREFIX)));
  });
  await Promise.all([renderCommands(), renderPages()]);
  chrome.storage.onChanged.addListener((changes, area) => {
    if (area !== 'local') return;
    if (Object.keys(changes).some((k) => k.startsWith(INK_PREFIX))) void renderPages();
    if (changes[SETTINGS_KEY]) {
      void loadSettings().then((s) => {
        settings = s;
        renderSettings();
      });
    }
  });
}

void main();
