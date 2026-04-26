# AI Integration (Distributable Model)

## Product Model
JCut is distributed as a client app. Users authenticate with your Supabase-backed system, and AI access is granted by your hosted gateway (Stripe-gated on the server).

The desktop app does not depend on local `../QTSynth` files for entitlement.

## Runtime Gate
AI is enabled only when all are true:
1. `feature_ai_panel=true` (project flag)
2. Gateway base URL is configured.
3. User access token is present.
4. Gateway entitlement check succeeds and reports `entitled=true`.
5. Contract version is supported (`1.x`).

## Client Flow
1. User clicks `AI Login` in the AI panel.
2. User provides:
   - Gateway URL (example: `https://your-gateway.example.com`)
3. App starts browser flow:
   - Starts localhost callback listener: `http://127.0.0.1:<port>/auth/callback`
   - Opens browser to `GET /api/auth/desktop/start` with:
     - `redirect_uri`
     - `state`
     - `code_challenge`
     - `code_challenge_method=S256`
4. Gateway redirects back with either:
   - `access_token` directly, or
   - `code` (authorization code)
5. If `code` is returned, app exchanges it via:
   - `POST /api/auth/desktop/exchange`
   - body includes `code`, `state`, `code_verifier`, `redirect_uri`
6. App calls `GET /api/ai/entitlements` with `Authorization: Bearer <token>`.
7. If entitled, app enables AI actions and uses `POST /api/ai/task` for jobs.

## Required Gateway Endpoints

### `GET /api/auth/desktop/start`
Starts browser login and eventually redirects to provided `redirect_uri`.

### `POST /api/auth/desktop/exchange`
Exchanges auth code for token. Expected to return `access_token` (or `session.access_token`).

### `GET /api/ai/entitlements`
Expected JSON fields:
- `entitled` (bool)
- `contract_version` or `version` (string, `1.x`)
- `user.id` (string)
- `models` (array of model ids or objects with `id`)
- `fallback_order` (array of model ids)
- `limits.requests_per_minute`
- `limits.project_budget`
- `limits.timeout_ms`
- `limits.retries`

### `POST /api/ai/task`
Request:
```json
{
  "action": "transcribe_clip",
  "model": "deepseek-chat",
  "payload": {},
  "context": {}
}
```
Must accept bearer auth.

## Local Enforcement in Client
The app enforces, per project:
- request budget cap
- requests/min rate limit
- timeout + retries
- model fallback chain

Usage counters are persisted with project state.

## Security Note
- Access tokens are persisted in OS credential storage when available:
  - Linux: `libsecret` via `secret-tool`
  - macOS: Keychain (`security`)
  - Windows: Credential Manager (`CredWrite/CredRead`)
- Project state stores non-sensitive AI settings only (gateway URL, model choice, usage counters), not bearer tokens.
- Legacy `aiAuthToken` values in old project files are treated as migration input and moved to secure storage on load.
