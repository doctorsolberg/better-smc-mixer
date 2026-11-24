#!/usr/bin/env python3
"""Portable package-documentation and output-safety tests, plus the actual images where built.
The image tests check files/manifest/SHA256SUMS, licenses, offline links and a planted private
string. Synthetic text packages and output guards run without an SDK or device images.
"""
import contextlib
import hashlib
import io
import itertools
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release  # noqa: E402

BUILT = all(os.path.exists(os.path.join(release.ROOT, d, b)) for _, d, b, _ in release.IMAGES.values())


class BuildRouting(unittest.TestCase):
    def test_each_image_writes_its_read_path_despite_inherited_flags(self):
        # A local runner models build_app.sh's output naming, not the release job table.
        # Stale nominal files make an incorrect route observable through images().
        runner = '''#!%s -I
import json, os, pathlib, sys
root = pathlib.Path.cwd()
if pathlib.Path(__file__).name == 'build_boot.sh':
    route, binary = 'boot', 'boot.bin'
else:
    route = os.environ.get('VARIANT') or 'app'
    route += '-bootmark' if os.environ.get('BOOTMARK') else ''
    route += '-ble' if os.environ.get('BLE') else ''
    binary = 'app.bin'
out = root / 'build' / route
out.mkdir(parents=True, exist_ok=True)
(out / binary).write_text('fresh:' + route + ':' + os.environ['SMC_RELEASE_KEEP'])
(out / pathlib.Path(binary).with_suffix('.smclog.json')).write_text(json.dumps({'build_id': route}))
with (root / 'calls.jsonl').open('a') as f:
    f.write(json.dumps({'route': route, 'args': sys.argv[1:],
                       'flags': {k: os.environ[k] for k in ('VARIANT', 'BLE', 'BOOTMARK') if k in os.environ}}) + '\\n')
''' % sys.executable
        expected = {'boot': ('boot', 'boot.bin', {}),
                    'controller': ('app', 'app.bin', {}),
                    'controller-ble': ('app-ble', 'app.bin', {'BLE': '1'}),
                    'hwtest': ('hwtest', 'app.bin', {'VARIANT': 'hwtest'})}
        for inherited in itertools.product((False, True), repeat=3):
            ambient = {k: v for k, v, enabled in zip(
                ('VARIANT', 'BLE', 'BOOTMARK'), ('hwtest', '1', '1'), inherited) if enabled}
            for force in (False, True):
                with self.subTest(ambient=ambient, force=force), tempfile.TemporaryDirectory() as root:
                    env = {k: v for k, v in os.environ.items() if k not in ('VARIANT', 'BLE', 'BOOTMARK')}
                    env.update(ambient, SMC_RELEASE_KEEP='preserved')
                    for script in ('build_boot.sh', 'build_app.sh'):
                        path = os.path.join(root, 'tools', 'env', script)
                        os.makedirs(os.path.dirname(path), exist_ok=True)
                        with open(path, 'w') as f:
                            f.write(runner)
                        os.chmod(path, 0o755)
                    for route, binary, _ in expected.values():
                        out = os.path.join(root, 'build', route)
                        os.makedirs(out)
                        with open(os.path.join(out, binary), 'w') as f:
                            f.write('stale')
                        with open(os.path.join(out, binary.replace('.bin', '.smclog.json')), 'w') as f:
                            json.dump({'build_id': 'stale'}, f)
                    with patch.object(release, 'ROOT', root), patch.dict(os.environ, env, clear=True), contextlib.redirect_stdout(io.StringIO()):
                        self.assertTrue(release.build(force=force))
                        images = release.images()
                    with open(os.path.join(root, 'calls.jsonl')) as f:
                        calls = [json.loads(line) for line in f]
                    self.assertEqual(len(calls), 4)
                    self.assertEqual([call['route'] for call in calls],
                                     [route for route, _, _ in expected.values()])
                    for (name, (route, binary, flags)), call in zip(expected.items(), calls):
                        self.assertEqual(call, {'route': route, 'args': ['-B'] if force else [], 'flags': flags}, name)
                        self.assertEqual(images[name]['bin'], 'build/' + route + '/' + binary)
                        self.assertEqual(images[name]['build_id'], route, name)
                        with open(os.path.join(root, images[name]['bin'])) as f:
                            self.assertEqual(f.read(), 'fresh:' + route + ':preserved', name)


