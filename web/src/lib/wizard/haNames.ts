// Home Assistant commands under the profile spec's command names, so HA devices fill the remote's
// buttons and the power steps the same way profiles do. Inputs come from `select_source`.

/** HA command id → spec command name. Mute is separate: HA sets mute on or off, it can't toggle. */
export const HA_NAMES: Record<string, string> = {
  'media_player.turn_on': 'PowerOn',
  'media_player.turn_off': 'PowerOff',
  'media_player.toggle': 'PowerToggle',
  'media_player.volume_up': 'VolumeUp',
  'media_player.volume_down': 'VolumeDown',
  'media_player.media_play_pause': 'PlayPause',
  'media_player.media_play': 'Play',
  'media_player.media_pause': 'Pause',
  'media_player.media_stop': 'Stop',
  'media_player.media_next_track': 'NextTrack',
  'media_player.media_previous_track': 'PreviousTrack',
  'light.turn_on': 'PowerOn',
  'light.turn_off': 'PowerOff',
  'light.toggle': 'PowerToggle',
  'switch.turn_on': 'PowerOn',
  'switch.turn_off': 'PowerOff',
  'switch.toggle': 'PowerToggle',
}

/** Mute: a press mutes, a hold unmutes. */
export const HA_MUTE = 'media_player.volume_mute'

/** The input command; its `source` options are the device's inputs. */
export const HA_SELECT_SOURCE = 'media_player.select_source'
