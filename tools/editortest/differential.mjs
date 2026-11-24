// The editor side of differential.py: JSON in, JSON out, no packages or device APIs.
import { readFileSync } from 'node:fs';
import { Codec, CodecError } from '../../app/editor/js/cfgbin.js';
import { Schema } from '../../app/editor/js/schema.js';

const schema = JSON.parse(readFileSync('config/schema.json', 'utf8'));
const codec = new Codec(schema);
const checks = new Schema(schema);
const corpus = JSON.parse(readFileSync(0, 'utf8'));
const bytes = (hex) => Uint8Array.from(Buffer.from(hex, 'hex'));
const hex = (data) => Buffer.from(data).toString('hex');
const faults = [];

for (const [i, item] of corpus.files.entries()) {
  try {
    const errors = checks.checkFile(item.file);
    if (errors.length) faults.push({ kind: 'file-check', index: i, error: errors.slice(0, 3) });
    const encoded = hex(codec.fileToBinary(item.file));
    if (encoded !== item.hex) faults.push({ kind: 'file-encode', index: i, actual: encoded });
  } catch (e) {
    faults.push({ kind: 'file-exception', index: i, error: `${e.name}: ${e.message}` });
  }
}

const decoded = corpus.binaries.map((blob, i) => {
  try {
    return `ok ${hex(codec.encode(codec.decode(bytes(blob))))}`;
  } catch (e) {
    if (!(e instanceof CodecError)) faults.push({ kind: 'decoder-exception', index: i, error: `${e.name}: ${e.message}` });
    return 'error';
  }
});

for (const [i, item] of corpus.edits.entries()) {
  try {
    const edits = codec.edits(codec.decode(bytes(item.old)), codec.decode(bytes(item.new)))
      .map(([preset, records]) => [preset, hex(records)]);
    if (JSON.stringify(edits) !== JSON.stringify(item.expected)) faults.push({ kind: 'edits', index: i, actual: edits });
  } catch (e) {
    faults.push({ kind: 'edits-exception', index: i, error: `${e.name}: ${e.message}` });
  }
}

process.stdout.write(JSON.stringify({ decoded, faults }) + '\n');
