import type { ButtonId } from '../api/types.ts'

// The 48 Companion buttons, in core/src/remote/button.cpp order. IDs are the
// stable public names; labels are UI-only.
export interface ButtonMeta {
  id: ButtonId
  label: string
  group: 'keypad' | 'media_home'
}

const kp = (id: ButtonId, label: string): ButtonMeta => ({ id, label, group: 'keypad' })
const mh = (id: ButtonId, label: string): ButtonMeta => ({ id, label, group: 'media_home' })

export const BUTTONS: readonly ButtonMeta[] = [
  kp('up', 'Up'),
  kp('down', 'Down'),
  kp('left', 'Left'),
  kp('right', 'Right'),
  kp('select', 'OK'),
  kp('menu', 'Menu'),
  kp('number_1', '1'),
  kp('number_2', '2'),
  kp('number_3', '3'),
  kp('number_4', '4'),
  kp('number_5', '5'),
  kp('number_6', '6'),
  kp('number_7', '7'),
  kp('number_8', '8'),
  kp('number_9', '9'),
  kp('number_0', '0'),
  kp('number_dot_dash', 'Dot / dash'),
  kp('number_enter', 'Enter'),
  mh('off', 'Off'),
  mh('music', 'Music'),
  mh('tv', 'TV'),
  mh('movie', 'Movie'),
  mh('light_1', 'Light 1'),
  mh('light_2', 'Light 2'),
  mh('plus', 'Plus'),
  mh('minus', 'Minus'),
  mh('switch_1', 'Switch 1'),
  mh('switch_2', 'Switch 2'),
  mh('red', 'Red'),
  mh('green', 'Green'),
  mh('yellow', 'Yellow'),
  mh('blue', 'Blue'),
  mh('dvr', 'DVR'),
  mh('guide', 'Guide'),
  mh('info', 'Info'),
  mh('back', 'Back'),
  mh('exit', 'Exit'),
  mh('volume_up', 'Volume up'),
  mh('volume_down', 'Volume down'),
  mh('mute', 'Mute'),
  mh('channel_up', 'Channel up'),
  mh('channel_down', 'Channel down'),
  mh('skip_backward', 'Skip back'),
  mh('skip_forward', 'Skip forward'),
  mh('play', 'Play'),
  mh('pause', 'Pause'),
  mh('record', 'Record'),
  mh('stop', 'Stop'),
]

const byId = new Map(BUTTONS.map((b) => [b.id, b]))

export function buttonLabel(id: string): string {
  return byId.get(id as ButtonId)?.label ?? id
}

export function isButtonId(id: string): id is ButtonId {
  return byId.has(id as ButtonId)
}
