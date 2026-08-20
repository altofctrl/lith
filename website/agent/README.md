# lith knapp backend

Oldowan chat + firmware compile + flash artifacts for lith.vidalion.co/knapp.
Flask on 127.0.0.1:3130, proxied by nginx at `lith.vidalion.co/api/`.

## Run

- Autostarts on boot via `@reboot` cron in aaron's crontab, in a screen
  session named `lith-knapp` (same pattern as backstage).
- Manually: `screen -dmS lith-knapp /home/aaron/lith-backend/run.sh`
- Health: `curl http://127.0.0.1:3130/api/health`

## Pieces

- `app.py`: Flask API (session, chat, build status, binaries, replay).
- `oldowan.py`: agent orchestration. JSON envelope protocol
  (say / ask / build / done), provider adapters, mock provider.
- `knappery.py`: community listings (SQLite `knappery.db`). No accounts;
  edit_token gates delete, voter_id dedups stars. Moderation is by hand via
  `admin.py`; nothing is hidden automatically.
- `builder.py`: single-worker compile queue using arduino-cli
  (`~/.local/bin/arduino-cli`, esp32 core 3.3.11, FQBN in
  `device_profile.json`). Hidden repair loop: compile errors go back to
  the agent as `[internal]` messages, up to 3 passes, invisible to the user.
- `providers.json`: the provider framework. `active` selects one. Formats:
  `anthropic` | `openai` (covers Groq/Mistral/DeepSeek/Ollama/vLLM/...) |
  `mock` (scripted, no key, for testing the full flow).
- `secrets.json` (not committed anywhere): `{"ANTHROPIC_API_KEY": "..."}`,
  referenced from providers.json as `${VARS}`. Env vars also work.
- `device_profile.json`: hardware ground truth injected into the system
  prompt; pin map is TODO until confirmed. Also carries FQBN and flash
  offsets for the esp-web-tools manifest.
- `sessions/`, `builds/`: runtime state, one JSON per session, one dir per
  build (sketch, out/ binaries, manifest.json, meta.json).

## Going live with a real provider

1. Put a key in `secrets.json` (chmod 600).
2. Set `"active"` in providers.json to `anthropic` (or another entry).
3. Restart: `screen -S lith-knapp -X quit; screen -dmS lith-knapp ./run.sh`

## Automated review (advisory)

Two small-model checks run on the provider's `guard_model`, both forced
single tool calls returning a JSON bool, and both fail open so a flaky API
call never blocks a user:

- `oldowan.is_adversarial()` screens chat input for jailbreak attempts
  before it reaches Oldowan's system prompt. Hits go to
  `adversarial_log.txt`. Two rules keep a false positive cheap: messages
  matching an option Oldowan just offered skip the guard entirely (that
  text is Oldowan's own, so there is nothing to classify), and a rejected
  turn is not written to the transcript, it just asks the user to rephrase
  and re-offers the question. Both exist because on 2026-08-13 the guard
  flagged one of Oldowan's own suggested options and silently killed a
  live session.
- `oldowan.review_knapp()` reviews each sketch published to the knappery.
  It runs off the request path in a daemon thread (`app._review_async`), so
  the listing goes live immediately either way. A concerning verdict files
  a normal row in the `reports` table prefixed `[auto] `, surfacing it in
  `./venv/bin/python admin.py reports` next to human flags. It never hides
  a listing or rejects a publish; a human still decides.

## Timeouts (the "oldowan is unreachable" failure)

Sketch generation is one non-streamed call with a big `max_tokens`.
Measured on claude-sonnet-5 (`bench_latency.py`): conversational turns run
9 to 21s, but the turn that writes the sketch took **137.9s**, and has been
seen over 180s. The original 150s `request_timeout_s` left ~12s of headroom,
so build turns timed out and the user got "oldowan is unreachable right
now" (a 502 from `/api/chat`) after waiting two and a half minutes.

Three values have to stay ordered, smallest to largest:

    guard_timeout_s (20s)  <  request_timeout_s (300s)  <  nginx proxy_read_timeout (330s)

If nginx is the smallest of the two large ones it 504s first and the user
sees a raw gateway error instead of Oldowan's own message. `nginx-api.snippet`
carries the matching value but the live config is root-owned, so changing it
needs sudo and a reload.

## Limits

Per-IP: 30 chat calls and 8 builds per hour (in-memory, resets on
restart). Sketch size capped at 200 KB. Compile timeout 7 min.

## Nginx

`nginx-api.snippet` in this directory must be included inside the
lith.vidalion.co server block (root-owned config, needs sudo).
