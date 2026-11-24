// One icon set: 20 × 20 strokes in currentColor, so icons follow the theme and the text beside them.

const P = {
  library: '<path d="M4 4h4v12H4zM10 4h3v12h-3zM14.5 4.5l2.8.8-3 11.4-2.8-.8z"/>',
  undo: '<path d="M7 5 3.5 8.5 7 12"/><path d="M4 8.5h8a4 4 0 0 1 0 8H9"/>',
  redo: '<path d="m13 5 3.5 3.5L13 12"/><path d="M16 8.5H8a4 4 0 0 0 0 8h3"/>',
  file: '<path d="M5 2.5h6.5L15 6v11.5H5z"/><path d="M11.5 2.5V6H15"/>',
  settings: '<circle cx="10" cy="10" r="2.5"/><path d="M10 2.5v2M10 15.5v2M2.5 10h2M15.5 10h2M4.7 4.7l1.4 1.4M13.9 13.9l1.4 1.4M4.7 15.3l1.4-1.4M13.9 6.1l1.4-1.4"/>',
  sliders: '<path d="M3.5 6h3M10.5 6h6M3.5 14h6M13.5 14h3"/><circle cx="8.5" cy="6" r="2"/><circle cx="11.5" cy="14" r="2"/>',
  chevron: '<path d="m6 8 4 4 4-4"/>',
  keys: '<rect x="3" y="5" width="14" height="10" rx="1.5"/><path d="M7.5 5v6M10 5v10M12.5 5v6"/>',
  more: '<circle cx="5" cy="10" r=".9" fill="currentColor"/><circle cx="10" cy="10" r=".9" fill="currentColor"/><circle cx="15" cy="10" r=".9" fill="currentColor"/>',
  lock: '<rect x="5" y="9" width="10" height="8" rx="1.5"/><path d="M7 9V6.5a3 3 0 0 1 6 0V9"/>',
  external: '<path d="M11.5 4.5h4v4"/><path d="M15.5 4.5 9 11"/><path d="M13.5 11.5v3a1.5 1.5 0 0 1-1.5 1.5H5.5A1.5 1.5 0 0 1 4 14.5V8a1.5 1.5 0 0 1 1.5-1.5h3"/>',
  link: '<path d="M8.5 11.5 11.5 8.5"/><path d="M9 6.5 10.5 5a3 3 0 0 1 4.5 4.5L13.5 11M11 13.5 9.5 15A3 3 0 0 1 5 10.5L6.5 9"/>',
  unlink: '<path d="M9 6.5 10.5 5a3 3 0 0 1 4.5 4.5L13.5 11M11 13.5 9.5 15A3 3 0 0 1 5 10.5L6.5 9"/><path d="M3.5 3.5 6 6M14 14l2.5 2.5"/>',
  learn: '<circle cx="10" cy="10" r="1.8"/><path d="M6.6 6.6a4.8 4.8 0 0 0 0 6.8M13.4 6.6a4.8 4.8 0 0 1 0 6.8M4 4a8.5 8.5 0 0 0 0 12M16 4a8.5 8.5 0 0 1 0 12"/>',
  swap: '<path d="M6 3v13M3 13l3 3 3-3M14 17V4M11 7l3-3 3 3"/>',
  sun: '<circle cx="10" cy="10" r="3.2"/><path d="M10 2.5v1.8M10 15.7v1.8M2.5 10h1.8M15.7 10h1.8M4.7 4.7l1.3 1.3M14 14l1.3 1.3M4.7 15.3 6 14M14 6l1.3-1.3"/>',
  moon: '<path d="M15.5 12.5A6 6 0 0 1 7.5 4.5a6 6 0 1 0 8 8z"/>',
  auto: '<circle cx="10" cy="10" r="6.5"/><path d="M10 3.5v13a6.5 6.5 0 0 0 0-13z" fill="currentColor"/>',
  usb: '<path d="M10 2.5v12M7.5 5 10 2.5 12.5 5M10 14.5a1.8 1.8 0 1 0 0 3.6 1.8 1.8 0 0 0 0-3.6zM10 11 6 9V7M10 9.5l4-2V6"/><rect x="5" y="5.5" width="2" height="2"/><circle cx="14" cy="5.5" r="1"/>',
  bluetooth: '<path d="m6 6.5 8 7-4 3.5v-14l4 3.5-8 7"/>',
  close: '<path d="m5 5 10 10M15 5 5 15"/>',
  check: '<path d="m4.5 10.5 3.5 3.5 7.5-8"/>',
  copy: '<rect x="7" y="7" width="9" height="10" rx="1.5"/><path d="M13 7V4.5A1.5 1.5 0 0 0 11.5 3h-6A1.5 1.5 0 0 0 4 4.5v8A1.5 1.5 0 0 0 5.5 14H7"/>',
  paste: '<rect x="5" y="4" width="10" height="13" rx="1.5"/><path d="M8 4V3h4v1"/>',
  trash: '<path d="M4 6h12M8 6V4h4v2M6 6l.8 11h6.4L14 6"/>',
  download: '<path d="M10 3v10M6 9l4 4 4-4M4 16.5h12"/>',
  upload: '<path d="M10 14V4M6 8l4-4 4 4M4 16.5h12"/>',
  monitor: '<path d="M3 15h14M5 15V9M8 15V5M11 15v-7M14 15V7"/>',
  sim: '<rect x="3" y="3" width="14" height="14" rx="2"/><path d="M7 6v8M13 6v8"/><rect x="5.5" y="10" width="3" height="2" rx=".5"/><rect x="11.5" y="7" width="3" height="2" rx=".5"/>',
  info: '<circle cx="10" cy="10" r="7"/><path d="M10 9v4.5M10 6.5v.1"/>',
  plus: '<path d="M10 4v12M4 10h12"/>',
  play: '<path d="M7 5v10l8-5z" fill="currentColor"/>',
  pause: '<path d="M7 5v10M13 5v10" stroke-width="2.6"/>',
  rec: '<circle cx="10" cy="10" r="4.5" fill="currentColor"/>',
  rew: '<path d="M10 5v10L4 10zM16 5v10l-6-5z" fill="currentColor"/>',
  ff: '<path d="M4 5v10l6-5zM10 5v10l6-5z" fill="currentColor"/>',
  bankL: '<path d="m9 5-5 5 5 5M15 5l-5 5 5 5"/>',
  bankR: '<path d="m5 5 5 5-5 5M11 5l5 5-5 5"/>',
  up: '<path d="M10 5 4.5 15h11z"/>',
  down: '<path d="M10 15 4.5 5h11z"/>',
  left: '<path d="M5 10 15 4.5v11z"/>',
  right: '<path d="M15 10 5 4.5v11z"/>',
};

export function icon(name, size = 18, title) {
  const span = document.createElement('span');
  span.className = 'ic';
  span.setAttribute('aria-hidden', title ? 'false' : 'true');
  span.innerHTML = `<svg width="${size}" height="${size}" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">${title ? `<title>${title}</title>` : ''}${P[name] ?? ''}</svg>`;
  return span;
}

// The faces printed on the mixer's bottom row (docs/hardware.md; the owner's photo).
export const ROW_ICON = { play: 'play', stop: 'pause', rec: 'rec', rew: 'rew', ff: 'ff', bankLeft: 'bankL', bankRight: 'bankR', up: 'up', down: 'down', left: 'left', right: 'right' };
