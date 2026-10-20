import React, {useState} from 'react';
import {Pressable, ScrollView, StyleSheet, Switch, Text, TextInput, View} from 'react-native';
import {send, type EngineSettings, type EngineState} from '../native/scrollpen';
import {Glyph, ICON_FONT, type Theme, useTheme} from '../theme';

export const SETTINGS_WIDTH = 340;
export const SETTINGS_HEIGHT = 640;

const HOTKEY_LABELS: Record<string, string> = {
  toggleDraw: 'Start / stop drawing',
  clear: 'Clear all ink',
  toggleInk: 'Hide / show ink',
  screenshot: 'Screenshot',
  toggleMode: 'Follow scrolling on / off',
};

const CLICK_THROUGH_KEYS = ['CapsLock', '`', 'F8', 'RCtrl', ''];

function update(settings: Partial<EngineSettings>) {
  send({cmd: 'updateSettings', settings});
}

function Row({label, hint, children, theme}: {label: string; hint?: string; children: React.ReactNode; theme: Theme}) {
  return (
    <View style={styles.row}>
      <View style={styles.rowText}>
        <Text style={[styles.label, {color: theme.fg}]}>{label}</Text>
        {hint ? <Text style={[styles.hint, {color: theme.muted}]}>{hint}</Text> : null}
      </View>
      {children}
    </View>
  );
}

function HotkeyField({name, value, error, theme}: {name: string; value: string; error: boolean; theme: Theme}) {
  const [draft, setDraft] = useState(value);
  return (
    <View style={styles.hotkeyRow}>
      <Text style={[styles.hotkeyLabel, {color: theme.fg}]}>{HOTKEY_LABELS[name] ?? name}</Text>
      <TextInput
        value={draft}
        onChangeText={setDraft}
        onBlur={() => draft !== value && update({hotkeys: {[name]: draft}})}
        onSubmitEditing={() => update({hotkeys: {[name]: draft}})}
        placeholder="none"
        style={[
          styles.hotkeyInput,
          {color: theme.fg, borderColor: error ? theme.danger : theme.border, backgroundColor: theme.surface},
        ]}
      />
    </View>
  );
}

