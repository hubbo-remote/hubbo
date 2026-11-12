// Wire constants shared by the client, the mock and the e2e harness.
// Dependency-free so Node code (mock, Playwright) can import it.
export const API_BASE = '/api/v1'
/** A remote scan gives up after this long (POST /admin/remote/pair). */
export const REMOTE_SCAN_TIMEOUT_S = 120
/** Remote channels are 0–124, like the console's `radio set` (RemoteEndpoint). */
export const REMOTE_CHANNEL_MAX = 124
/** A Bluetooth pairing window's default and range in seconds (POST /admin/devices/{id}/pair?window_s=). */
export const BLE_PAIR_WINDOW_S = 120
export const BLE_PAIR_WINDOW_MIN_S = 30
export const BLE_PAIR_WINDOW_MAX_S = 300
/** At most this many Bluetooth devices (a fifth is 422 too_many_devices). */
export const BLE_MAX_DEVICES = 4
/** A wait step's and a Bluetooth wait-until-connected's time bounds in ms (system `wait`, BLE `wait_connected`), for before the catalog loads. */
export const WAIT_MS = { minimum: 0, maximum: 60_000 }
export const WAIT_CONNECTED_MS = { minimum: 1000, maximum: 60_000, default: 15_000 }
