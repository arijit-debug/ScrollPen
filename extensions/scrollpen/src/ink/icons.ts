// Toolbar icons: 24x24 stroked outlines, drawn with DOM calls.
import { svg } from './render';

type Shape = ['path', string] | ['circle', number, number, number] | ['rect', number, number, number, number, number];

const ICONS: Record<string, Shape[]> = {
  grip: [['circle', 9, 6, 1], ['circle', 15, 6, 1], ['circle', 9, 12, 1], ['circle', 15, 12, 1], ['circle', 9, 18, 1], ['circle', 15, 18, 1]],
  cursor: [['path', 'M6 3l12 9-5.5 1.2L15.5 20l-2.6 1.2-3-6.6L6 18z']],
  pen: [['path', 'M4 20l1.2-4.6L16 4.6a2 2 0 012.8 0l.6.6a2 2 0 010 2.8L8.6 18.8z'], ['path', 'M14 6.6l3.4 3.4']],
  highlighter: [['path', 'M8 15l-2 4h5l1.5-2'], ['path', 'M8 15l8.6-10a2 2 0 012.9-.2l.7.7a2 2 0 01-.2 2.9L10 17z'], ['path', 'M3 21h18']],
  laser: [['circle', 12, 12, 3], ['path', 'M12 3v3M12 18v3M3 12h3M18 12h3M5.6 5.6l2.1 2.1M16.3 16.3l2.1 2.1M5.6 18.4l2.1-2.1M16.3 7.7l2.1-2.1']],
  eraser: [['path', 'M8.5 20H20'], ['path', 'M4.6 15.4l9.9-9.9a2 2 0 012.8 0l2.2 2.2a2 2 0 010 2.8L11 19H8.2z'], ['path', 'M9.5 10.5l5 5']],
  line: [['path', 'M5 19L19 5']],
  arrow: [['path', 'M5 19L19 5'], ['path', 'M10 5h9v9']],
  rect: [['rect', 4, 6, 16, 12, 1.5]],
  ellipse: [['path', 'M20 12c0 3.3-3.6 6-8 6s-8-2.7-8-6 3.6-6 8-6 8 2.7 8 6z']],
  text: [['path', 'M5 6V4.5h14V6'], ['path', 'M12 4.5V20'], ['path', 'M9 20h6']],
  undo: [['path', 'M9 14L4 9l5-5'], ['path', 'M4 9h10.5a5.5 5.5 0 010 11H11']],
  redo: [['path', 'M15 14l5-5-5-5'], ['path', 'M20 9H9.5a5.5 5.5 0 000 11H13']],
  clear: [['path', 'M4 7h16'], ['path', 'M9 7V4.5h6V7'], ['path', 'M6.5 7l1 13h9l1-13'], ['path', 'M10 11v5M14 11v5']],
  board: [['rect', 3, 4, 18, 12, 1.5], ['path', 'M12 16v4M8 20h8']],
  eye: [['path', 'M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12z'], ['circle', 12, 12, 3]],
  eyeOff: [['path', 'M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12z'], ['path', 'M4 4l16 16']],
  camera: [['path', 'M4 8h3l1.8-2.5h6.4L17 8h3v11H4z'], ['circle', 12, 13, 3.5]],
  settings: [
    ['path', 'M4 6h9M17 6h3M4 12h3M11 12h9M4 18h11M19 18h1'],
    ['circle', 15, 6, 2],
    ['circle', 9, 12, 2],
    ['circle', 17, 18, 2],
  ],
  close: [['path', 'M6 6l12 12M18 6L6 18']],
  pin: [['path', 'M9 3h6l-1 6 3 3v2H7v-2l3-3z'], ['path', 'M12 14v7']],
  pinOff: [['path', 'M9 3h6l-1 6 3 3v2H7v-2l3-3z'], ['path', 'M12 14v7'], ['path', 'M4 4l16 16']],
  minimize: [['path', 'M6 12h12']],
  restore: [['rect', 5, 5, 14, 14, 2]],
};

export type IconName = keyof typeof ICONS;

export function icon(name: IconName): SVGSVGElement {
  const root = svg('svg', {
    viewBox: '0 0 24 24',
    width: 20,
    height: 20,
    fill: 'none',
    stroke: 'currentColor',
    'stroke-width': 1.8,
    'stroke-linecap': 'round',
    'stroke-linejoin': 'round',
    'aria-hidden': 'true',
  });
  for (const s of ICONS[name]) {
    if (s[0] === 'path') root.append(svg('path', { d: s[1] }));
    else if (s[0] === 'circle') root.append(svg('circle', { cx: s[1], cy: s[2], r: s[3], ...(s[3] <= 1 ? { fill: 'currentColor' } : {}) }));
    else root.append(svg('rect', { x: s[1], y: s[2], width: s[3], height: s[4], rx: s[5] }));
  }
  return root;
}
