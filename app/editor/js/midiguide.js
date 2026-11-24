// MIDI Guide (Pencil Research): browse the public GitHub tree, then read individual device CSVs.
const REPO = 'https://github.com/pencilresearch/midi';
const TREE = 'https://api.github.com/repos/pencilresearch/midi/git/trees/main?recursive=1';
const RAW = 'https://raw.githubusercontent.com/pencilresearch/midi/main/';
const INDEX_LIMIT = 2 * 1024 * 1024;
const CSV_LIMIT = 512 * 1024;
const cacheDefault = new Map();
const indexKey = 'midiguide:v1:index';

export const MIDI_GUIDE_SOURCE = Object.freeze({
  name: 'MIDI Guide', author: 'Pencil Research and MIDI Guide contributors', url: 'https://midi.guide',
  data: REPO, license: 'CC BY-SA 4.0', licenseUrl: 'https://creativecommons.org/licenses/by-sa/4.0/',
});

// Use exact directory/file names as IDs: lowercasing or slugifying would lose information.
const segment = (s) => typeof s === 'string' && s.length > 0 && s !== '.' && s !== '..' && !/[/\\\u0000-\u001f\u007f\p{Surrogate}]/u.test(s);
const byName = (a, b) => a.name.localeCompare(b.name, 'en', { numeric: true }) || (a.name < b.name ? -1 : Number(a.name > b.name));

function indexFromTree(data) {
  if (!Array.isArray(data?.tree) || data.truncated !== false) throw new Error('MIDI Guide: incomplete GitHub tree');
  const makers = new Map();
  for (const entry of data.tree) {
    if (entry?.type !== 'blob' || !['100644', '100755', undefined].includes(entry.mode) || typeof entry.path !== 'string') continue;
    const [maker, filename, extra] = entry.path.split('/');
    if (extra !== undefined || !segment(maker) || !filename?.endsWith('.csv')) continue;
    const triggers = filename.endsWith('.triggers.csv');
    const device = filename.slice(0, -(triggers ? '.triggers.csv' : '.csv').length);
    if (!segment(device)) continue;
    if (!makers.has(maker)) makers.set(maker, new Map());
    const devices = makers.get(maker);
    if (!devices.has(device)) devices.set(device, { name: device, slug: device, has_ccs: false, has_triggers: false });
    const item = devices.get(device);
    item[triggers ? 'has_triggers' : 'has_ccs'] = true;
    item[triggers ? 'triggers_bytes' : 'ccs_bytes'] = Number.isSafeInteger(entry.size) && entry.size >= 0 ? entry.size : null;
  }
  if (!makers.size) throw new Error('MIDI Guide: no devices found in GitHub tree');
  return [...makers].map(([name, devices]) => ({ name, slug: name, devices: [...devices.values()].sort(byName) })).sort(byName);
}

function responseError(label, cause) {
  return new Error(`MIDI Guide ${label}: ${cause?.message ?? cause}`, { cause });
}

// Content-Length can be missing or describe compressed bytes. Also bound the decoded fetch body.
async function readText(fetch, url, limit) {
  const response = await fetch(url, { credentials: 'omit', redirect: 'error' });
  if (!response.ok || Number(response.headers.get('content-length')) > limit) {
    response.body?.cancel().catch(() => {});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    throw new Error(`response too large (limit: ${limit} bytes)`);
  }
  if (!response.body) return '';
  const reader = response.body.getReader();
  const decoder = new TextDecoder('utf-8', { fatal: true });
  let size = 0;
  const chunks = [];
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > limit) throw new Error(`response too large (limit: ${limit} bytes)`);
      chunks.push(decoder.decode(value, { stream: true }));
    }
    return chunks.join('') + decoder.decode();
  } catch (e) {
    reader.cancel().catch(() => {});
    throw e;
  } finally {
    reader.releaseLock();
  }
}

