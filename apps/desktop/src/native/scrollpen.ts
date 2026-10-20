import {DeviceEventEmitter, NativeModules} from 'react-native';

export type ToolName =
  | 'pen'
  | 'highlighter'
  | 'laser'
  | 'eraser'
  | 'line'
  | 'arrow'
  | 'rect'
  | 'ellipse'
  | 'text';

export type BoardName = 'none' | 'white' | 'black';
export type SyncSourceName = 'none' | 'browser' | 'uia' | 'optical';

export interface EngineSettings {
  tool: ToolName;
  color: string;
  penWidth: number;
  highlighterWidth: number;
  shapeWidth: number;
  fontSize: number;
  fill: boolean;
  laserFadeSeconds: number;
  palette: string[];
  documentMode: boolean;
  clickThroughKey: string;
  hideDockFromCapture: boolean;
  startActive: boolean;
  hotkeys: Record<string, string>;
  syncPort: number;
  allowedExtensionIds: string[];
}

export interface EngineState {
  active: boolean;
  clickThroughHeld: boolean;
  tool: ToolName;
  board: BoardName;
  inkVisible: boolean;
  textEditing: boolean;
  canUndo: boolean;
  canRedo: boolean;
  itemCount: number;
  sync: {source: SyncSourceName; confidence: number; serverRunning: boolean};  hotkeyErrors: string[];
  hookReinstalls: number;
  monitors: number;
  settings: EngineSettings;
}

export type EngineEvent =
  | ({ev: 'state'} & EngineState)
  | {ev: 'screenshot'; path: string}
  | {ev: 'error'; message: string}
  | {ev: 'log'; level: string; message: string};

export type EngineCommand =
  | {cmd: 'getState'}
  | {cmd: 'setTool'; tool: ToolName}
  | {cmd: 'setActive'; active: boolean}
  | {cmd: 'toggleActive'}
  | {cmd: 'setBoard'; board: BoardName}
  | {cmd: 'setDocumentMode'; enabled: boolean}
  | {cmd: 'setInkVisible'; visible: boolean}
  | {cmd: 'undo'}
  | {cmd: 'redo'}
  | {cmd: 'clear'}
  | {cmd: 'screenshot'}
  | {cmd: 'updateSettings'; settings: Partial<EngineSettings>}
  | {cmd: 'resetSettings'};

interface NativeScrollPen {
  send(json: string): void;
  startDockDrag(): void;
  resizeDock(width: number, height: number): void;
  minimizeDock(): void;
  quit(): void;
}

const native: NativeScrollPen | undefined = NativeModules.ScrollPenNative;

/** True when running inside the Windows app (false in Jest). */
export const isNativeAvailable = native !== undefined;

export function send(command: EngineCommand): void {
  native?.send(JSON.stringify(command));
}

export function onEngineEvent(listener: (e: EngineEvent) => void): () => void {
  const sub = DeviceEventEmitter.addListener('scrollpenEvent', (raw: unknown) => {
    try {
      listener(JSON.parse(String(raw)) as EngineEvent);
    } catch {
      // Ignore malformed events rather than crashing the dock.
    }
  });
  return () => sub.remove();
}

export const dock = {
  startDrag: () => native?.startDockDrag(),
  resize: (width: number, height: number) => native?.resizeDock(width, height),
  minimize: () => native?.minimizeDock(),
  quit: () => native?.quit(),
};
