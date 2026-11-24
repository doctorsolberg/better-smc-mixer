"""macOS identity checks and explicit native-UI handoffs.

No administrator privileges, coordinate clicks or system Bluetooth resets.
Native endpoint enumeration is called only by an authorized hardware run.
"""
from pathlib import Path
import subprocess
import sys
import time

def choose_midi_row(rows, name):
    if name not in {'SMC-Mixer','Better-SMC-Mixer'}:raise ValueError('unapproved MIDI device')
    candidates=[r for r in rows if r.get('name')==name]
    if (len(candidates)!=1 or candidates[0].get('action') not in {'Connect','Disconnect'}
            or not candidates[0].get('enabled', True)):
        raise ValueError('expected one unambiguous available mixer row')
    return dict(candidates[0])

def forget_confirmation(text, name):
    return name=='SMC-Mixer' and text=='Are you sure you want to forget “SMC-Mixer”?' 

def midi_endpoints(executor=subprocess.run):
    script=Path(__file__).resolve().parents[1]/'hostmidi'/'hostmidi.py'
    result=executor([sys.executable,'-I',str(script),'list'],capture_output=True,text=True,timeout=15,check=True)
    ports={'inputs':[],'outputs':[]}
    seen=set()
    section=None
    for line in result.stdout.splitlines():
        if line in {'inputs:','outputs:'}:
            section=line[:-1]
            if section in seen:raise ValueError('duplicate MIDI listing section')
            seen.add(section)
        elif line.startswith('  ') and section:ports[section].append(line.strip())
        elif line.strip():raise ValueError('unrecognized MIDI listing')
    if seen!={'inputs','outputs'}:raise ValueError('incomplete MIDI listing')
    return ports

# No launch, focus, keyboard input, pairing deletion or global Bluetooth changes.
# Unknown/localized layouts stop at a manual checkpoint rather than guessing.
_BLE_SCRIPT = r'''
on mixerRow(mixerName)
    tell application "System Events"
        if not (exists process "Audio MIDI Setup") then error "Audio MIDI Setup is not running"
        tell process "Audio MIDI Setup"
            set panels to windows whose name is "Bluetooth Configuration"
            if (count panels) is not 1 then error "Expected one Bluetooth Configuration window"
            set panel to item 1 of panels
            if (count sheets of panel) is not 0 then error "Unexpected native sheet"
            set tablesFound to {}
            set panelElements to entire contents of panel
            repeat with elementRef in panelElements
                if role of elementRef is "AXTable" then set end of tablesFound to contents of elementRef
            end repeat
            if (count tablesFound) is not 1 then error "Expected one MIDI device table"
            set matches to {}
            set offlineRows to {}
            repeat with elementRef in rows of (item 1 of tablesFound)
                set exactName to false
                set offlineRow to false
                if name of elementRef is mixerName then set exactName to true
                set rowElements to entire contents of elementRef
                repeat with childRef in rowElements
                    if role of childRef is "AXStaticText" then
                        set childText to value of childRef
                        if childText is mixerName then set exactName to true
                        if childText contains "Offline" then set offlineRow to true
                    end if
                end repeat
                if exactName then
                    set end of matches to contents of elementRef
                    set end of offlineRows to offlineRow
                end if
            end repeat
            if (count matches) is 0 and mixerName is "SMC-Mixer" then return {"Absent"}
            if (count matches) is not 1 then error "Expected one exact mixer row"
            if item 1 of offlineRows then error "Mixer is Offline"
            set targetRow to item 1 of matches
            set controls to {}
            set buttonElements to entire contents of targetRow
            repeat with childRef in buttonElements
                if role of childRef is "AXButton" then set end of controls to contents of childRef
            end repeat
            if (count controls) is not 1 then error "Expected one mixer row button"
            set targetButton to item 1 of controls
            set buttonLabel to name of targetButton
            if buttonLabel is not "Connect" and buttonLabel is not "Disconnect" then error "Unknown mixer action"
            if not enabled of targetButton then error "Mixer row is unavailable"
            return {buttonLabel, targetButton}
        end tell
    end tell
end mixerRow

on run argv
    with timeout of 10 seconds
        if (count argv) is not 1 then error "Expected one phase"
        if item 1 of argv is "ours" then
            set mixerName to "Better-SMC-Mixer"
            set desiredLabel to "Disconnect"
            set allowedLabel to "Connect"
        else if item 1 of argv is "stock" then
            set mixerName to "SMC-Mixer"
            set desiredLabel to "Connect"
            set allowedLabel to "Disconnect"
        else
            error "Invalid phase"
        end if
        set initialRow to my mixerRow(mixerName)
        if item 1 of initialRow is desiredLabel or item 1 of initialRow is "Absent" then return "ready"
        -- Re-resolve identity and action immediately before the only press.
        set currentRow to my mixerRow(mixerName)
        if item 1 of currentRow is not allowedLabel then error "Mixer row changed"
        tell application "System Events" to perform action "AXPress" of (item 2 of currentRow)
        -- Native labels may transiently disappear after AXPress. Python verifies
        -- the exact endpoint transition without issuing another native action.
        return "pressed"
    end timeout
end run
'''


