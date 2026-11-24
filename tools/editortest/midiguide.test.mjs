import test from 'node:test';
import assert from 'node:assert/strict';
import { loadIndex, loadDevice } from '../../app/editor/js/midiguide.js';
import { fromMidiGuide } from '../../app/editor/js/templates.js';

const TREE = 'https://api.github.com/repos/pencilresearch/midi/git/trees/main?recursive=1';
const RAW = 'https://raw.githubusercontent.com/pencilresearch/midi/main/';
const blob = (path, size = 100) => ({ type: 'blob', path, size });
const tree = (...entries) => Response.json({ tree: entries, truncated: false });

// Mock only the public read boundary. Use real Response bodies, decoding and conversion.
function server(...responses) {
  const calls = [];
  const fetch = async (url, options) => {
    calls.push(String(url));
    assert.equal(options.credentials, 'omit');
    assert.ok(responses.length, `unexpected request for ${url}`);
    const response = responses.shift();
    if (response instanceof Error) throw response;
    return response;
  };
  return { fetch, calls };
}

test('the GitHub tree lists CC-only, trigger-only and paired instruments without prefetching CSVs', async () => {
  const { fetch, calls } = server(tree(
    blob('Z Tools/Kit.triggers.csv'), blob('A Tools/Synth+.triggers.csv'), blob('A Tools/Synth+.csv'),
    blob('A Tools/Synth.csv'), blob('A Tools/nested/Demo.csv'), blob('template.csv'),
    blob('A Tools/readme.md'), { type: 'tree', path: 'A Tools' },
  ));
  const index = await loadIndex({ fetch, cache: new Map() });
  assert.deepEqual(calls, [TREE]);
  assert.deepEqual(index.makers.map(({ name }) => name), ['A Tools', 'Z Tools']);
  assert.deepEqual(index.makers.flatMap(({ devices }) => devices.map(({ name, slug, has_ccs, has_triggers }) => (
    [name, slug, has_ccs, has_triggers]
  ))), [['Synth', 'Synth', true, false], ['Synth+', 'Synth+', true, true], ['Kit', 'Kit', false, true]]);
  assert.equal(index.makers[0].slug, 'A Tools');
  assert.equal(index.source.license, 'CC BY-SA 4.0');
  assert.equal(index.source.url, 'https://midi.guide');
  assert.equal(index.stale, false);
});

const ccHeader = 'manufacturer,device,section,parameter_name,parameter_description,cc_msb,cc_lsb,cc_min_value,cc_max_value,cc_default_value,nrpn_msb,nrpn_lsb,nrpn_min_value,nrpn_max_value,nrpn_default_value,orientation,notes,usage';
const noteHeader = 'manufacturer,device,section,sound_name,sound_description,midi_channel,note_number_min_value,note_number_max_value,note_number_default_value,comments,velocity_usage';
const text = (...lines) => new Response(lines.join('\r\n'));

test('one selected device fetches only its known CSVs and parses quoted data for the template converter', async () => {
  const { fetch, calls } = server(tree(blob('Á Tools/Synth+ #1.csv'), blob('Á Tools/Synth+ #1.triggers.csv'), blob('Á Tools/Other.csv')),
    text('\uFEFF' + ccHeader, 'Á Tools,Synth+ #1,Filter,"Filter: Cutoff, ""fine""","First line',
      'second line",74,,0,127,,1,0,0,16383,,0-based,,', '',
      'Á Tools,Synth+ #1,__proto__,Resonance,,0,,0,127,,,,,,,,,'),
    text(noteHeader, 'Á Tools,Synth+ #1,Drums,Kick,,10,35,36,36,,1~127: Volume',
      'Á Tools,Synth+ #1,Drums,Chromatic,,auto,0,127,,,',
      'Á Tools,Synth+ #1,Drums,Invalid,,99,128,128,128,,'));
  const cache = new Map();
  const index = await loadIndex({ fetch, cache });
  const maker = index.makers[0], instrument = maker.devices[1];
  const device = await loadDevice(maker.slug, instrument.slug, { fetch, cache });
  assert.deepEqual(calls, [TREE, RAW + '%C3%81%20Tools/Synth%2B%20%231.csv', RAW + '%C3%81%20Tools/Synth%2B%20%231.triggers.csv']);
  assert.equal(device.metadata.parameter_count, 2);
  assert.equal(device.metadata.trigger_count, 2);
  const list = fromMidiGuide(device);
  assert.equal(list.name, 'Á Tools Synth+ #1');
  assert.deepEqual(list.params.map(({ name, type, number, channel }) => [name, type, number, channel]), [
    ['Cutoff, "fine"', 'nrpn', 128, undefined], ['Resonance', 'cc7', 0, undefined],
    ['Kick', 'note', 36, 10], ['Chromatic', 'note', 0, undefined],
  ]);
  assert.equal(list.params[0].hi, 16383);
  assert.equal(list.params[0].about, 'First line\nsecond line');
  assert.equal(list.source.license, 'CC BY-SA 4.0');
  assert.equal(list.source.data, 'https://github.com/pencilresearch/midi/tree/main/%C3%81%20Tools');
  assert.deepEqual(list.source.files, calls.slice(1).map((x) => x.replace(RAW, 'https://github.com/pencilresearch/midi/blob/main/')));
});