class LauncherHash(unittest.TestCase):
    def test_single_hash_utility_produces_same_dockerfile_tag(self):
        with open(os.path.join(release.ROOT, 'docker', 'Dockerfile.build'), 'rb') as f:
            tag = 'better-smc-mixer-build:' + hashlib.sha256(f.read()).hexdigest()[:12]
        for utility in ('sha256sum', 'shasum'):
            with self.subTest(utility=utility), tempfile.TemporaryDirectory() as root:
                path = os.path.join(root, 'bin')
                os.makedirs(path)
                for tool in ('dirname', 'cut', 'find', 'wc', 'id', utility):
                    executable = shutil.which(tool)
                    if executable is None:
                        self.skipTest(tool + ' is unavailable on this host')
                    os.symlink(executable, os.path.join(path, tool))
                for dep in ('jieli-toolchain', 'jieli-post-build'):
                    os.makedirs(os.path.join(root, 'deps', dep, 'version'))
                record = os.path.join(root, 'docker.jsonl')
                with open(os.path.join(path, 'docker'), 'w') as f:
                    f.write('#!%s -I\nimport json, os, sys\n'
                            'with open(os.environ["DOCKER_RECORD"], "a") as f:\n'
                            '    f.write(json.dumps(sys.argv[1:]) + "\\n")\n' % sys.executable)
                os.chmod(os.path.join(path, 'docker'), 0o755)
                env = dict(os.environ, PATH=path, DEPS=os.path.join(root, 'deps'), DOCKER_RECORD=record)
                env.pop('EXTRA_DOCKER_ARGS', None)
                run = subprocess.run(['/bin/bash', os.path.join(release.ROOT, 'tools', 'env', 'run.sh'), 'true'],
                                     env=env, capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stderr)
                with open(record) as f:
                    calls = [json.loads(line) for line in f]
                self.assertEqual(len(calls), 2)
                self.assertEqual(calls[0], ['image', 'inspect', tag])
                self.assertEqual(calls[1][0], 'run')
                self.assertEqual(calls[1][-2:], [tag, 'true'])