export function SettingsPanel({state, onClose}: {state: EngineState; onClose: () => void}) {
  const theme = useTheme();
  const s = state.settings;

  return (
    <View style={[styles.panel, {backgroundColor: theme.bg, borderColor: theme.border}]}>
      <View style={styles.header}>
        <Pressable accessibilityLabel="Back" onPress={onClose} style={styles.back}>
          <Text style={[styles.icon, {color: theme.fg}]}>{Glyph.back}</Text>
        </Pressable>
        <Text style={[styles.title, {color: theme.fg}]}>Settings</Text>
      </View>
      <ScrollView contentContainerStyle={styles.content}>
        <Text style={[styles.section, {color: theme.muted}]}>DRAWING</Text>
        <Row theme={theme} label="Follow scrolling" hint="Ink sticks to the page or document as you scroll.">
          <Switch value={s.documentMode} onValueChange={v => update({documentMode: v})} />
        </Row>
        <Row theme={theme} label="Filled shapes">
          <Switch value={s.fill} onValueChange={v => update({fill: v})} />
        </Row>
        <Row theme={theme} label="Laser fade" hint={`${s.laserFadeSeconds.toFixed(1)} s`}>
          <View style={styles.stepper}>
            {[-0.4, 0.4].map(d => (
              <Pressable
                key={d}
                accessibilityLabel={d < 0 ? 'Shorter' : 'Longer'}
                onPress={() => update({laserFadeSeconds: Math.round((s.laserFadeSeconds + d) * 10) / 10})}
                style={[styles.stepButton, {borderColor: theme.border}]}>
                <Text style={{color: theme.fg}}>{d < 0 ? '−' : '+'}</Text>
              </Pressable>
            ))}
          </View>
        </Row>
        <Row theme={theme} label="Sizes" hint={`Pen ${s.penWidth.toFixed(0)} · Marker ${s.highlighterWidth.toFixed(0)} · Shapes ${s.shapeWidth.toFixed(0)} · Text ${s.fontSize.toFixed(0)}. Change with [ and ] while drawing.`}>
          <View />
        </Row>

        <Text style={[styles.section, {color: theme.muted}]}>WHILE DRAWING</Text>
        <Row theme={theme} label="Hold to click through" hint="Hold this key to click or type in apps without leaving the pen.">
          <View />
        </Row>
        <View style={styles.chips}>
          {CLICK_THROUGH_KEYS.map(k => (
            <Pressable
              key={k || 'none'}
              onPress={() => update({clickThroughKey: k})}
              style={[
                styles.chip,
                {borderColor: theme.border},
                s.clickThroughKey === k && {backgroundColor: theme.accent, borderColor: theme.accent},
              ]}>
              <Text style={{color: s.clickThroughKey === k ? theme.accentFg : theme.fg, fontSize: 12}}>{k || 'Off'}</Text>
            </Pressable>
          ))}
        </View>
        <Text style={[styles.hint, styles.para, {color: theme.muted}]}>
          Shortcuts while drawing: P pen · H highlighter · L laser · E eraser · I line · A arrow · R rectangle · O ellipse ·
          T text · W whiteboard · K blackboard · 1-9 colours · Ctrl+Z / Ctrl+Y · Ctrl+↑/↓ nudge synced ink · Delete clear ·
          Esc stop drawing. The mouse wheel and touchpad keep scrolling the app underneath.
        </Text>

        <Text style={[styles.section, {color: theme.muted}]}>GLOBAL HOTKEYS</Text>
        {Object.entries(s.hotkeys).map(([name, value]) => (
          <HotkeyField
            key={name + value}
            name={name}
            value={value}
            error={state.hotkeyErrors.includes(name)}
            theme={theme}
          />
        ))}
        {state.hotkeyErrors.length > 0 && (
          <Text style={[styles.hint, {color: theme.danger}]}>
            Red hotkeys are invalid or already used by another app. Try a different combination.
          </Text>
        )}

        <Text style={[styles.section, {color: theme.muted}]}>SCREEN SHARING</Text>
        <Row theme={theme} label="Hide toolbar from screen shares" hint="Viewers see your ink but not this toolbar. Share your entire screen (not one window) so ink is visible.">
          <Switch value={s.hideDockFromCapture} onValueChange={v => update({hideDockFromCapture: v})} />
        </Row>

        <Text style={[styles.section, {color: theme.muted}]}>BROWSER SYNC</Text>
        <Text style={[styles.hint, styles.para, {color: theme.muted}]}>
          {state.sync.serverRunning
            ? `Listening on this PC only (127.0.0.1:${s.syncPort}). Install the ScrollPen Browser Sync extension in Chrome or Edge for exact sync on web pages.`
            : `Browser sync is off: port ${s.syncPort} is in use by another program.`}
        </Text>

        <Text style={[styles.section, {color: theme.muted}]}>PRIVACY</Text>
        <Text style={[styles.hint, styles.para, {color: theme.muted}]}>
          Everything stays on this PC. ScrollPen only watches the keyboard and mouse while you are drawing, never
          records keystrokes, and stores ink in memory only. Settings live in %AppData%\ScrollPen.
        </Text>

        <Pressable onPress={() => send({cmd: 'resetSettings'})} style={[styles.reset, {borderColor: theme.border}]}>
          <Text style={{color: theme.fg}}>Reset all settings</Text>
        </Pressable>
        <Text style={[styles.hint, {color: theme.muted}]}>ScrollPen 0.1.0</Text>
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  panel: {flex: 1, borderWidth: 1},
  header: {flexDirection: 'row', alignItems: 'center', padding: 8},
  back: {width: 32, height: 32, alignItems: 'center', justifyContent: 'center'},
  icon: {fontFamily: ICON_FONT, fontSize: 14},
  title: {fontSize: 16, fontWeight: '600', marginLeft: 4},
  content: {paddingHorizontal: 14, paddingBottom: 24},
  section: {fontSize: 11, fontWeight: '600', marginTop: 16, marginBottom: 6, letterSpacing: 0.5},
  row: {flexDirection: 'row', alignItems: 'center', paddingVertical: 6},
  rowText: {flex: 1, paddingRight: 8},
  label: {fontSize: 13},
  hint: {fontSize: 11, marginTop: 2},
  para: {lineHeight: 16},
  stepper: {flexDirection: 'row'},
  stepButton: {width: 28, height: 26, borderWidth: 1, borderRadius: 4, marginLeft: 4, alignItems: 'center', justifyContent: 'center'},
  chips: {flexDirection: 'row', flexWrap: 'wrap', marginBottom: 6},
  chip: {paddingHorizontal: 10, paddingVertical: 4, borderWidth: 1, borderRadius: 12, marginRight: 6, marginBottom: 6},
  hotkeyRow: {flexDirection: 'row', alignItems: 'center', marginBottom: 6},
  hotkeyLabel: {flex: 1, fontSize: 12},
  hotkeyInput: {width: 130, height: 28, borderWidth: 1, borderRadius: 4, paddingHorizontal: 6, fontSize: 12},
  reset: {marginTop: 20, padding: 8, borderWidth: 1, borderRadius: 6, alignItems: 'center', marginBottom: 8},
});
