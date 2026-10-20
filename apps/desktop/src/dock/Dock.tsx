import React, {useState} from 'react';
import {Pressable, StyleSheet, Text, View} from 'react-native';
import {dock, send, type ToolName} from '../native/scrollpen';
import type {EngineState} from '../native/scrollpen';
import {Glyph, ICON_FONT, type Theme, useTheme} from '../theme';

export const DOCK_WIDTH = 64;

// react-native-windows supports a native tooltip prop that RN's types don't declare.
const tip = (text: string) => ({tooltip: text}) as object;
export const DOCK_HEIGHT = 640;

const TOOLS: {tool: ToolName; glyph: string; label: string; key: string}[] = [
  {tool: 'pen', glyph: Glyph.pen, label: 'Pen', key: 'P'},
  {tool: 'highlighter', glyph: Glyph.highlighter, label: 'Highlighter', key: 'H'},
  {tool: 'laser', glyph: '', label: 'Laser', key: 'L'},
  {tool: 'eraser', glyph: Glyph.eraser, label: 'Eraser', key: 'E'},
  {tool: 'line', glyph: Glyph.line, label: 'Line', key: 'I'},
  {tool: 'arrow', glyph: Glyph.arrow, label: 'Arrow', key: 'A'},
  {tool: 'rect', glyph: Glyph.rect, label: 'Rectangle', key: 'R'},
  {tool: 'ellipse', glyph: Glyph.ellipse, label: 'Ellipse', key: 'O'},
  {tool: 'text', glyph: Glyph.text, label: 'Text', key: 'T'},
];

interface IconButtonProps {
  glyph?: string;
  label: string;
  selected?: boolean;
  disabled?: boolean;
  onPress: () => void;
  theme: Theme;
  children?: React.ReactNode;
}

function IconButton({glyph, label, selected, disabled, onPress, theme, children}: IconButtonProps) {
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={label}
      accessibilityState={{selected, disabled}}
      {...tip(label)}
      disabled={disabled}
      onPress={onPress}
      style={({pressed}) => [
        styles.button,
        selected && {backgroundColor: theme.accent},
        pressed && !selected && {backgroundColor: theme.border},
        disabled && styles.disabled,
      ]}>
      {children ?? (
        <Text style={[styles.icon, {color: selected ? theme.accentFg : theme.fg}]}>{glyph}</Text>
      )}
    </Pressable>
  );
}

const SYNC_LABEL: Record<string, string> = {
  browser: 'Synced with browser page',
  uia: 'Synced with app scroll position',
  optical: 'Synced by watching the window (best effort)',
  none: 'Not synced: ink stays fixed on screen',
};

interface DockProps {
  state: EngineState;
  toast: string | null;
  onOpenSettings: () => void;
}

