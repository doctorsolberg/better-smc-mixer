"""Finite assisted supervisor. The adapter alone owns editor/device operations.

Adapter: snapshot()->dict, action(name, transport, **params)->dict,
optional connection_readiness(name, transport)->{ready, kind?, message?},
export_config(name)->completed local file path, release(pin)->{matched: bool}.
A readiness result is prerequisite-only; after ready, Connect accepts
readiness_checked=True and is durably marked before UI dispatch. Install receives
binding={pageId,jobId} from the validated backup rather than a replacement job.
Action names: rollback, stockConnect, backup, install, oursConnect. No generic JS.
Snapshots extend observer v1 with config {saved,dirty,pulled}, rollback
{ready,completed}, stock {verified}, backup {complete,path,plan_ok,release_match},
install {completed,readback_verified,boot_selection_verified}, restart {verified}.
Absent positive evidence pauses/fails closed. Acknowledgment is never observation.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import sys
import time

try:
    from .model import transition, GuardError, check_identity
except ImportError:
    from model import transition, GuardError, check_identity


def encoded_config(path):
    """Use the product's canonical codec, including configuration ID/revision."""
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.name.endswith('.crdownload'):
        raise GuardError('configuration export not a completed regular file')
    codec_dir = Path(__file__).resolve().parents[1] / 'smcconf'
    sys.path.insert(0, str(codec_dir))
    try:
        spec = importlib.util.spec_from_file_location('_fwcycle_codec', codec_dir / 'codec.py')
        assert spec is not None and spec.loader is not None
        codec = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(codec)
        raw = path.read_bytes()
        if raw.startswith(b'SMC') and not raw.lstrip().startswith(b'{'):
            model = codec.decode(raw)
        else:
            model = codec.from_json(json.loads(raw))
        return raw, codec.encode(model)
    finally:
        sys.path.remove(str(codec_dir))


def content_of(binary):
    """(SHA-256 of the binary form without its revision, the revision). The revision is the u32 at
    header offset 16 (docs/config.md, "Binary form"); the CRC at 12 covers it and is left out as
    well, so two forms equal but for the revision give the same digest."""
    if len(binary) < 36 or binary[:4] != b'SMCF':
        raise GuardError('configuration export is not a binary form')
    revision = int.from_bytes(binary[16:20], 'little')
    return hashlib.sha256(binary[:12] + binary[20:]).hexdigest(), revision


