import {useColorScheme} from 'react-native';

// Segoe MDL2 Assets ships with both Windows 10 and 11.
export const ICON_FONT = 'Segoe MDL2 Assets';

export const Glyph = {
  grip: '',
  cursor: '',
  pen: '',
  highlighter: '',
  eraser: '',
  line: '',
  arrow: '',
  rect: '',
  ellipse: '',
  text: '',
  undo: '',
  redo: '',
  clear: '',
  board: '',
  pin: '',
  camera: '',
  eye: '',
  hide: '',
  settings: '',
  minimize: '',
  close: '',
  back: '',
} as const;

export interface Theme {
  bg: string;
  surface: string;
  border: string;
  fg: string;
  muted: string;
  accent: string;
  accentFg: string;
  danger: string;
}

const light: Theme = {
  bg: '#F7F7F8',
  surface: '#FFFFFF',
  border: '#E1E1E6',
  fg: '#1B1B1F',
  muted: '#6B6B76',
  accent: '#2563EB',
  accentFg: '#FFFFFF',
  danger: '#D32F2F',
};

const dark: Theme = {
  bg: '#1F1F23',
  surface: '#2A2A30',
  border: '#3A3A42',
  fg: '#F2F2F5',
  muted: '#A0A0AB',
  accent: '#5B8DEF',
  accentFg: '#0B0B0D',
  danger: '#EF5350',
};

export function useTheme(): Theme {
  return useColorScheme() === 'dark' ? dark : light;
}