test('a failed refresh returns only the previously validated index, marked stale', async () => {
  const cache = new Map();
  const { fetch, calls } = server(tree(blob('A/Synth.csv')), Response.json({ tree: [], truncated: true }), new Response('', { status: 403 }));
  const fresh = await loadIndex({ fetch, cache });
  for (let i = 0; i < 2; i++) {
    const stale = await loadIndex({ fetch, cache });
    assert.equal(stale.stale, true);
    assert.deepEqual(stale.makers, fresh.makers);
  }
  assert.equal(fresh.stale, false);
  assert.deepEqual(calls, [TREE, TREE, TREE]);
});

test('an offline device reopens only from a complete cached download', async () => {
  const cache = new Map();
  const { fetch, calls } = server(tree(blob('A/Kit.csv'), blob('A/Kit.triggers.csv')),
    text(ccHeader, 'A,Kit,Levels,Level,,7,,0,127,,,,,,,,,'), text(noteHeader, 'A,Kit,Kit,Kick,,10,36,36,36,,'),
    text(ccHeader, 'A,Kit,Levels,New level,,8,,0,127,,,,,,,,,'), new Error('offline'));
  const fresh = await loadDevice('A', 'Kit', { fetch, cache });
  const stale = await loadDevice('A', 'Kit', { fetch, cache });
  assert.equal(fresh.stale, false);
  assert.equal(stale.stale, true);
  assert.deepEqual(fromMidiGuide(stale), fromMidiGuide(fresh));
  assert.deepEqual(calls, [TREE, RAW + 'A/Kit.csv', RAW + 'A/Kit.triggers.csv', RAW + 'A/Kit.csv', RAW + 'A/Kit.triggers.csv']);
});

test('unavailable storage does not break successful network reads', async () => {
  const { fetch, calls } = server(tree(blob('A/Kit.triggers.csv')), text(noteHeader, 'A,Kit,Kit,Kick,,10,36,36,36,,'));
  const cache = { get() { throw Error('blocked storage'); }, set() { throw Error('storage limit'); } };
  assert.equal((await loadDevice('A', 'Kit', { fetch, cache })).metadata.trigger_count, 1);
  assert.deepEqual(calls, [TREE, RAW + 'A/Kit.triggers.csv']);
});

test('an oversized announced index is rejected before its body is read', async () => {
  const { fetch } = server(new Response('not JSON', { headers: { 'content-length': '2097153' } }));
  await assert.rejects(loadIndex({ fetch, cache: null }), /index.*too large/i);
});

test('a CSV that exceeds the byte limit cancels the stream even without Content-Length', async () => {
  let cancelled = false;
  const payload = new TextEncoder().encode(ccHeader + '\n');
  const body = new ReadableStream({
    start(controller) { controller.enqueue(payload); controller.enqueue(new Uint8Array(512 * 1024)); },
    cancel() { cancelled = true; },
  });
  const { fetch } = server(tree(blob('A/Synth.csv')), new Response(body));
  await assert.rejects(loadDevice('A', 'Synth', { fetch, cache: null }), /Synth\.csv.*too large/i);
  assert.equal(cancelled, true);
});

