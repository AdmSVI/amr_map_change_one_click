# Fleet map agent

`amr-map-manager` (desktop) and `amr-map-agent` (headless) share
`src/core/MapDeployer`. The core links only Qt Core and Network. It uses the
existing RequestJob TCP buffering and RBK packet codec, never cancels tasks,
resolves the stored filename by MD5, and checks navigation status again before
switching. An already-current MD5 is verified without another upload or switch.

## Build and check

Use CMake 3.21+, C++17 and Qt 6.2+. Both applications build by default:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

For a server with only Qt Core and Network installed, add
`-DBUILD_MAP_MANAGER=OFF`. The Python agent integration test needs Python 3.9+
and binds only localhost HTTP plus TCP port 19204. Do not run it on a machine
already using that port. The C++ deployment test uses ephemeral localhost ports.
Tests do not access Fleet or real robots. The Windows packaging script includes
both executables, their Qt runtime dependencies and `agent.ini.example`.

## Configure

1. Deploy the companion Fleet API update first. A claim must include `claim_id`;
   a result must return it. A `done` result must include `map_md5_after` matching
   the rollout file. No additional SQL columns are needed for these changes.
2. Fleet's existing traffic and rollout SQL tables must already be provisioned
   (`docs/sql_traffic.sql`, then `docs/sql_map_rollout.sql` in the Fleet repo).
3. Create a dedicated staff service account and its DRF token in Fleet:
   `python manage.py drf_create_token <username>`.
4. Copy `agent.ini.example` to a private persistent directory as `agent.ini`.
   Set `fleet/url` to the origin, without a path, query or user information.
   Configure the Fleet reverse proxy so `file_url` uses that same origin.
5. Set `AMR_MAP_AGENT_TOKEN` in the account environment (preferred), or put
   `fleet/token` in the INI restricted to that account. Do not commit it.
   HTTP requires explicitly setting `allow_http=true` for a trusted intranet;
   HTTPS uses normal certificate verification. Redirects are not followed.
6. Keep concurrency at 1 for the pilot. Three is the maximum.

```powershell
.\amr-map-agent.exe --config C:\AMRMapAgent\agent.ini --check-config
.\amr-map-agent.exe --config C:\AMRMapAgent\agent.ini --log-file C:\AMRMapAgent\agent.log
```

`--check-config` performs no network or robot operations. Normal startup begins
claiming immediately, so first select only the stationary pilot robot in Fleet.
The process polls every ten seconds by default. It opens no inbound server.
The host must reach Fleet and the robots on TCP 19204, 19205 and 19207.

## Run on Windows

After the one-robot pilot, create a Task Scheduler task under a dedicated account:

- Trigger: at system startup.
- Program: the absolute path to `amr-map-agent.exe`.
- Arguments: `--config C:\AMRMapAgent\agent.ini --log-file C:\AMRMapAgent\agent.log`.
- Start in: the directory containing the executable and its runtime DLLs.
- Run whether the user is logged on or not; restart on failure.
- Disable the default task duration limit. Do not start another instance while
  the existing task is running.

Run exactly **one agent for this Fleet installation**. QLockFile prevents two
local processes using the same state file; it is not a distributed agent lock.
The application is a console process suitable for Task Scheduler or a service
wrapper, not a native Windows SCM service executable. Rotate the log externally.

## Recovery and cancellation

Claimed jobs and results are atomically saved to `agent-state.json` (relative to
the INI unless absolute). Keep the directory writable and persistent. Completed
results occupy capacity until Fleet acknowledges them. A failed result POST is
retried; HTTP 409 means the claim is obsolete and its report is discarded.
Other API errors retain the report for investigation rather than redeploying.

After restart, saved results are resent. An unfinished job is reported failed,
not blindly resumed; Fleet decides whether to retry. Never delete the state file
to clear an error while the agent is running. State corruption or persistence
failure stops the agent rather than continuing without an audit record.

An operation has an eight-minute local deadline including map download time;
Fleet's lease is ten minutes. Timeouts close local sockets and prevent later
steps but cannot undo a command already accepted by a robot. Retry first checks
its current MD5. Cancelling in Fleet stops new work; claimed work finishes.
A replacement rollout waits for a fresh claim on that robot to finish or expire.
Do not run the desktop upload workflow or Roboshop edits on the same robot while
the agent owns its rollout.

## Pilot acceptance

On one stationary robot, verify the original GUI (including Chinese/English),
then agent upload, remote MD5, stored filename, switch and current MD5. Confirm
busy robots remain unchanged, wrong checksum never switches, loss of Fleet
retains the result, and restart recovers it. Inspect localization and perform an
approved route check before expanding to two or three robots.

Fleet currently activates its traffic map and synchronizes POIs when a draft
is uploaded, before robot rollout. This agent does not change that behavior.
Keep traffic control in shadow mode and use a controlled maintenance window.
Prevent other systems, including queue dispatch and automated charging/home
tasks, from assigning work to the selected robots during that window: a busy
check is not an atomic dispatch lock. Keep these controls in place
until revision transitions have been validated. A successful map checksum does
not establish localization quality or route safety.
