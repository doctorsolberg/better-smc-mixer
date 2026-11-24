// ?selftest: T-4's checklist run by the page itself (docs/integration.md, T-4, "The page against
// devhost"), so any browser checks it with no driver: open
// http://localhost:8765/app/editor/?devhost&selftest (serve.py) and read the report in the
// corner, in the console, in the tab's title ("selftest: passed" or "selftest: N failed") and in
// window.smcSelftest. It drives the page through its own functions (the ones its buttons call) and
// its dialogs, against the mixer it is connected to: with ?devhost the firmware built for the host.
// On the mixer itself it would change the mixer's configuration and save it: the page asks first.

import { CMD } from './link.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(cond, ms = 4000) {
  const end = performance.now() + ms;
  while (performance.now() < end) { if (cond()) return true; await sleep(25); }
  return !!cond();
}
const noNames = (v) => (Array.isArray(v) ? v.map(noNames) : v && typeof v === 'object'
  ? Object.fromEntries(Object.entries(v).filter(([k]) => k !== 'name').map(([k, x]) => [k, noNames(x)])) : v);
const content = (file) => { const { id, revision, ...rest } = noNames(file); return JSON.stringify(rest); };

export async function selftest(T) {
  const lines = [];
  let failed = 0;
  const box = document.createElement('pre');
  box.id = 'selftest';
  Object.assign(box.style, {
    position: 'fixed', right: '8px', bottom: '8px', zIndex: 9999, maxWidth: 'min(560px, calc(100vw - 16px))',
    maxHeight: '50vh', overflow: 'auto', margin: 0, padding: '8px 10px', font: '12px/1.4 ui-monospace, monospace',
    background: 'var(--bg-2, #fff)', color: 'var(--fg, #000)', border: '1px solid var(--line, #888)', borderRadius: '6px',
  });
  box.setAttribute('role', 'log');
  box.setAttribute('aria-label', 'Self-test report');
  document.body.append(box);
  const say = (s) => { lines.push(s); box.textContent = lines.join('\n'); box.scrollTop = box.scrollHeight; console.log(`selftest: ${s}`); };
  const check = (name, cond, detail = '') => {
    if (cond) say(`ok   ${name}`);
    else { failed++; say(`FAIL ${name}${detail ? `: ${detail}` : ''}`); }
  };
  document.title = 'selftest: running';
  window.smcSelftest = { done: false, failed: 0, lines };

  if (!T.devhost && !confirm('The self-test changes the connected mixer\'s configuration and saves it. Run it?')) {
    say('not run: not confirmed');
    return;
  }
  const dev = () => T.dev;
  const errors = [];
  const settled = () => until(() => !dev().pushing && !dev().pending, 8000);
  const dialogButton = (text) => [...document.querySelectorAll('#dlg button')].find((b) => b.textContent.includes(text));

  try {
    // The editor starts from the factory configuration, as a fresh mixer does: no "differ" question.
    T.st.follow = true;
    T.openFile(await T.fetchFirst(T.DATA('factory.json')), 'factory', true);

    T.connectMixer();
    check('connect: the mixer found and read', await until(() => dev().connected, 8000), 'not connected');
    if (!dev().connected) throw new Error('no mixer');
    dev().addEventListener('error', (e) => errors.push(e.detail.text));
    const midi = [];
    dev().addEventListener('midi', (e) => midi.push(e.detail));
    check('connect: over the configuration port (USB)', dev().transport === 'USB', dev().transport);
    check('connect: the mixer plays the editor\'s configuration', dev().running === T.docText());
    check('connect: no question asked (the two agree)', !document.querySelector('#dlg')?.open);
    check('connect: nothing to save', dev().storage === 'clean', dev().storage);
    check('connect: the mixer names what it sends (P-5)', dev().withSent);

    // An edit in the editor reaches the mixer as a live edit (P-1).
    T.edit('selftest: fader 1 to CC 1', () => { T.cfg.control(0, 0, 's0.fader').number = 1; });
    check('edit: unconfirmed transfer is Sending, not Saved', dev().sending && document.querySelector('#device-pill').textContent.includes('Sending'));
    await settled();
    const last = () => dev().logLines.at(-1)?.text ?? '';
    check('edit: sent as a live edit', /selftest: fader 1 to CC 1: applied \(1 live edit/.test(last()), last());
    check('edit: unsaved on the mixer', dev().storage === 'unsaved', dev().storage);
    check('edit: the pill says so', T.unsaved());

    // Save (Q-26).
    await T.saveOnMixer();
    check('save: the mixer starts with it now', await until(() => dev().storage === 'clean'), dev().storage);

    // Another edit, then Go back through its dialog.
    const was = T.cfg.control(0, 0, 's1.encoder').number;
    T.edit('selftest: encoder 2 to CC 2', () => { T.cfg.control(0, 0, 's1.encoder').number = 2; });
    await settled();
    check('second edit: unsaved', dev().storage === 'unsaved', dev().storage);
    T.revertDialog();
    const go = dialogButton('Go back');
    check('go back: the dialog asks', !!go);
    go?.click();
    check('go back: the editor shows the saved configuration', await until(() => T.cfg.control(0, 0, 's1.encoder').number === was),
      String(T.cfg.control(0, 0, 's1.encoder').number));
    check('go back: clean', await until(() => dev().storage === 'clean'), dev().storage);
    check('go back: fader 1 keeps the saved CC 1', T.cfg.control(0, 0, 's0.fader').number === 1);

    // The tabs switch the mixer (P-3); its EVENT confirms it.
    T.selectView(2, 3, true);
    check('tabs: the mixer goes to preset 3 page 4', await until(() => dev().panelState?.preset === 2 && dev().panelState?.page === 3),
      JSON.stringify(dev().panelState));
    await sleep(600);
    check('tabs: the editor stays there', T.st.preset === 2 && T.st.page === 3, `${T.st.preset}/${T.st.page}`);

    // A touch on the mixer selects the control (P-2); the monitor names what it sent (P-5).
    const inject = (p) => dev().client.call(CMD.inject, p);
    check('touch: injection on', (await inject([1])).status === 'ok');
    await inject([2, 2, 0, 1, 0, 0]); // encoder 1, one click to the right
    check('touch: the encoder is selected', await until(() => T.st.sel.has('s0.encoder')), [...T.st.sel].join(','));
    check('touch: and shown selected on the panel', !!document.querySelector('#panel .ctl.sel[data-id="s0.encoder"]'));
    check('monitor: the encoder\'s message, named', await until(() => midi.some((m) => m.id === 's0.encoder' && m.preset === 2 && m.page === 3)),
      JSON.stringify(midi.slice(-3)));
    await inject([2, 1, 32 + 3, 1, 0, 0]); // the row's Rew pressed
    await inject([2, 1, 32 + 3, 0, 0, 0]); // and released
    check('monitor: the row\'s Rew, named, press and release', await until(() => midi.filter((m) => m.id === 'row.rew').length >= 2),
      JSON.stringify(midi.slice(-3)));
    check('monitor: the page\'s list has them', T.st.monitor.some((r) => r.who.startsWith(T.ctlName('row.rew'))),
      JSON.stringify(T.st.monitor.slice(-2).map((r) => r.who)));
    check('monitor: no message left unnamed', midi.every((m) => m.id), JSON.stringify(midi.filter((m) => !m.id)));
    await inject([0]);

    // A whole file opened: sent whole, the core restarts on it.
    T.selectView(0, 0, true);
    T.openFile(await T.fetchFirst(T.DATA('test.json')), 'test.json', false);
    await settled();
    check('open a file: sent whole', /configuration opened: applied \(\d+ bytes, the core restarted/.test(last()), last());
    const back = await dev().pullFile();
    check('open a file: the mixer plays it', content(back) === content(T.cfg.canonical()));
    dev().running = T.docText(); // the pull keeps no names; the editor's copy has them

    // The saving setting: by itself, after the quiet pause (Q-22).
    T.edit('selftest: the mixer saves by itself', () => { T.cfg.file.global.save = 'auto'; });
    await settled();
    check('autosave: waiting, then saved by itself', await until(() => dev().storage === 'clean', 8000), dev().storage);

    check('no error from the mixer', errors.length === 0, errors.join(' | '));
  } catch (e) {
    failed++;
    say(`FAIL ${e.stack ?? e}`);
  }
  say(failed ? `${failed} failed` : 'passed');
  document.title = failed ? `selftest: ${failed} failed` : 'selftest: passed';
  Object.assign(window.smcSelftest, { done: true, failed });
}