test('a cold network error identifies the failing MIDI Guide request', async () => {
  const cause = new TypeError('Failed to fetch');
  const { fetch } = server(cause);
  await assert.rejects(loadIndex({ fetch, cache: null }), (e) => /MIDI Guide index.*Failed to fetch/i.test(e.message) && e.cause === cause);
});

test('an advertised CSV returning an HTTP error never creates a partial offline device', async () => {
  const cache = new Map();
  const { fetch } = server(tree(blob('A/Kit.csv'), blob('A/Kit.triggers.csv')),
    text(ccHeader, 'A,Kit,Levels,Level,,7,,0,127,,,,,,,,,'), new Response('', { status: 404 }),
    new Error('offline'), new Error('offline'));
  await assert.rejects(loadDevice('A', 'Kit', { fetch, cache }), /Kit\.triggers\.csv.*HTTP 404/i);
  await assert.rejects(loadDevice('A', 'Kit', { fetch, cache }), /offline/i);
});

test('traversal IDs and unknown devices never cause raw GitHub requests', async () => {
  const cache = new Map();
  const { fetch, calls } = server(tree(blob('A/Synth.csv')));
  await assert.rejects(loadDevice('..', 'Synth', { fetch, cache }), /invalid/i);
  await assert.rejects(loadDevice('A', 'bad/synth', { fetch, cache }), /invalid/i);
  await assert.rejects(loadDevice('A', '%2e%2e', { fetch, cache }), /not found/i);
  assert.deepEqual(calls, [TREE]);
});

test('documented dash ranges make a switch but mixed continuous ranges do not', async () => {
  const { fetch } = server(tree(blob('A/Synth.csv')), text(
    'section,parameter_name,cc_msb,cc_min_value,cc_max_value,usage',
    'Main,Mute,94,0,127,0-63: Off; 64-127: On',
    'Main,Morph,95,0,127,0: Off; 1-10: Square; 11-20: Saw; 21~127: Morph',
    'Main,Malformed,96,0,127,0: Off; 128: Outside',
  ));
  const [mute, morph, malformed] = fromMidiGuide(await loadDevice('A', 'Synth', { fetch, cache: null })).params;
  assert.deepEqual(mute.values, [[0, 'Off'], [64, 'On']]);
  assert.equal(mute.button, true);
  assert.equal(morph.values, undefined);
  assert.equal(malformed.values, undefined);
});

test('explicit high-resolution pairs preserve a valid 14-bit CC range', async () => {
  const { fetch } = server(tree(blob('A/Synth.csv')), text(
    'section,parameter_name,cc_msb,cc_lsb,cc_min_value,cc_max_value',
    'Main,Fine,1,33,200,1000',
  ));
  const [p] = fromMidiGuide(await loadDevice('A', 'Synth', { fetch, cache: null })).params;
  assert.deepEqual([p.type, p.number, p.lo, p.hi], ['cc14', 1, 200, 1000]);
});

test('invalid CSV does not become a seemingly valid offline entry', async () => {
  const { fetch } = server(tree(blob('A/Synth.csv')), text(ccHeader, 'A,Synth,Main,"Unclosed,continued'), new Error('offline'));
  const cache = new Map();
  await assert.rejects(loadDevice('A', 'Synth', { fetch, cache }), /Synth\.csv.*unclosed CSV quote/i);
  await assert.rejects(loadDevice('A', 'Synth', { fetch, cache }), /offline/i);
});

test('an oversized device reported in the index is rejected without downloading its CSV', async () => {
  const { fetch, calls } = server(tree(blob('A/Large.csv', 524289)));
  await assert.rejects(loadDevice('A', 'Large', { fetch, cache: null }), /Large\.csv.*too large/i);
  assert.deepEqual(calls, [TREE]);
});

test('a device with no valid MIDI mappings is not treated as a successful load', async () => {
  const { fetch } = server(tree(blob('A/Synth.csv')), text(ccHeader, 'A,Synth,Main,Invalid,,74x,,,,,,,,,,,,'));
  await assert.rejects(loadDevice('A', 'Synth', { fetch, cache: null }), /no usable MIDI mappings/i);
});