class Documentation(unittest.TestCase):
    """Synthetic trees: no images, SDK, private data or device needed, including on Linux."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, 'source')
        self.out = os.path.join(self.tmp.name, 'package')
        os.makedirs(self.root); os.makedirs(self.out)
        self.write(self.root, '.gitignore', '/materials/\n/build/\n/deps/\n/logs/\n')
        self.guide = '# Start\n[Technical](../technical.md#detail)\n[Editor](../../app/editor/)\n[Schema](../../config/schema.json)\n`[example](missing.md)`\n```\n[example](missing.md)\n```\n'
        self.write(self.root, 'docs/user/start.md', self.guide)
        self.write(self.root, 'docs/technical.md', '# Technical\n\n## Detail\n[Guide](user/start.md)\n[Source](../src/x.py:4)\n[Data](../config/extra.json)\n[Web](https://example.com)\n')
        self.write(self.root, 'src/x.py', 'pass\n')
        self.write(self.root, 'config/schema.json', '{}\n')
        self.write(self.root, 'config/extra.json', '{"public": true}\n')
        self.write(self.root, 'app/editor/index.html', '<!doctype html>\n')
        self.write(self.out, 'editor/index.html', '<!doctype html>\n')
        self.write(self.out, 'editor/data/schema.json', '{}\n')
        self.aliases = {'app/editor': 'editor', 'config/schema.json': 'editor/data/schema.json'}

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def write(root, rel, text):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf8') as f:
            f.write(text)

    def read(self, rel):
        with open(os.path.join(self.out, rel), encoding='utf8') as f:
            return f.read()

    def test_public_guides_references_assets_anchors_and_examples(self):
        files = release.docbundle.bundle(self.root, self.out, self.aliases)
        self.assertEqual(set(files), {'guide/start.md', 'reference/docs/technical.md', 'reference/config/extra.json'})
        guide = self.read('guide/start.md')
        self.assertIn('../reference/docs/technical.md#detail', guide)
        self.assertIn('../editor/', guide)
        self.assertIn('../editor/data/schema.json', guide)
        self.assertIn('`[example](missing.md)`', guide)
        self.assertIn('```\n[example](missing.md)\n```', guide)
        ref = self.read('reference/docs/technical.md')
        self.assertIn('Developer reference', ref)
        self.assertIn('source reference, not included', ref)
        self.assertIn('../src/x.py:4', ref)
        self.assertIn('https://example.com', ref)
        self.assertEqual(release.check_docs.check(self.out), [])
        self.assertFalse(os.path.exists(os.path.join(self.out, 'reference/src/x.py')))
        with open(os.path.join(self.root, 'docs/user/start.md'), encoding='utf8') as f:
            self.assertEqual(f.read(), self.guide)

    def test_complete_text_package_is_deterministic_without_device_images(self):
        for path, text in (('VERSION', '0.1.0\n'), ('NOTICE', 'Fixture notice\n'),
                           ('LICENSE', 'Fixture license\n'), ('config/factory.json', '{}\n'),
                           ('app/editor/app.css', 'body {}\n'), ('app/editor/js/app.js', 'export {};\n')):
            self.write(self.root, path, text)
        one, two = (os.path.join(self.tmp.name, x) for x in ('one', 'two'))
        versions = {'schema': 1, 'binary': 1, 'configuration_protocol': 4, 'link': 1, 'log_format': 1}
        with patch.object(release, 'ROOT', self.root), patch.object(release, 'versions', return_value=versions):
            first, second = release.pack(one, {}), release.pack(two, {})
        self.assertEqual(first, second)
        for rel in first:
            with open(os.path.join(one, rel), 'rb') as a, open(os.path.join(two, rel), 'rb') as b:
                self.assertEqual(a.read(), b.read(), rel)
        self.assertEqual(release.check_docs.check(one), [])

    def test_ignored_markdown_is_not_pulled_into_package(self):
        self.write(self.root, 'materials/private.md', 'PRIVATE-ONLY-NOT-FOR-PACKAGE\n')
        self.write(self.root, 'docs/user/start.md', self.guide + '[local](../../materials/private.md)\n')
        with self.assertRaisesRegex(ValueError, 'not publishable'):
            release.docbundle.bundle(self.root, self.out, self.aliases)
        self.assertFalse(os.path.exists(os.path.join(self.out, 'reference/materials/private.md')))

    def test_public_symlink_cannot_escape_to_private_content(self):
        external = os.path.join(self.tmp.name, 'outside.md')
        self.write(self.tmp.name, 'outside.md', 'PRIVATE-OUTSIDE\n')
        link = os.path.join(self.root, 'docs/leak.md')
        try:
            os.symlink(external, link)
        except OSError as error:
            self.skipTest('symlinks unavailable: ' + str(error))
        with self.assertRaisesRegex(ValueError, 'not in the public source tree'):
            release.docbundle.bundle(self.root, self.out, self.aliases)
        self.assertFalse(os.path.exists(os.path.join(self.out, 'reference/docs/leak.md')))

    def test_symlink_cannot_launder_ignored_or_vendor_markdown(self):
        for target in ('materials/inside.md', 'third_party/vendor/inside.md'):
            with self.subTest(target=target):
                self.write(self.root, target, 'NOT-ADDED-THROUGH-AN-ALIAS\n')
                link = os.path.join(self.root, 'docs/alias.md')
                try:
                    os.symlink(os.path.join(self.root, target), link)
                except OSError as error:
                    self.skipTest('symlinks unavailable: ' + str(error))
                try:
                    with self.assertRaisesRegex(ValueError, 'not in the public source tree'):
                        release.docbundle.bundle(self.root, self.out, self.aliases)
                finally:
                    os.unlink(link)

    def test_alias_cannot_leave_the_output(self):
        for target in ('../escape', '/escape', 'C:/escape'):
            with self.subTest(target=target), self.assertRaisesRegex(ValueError, 'leaves the package'):
                release.docbundle.bundle(self.root, self.out, {'app/editor': target})


class SafeOutput(unittest.TestCase):
    @staticmethod
    def previous(out):
        os.makedirs(out)
        with open(os.path.join(out, 'manifest.json'), 'w') as f:
            json.dump({'product': 'better-smc-mixer', 'dry': True}, f)
        with open(os.path.join(out, 'keep.txt'), 'w') as f:
            f.write('previous package')

    def test_checkout_parents_and_unrelated_directories_are_refused(self):
        for path in (release.ROOT, os.path.dirname(release.ROOT)):
            with self.assertRaises(ValueError):
                release.validate_output(path)
        with tempfile.TemporaryDirectory() as out:
            with open(os.path.join(out, 'keep.txt'), 'w') as f:
                f.write('user data')
            with self.assertRaisesRegex(ValueError, 'non-release directory'):
                release.pack(out, {})
            with open(os.path.join(out, 'keep.txt')) as f:
                self.assertEqual(f.read(), 'user data')
            with patch.object(release, 'build') as build, contextlib.redirect_stderr(io.StringIO()) as errors:
                self.assertEqual(release.main(['--out', out]), 1)
                build.assert_not_called()
            self.assertIn('non-release directory', errors.getvalue())

    def test_failed_assembly_keeps_previous_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, 'release'); self.previous(out)
            def fail(stage, images, date=None):
                os.makedirs(stage)
                with open(os.path.join(stage, 'partial.txt'), 'w') as f:
                    f.write('incomplete')
                raise ValueError('model copy failure')
            with patch.object(release, '_pack_contents', side_effect=fail), self.assertRaisesRegex(ValueError, 'model copy failure'):
                release.pack(out, {})
            with open(os.path.join(out, 'keep.txt')) as f:
                self.assertEqual(f.read(), 'previous package')
            self.assertEqual(os.listdir(tmp), ['release'])

    def test_failed_commit_restores_previous_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, 'release'); self.previous(out)
            def assemble(stage, images, date=None):
                os.makedirs(stage)
                with open(os.path.join(stage, 'new.txt'), 'w') as f:
                    f.write('new package')
                return ['new.txt']
            replace = os.replace
            def commit(src, dst):
                if os.path.basename(src) == 'package':
                    raise OSError('model commit failure')
                return replace(src, dst)
            with patch.object(release, '_pack_contents', side_effect=assemble), patch.object(release.os, 'replace', side_effect=commit), self.assertRaisesRegex(OSError, 'model commit failure'):
                release.pack(out, {})
            with open(os.path.join(out, 'keep.txt')) as f:
                self.assertEqual(f.read(), 'previous package')
            self.assertFalse(os.path.exists(os.path.join(out, 'new.txt')))
            self.assertEqual(os.listdir(tmp), ['release'])

    def test_failed_rollback_keeps_previous_package_outside_temporary_workspace(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, 'release'); self.previous(out)
            def assemble(stage, images, date=None):
                os.makedirs(stage)
                return []
            replace = os.replace
            def commit(src, dst):
                if os.path.basename(src) == 'package' or os.path.basename(src).startswith('.smc-release-previous-'):
                    raise OSError('model replacement/rollback failure')
                return replace(src, dst)
            with patch.object(release, '_pack_contents', side_effect=assemble), patch.object(release.os, 'replace', side_effect=commit), self.assertRaisesRegex(ValueError, 'previous package kept at'):
                release.pack(out, {})
            backups = [name for name in os.listdir(tmp) if name.startswith('.smc-release-previous-')]
            self.assertEqual(len(backups), 1)
            with open(os.path.join(tmp, backups[0], 'keep.txt')) as f:
                self.assertEqual(f.read(), 'previous package')

    def test_successful_replacement_removes_only_previous_generated_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, 'release'); self.previous(out)
            def assemble(stage, images, date=None):
                os.makedirs(stage)
                with open(os.path.join(stage, 'manifest.json'), 'w') as f:
                    json.dump({'product': 'better-smc-mixer', 'dry': True}, f)
                with open(os.path.join(stage, 'new.txt'), 'w') as f:
                    f.write('new package')
                return ['manifest.json', 'new.txt']
            with patch.object(release, '_pack_contents', side_effect=assemble):
                self.assertEqual(release.pack(out, {}), ['manifest.json', 'new.txt'])
            self.assertTrue(os.path.exists(os.path.join(out, 'new.txt')))
            self.assertFalse(os.path.exists(os.path.join(out, 'keep.txt')))
            self.assertEqual(os.listdir(tmp), ['release'])

    def test_published_package_may_replace_and_be_replaced(self):
        # a package made for publication (--date) is ours too: dry false
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, 'release')
            os.makedirs(out)
            with open(os.path.join(out, 'manifest.json'), 'w') as f:
                json.dump({'product': 'better-smc-mixer', 'dry': False, 'date': '2026-11-24'}, f)
            self.assertEqual(release.validate_output(out), out)

    def test_publication_date_is_checked(self):
        for bad in ('24.11.2026', '2026-13-01', '2026-11-24T00:00', ''):
            with self.subTest(date=bad), self.assertRaises(SystemExit):
                release.main(['--no-build', '--date', bad, '--out', os.path.join(tempfile.gettempdir(), 'x')])


@unittest.skipUnless(BUILT, "the device images are not built (make device)")
class Package(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = os.path.join(self.tmp.name, "release")
        self.imgs = release.images()
        self.files = release.pack(self.out, self.imgs)

    def tearDown(self):
        self.tmp.cleanup()

    def test_manifest_and_sums(self):
        with open(os.path.join(self.out, "manifest.json"), encoding="utf-8") as f:
            m = json.load(f)
        self.assertEqual(set(m["images"]), set(release.IMAGES))
        with open(os.path.join(release.ROOT, "VERSION"), encoding="ascii") as f:
            self.assertEqual((m["version"], m["editor"]), (f.read().strip(),) * 2)  # one version for the product
        self.assertIsNone(m["date"])
        self.assertIs(m["dry"], True)
        dated = os.path.join(self.tmp.name, "dated")
        release.pack(dated, self.imgs, date="2026-11-24")
        with open(os.path.join(dated, "manifest.json"), encoding="utf-8") as f:
            d = json.load(f)
        self.assertEqual((d["date"], d["dry"]), ("2026-11-24", False))
        # what the editor installs: the controller with Bluetooth and the SDK's cfg_tool.bin
        if os.path.exists(os.path.join(release.ROOT, release.CFG_TOOL)):
            self.assertEqual(m["install"], {"app": "firmware/controller-ble.bin", "cfg": "firmware/cfg_tool.bin"})
            for f in m["install"].values():
                self.assertIn(f, m["files"])
        self.assertEqual(m["versions"]["schema"], 1)
        for rel, e in m["files"].items():
            with open(os.path.join(self.out, rel), "rb") as f:
                data = f.read()
            self.assertEqual((len(data), hashlib.sha256(data).hexdigest()), (e["size"], e["sha256"]), rel)
        with open(os.path.join(self.out, "SHA256SUMS"), encoding="utf-8") as f:
            sums = dict(reversed(line.rstrip("\n").split("  ", 1)) for line in f)
        self.assertEqual(set(sums), set(self.files) - {"SHA256SUMS"})
        for name, i in self.imgs.items():
            self.assertEqual(sums["firmware/%s.bin" % name], i["sha256"])

    def test_licenses(self):
        # NOTICE and the license texts it names (the SDK's only where deps/ is here)
        for f in ("NOTICE", "LICENSE", "licenses/newlib.txt", "licenses/compiler-rt.txt", "licenses/tinyusb.txt"):
            self.assertIn(f, self.files)
        self.assertEqual("licenses/jieli-ac63-sdk.txt" in self.files,
                         os.path.exists(os.path.join(release.ROOT, "deps", "fw-AC63_BT_SDK", "LICENSE")))
        with open(os.path.join(self.out, "NOTICE"), encoding="utf-8") as f:
            notice = f.read()
        for f in ("licenses/newlib.txt", "licenses/compiler-rt.txt"):
            self.assertIn(f, notice)
        self.assertNotIn("not on this machine", notice)

    def test_editor_complete(self):
        for f in ("editor/index.html", "editor/js/app.js", "editor/data/schema.json", "editor/data/factory.json"):
            self.assertIn(f, self.files)
        self.assertFalse(any(f.endswith((".py", "package.json")) for f in self.files))

    def test_offline_documentation_links(self):
        self.assertIn('reference/docs/recovery.md', self.files)
        self.assertIn('reference/docs/app.md', self.files)
        self.assertEqual(release.check_docs.check(self.out), [])

    def test_private_string_in_an_image(self):
        path = os.path.join(self.out, "firmware", "controller.bin")
        with open(path, "ab") as f:
            f.write("SN-PLANTED-0001".encode("utf-16-le"))
        real = release.check_public.load_private
        release.check_public.load_private = lambda root: ["sn-planted-0001"]
        try:
            found, _ = release.scan(self.out, self.files)
        finally:
            release.check_public.load_private = real
        self.assertIn(("firmware/controller.bin", "private"), [(r, k) for r, k, _, _ in found])

    def test_clean_package_has_no_findings(self):
        found, _ = release.scan(self.out, self.files)
        self.assertEqual(found, [])


if __name__ == "__main__":
    unittest.main()