def ensure_midi(transport, phase, *, executor=subprocess.run):
    """Run only after current-session owner consent, enforced by the caller.

    Injection is mandatory in host tests. Imports do not query macOS or MIDI.
    Each call is bounded; a missing prerequisite is returned to the parent
    rather than waiting indefinitely or stealing focus to repair native UI.
    """
    if transport not in {'usb', 'ble'} or phase not in {'ours', 'stock'}:
        raise ValueError('invalid transport or phase')
    try:
        ports = midi_endpoints(executor)
    except (OSError, subprocess.SubprocessError, ValueError):
        return {'ready': False, 'kind': 'midi_enumeration',
                'message': 'MIDI endpoint enumeration failed; verify the selected transport before continuing.'}
    checkpoint = native_checkpoint(transport, phase, ports)
    if checkpoint is None:
        return {'ready': True}
    if transport == 'ble':
        kind = 'native_connection' if phase == 'ours' else 'disconnect_stock_midi'
        message = ('Open Audio MIDI Setup > Bluetooth Configuration; verify Better-SMC-Mixer > Connect.'
                   if phase == 'ours' else
                   'Open Audio MIDI Setup > Bluetooth Configuration; disconnect only SMC-Mixer, never connect stock MIDI.')
        try:
            result = executor(['/usr/bin/osascript', '-', phase], input=_BLE_SCRIPT,
                              capture_output=True, text=True, timeout=15, check=True)
        except (OSError, subprocess.SubprocessError):
            return {'ready': False, 'kind': kind, 'message': message}
        if result.stdout.strip() not in {'ready','pressed'}:
            return {'ready': False, 'kind': kind, 'message': message}
        try:
            ports = midi_endpoints(executor)
        except (OSError, subprocess.SubprocessError, ValueError):
            return {'ready': False, 'kind': 'midi_enumeration',
                    'message': 'MIDI endpoint enumeration failed; verify the selected transport before continuing.'}
        checkpoint = native_checkpoint(transport, phase, ports)
        if result.stdout.strip()=='pressed':
            deadline=time.monotonic()+10
            for _ in range(19):
                if checkpoint is None or time.monotonic()>=deadline:break
                time.sleep(0.5)
                try:
                    ports=midi_endpoints(executor)
                except (OSError,subprocess.SubprocessError,ValueError):
                    break
                checkpoint=native_checkpoint(transport,phase,ports)
        if checkpoint is None:
            return {'ready': True}
    return {'ready': False, 'kind': checkpoint['kind'], 'message': checkpoint['message']}


def native_checkpoint(transport, phase, ports):
    """Return a concrete owner action only when the native endpoint prerequisite is absent."""
    if transport not in {'usb','ble'}:raise ValueError('invalid transport')
    if transport=='usb':
        name='SMC-Mixer Config' if phase=='ours' else 'SMC-Mixer -Private'
        text='Connect the USB cable to the mixer; do not pair Bluetooth for this USB run.'
    elif phase=='ours':
        name='Better-SMC-Mixer Bluetooth'
        text='Audio MIDI Setup > Bluetooth Configuration > Better-SMC-Mixer > Connect.'
    else:
        names=ports.get('inputs',[])+ports.get('outputs',[])
        if any(n=='SMC-Mixer Bluetooth' for n in names):
            return {'kind':'disconnect_stock_midi','message':'Disconnect only SMC-Mixer in Audio MIDI Setup before browser Bluetooth selection.'}
        return None
    if all(name in ports.get(k,[]) for k in ('inputs','outputs')):return None
    return {'kind':'native_connection','message':text,'expected_endpoint':name}
