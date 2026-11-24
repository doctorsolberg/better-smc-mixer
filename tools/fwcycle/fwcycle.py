#!/usr/bin/env python3
"""Assisted USB/BLE cycle CLI; check/plan/status never attach hardware.

Finite invocation lifecycle: start/continue run until a manual checkpoint,
uncertainty, failure or success, then exit. They do not launch a daemon. The
browser is deliberately left intact, including after an uncertain write.
"""
import argparse
import json
from pathlib import Path
import shutil
import sys

# Explicit local import convention works with the required python3 -I.
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from evidence import Evidence, EvidenceError
from model import initial, GuardError
from runner import Runner


def result_code(status):
    phase = status.get('state')
    if phase == 'passed_assisted':
        return 0
    if phase == 'failed':
        return 1
    if phase == 'needs_reconciliation':
        return 4
    return 3


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    check = sub.add_parser('check')
    check.add_argument('--host-only', action='store_true', required=True)
    check.add_argument('--json', action='store_true')
    plan = sub.add_parser('plan')
    plan.add_argument('--release', required=True)
    plan.add_argument('--transport', choices=('usb', 'ble'), default='ble')
    plan.add_argument('--expected-build')
    plan.add_argument('--json', action='store_true')
    start = sub.add_parser('start')
    start.add_argument('--release', required=True)
    start.add_argument('--transport', choices=('usb', 'ble'), required=True)
    start.add_argument('--run-dir', '--out', dest='run', required=True)
    start.add_argument('--expected-build')
    for name in ('status', 'ack', 'continue', 'stop'):
        cmd = sub.add_parser(name)
        cmd.add_argument('--run', '--run-dir', dest='run', required=True)
        cmd.add_argument('--json', action='store_true')
        if name == 'ack':
            cmd.add_argument('--checkpoint', required=True)
            cmd.add_argument('--token', required=True)
        if name == 'continue':
            hardware_args(cmd)
    hardware_args(start)
    start.add_argument('--json', action='store_true')
    return p


def hardware_args(cmd):
    cmd.add_argument('--allow-device', action='store_true', help='current-session audit confirmation, not owner consent')
    cmd.add_argument('--allow-flash', action='store_true', help='current-session audit confirmation, not reusable consent')
    cmd.add_argument('--endpoint', help='owned loopback Chrome CDP endpoint')
    cmd.add_argument('--origin', help='expected editor origin')
    cmd.add_argument('--profile', help='exact owned Chrome user-data-dir (required for hardware)')


def load_adapter(args, evidence):
    # Parent provides the real JSON-lines bridge wrapper; unavailable is an error,
    # never a fake adapter or synthetic success fallback.
    from bridge import BrowserAdapter
    return BrowserAdapter(endpoint=args.endpoint, origin=args.origin, run_dir=evidence.root, profile=args.profile)


def validate_pin(args):
    from release import validate
    pin = validate(args.release)
    expected = args.expected_build or pin.get('expected_build')
    if not expected:
        raise GuardError('expected build required via manifest or --expected-build')
    if args.expected_build and pin.get('expected_build') and args.expected_build != pin['expected_build']:
        raise GuardError('CLI expected build differs from pinned release')
    return pin, expected


def emit(value):
    print(json.dumps(value, sort_keys=True))


def main(argv=None, adapter_factory=None):
    args = parser().parse_args(argv)
    if args.command in ('start', 'continue'):
        # This project-local lock excludes other run directories, not arbitrary
        # editors/phones. External ownership remains a preflight responsibility.
        from evidence import project_lock
        if not args.allow_device or not args.allow_flash:
            print('fwcycle: --allow-device and --allow-flash current-session audit confirmations required', file=sys.stderr)
            return 2
        if not args.profile and adapter_factory is None:
            print('fwcycle: --profile required to verify owned browser', file=sys.stderr)
            return 2
        try:
            (HERE.parents[1] / 'build').mkdir(exist_ok=True)
            with project_lock(HERE.parents[1] / 'build' / 'fwcycle.lock'):
                return execute(args, adapter_factory)
        except (OSError, EvidenceError) as exc:
            print('fwcycle: ' + str(exc), file=sys.stderr)
            return 2
    return execute(args, adapter_factory)


def execute(args, adapter_factory=None):
    try:
        if args.command == 'check':
            node = shutil.which('node')
            value = {'schema': 1, 'host_only': True, 'hardware_tested': False,
                     'browser_attached': False, 'python': sys.version.split()[0],
                     'node_available': node is not None,
                     'not_tested': ['device access', 'native permissions', 'browser connection', 'firmware']}
            emit(value)
            return 0 if node else 2
        if args.command == 'plan':
            pin, expected = validate_pin(args)
            emit({'schema': 1, 'transport': args.transport, 'expected_build': expected,
                  'release': pin, 'hardware_tested': False,
                  'manual_boundaries': ['native connection/chooser consent', 'physical off/on',
                                        'uncertain write recovery requires owner assessment']})
            return 0
        if args.command in ('start', 'continue'):
            if not args.allow_device or not args.allow_flash:
                raise GuardError('--allow-device and --allow-flash current-session audit confirmations required; flags do not grant owner consent')
            if not args.endpoint or not args.origin:
                raise GuardError('--endpoint and --origin required for owned editor attachment')
        if args.command == 'start':
            pin, expected = validate_pin(args)
            with Evidence.create(args.run, {'schema': 1, 'release': pin, 'transport': args.transport,
                                            'expected_build': expected, 'consent_audit_only': True}) as evidence:
                state = initial(args.transport, expected)
                evidence.save_status(state)
                adapter = (adapter_factory or load_adapter)(args, evidence)
                try:
                    result = Runner(evidence, adapter, pin, state).start(True, True)
                finally:
                    if hasattr(adapter, 'close'):adapter.close()
                emit(result)
                return result_code(result)
        if args.command in ('status', 'stop'):
            # Atomic status reads and a separate typed stop mailbox do not take
            # the writer lock or attach a second hardware client.
            from evidence import safe_path
            root = safe_path(args.run)
            if not root.is_dir() or root.stat().st_mode & 0o077:
                raise EvidenceError('run directory must be private (0700)')
            view = Evidence.__new__(Evidence)
            view.root, view.lock_fd = root, None
            state = view.read_status()
            if args.command == 'stop':
                view.write_json('stop-request.json', {'requested': True})
                state = dict(state, stop_requested=True, stop_delivery='persisted mailbox; supervisor checks before new actions')
            emit(state)
            return result_code(state)
        with Evidence.open(args.run) as evidence:
            state = evidence.read_status()
            runner = Runner(evidence, None, evidence.read_json('run.json').get('release', {}))
            if args.command == 'ack':
                result = runner.ack(args.checkpoint, args.token)
            elif args.command == 'stop':
                result = runner.stop()
            else:
                runner.adapter = (adapter_factory or load_adapter)(args, evidence)
                try:
                    result = runner.resume(True, True)
                finally:
                    if hasattr(runner.adapter, 'close'):runner.adapter.close()
            emit(result)
            return result_code(result)
    except (OSError, ValueError, EvidenceError, ImportError) as exc:
        print('fwcycle: ' + str(exc), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
