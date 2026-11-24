// Names and help for the supported message types, modes and settings.

export const TYPE_HELP = {
  none: 'The control does nothing.',
  cc7: 'A Control Change, 0–127: what most synths and apps learn.',
  cc14: 'A high-resolution Control Change (two CCs: n and n + 32), 0–16383. Only for parameters the instrument reads in 14 bits.',
  pitchbend: 'Pitch Bend, 0–16383; the middle is no bend.',
  note: 'A note: pressing plays it, the value is the velocity.',
  program: 'Program Change: selects a sound on the instrument.',
  aftertouch: 'Channel pressure, 0–127.',
  relative2c: 'Sends steps up or down instead of a value; the instrument keeps the value. Encoding: 1 up, 127 down.',
  relativeBinOffset: 'Sends steps up or down instead of a value. Encoding: 65 up, 63 down.',
  relativeSignedBit: 'Sends steps up or down instead of a value. Encoding: 1 up, 65 down.',
  nrpn: 'A parameter by its NRPN number, in high resolution (0–16383). Many synths list these in their manual.',
  rpn: 'A registered parameter (RPN), such as pitch-bend range (0) or fine tuning (1).',
};

export const MODE_LABEL = {
  fader: {
    global: ['Default', 'Uses the default set in Settings.'],
    jump: ['Jump', 'Sends as soon as it moves; the value may jump.'],
    pickup: ['Pickup', 'Waits until the fader reaches the value known to the mixer. If none is known, the first move sends.'],
    scale: ['Scale', 'Scales from the value known to the mixer towards the end of travel. If none is known, the first move sends.'],
  },
  encoder: {
    acc0: ['No acceleration', 'One small step per click, however fast you turn.'],
    acc1: ['Acceleration: light', 'Faster turns take bigger steps.'],
    acc2: ['Acceleration: medium', 'Faster turns take bigger steps. A fast turn crosses the range in about one turn.'],
    acc3: ['Acceleration: strong', 'Faster turns take much bigger steps.'],
    div: ['Slow (clicks per step)', 'One step every few clicks: for switches with few positions, such as a waveform.'],
    step: ['Fixed step', 'The same step per click, without acceleration.'],
    fine: ['Fine', 'The smallest step, without acceleration.'],
  },
  button: {
    momentary: ['Momentary', 'On while held: press sends the on value, release the off value.'],
    toggle: ['Toggle', 'Each press switches between on and off.'],
    trigger: ['Trigger', 'Press sends the on value; release sends nothing.'],
  },
};

// A Program Change button is deliberately independent of press mode (core.c, press_assign).
// Its lo value is the LED-off comparison, not a program sent when the button is released.
export const PROGRAM_BUTTON_HELP = {
  mode: 'Every press sends the on-press program in every mode; release sends nothing.',
  valuesLabel: 'Programs: LED off and on press',
  values: 'Only the on-press program is sent. The other value marks when the LED is off.',
  swap: 'Swap program values',
  defaults: 'Reset the LED-off and on-press programs to their defaults.',
};

// Settings forms: [key, label, help, kind, options]. kind: enum from the schema (options from
// it), 'enum' with own options, 'int' [min, max, unit], 'text', 'bool'.
export const PRESET_FORM = [
  { h: 'MIDI' },
  ['channel', 'MIDI channel', 'Controls set to “Preset channel” send on it.'],
  ['noteOff', 'Note release', 'How a released note is sent.', { '9n': 'Note On, velocity 0 (most devices)', '8n': 'Note Off' }],
  ['paramNull', 'After NRPN/RPN moves', 'Some synths need the parameter deselected after a move or a press.', { off: 'Leave the parameter selected', gesture: 'Deselect it (RPN 127/127)' }],
  { key: 'middleC', label: 'Note names', help: 'How the editor names notes; the notes sent stay the same. Instruments differ: most call note 60 C4, some C3.', editor: true, options: { C4: 'Middle C (60) is C4', C3: 'Middle C (60) is C3' }, def: 'C4' },
  { h: 'Shift' },
  ['shiftMode', 'Shift button', 'How the Shift button works in this preset.', { hold: 'Hold: on while held', latch: 'Latch: each press switches', hybrid: 'Both: tap to latch, hold for a moment' }],
];

export const GLOBAL_FORM = [
  { h: 'Faders' },
  ['pickup', 'Default fader behaviour', 'For faders set to “Default”.', { jump: 'Jump', pickup: 'Pickup', scale: 'Scale' }],
  { h: 'Encoders' },
  ['encAccel', 'Acceleration sensitivity', 'Higher: big steps start at slower turns.', null, [50, 200, '%']],
  ['fine14', 'Fine step on high-resolution targets', 'Range ÷ 2048 is rounded down, at least 1. Choose 1 for the finest step on a 14-bit parameter.', { range: 'Range ÷ 2048 (7 on 0–16383)', lsb: '1: the finest possible' }],
  { h: 'Shift' },
  ['layerMode', 'Shift during a move', 'What happens when Shift is pressed or released while a control moves.', { gesture: 'Finish the move on its layer', immediate: 'Switch at once' }],
  ['gestureMs', 'A move ends after', 'A pause this long ends a move.', null, [1, 60000, 'ms']],
  ['hybridMs', 'Shift tap is shorter than', 'For the “Both” Shift mode: a shorter press latches.', null, [1, 60000, 'ms']],
];

export const ROW_NAME = { play: 'Play', stop: 'Stop', rec: 'Record', rew: 'Rewind', ff: 'Fast forward', bankLeft: 'Bank left', bankRight: 'Bank right', up: 'Up', down: 'Down', left: 'Left', right: 'Right' };
export const KEY_NAME = { fader: 'Fader', encoder: 'Encoder', m: 'M', s: 'S', r: 'R', select: 'Select (□)' };
