// The remote's button rules from the spec (hubbo-devices `buttons.json`, synced into profiles/).
import data from '../../../../profiles/buttons.json'
import type { ButtonRule } from './build.ts'

export const BUTTON_RULES = data.buttons as ButtonRule[]