// A Map-like { get(key), set(key, value) }; callers can persist it to survive a page reload.
// null disables caching. Storage failures do not affect a good network response.
function cached(cache, key) {
  try { return cache?.get(key); } catch { return undefined; }
}

async function refresh(cache, key, read) {
  let fresh;
  try { fresh = await read(); }
  catch (e) {
    const previous = cached(cache, key);
    if (!previous) throw e;
    return { ...previous, stale: true };
  }
  try { cache?.set(key, fresh); } catch { /* storage blocked or full */ }
  return fresh;
}

export async function loadIndex({ fetch = globalThis.fetch, cache = cacheDefault } = {}) {
  return refresh(cache, indexKey, async () => {
    try {
      const makers = indexFromTree(JSON.parse(await readText(fetch, TREE, INDEX_LIMIT)));
      return { makers, source: MIDI_GUIDE_SOURCE, stale: false };
    } catch (e) { throw responseError('index', e); }
  });
}

// CSV records can contain quoted commas, newlines, and doubled quotes. Normalize record endings
// before parsing and allow spreadsheet whitespace after a closing quote, not arbitrary characters.
function* records(text) {
  const csv = text.replace(/^\uFEFF/, '').replace(/\r\n?/g, '\n');
  let row = [], field = '', state = 'start';
  for (let i = 0; i < csv.length; i++) {
    const c = csv[i];
    if (state === 'quoted') {
      if (c !== '"') field += c;
      else if (csv[i + 1] === '"') { field += '"'; i++; }
      else state = 'closed';
    } else if (c === ',' || c === '\n') {
      row.push(field);
      if (c === '\n') { yield row; row = []; }
      field = '';
      state = 'start';
    } else if (state === 'closed') {
      if (c !== ' ' && c !== '\t') throw new Error('invalid CSV quoting');
    } else if (state === 'start' && c === '"') {
      state = 'quoted';
    } else {
      field += c;
      state = 'plain';
    }
  }
  if (state === 'quoted') throw new Error('unclosed CSV quote');
  if (row.length || state !== 'start') yield [...row, field];
}

const int = (value, max, min = 0) => /^\d+$/.test(value) && Number(value) >= min && Number(value) <= max ? Number(value) : null;

function ccRow(cell) {
  const parameter_name = cell('parameter_name');
  const cc_msb = int(cell('cc_msb'), 127);
  const cc_lsb = int(cell('cc_lsb'), 127);
  const nrpn_msb = int(cell('nrpn_msb'), 127);
  const nrpn_lsb = int(cell('nrpn_lsb'), 127);
  if (!parameter_name || (cc_msb === null && (nrpn_msb === null || nrpn_lsb === null))) return null;
  const ccMax = cc_msb !== null && cc_msb < 32 && cc_lsb === cc_msb + 32 ? 16383 : 127;
  return { parameter_name, cc_msb, cc_lsb, nrpn_msb, nrpn_lsb,
    ...values(cell, 'cc', ccMax), ...values(cell, 'nrpn', 16383),
    parameter_description: cell('parameter_description') || null,
    orientation: cell('orientation') || null, notes: cell('notes') || null, usage: cell('usage') || null };
}

function values(cell, prefix, limit) {
  const lo = int(cell(`${prefix}_min_value`), limit);
  const hi = int(cell(`${prefix}_max_value`), limit);
  return {
    [`${prefix}_min_value`]: (lo ?? 0) <= (hi ?? limit) ? lo : null,
    [`${prefix}_max_value`]: hi,
    [`${prefix}_default_value`]: int(cell(`${prefix}_default_value`), hi ?? limit, (lo ?? 0) <= (hi ?? limit) ? lo ?? 0 : 0),
  };
}

