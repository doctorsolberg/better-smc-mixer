"""Pure, copy-on-transition firmware-cycle guards. No hardware imports.

Persisted shape: schema, state, transport, expected_build, consent, baseline,
intents, effect, stop_requested and assertions. Observations are dictionaries;
only explicit positive evidence advances a write or a final acceptance guard.
"""
import copy


class GuardError(ValueError):
    """A required observation or phase is absent."""


def require(condition, message):
    if not condition:
        raise GuardError(message)


def initial(transport, expected_build):
    require(transport in ('usb', 'ble'), 'transport must be usb or ble')
    require(isinstance(expected_build, str) and bool(expected_build), 'expected build required')
    return {'schema': 1, 'state': 'preflight', 'transport': transport,
            'expected_build': expected_build, 'consent': {}, 'baseline': None,
            'intents': [], 'effect': 'none', 'stop_requested': False, 'assertions': {}}


def check_identity(s, snapshot, kind):
    require(snapshot.get('initialized') is True, 'editor not initialized')
    identity = snapshot.get('identity', {})
    require(identity.get('connected') is True, 'device not connected')
    require(identity.get('kind') == kind, 'unexpected firmware kind')
    require(str(identity.get('transport', '')).lower() == s['transport'],
            'transport mismatch; no fallback permitted')
    if kind == 'ours':
        require(identity.get('build') == s['expected_build'], 'build mismatch')
        require(all(identity.get(k) is not None for k in
                    ('version', 'protocol', 'binaryForm', 'schema')), 'identity incomplete')
        baseline = s.get('baseline')
        if baseline:
            before = baseline['snapshot']['identity']
            require(all(identity.get(k) == before.get(k) for k in
                        ('version', 'protocol', 'binaryForm', 'schema')), 'firmware identity contract drift')
        panel=snapshot.get('panel',{})
        require(snapshot.get('panelObservation',{}).get('fresh') is True
                and all(panel.get(k) is not None for k in ('preset','page','shift','bluetooth')),
                'fresh complete panel observation required')
        cfg = snapshot.get('config', {})
        require(cfg.get('saved') is True and cfg.get('dirty') is False
                and cfg.get('pulled') is True, 'configuration must be pulled, saved and clean')
    else:
        require(snapshot.get('stock', {}).get('verified') is True, 'stock protocol not verified')


def transition(current, event, **data):
    s = copy.deepcopy(current)
    phase = s['state']
    phases = {'authorized': 'preflight', 'baseline': 'baseline',
              'arm_rollback': 'rollback_ready', 'rollback_intent': 'rollback_ready',
              'rollback_complete': 'rollback_in_flight', 'stock_connected': 'stock_connect',
              'backup_complete': 'backup', 'install_intent': 'install_ready',
              'install_complete': 'install_in_flight', 'power_ack': 'await_power',
              'ours_connected': 'ours_connect', 'verify': 'verify'}
    require(event in phases and phase == phases[event], 'invalid event for state: ' + event)
    if event == 'authorized':
        require(data.get('device') is True and data.get('flash') is True,
                'current-session device and flash confirmations required')
        s['consent'] = {'device': True, 'flash': True, 'audit_only': True}
        s['state'] = 'baseline'
    elif event == 'baseline':
        check_identity(s, data['snapshot'], 'ours')
        require(bool(data.get('digest')), 'baseline binary evidence missing')
        s['baseline'] = {'snapshot': data['snapshot'], 'digest': data['digest']}
        if data.get('content_digest'):
            s['baseline'].update(content_digest=data['content_digest'], revision=data.get('revision'))
        s['state'] = 'rollback_ready'
    elif event == 'arm_rollback':
        require(s['baseline']['snapshot'].get('rollback', {}).get('ready') is True,
                'stock integrity / rollback readiness absent')
        s['rollback_armed'] = True
    elif event in ('rollback_intent', 'install_intent'):
        action = event.removesuffix('_intent')
        require(s['consent'].get('flash') is True and s['consent'].get('device') is True,
                'current-session authorization missing')
        require(not s['stop_requested'], 'stop requested')
        require(action not in s['intents'], 'write already attempted; reconcile, never retry')
        if action == 'rollback':
            require(s.get('rollback_armed') is True, 'rollback not armed')
        s['intents'].append(action)
        s['effect'] = 'unknown'
        s['state'] = action + '_in_flight'
    elif event == 'rollback_complete':
        require(data.get('completed') is True, 'rollback completion unknown')
        s['effect'] = 'rollback_complete'
        s['state'] = 'stock_connect'
    elif event == 'stock_connected':
        check_identity(s, data['snapshot'], 'stock')
        s['state'] = 'backup'
    elif event == 'backup_complete':
        require(data.get('complete') is True and data.get('size') == 524288
                and bool(data.get('sha256')), 'fresh full backup missing')
        require(data.get('plan_ok') is True and data.get('release_match') is True,
                'verified plan and pinned release required')
        binding=data.get('binding') or {}
        require(bool(binding.get('pageId')) and type(binding.get('jobId')) is int and binding['jobId']>0,'backup job/page binding missing')
        s['backup'] = data
        s['state'] = 'install_ready'
    elif event == 'install_complete':
        require(all(data.get(k) is True for k in
                    ('completed', 'readback_verified', 'boot_selection_verified')),
                'installation/readback/boot-selection completion not verified')
        require(data.get('binding') is not None and data['binding']==s.get('backup',{}).get('binding'),'install job/page binding mismatch')
        require(type(data.get('completion_sequence')) is int and data['completion_sequence']>0,'install completion sequence missing')
        s['completion_sequence']=data['completion_sequence']
        s['effect'] = 'install_complete'
        s['state'] = 'await_power'
    elif event == 'power_ack':
        s['assistance_used'] = True
        s['state'] = 'ours_connect'
    elif event == 'ours_connected':
        require(data.get('restarted') is True, 'fresh restart observation required, not ack alone')
        check_identity(s, data['snapshot'], 'ours')
        require(data['snapshot'].get('restart',{}).get('binding')==s.get('backup',{}).get('binding'),'restart job/page binding mismatch')
        s['state'] = 'verify'
    elif event == 'verify':
        check_identity(s, data['snapshot'], 'ours')
        exact = data.get('digest') == s['baseline']['digest']
        revision = None
        if not exact:
            # The mixer assigns revisions (P-6) and a restart boots the saved one, so the revision
            # alone may differ (owner decision A, 2026-11-23): only with byte-identical content
            # and the restarted mixer running exactly its saved configuration.
            content = s['baseline'].get('content_digest')
            require(bool(content) and data.get('content_digest') == content,
                    'configuration content mismatch')
            identity = data['snapshot'].get('identity', {})
            require(identity.get('storage') == 'clean',
                    'revision differs and the running configuration is not the saved one')
            revision = {'before': s['baseline'].get('revision'), 'after': data.get('revision'),
                        'after_is_saved': True}
        require(data['snapshot'].get('panel')==s['baseline']['snapshot'].get('panel'),'panel state mismatch')
        s['assertions'] = {'configuration_exact': exact, 'configuration_content_exact': True,
                           'transport_exact': True,
                           'build_exact': True,
                           'panel_equal': data['snapshot'].get('panel') ==
                           s['baseline']['snapshot'].get('panel')}
        if revision:
            s['assertions']['revision'] = revision
        s['state'] = 'passed_assisted'
    return s
