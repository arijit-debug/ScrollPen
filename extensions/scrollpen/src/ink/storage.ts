// Ink and settings live in chrome.storage.local: on this computer, in this Chrome profile
// only. Nothing is synced or sent anywhere.
import { sanitizeItems, sanitizeSettings, type Item, type Settings } from './types';

export const INK_PREFIX = 'ink:';
export const SETTINGS_KEY = 'settings';
export const inkKey = (docKey: string) => INK_PREFIX + docKey;

export interface SavedPage {
  v: 1;
  /** The page's address (or "pdf:<address>" for PDFs). */
  url: string;
  title: string;
  updated: number;
  items: Item[];
}

export function sanitizeSaved(raw: unknown, docKey: string): SavedPage | null {
  if (!raw || typeof raw !== 'object') return null;
  const r = raw as Record<string, unknown>;
  const items = sanitizeItems(r.items);
  if (!items.length) return null;
  return {
    v: 1,
    url: docKey,
    title: typeof r.title === 'string' ? r.title.slice(0, 500) : '',
    updated: typeof r.updated === 'number' && Number.isFinite(r.updated) ? r.updated : 0,
    items,
  };
}

export async function loadInk(docKey: string): Promise<SavedPage | null> {
  const key = inkKey(docKey);
  const { [key]: raw } = await chrome.storage.local.get(key);
  return sanitizeSaved(raw, docKey);
}

/**
 * Saves the page's ink (a page without ink is removed from storage). `updated` is set by
 * the caller before saving, so it can recognise its own save when storage reports it.
 */
export async function saveInk(docKey: string, title: string, items: Item[], updated: number): Promise<void> {
  const key = inkKey(docKey);
  if (!items.length) await chrome.storage.local.remove(key);
  else await chrome.storage.local.set({ [key]: { v: 1, url: docKey, title: title.slice(0, 500), updated, items } });
}

export async function loadSettings(): Promise<Settings> {
  const { [SETTINGS_KEY]: raw } = await chrome.storage.local.get(SETTINGS_KEY);
  return sanitizeSettings(raw);
}

export async function saveSettings(settings: Settings): Promise<void> {
  await chrome.storage.local.set({ [SETTINGS_KEY]: settings });
}
