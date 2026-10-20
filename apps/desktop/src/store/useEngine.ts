import {useEffect, useState} from 'react';
import {
  type EngineState,
  isNativeAvailable,
  onEngineEvent,
  send,
} from '../native/scrollpen';

export const defaultState: EngineState = {
  active: false,
  clickThroughHeld: false,
  tool: 'pen',
  board: 'none',
  inkVisible: true,
  textEditing: false,
  canUndo: false,
  canRedo: false,
  itemCount: 0,
  sync: {source: 'none', confidence: 0, serverRunning: false},
  hotkeyErrors: [],
  hookReinstalls: 0,
  monitors: 1,
  settings: {
    tool: 'pen',
    color: '#E53935',
    penWidth: 4,
    highlighterWidth: 22,
    shapeWidth: 4,
    fontSize: 28,
    fill: false,
    laserFadeSeconds: 1.2,
    palette: ['#E53935', '#FB8C00', '#FDD835', '#43A047', '#1E88E5', '#8E24AA', '#212121', '#FFFFFF', '#00ACC1'],
    documentMode: true,
    clickThroughKey: 'CapsLock',
    hideDockFromCapture: true,
    startActive: false,
    hotkeys: {},
    syncPort: 47615,
    allowedExtensionIds: [],
  },
};

/** Mirrors the native engine's state; the engine is the source of truth. */
export function useEngine() {
  const [state, setState] = useState<EngineState>(defaultState);
  const [toast, setToast] = useState<string | null>(null);

  useEffect(() => {
    const off = onEngineEvent(e => {
      if (e.ev === 'state') {
        const {ev: _ev, ...rest} = e;
        setState(rest);
      } else if (e.ev === 'screenshot') {
        setToast(e.path ? 'Screenshot saved & copied' : 'Screenshot copied');
      } else if (e.ev === 'error') {
        setToast(e.message);
      }
    });
    if (isNativeAvailable) send({cmd: 'getState'});
    return off;
  }, []);

  useEffect(() => {
    if (!toast) return;
    const t = setTimeout(() => setToast(null), 2500);
    return () => clearTimeout(t);
  }, [toast]);

  return {state, toast};
}
