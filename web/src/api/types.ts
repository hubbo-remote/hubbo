// Wire types for /api/v1, generated from api/openapi.yaml.
//
// `pnpm gen:api` regenerates src/api/schema.gen.ts from the spec. This file
// only gives the generated component schemas friendly names, plus a few
// UI-only types (marked). Nothing else in the app declares wire shapes.
import type { components } from './schema.gen.ts'

type S = components['schemas']

export type EventKind = S['EventKind']
export type ButtonId = S['Button']
/** `${button}:${event}`, e.g. `volume_up:down`. */
export type TriggerKey = string

export type Action = S['Action']
export type ActionParams = S['ActionParams']
export type ParamValue = ActionParams[string]
export type Bindings = S['Bindings']
export type Activity = S['Activity']
export type ActivityInput = S['ActivityInput']
export type ActivityList = S['ActivityList']
export type GlobalBindings = S['GlobalBindings']
export type TransitionResult = S['TransitionResult']
export type PhaseResult = S['PhaseResult']
export type ConfigState = S['ConfigState']
export type AuthState = S['AuthState']
/** UI-only: what the editor holds. `id`/`revision` are absent for a new activity. */
/** A stored activity being edited: its input fields plus which revision they came from. */
export type ActivityDraft = ActivityInput & { id: string; revision: number }

export type Device = S['Device']
export type DeviceKind = S['DeviceKind']
export type DeviceDetail = S['DeviceDetail']
export type Profile = S['Profile']
export type ProfileSummary = S['ProfileSummary']
export type ProfileSteps = S['ProfileSteps']
export type ProfileButtons = S['ProfileButtons']
export type Kind = S['Kind']
export type Provider = S['Provider']
export type HomeAssistantProvider = S['HomeAssistantProvider']
export type DeviceRegister = S['DeviceRegister']
export type DeviceRegisterProfile = S['DeviceRegisterProfile']
export type DeviceUpdate = S['DeviceUpdate']
export type Command = S['Command']
export type ParamSpec = S['ParamSpec']
export type Capabilities = S['DeviceCapabilities']
export type ExecuteResult = S['ExecuteResult']

export type IntegrationType = S['IntegrationType']
export type IntegrationState = S['IntegrationState']
export type Integration = S['IntegrationSummary']
export type IntegrationConfig = S['IntegrationConfig']
export type IntegrationConfigUpdate = S['IntegrationConfigUpdate']
export type DiscoverCandidate = S['DiscoverCandidate']
export type DiscoverStatus = S['DiscoverStatus']
export type BluetoothStatus = S['BluetoothStatus']
export type EntityDetailsStatus = S['EntityDetailsStatus']
export type RemotePairStatus = S['RemotePairStatus']
export type RemoteConfig = S['RemoteConfig']
export type RemoteEndpoint = S['RemoteEndpoint']
export type RemoteEndpointUpdate = S['RemoteEndpointUpdate']

export type Status = S['Status']
export type LatencyStats = S['LatencyStats']
export type RemoteStatus = S['RemoteStatus']
export type RemoteButtons = S['RemoteButtons']
export type AuthMode = S['AuthMode']

export type Session = S['Session']
export type TokenSummary = S['TokenSummary']
export type TokenCreated = S['TokenCreated']

export type Backup = S['Backup']
export type RestoreResult = S['RestoreResult']
export type FirmwareStatus = S['FirmwareStatus']

export type ApiErrorBody = S['Error']
export type ErrorCode = S['ErrorCode']

/**
 * A validation problem. Wire `ErrorDetail` codes are an enum; the UI's own
 * advisory checks add a few more (`mixed_semantics`, `missing_release`), so
 * the code is widened to string here.
 */
export type ValidationIssue = Omit<S['ErrorDetail'], 'code'> & { code: S['ErrorDetail']['code'] | (string & {}) }
/** UI-only: wire `ValidationResult` plus local warnings. */
export interface LocalValidation {
  valid: boolean
  errors: ValidationIssue[]
  warnings: ValidationIssue[]
}
