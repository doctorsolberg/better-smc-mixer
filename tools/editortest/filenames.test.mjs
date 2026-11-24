// Host-only: default configuration export names (owner request: BSM-Config).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { CONFIG_FILE_BASE, configFileName } from '../../app/editor/js/filenames.js';

test('whole-configuration export defaults to BSM-Config', () => {
  assert.equal(CONFIG_FILE_BASE, 'BSM-Config');
  assert.equal(configFileName('', 'Everything'), 'BSM-Config');
  assert.equal(configFileName(undefined, undefined), 'BSM-Config');
});

test('preset and page exports keep a meaningful scope suffix', () => {
  assert.equal(configFileName('', 'Preset 2'), 'BSM-Config - Preset 2');
  assert.equal(configFileName('', 'Preset 1 · Page 3'), 'BSM-Config - Preset 1 Page 3');
});

test('an opened file keeps its own name; unsafe characters are dropped', () => {
  assert.equal(configFileName('My live set', 'Everything'), 'My live set');
  assert.equal(configFileName('../evil/<name>', 'Everything'), 'evilname');
  assert.equal(configFileName('***', 'Everything'), 'BSM-Config');
});

test('the editor no longer offers SMC-Mixer as a configuration file name', () => {
  const app = readFileSync('app/editor/js/app.js', 'utf8');
  assert.doesNotMatch(app, /'SMC-Mixer'\}|'SMC-Mixer configuration'|'smc-config'/);
});
