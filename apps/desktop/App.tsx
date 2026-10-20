/**
 * ScrollPen dock: the floating toolbar that drives the native ink engine.
 *
 * @format
 */

import React, {useEffect, useState} from 'react';
import {Dock, DOCK_HEIGHT, DOCK_WIDTH} from './src/dock/Dock';
import {dock} from './src/native/scrollpen';
import {SettingsPanel, SETTINGS_HEIGHT, SETTINGS_WIDTH} from './src/settings/SettingsPanel';
import {useEngine} from './src/store/useEngine';

function App() {
  const {state, toast} = useEngine();
  const [showSettings, setShowSettings] = useState(false);

  useEffect(() => {
    if (showSettings) dock.resize(SETTINGS_WIDTH, SETTINGS_HEIGHT);
    else dock.resize(DOCK_WIDTH, DOCK_HEIGHT);
  }, [showSettings]);

  return showSettings ? (
    <SettingsPanel state={state} onClose={() => setShowSettings(false)} />
  ) : (
    <Dock state={state} toast={toast} onOpenSettings={() => setShowSettings(true)} />
  );
}

export default App;
