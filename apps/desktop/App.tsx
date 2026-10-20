/**
 * ScrollPen dock (placeholder).
 * The real toolbar lands in src/dock once the native overlay modules exist.
 *
 * @format
 */

import { StyleSheet, Text, useColorScheme, View } from 'react-native';

function App() {
  const isDarkMode = useColorScheme() === 'dark';
  const fg = isDarkMode ? '#f3f3f3' : '#1b1b1b';

  return (
    <View
      style={[
        styles.container,
        { backgroundColor: isDarkMode ? '#202020' : '#fafafa' },
      ]}>
      <Text style={[styles.title, { color: fg }]}>ScrollPen</Text>
      <Text style={[styles.subtitle, { color: fg }]}>
        Dock placeholder: overlay and input modules come next.
      </Text>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    padding: 24,
  },
  title: {
    fontSize: 28,
    fontWeight: '600',
  },
  subtitle: {
    marginTop: 8,
    fontSize: 14,
    opacity: 0.75,
  },
});

export default App;