class Runner:
    def __init__(self, evidence, adapter, pin, state=None):
        self.evidence, self.adapter, self.pin = evidence, adapter, pin
        self.state = state if state is not None else evidence.read_status()
        self.attempt = evidence.new_attempt()
        self.authorized = False
        self.poll_interval = 1.0
        self.operation_timeout = 900
        self.operation_started = None
        if state is None and self.state['state'] in ('rollback_in_flight', 'install_in_flight'):
            self.state['uncertain_action'] = self.state['state'].removesuffix('_in_flight')
            self.state['state'] = 'needs_reconciliation'
            self.state['monitor_only'] = True
        # An intent may have reached disk while the status update did not.
        for record in evidence.records():
            if record['kind'] == 'intent':
                action = record['data']['action']
                if action not in self.state['intents']:
                    self.state['intents'].append(action)
                    self.state['effect'] = 'unknown'
                    self.state['state'] = 'needs_reconciliation'
                    self.state['uncertain_action'] = action

    def save(self):
        self.evidence.save_status(self.state)
        return self.state

    def move(self, event, **data):
        proposed = transition(self.state, event, **data)
        self.evidence.append('transition', {'event': event, 'state': proposed['state']})
        self.state = proposed
        return self.save()

    def checkpoint(self, kind, message):
        self.state['checkpoint'] = {'id': secrets.token_hex(8), 'token': secrets.token_urlsafe(24),
                                    'kind': kind, 'message': message}
        return self.save()

    def wait(self, message):
        self.state['resume_state'] = self.state['state']
        self.state['state'] = 'await_owner'
        return self.checkpoint('observe', message)

    def ack(self, checkpoint_id, token):
        checkpoint = self.state.get('checkpoint', {})
        if (not checkpoint or checkpoint.get('id') != checkpoint_id or
                not secrets.compare_digest(str(checkpoint.get('token', '')), str(token))):
            raise GuardError('checkpoint ID/token does not match current pending checkpoint')
        self.evidence.append('ack', {'id': checkpoint_id, 'kind': checkpoint['kind']})
        self.state['acknowledged'] = checkpoint['kind']
        del self.state['checkpoint']
        return self.save()

    def start(self, allow_device=False, allow_flash=False):
        self.move('authorized', device=allow_device, flash=allow_flash)
        self.authorized = True
        return self.run()

    def resume(self, allow_device=False, allow_flash=False):
        self.state.pop('final_digest', None)
        if hasattr(self.adapter, 'power_acknowledged'):
            self.adapter.power_acknowledged = self.state.get('power_boundary_acknowledged') is True
        self.authorized = allow_device is True and allow_flash is True
        if not self.authorized:
            raise GuardError('renew current-session device and flash confirmations before continuation')
        if self.state.get('checkpoint'):
            return self.state
        if self.state['state'] not in ('needs_reconciliation','rollback_in_flight','install_in_flight','await_power'):
            if self.adapter.release(self.pin).get('matched') is not True:
                raise GuardError('release pin no longer matches on safe continuation')
        if self.state['state'] == 'await_power':
            if self.state.pop('acknowledged', None) != 'power':
                return self.checkpoint('power', 'Owner must operate physical power switch; ack alone is not proof.')
            if hasattr(self.adapter, 'power_acknowledged'):
                self.adapter.power_acknowledged = True
            self.state['power_boundary_acknowledged'] = True
            self.move('power_ack')
        elif self.state['state'] == 'await_owner':
            self.state.pop('acknowledged', None)
            self.state['state'] = self.state.pop('resume_state')
            self.save()
        return self.run()

    def stop(self):
        self.state['stop_requested'] = True
        self.evidence.append('stop_requested', {'state': self.state['state']})
        if self.state['state'] in ('rollback_in_flight', 'install_in_flight', 'needs_reconciliation'):
            self.state['monitor_only'] = True
        else:
            self.state['state'] = 'failed'
            self.state['reason'] = 'stopped before issuing further actions; device left untouched'
        return self.save()

    def snapshot(self):
        observation = self.adapter.snapshot()
        self.evidence.append('observation', observation)
        self.state['last_observation'] = observation
        self.save()
        return observation

    def dispatch(self, name, **params):
        if self.evidence.path('stop-request.json').exists():
            self.stop()
        if not self.authorized:
            raise GuardError('renew current-session device and flash confirmations before actions')
        if self.state.get('stop_requested') or self.state.get('monitor_only'):
            raise GuardError('monitor-only; no new actions permitted')
        if name == 'install':
            params['binding'] = self.state.get('backup', {}).get('binding')
        return self.adapter.action(name, transport=self.state['transport'], **params)

    def connect(self, name, requested):
        """Check prerequisites before durable intent; never replay a dispatched Connect."""
        if not self.authorized:
            raise GuardError('renew current-session device and flash confirmations before readiness')
        if self.evidence.path('stop-request.json').exists():
            self.stop()
        if self.state.get('stop_requested') or self.state.get('monitor_only'):
            raise GuardError('monitor-only; no connection prerequisites permitted')
        params = {}
        if hasattr(self.adapter, 'connection_readiness'):
            readiness = self.adapter.connection_readiness(name, self.state['transport'])
            if readiness.get('ready') is not True:
                self.evidence.append('connection_prerequisite', {'action': name, **readiness})
                self.wait(readiness.get('message', 'Inspect connection prerequisites before rechecking.'))
                return False
            params['readiness_checked'] = True
        self.state[requested] = True
        self.save()  # Persist before any UI action can be dispatched.
        self.dispatch(name, **params)
        self.operation_started = time.monotonic()
        return True

    def write(self, action):
        if not self.authorized:
            raise GuardError('renew current-session device and flash confirmations before writes')
        proposed = transition(self.state, action + '_intent')
        # Both records must be durable before the one and only effect dispatch.
        self.evidence.append('intent', {'action': action, 'transport': self.state['transport']})
        self.state = proposed
        self.save()
        try:
            self.dispatch(action)
        except Exception as exc:
            self.state['state'] = 'needs_reconciliation'
            self.state['uncertain_action'] = action
            self.state['reason'] = 'write dispatch outcome unknown: ' + str(exc)
            self.save()
            return False
        self.operation_started = time.monotonic()
        return True

    def pending(self, observation):
        if observation.get('active') is not True:
            return False
        if self.operation_started is None:
            self.operation_started = time.monotonic()
        if time.monotonic() - self.operation_started >= self.operation_timeout:
            return False
        time.sleep(self.poll_interval)
        return True

    def export(self, name):
        path = self.adapter.export_config(name)
        raw, binary = encoded_config(path)
        self.evidence.write_bytes(name + '.json', raw)
        self.evidence.write_bytes(name + '.bin', binary)
        return hashlib.sha256(binary).hexdigest()

    def content(self, name):
        content, revision = content_of(self.evidence.path(name + '.bin').read_bytes())
        return {'content_digest': content, 'revision': revision}

    def run(self, max_steps=2400):
        try:
            for _ in range(max_steps):
                if self.evidence.path('stop-request.json').exists():
                    self.stop()
                phase = self.state['state']
                if phase in ('passed_assisted', 'failed', 'await_owner', 'await_power'):
                    return self.state
                if phase == 'needs_reconciliation':
                    self.snapshot()  # Observation only; no write replay or connection change.
                    return self.state
                if self.state.get('stop_requested') and phase not in ('rollback_in_flight', 'install_in_flight'):
                    return self.stop()
                obs = self.snapshot()
                if phase == 'baseline':
                    identity=obs.get('identity',{})
                    if not obs.get('initialized') or not identity.get('connected') or identity.get('kind')!='ours' or str(identity.get('transport','')).lower()!=self.state['transport']:
                        raise GuardError('initial identity/transport unavailable')
                    if self.adapter.release(self.pin).get('matched') is not True:
                        raise GuardError('editor release does not match pinned inputs')
                    digest=self.export('before')
                    obs=self.snapshot()
                    check_identity(self.state, obs, 'ours')
                    self.move('baseline', snapshot=obs, digest=digest, **self.content('before'))
                elif phase == 'rollback_ready':
                    check_identity(self.state, obs, 'ours')
                    if obs.get('rollback', {}).get('ready') is not True:
                        if hasattr(self.adapter, 'prepare_rollback'):
                            obs = self.adapter.prepare_rollback()
                            self.state['baseline']['snapshot']['rollback'] = obs.get('rollback', {})
                        if obs.get('rollback', {}).get('ready') is not True:
                            raise GuardError('rollback integrity no longer ready')
                    self.move('arm_rollback')
                    if not self.write('rollback'):
                        return self.state
                elif phase == 'rollback_in_flight':
                    if obs.get('rollback', {}).get('completed') is not True:
                        if self.pending(obs.get('rollback', {})):continue
                        return self.uncertain('rollback')
                    self.move('rollback_complete', completed=True)
                elif phase == 'stock_connect':
                    if obs.get('identity', {}).get('kind') != 'stock' or obs.get('identity',{}).get('connected') is not True:
                        if not self.state.get('stock_connect_requested'):
                            if not self.connect('stockConnect', 'stock_connect_requested'):
                                return self.state
                        if self.operation_started is None:self.operation_started=time.monotonic()
                        if time.monotonic()-self.operation_started<50:
                            time.sleep(self.poll_interval)
                            continue
                        return self.wait('Bounded stock discovery ended; inspect connection prerequisites before a new attempt.')
                    self.move('stock_connected', snapshot=obs)
                elif phase == 'backup':
                    if not self.state.get('backup_requested'):
                        self.state['backup_requested'] = True
                        self.save()
                        self.dispatch('backup')
                        continue
                    backup = obs.get('backup', {})
                    if backup.get('complete') is not True:
                        if self.pending({'active': bool(obs.get('job') and not obs['job'].get('error'))}):continue
                        return self.wait('Await completed fresh backup and verified install plan.')
                    path = Path(backup.get('path', ''))
                    if path.is_symlink() or not path.is_file() or path.name.endswith('.crdownload'):
                        raise GuardError('backup not a completed regular file')
                    raw = path.read_bytes()
                    self.evidence.write_bytes('full-backup.bin', raw)
                    self.move('backup_complete', complete=True, size=len(raw),
                              sha256=hashlib.sha256(raw).hexdigest(), plan_ok=backup.get('plan_ok'),
                              release_match=backup.get('release_match'), binding=backup.get('binding'))
                elif phase == 'install_ready':
                    binding = self.state.get('backup', {}).get('binding')
                    job = obs.get('job') or {}
                    if not binding or obs.get('backup', {}).get('binding') != binding:
                        raise GuardError('install backup job/page binding mismatch')
                    if job.get('stage') != 'backup_waiting' or job.get('completed') is not False:
                        raise GuardError('install requires the current uncompleted backup_waiting job')
                    if self.adapter.release(self.pin).get('matched') is not True:
                        raise GuardError('editor release changed before install')
                    if not self.write('install'):
                        return self.state
                elif phase == 'install_in_flight':
                    install = obs.get('install', {})
                    if not all(install.get(k) is True for k in
                               ('completed', 'readback_verified', 'boot_selection_verified')):
                        if self.pending(install):continue
                        return self.uncertain('install')
                    if (not self.state.get('backup', {}).get('binding')
                            or install.get('binding') != self.state['backup']['binding']):
                        return self.uncertain('install')
                    self.move('install_complete', binding=install.get('binding'),
                              completion_sequence=install.get('completion_sequence'),
                              **{k: install[k] for k in
                                 ('completed', 'readback_verified', 'boot_selection_verified')})
                    if self.state.get('stop_requested'):
                        return self.stop()
                    return self.checkpoint('power', 'Owner must switch mixer off/on; leave writer and browser intact.')
                elif phase == 'ours_connect':
                    if obs.get('identity', {}).get('kind') != 'ours' or obs.get('identity', {}).get('connected') is not True:
                        if not self.state.get('ours_connect_requested'):
                            if not self.connect('oursConnect', 'ours_connect_requested'):
                                return self.state
                        if self.pending({'active':True}):continue
                        return self.wait('Connect the expected system MIDI endpoint; no automatic Connect replay.')
                    if obs.get('config',{}).get('pulled') is not True:
                        self.state['final_digest']=self.export('final')
                        obs=self.snapshot()
                    self.move('ours_connected', snapshot=obs,
                              restarted=obs.get('restart', {}).get('verified'))
                elif phase == 'verify':
                    digest = self.state.get('final_digest')
                    if not digest:
                        digest = self.export('final')
                        obs = self.snapshot()
                    self.move('verify', snapshot=obs, digest=digest, **self.content('final'))
                    self.state.pop('reason',None)
                    self.state.pop('uncertain_action',None)
                    self.save()
                    self.evidence.write_json('result.json', self.state)
                    return self.state
                else:
                    raise GuardError('unsupported supervisor phase: ' + phase)
            return self.wait('Bounded observation budget reached; inspect before continuing.')
        except GuardError as exc:
            self.state['state'] = 'failed'
            self.state['reason'] = str(exc)
            if 'rollback' in self.state['intents']:
                self.state['restoration_verified'] = False
            return self.save()
        except Exception as exc:
            if self.state['effect'] == 'unknown':
                self.state['state'] = 'needs_reconciliation'
                self.state['monitor_only'] = True
            else:
                self.state['state'] = 'failed'
            self.state['reason'] = 'infrastructure/evidence failure: ' + str(exc)
            # Persistence failure must propagate, never dispatch another effect.
            self.save()
            return self.state

    def reconcile_install(self):
        """Explicit observation-only reconciliation; never dispatch another write."""
        if not self.authorized or not self.state.get('intents') or self.state['intents'][-1]!='install' or self.state['state']!='needs_reconciliation':
            raise GuardError('explicit authorized install reconciliation required')
        obs=self.snapshot()
        install=obs.get('install',{})
        if not all(install.get(k) is True for k in ('completed','readback_verified','boot_selection_verified')):
            raise GuardError('completed install not observed; leave device on')
        if (not self.state.get('backup',{}).get('binding')
                or install.get('binding') != self.state['backup']['binding']):
            raise GuardError('install job/page binding mismatch during reconciliation')
        sequence = install.get('completion_sequence')
        if type(sequence) is not int or sequence <= 0:
            raise GuardError('install completion sequence missing during reconciliation')
        if not self.state.get('backup',{}).get('plan_ok'):
            raise GuardError('pre-write validated backup evidence missing')
        if self.adapter.release(self.pin).get('matched') is not True:
            raise GuardError('completed operation release differs')
        self.evidence.append('reconciled',{'action':'install','completed':True})
        self.state.update(state='install_in_flight',monitor_only=False)
        self.move('install_complete', binding=install['binding'], completion_sequence=sequence,
                  **{k:True for k in ('completed','readback_verified','boot_selection_verified')})
        return self.checkpoint('power','Verified installation complete; owner may switch mixer off/on.')

    def uncertain(self, action):
        self.state['state'] = 'needs_reconciliation'
        self.state['uncertain_action'] = action
        self.state['monitor_only'] = True
        self.state['reason'] = 'write completion not positively observed; never automatically retry'
        return self.save()
