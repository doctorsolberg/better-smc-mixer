#!/usr/bin/env python3
"""Bounded C/Python/editor codec agreement on seeded random data; no device operations.

  python3 -I tools/editortest/differential.py [--seed N] [--models 32] [--mutations 1000]
                                            [--pairs 8] [--out EMPTY_DIRECTORY]

Reuses smcconf's random-model and malformed-binary generators. Compare file encoding, binary
acceptance/canonical bytes and live-edit records in all three implementations. The C codec runs
through build/host/cfgtool; the editor runs in the existing host-test Docker image's Node.
SMC_NODE names Node when already inside that image. Nothing is downloaded or installed.
Failures retain the deterministic corpus and the first discrepancies under build/differential/.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/smcconf'))
import codec  # noqa: E402
import test_smcconf as generators  # noqa: E402 - one set of generators for both test layers


def node_command():
    if os.environ.get('SMC_NODE'):
        return [os.environ['SMC_NODE'], 'tools/editortest/differential.mjs']
    # Match host_image.sh without executing a shell script from native Windows Python.
    tag = 'better-smc-mixer-host:' + hashlib.sha256((ROOT / 'docker/Dockerfile.host').read_bytes()).hexdigest()[:12]
    subprocess.run(['docker', 'image', 'inspect', tag], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    user = '%d:%d' % (os.getuid(), os.getgid()) if hasattr(os, 'getuid') else '1000:1000'
    return ['docker', 'run', '--rm', '-i', '--network', 'none', '--user', user,
            '-v', str(ROOT) + ':/src:ro', '-w', '/src', tag,
            'node', 'tools/editortest/differential.mjs']


def make_corpus(seed, models, mutations, pairs):
    rnd = random.Random(seed)
    values = [generators.random_model(rnd) for _ in range(models)]
    valid = [codec.encode(model) for model in values]
    binaries = valid + [generators.damage(rnd.choice(valid), rnd) for _ in range(mutations)]
    corpus = {'files': [{'file': codec.to_json(model), 'hex': blob.hex()}
                        for model, blob in zip(values, valid)],
              'binaries': [blob.hex() for blob in binaries], 'edits': []}
    patch_failures = []
    for i in range(pairs):
        old, new = rnd.choice(values), rnd.choice(values)
        changes = codec.edits(old, new)
        expected = codec.encode(dict(new, id=old['id'], revision=old['revision']))
        actual = generators.c_patch(codec.encode(old), changes)
        if actual != 'ok ' + expected.hex():
            patch_failures.append({'kind': 'c-patch', 'index': i, 'actual': actual,
                                   'expected': expected.hex()})
        corpus['edits'].append({'old': codec.encode(old).hex(), 'new': codec.encode(new).hex(),
                                'expected': [[preset, records.hex()] for preset, records in changes]})
    return corpus, binaries, patch_failures


def compare(seed, models, mutations, pairs, out):
    started = time.monotonic()
    corpus, binaries, failures = make_corpus(seed, models, mutations, pairs)
    c_results, _ = generators.c_results(binaries)
    if len(c_results) != len(binaries):
        raise RuntimeError('C batch result count differs from corpus length')
    expected = [generators.py_result(blob) for blob in binaries]
    for i, (c, py) in enumerate(zip(c_results, expected)):
        if c != py:
            failures.append({'kind': 'c-python', 'index': i, 'c': c, 'python': py})
    proc = subprocess.run(node_command(), cwd=ROOT, input=json.dumps(corpus), capture_output=True,
                          text=True, timeout=300)
    if proc.returncode:
        raise RuntimeError('editor worker failed: ' + proc.stderr[-2000:])
    editor = json.loads(proc.stdout)
    if len(editor['decoded']) != len(binaries):
        raise RuntimeError('Editor result count differs from corpus length')
    failures.extend(editor['faults'])
    for i, (js, py) in enumerate(zip(editor['decoded'], expected)):
        if js != py:
            failures.append({'kind': 'editor-python', 'index': i, 'editor': js, 'python': py})
    result = {'seed': seed, 'models': models, 'mutations': mutations, 'pairs': pairs,
              'accepted_binaries': sum(x.startswith('ok ') for x in expected),
              'refused_binaries': expected.count('error'), 'failures': len(failures),
              'seconds': round(time.monotonic() - started, 3),
              'schema_sha256': hashlib.sha256((ROOT / 'config/schema.json').read_bytes()).hexdigest()}
    (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf8')
    if failures:
        (out / 'corpus.json').write_text(json.dumps(corpus) + '\n', encoding='utf8')
        (out / 'failures.json').write_text(json.dumps(failures[:20], indent=2) + '\n', encoding='utf8')
    print('differential: ' + json.dumps(result), flush=True)
    return int(bool(failures))


def bounded(value, maximum):
    n = int(value)
    if not 0 <= n <= maximum:
        raise argparse.ArgumentTypeError('must be between 0 and %d' % maximum)
    return n


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--seed', type=int, default=20261115)
    parser.add_argument('--models', type=lambda x: bounded(x, 256), default=32)
    parser.add_argument('--mutations', type=lambda x: bounded(x, 20000), default=1000)
    parser.add_argument('--pairs', type=lambda x: bounded(x, 64), default=8)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args(argv)
    if args.models < 1:
        parser.error('--models must be at least 1')
    out = args.out or ROOT / 'build/differential' / (
        '%s-seed-%d' % (datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'), args.seed))
    if out.exists() and any(out.iterdir()):
        parser.error('--out must be an empty directory')
    out.mkdir(parents=True, exist_ok=True)
    executable = shutil.which(generators.CFGTOOL)  # resolves .exe through PATHEXT on Windows
    if executable is None:
        parser.error('build/host/cfgtool is missing: make -C firmware sim')
    generators.CFGTOOL = executable
    return compare(args.seed, args.models, args.mutations, args.pairs, out)


if __name__ == '__main__':
    raise SystemExit(main())
