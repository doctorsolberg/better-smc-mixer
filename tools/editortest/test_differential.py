"""Tests of the differential harness, including planted disagreements. No physical device.

python3 -I tools/editortest/test_differential.py
The worker integration test needs the cached host image's Node, or SMC_NODE inside that image.
"""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import differential as diff  # noqa: E402


class ComparisonTest(unittest.TestCase):
    def run_case(self, c=None, js=None, faults=(), worker_exit=0):
        blob = diff.codec.encode(diff.codec.empty_config())
        expected = 'ok ' + blob.hex()
        corpus = {'files': [], 'binaries': [blob.hex()], 'edits': []}
        worker = subprocess.CompletedProcess([], worker_exit,
                    json.dumps({'decoded': [expected if js is None else js], 'faults': list(faults)}),
                    'worker failure' if worker_exit else '')
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            with patch.object(diff, 'make_corpus', return_value=(corpus, [blob], [])), \
                 patch.object(diff.generators, 'c_results', return_value=([expected if c is None else c], [])), \
                 patch.object(diff, 'node_command', return_value=['worker']), \
                 patch.object(diff.subprocess, 'run', return_value=worker), \
                 contextlib.redirect_stdout(io.StringIO()):
                result = diff.compare(7, 1, 0, 0, out)
            summary = json.loads((out / 'summary.json').read_text())
            failures = json.loads((out / 'failures.json').read_text()) if (out / 'failures.json').exists() else []
            self.assertEqual((out / 'corpus.json').exists(), bool(failures))
            return result, summary, failures

    def test_agreement(self):
        result, summary, failures = self.run_case()
        self.assertEqual((result, summary['failures'], failures), (0, 0, []))
        self.assertEqual(summary['accepted_binaries'], 1)

    def test_planted_c_disagreement_is_not_missed(self):
        result, _, failures = self.run_case(c='error')
        self.assertEqual(result, 1)
        self.assertEqual(failures[0]['kind'], 'c-python')

    def test_planted_editor_disagreement_is_not_missed(self):
        result, _, failures = self.run_case(js='error')
        self.assertEqual(result, 1)
        self.assertEqual(failures[0]['kind'], 'editor-python')

    def test_worker_fault_is_not_missed(self):
        result, _, failures = self.run_case(faults=[{'kind': 'file-encode', 'index': 0}])
        self.assertEqual(result, 1)
        self.assertEqual(failures[0]['kind'], 'file-encode')

    def test_worker_crash_is_not_success(self):
        with self.assertRaisesRegex(RuntimeError, 'editor worker failed'):
            self.run_case(worker_exit=1)

    def test_seeded_generation_is_reproducible(self):
        first = diff.make_corpus(7654, 1, 5, 0)
        second = diff.make_corpus(7654, 1, 5, 0)
        self.assertEqual(first, second)

    def test_case_bounds(self):
        self.assertEqual(diff.bounded('0', 8), 0)
        self.assertEqual(diff.bounded('8', 8), 8)
        for value in ('-1', '9'):
            with self.assertRaises(argparse.ArgumentTypeError):
                diff.bounded(value, 8)

    def test_cached_node_command_does_not_need_a_posix_shell(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(diff.subprocess, 'run') as run:
            command = diff.node_command()
        self.assertEqual(run.call_args.args[0][:3], ['docker', 'image', 'inspect'])
        self.assertTrue(run.call_args.args[0][3].startswith('better-smc-mixer-host:'))
        self.assertEqual(command[:4], ['docker', 'run', '--rm', '-i'])
        self.assertFalse(any(str(arg).endswith('.sh') for arg in command))

    def test_native_executable_name_is_resolved_before_comparison(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(diff.generators, 'CFGTOOL', 'cfgtool'), \
             patch.object(diff.shutil, 'which', return_value='cfgtool.exe') as which, \
             patch.object(diff, 'compare', return_value=0) as compare:
            self.assertEqual(diff.main(['--out', tmp]), 0)
            which.assert_called_once_with('cfgtool')
            self.assertEqual(diff.generators.CFGTOOL, 'cfgtool.exe')
            compare.assert_called_once()

    def test_inside_host_image_uses_its_node(self):
        with patch.dict(os.environ, {'SMC_NODE': 'node'}):
            self.assertEqual(diff.node_command(), ['node', 'tools/editortest/differential.mjs'])

    def test_existing_diagnostics_are_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            keep = Path(tmp) / 'keep.txt'
            keep.write_text('keep')
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as cm:
                diff.main(['--out', tmp])
            self.assertEqual(cm.exception.code, 2)
            self.assertEqual(keep.read_text(), 'keep')


class WorkerTest(unittest.TestCase):
    def test_actual_worker_catches_planted_encoding_and_edit_faults(self):
        try:
            command = diff.node_command()
        except (OSError, subprocess.CalledProcessError) as error:
            if os.environ.get('SMC_EDITOR_TESTS') == 'required':
                raise
            self.skipTest('cached Node environment unavailable: ' + str(error))
        model = diff.codec.empty_config()
        blob = diff.codec.encode(model).hex()
        corpus = {'files': [{'file': diff.codec.to_json(model), 'hex': '00'}],
                  'binaries': [blob, '00'],
                  'edits': [{'old': blob, 'new': blob, 'expected': [[0, '00']]}]}
        result = subprocess.run(command, cwd=diff.ROOT, input=json.dumps(corpus), capture_output=True,
                                text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data['decoded'], ['ok ' + blob, 'error'])
        self.assertEqual({f['kind'] for f in data['faults']}, {'file-encode', 'edits'})


if __name__ == '__main__':
    unittest.main()