export function Dock({state, toast, onOpenSettings}: DockProps) {
  const theme = useTheme();
  const [showPalette, setShowPalette] = useState(false);
  const {settings} = state;

  const pickTool = (tool: ToolName) => {
    send({cmd: 'setTool', tool});
    if (!state.active) send({cmd: 'setActive', active: true});
  };

  const syncColor =
    state.sync.source === 'browser' ? '#43A047' : state.sync.source === 'none' ? theme.muted : '#FB8C00';

  return (
    <View style={[styles.dock, {backgroundColor: theme.bg, borderColor: theme.border}]}>
      <Pressable
        accessibilityLabel="Move toolbar"
        {...tip('Drag to move')}
        onPressIn={() => dock.startDrag()}
        style={styles.grip}>
        <Text style={[styles.icon, {color: theme.muted}]}>{Glyph.grip}</Text>
      </Pressable>

      <IconButton
        theme={theme}
        glyph={Glyph.cursor}
        label={state.active ? 'Stop drawing (Esc)' : 'Cursor mode'}
        selected={!state.active}
        onPress={() => send({cmd: 'setActive', active: false})}
      />

      <View style={[styles.sep, {backgroundColor: theme.border}]} />

      {TOOLS.map(t => (
        <IconButton
          key={t.tool}
          theme={theme}
          glyph={t.glyph}
          label={`${t.label} (${t.key})`}
          selected={state.active && state.tool === t.tool}
          onPress={() => pickTool(t.tool)}>
          {t.tool === 'laser' ? (
            <View style={[styles.laserDot, state.active && state.tool === 'laser' && styles.laserDotSelected]} />
          ) : undefined}
        </IconButton>
      ))}

      <View style={[styles.sep, {backgroundColor: theme.border}]} />

      <IconButton theme={theme} label="Colour (1-9)" onPress={() => setShowPalette(v => !v)}>
        <View style={[styles.swatch, {backgroundColor: settings.color, borderColor: theme.border}]} />
      </IconButton>
      {showPalette && (
        <View style={styles.palette}>
          {settings.palette.map((c, i) => (
            <Pressable
              key={c + i}
              accessibilityLabel={`Colour ${i + 1}`}
              onPress={() => {
                send({cmd: 'updateSettings', settings: {color: c}});
                setShowPalette(false);
              }}
              style={[
                styles.paletteSwatch,
                {backgroundColor: c, borderColor: c === settings.color ? theme.accent : theme.border},
              ]}
            />
          ))}
        </View>
      )}

      <IconButton theme={theme} glyph={Glyph.undo} label="Undo (Ctrl+Z)" disabled={!state.canUndo} onPress={() => send({cmd: 'undo'})} />
      <IconButton theme={theme} glyph={Glyph.redo} label="Redo (Ctrl+Y)" disabled={!state.canRedo} onPress={() => send({cmd: 'redo'})} />
      <IconButton theme={theme} glyph={Glyph.clear} label="Clear all (Delete)" disabled={state.itemCount === 0} onPress={() => send({cmd: 'clear'})} />

      <View style={[styles.sep, {backgroundColor: theme.border}]} />

      <IconButton
        theme={theme}
        glyph={Glyph.board}
        label={state.board === 'none' ? 'Whiteboard (W) / Blackboard (K)' : 'Close board'}
        selected={state.board !== 'none'}
        onPress={() =>
          send({cmd: 'setBoard', board: state.board === 'none' ? 'white' : state.board === 'white' ? 'black' : 'none'})
        }
      />
      <IconButton
        theme={theme}
        glyph={Glyph.pin}
        label={settings.documentMode ? `Ink follows scrolling. ${SYNC_LABEL[state.sync.source]}` : 'Ink fixed to screen'}
        selected={settings.documentMode}
        onPress={() => send({cmd: 'setDocumentMode', enabled: !settings.documentMode})}>
        <View>
          <Text style={[styles.icon, {color: settings.documentMode ? theme.accentFg : theme.fg}]}>{Glyph.pin}</Text>
          {settings.documentMode && <View style={[styles.syncDot, {backgroundColor: syncColor}]} />}
        </View>
      </IconButton>
      <IconButton
        theme={theme}
        glyph={state.inkVisible ? Glyph.eye : Glyph.hide}
        label={state.inkVisible ? 'Hide ink' : 'Show ink'}
        onPress={() => send({cmd: 'setInkVisible', visible: !state.inkVisible})}
      />
      <IconButton theme={theme} glyph={Glyph.camera} label="Screenshot (Ctrl+Shift+S)" onPress={() => send({cmd: 'screenshot'})} />

      <View style={styles.flex} />

      <IconButton theme={theme} glyph={Glyph.settings} label="Settings" onPress={onOpenSettings} />
      <IconButton theme={theme} glyph={Glyph.minimize} label="Minimize" onPress={() => dock.minimize()} />
      <IconButton theme={theme} glyph={Glyph.close} label="Quit ScrollPen" onPress={() => dock.quit()} />

      {toast && (
        <View style={[styles.toast, {backgroundColor: theme.fg}]}>
          <Text style={[styles.toastText, {color: theme.bg}]} numberOfLines={3}>
            {toast}
          </Text>
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  dock: {
    flex: 1,
    alignItems: 'center',
    paddingVertical: 6,
    borderWidth: 1,
  },
  grip: {
    width: 48,
    height: 18,
    alignItems: 'center',
    justifyContent: 'center',
  },
  button: {
    width: 40,
    height: 30,
    marginVertical: 1,
    borderRadius: 6,
    alignItems: 'center',
    justifyContent: 'center',
  },
  disabled: {opacity: 0.35},
  icon: {fontFamily: ICON_FONT, fontSize: 16},
  sep: {width: 32, height: 1, marginVertical: 4},
  flex: {flex: 1},
  swatch: {width: 18, height: 18, borderRadius: 9, borderWidth: 2},
  palette: {width: 48, flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'center', marginBottom: 4},
  paletteSwatch: {width: 18, height: 18, borderRadius: 9, margin: 2, borderWidth: 2},
  laserDot: {width: 10, height: 10, borderRadius: 5, backgroundColor: '#FF5252'},
  laserDotSelected: {borderWidth: 2, borderColor: '#FFFFFF'},
  syncDot: {position: 'absolute', right: -6, top: -2, width: 7, height: 7, borderRadius: 4},
  toast: {position: 'absolute', bottom: 4, left: 2, right: 2, padding: 4, borderRadius: 4},
  toastText: {fontSize: 9, textAlign: 'center'},
});