function noteRow(cell) {
  const sound_name = cell('sound_name');
  const range = values(cell, 'note_number', 127);
  if (!sound_name || (range.note_number_default_value === null && range.note_number_min_value === null)) return null;
  const ch = cell('midi_channel').toLowerCase();
  const fixed = int(ch, 16, 1);
  return { sound_name, midi_channel: fixed !== null ? String(fixed) : ['basic', 'omni', 'auto'].includes(ch) ? ch : null,
    ...range, sound_description: cell('sound_description') || null,
    comments: cell('comments') || null, velocity_usage: cell('velocity_usage') || null };
}

function parseCSV(text, kind) {
  const rows = records(text);
  let header;
  for (let next = rows.next(); !next.done; next = rows.next()) {
    if (next.value.some((s) => s.trim())) { header = next.value.map((s) => s.trim().toLowerCase()); break; }
  }
  const columns = new Map(header?.map((name, i) => [name, i]));
  if (kind === 'ccs' ? !(columns.has('parameter_name') && (columns.has('cc_msb') || (columns.has('nrpn_msb') && columns.has('nrpn_lsb'))))
    : !(columns.has('sound_name') && (columns.has('note_number_default_value') || columns.has('note_number_min_value')))) {
    throw new Error('unexpected CSV columns');
  }
  const groups = new Map();
  let count = 0;
  for (const row of rows) {
    const cell = (name) => row[columns.get(name)]?.trim() ?? '';
    const item = kind === 'ccs' ? ccRow(cell) : noteRow(cell);
    if (!item) continue;
    const section = cell('section') || 'General';
    if (!groups.has(section)) groups.set(section, []);
    groups.get(section).push(item);
    count++;
  }
  return { groups: Object.fromEntries(groups), count };
}

export async function loadDevice(maker, device, { fetch = globalThis.fetch, cache = cacheDefault } = {}) {
  if (!segment(maker) || !segment(device)) throw new Error('MIDI Guide: invalid maker or device name');
  const makerPath = encodeURIComponent(maker);
  const devicePath = `${makerPath}/${encodeURIComponent(device)}`;
  return refresh(cache, `midiguide:v1:device:${devicePath}`, async () => {
    const index = cached(cache, indexKey) ?? await loadIndex({ fetch, cache });
    const record = index.makers.find((m) => m.slug === maker)?.devices.find((d) => d.slug === device);
    if (!record) throw new Error('MIDI Guide: device not found in the index');
    return readDevice(maker, device, makerPath, devicePath, record, fetch);
  });
}

async function readDevice(maker, device, makerPath, devicePath, record, fetch) {
  for (const [size, suffix] of [[record.ccs_bytes, '.csv'], [record.triggers_bytes, '.triggers.csv']]) {
    if (size > CSV_LIMIT) throw responseError(device + suffix, new Error(`file too large (limit: ${CSV_LIMIT} bytes)`));
  }
  const files = [];
  const load = async (enabled, suffix, kind) => {
    if (!enabled) return { groups: {}, count: 0 };
    const path = devicePath + suffix;
    files.push(`${REPO}/blob/main/${path}`);
    try { return parseCSV(await readText(fetch, RAW + path, CSV_LIMIT), kind); }
    catch (e) { throw responseError(device + suffix, e); }
  };
  const [ccs, triggers] = await Promise.all([
    load(record.has_ccs, '.csv', 'ccs'), load(record.has_triggers, '.triggers.csv', 'triggers'),
  ]);
  if (!ccs.count && !triggers.count) throw new Error(`MIDI Guide ${device}: no usable MIDI mappings`);
  const repo_url = files.length === 1 ? files[0] : `${REPO}/tree/main/${makerPath}`;
  return {
    metadata: { manufacturer: maker, device, manufacturer_slug: maker, device_slug: device,
      repo_url, last_updated: null, has_ccs: record.has_ccs, has_triggers: record.has_triggers,
      parameter_count: ccs.count, trigger_count: triggers.count },
    ccs: ccs.groups, triggers: triggers.groups,
    source: { ...MIDI_GUIDE_SOURCE, data: repo_url, files }, stale: false,
  };
}
