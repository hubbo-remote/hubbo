// Wire constants shared by the client, the mock and the e2e harness.
// Dependency-free so Node code (mock, Playwright) can import it.
export const API_BASE = '/api/v1'
/** Header carrying the session's CSRF token on unsafe requests (api/openapi.yaml). */
export const CSRF_HEADER = 'x-hubbo-csrf'
