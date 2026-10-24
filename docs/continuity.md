# Shared keyboard and mouse: how it works

## Pieces

| Piece | Where | What it does |
|---|---|---|
| `lingmo-continuityd` | this repo | Session daemon. Keeps the state (`~/.config/lingmo-continuity/continuity.json`), writes Deskflow's files, runs `deskflow-core`, finds and pairs Lingmo PCs, sends and receives dragged files, shows the toast, dialog and fly-in. |
| Deskflow 1.27 (`deskflow-core`) | Arch `[extra]` (`deskflow`) | Moves the pointer, keyboard and clipboard between PCs over TLS. |
| Settings page | lingmo-settings `src/qml/Continuity/`, `src/continuitymodule.*` | On/off, role, layout editor, pairing, options, security. Talks only D-Bus. |

Start-up: `/etc/xdg/autostart/lingmo-continuityd.desktop` (`OnlyShowIn=Lingmo`,
read by lingmo-session) starts `lingmo-continuityd --autostart`, which quits
at once when the feature is off. Settings reaches it on D-Bus, and
`com.lingmo.Continuity.service` lets D-Bus start it when someone turns the
feature on (lingmo-session already exports the display with
`dbus-update-activation-environment`). lingmo-core needs no change.

## Deskflow, without its GUI

Deskflow 1.2x has one core binary: `deskflow-core server|client -s <file>
--new-instance`. The daemon writes, under `~/.config/lingmo-continuity/deskflow/`:

- `Deskflow.conf`: Deskflow's settings (INI): computer name, port, TLS always
  on, `checkPeerFingerprints=true`, certificate path; on the principal the
  clipboard, switch delay / double tap, lock key and one `[computer_<name>]`
  group per PC in the layout; on a secundário the principal's address.
- `server.conf`: the links, generated from the layout. Edges that touch are
  linked both ways, with ranges when they only partly overlap
  (`right(50,100) = notebook`), plus the hot keys
  (`keystroke(ScrollLock) = lockCursorToScreen(toggle)`,
  `keystroke(control+super+Right) = switchToNextScreen`).
- `tls/deskflow.pem` (key + self-signed certificate, 0600, made with
  `openssl req`), `tls/trusted-clients` and `tls/trusted-servers`
  (`v2:sha256:<fingerprint>` of every paired PC).

Deskflow reads its files at start, so a change (layout, options, pairing) means
a restart of `deskflow-core`; the daemon only restarts it when the generated
files actually change. It also restarts it when it dies (1 s … 30 s back-off),
reconnects a secundário on network changes (`QNetworkInformation`), and
Deskflow goes down with the daemon (`PR_SET_PDEATHSIG`, SIGTERM handling).

What the daemon learns from Deskflow:

- its IPC socket (`$XDG_RUNTIME_DIR/lingmo-continuity/deskflow-core`, private
  because `TMPDIR` points there): `connectionState`, `connectedClients`,
  `peerFingerprint` (each TLS check), `unrecognisedClient`; the hello must echo
  the core's exact version, which it says back in `versionMismatch=`;
- its log (stdout): `switch from "A" to "B"`, `entering computer`,
  `leaving computer`.

## Roles and layout

- **Principal** (server): its keyboard and mouse are shared. Its Settings page
  shows the layout editor: every paired PC drawn with its own monitors; drag
  one next to another and the touching edges are where the cursor crosses
  (snapping like Settings › Display). Positions are in pixels of each PC's
  desktop, so a 4K monitor next to a 1080p laptop maps the edge proportionally.
- **Secundário** (client): uses a principal's keyboard and mouse. With more
  than one paired principal it picks which one.
- The principal sends the layout to its Lingmo secundários (channel, below),
  so a file dragged against any edge knows where to go.

Lingmo PCs report their monitors in discovery/pairing. For a PC on another OS
the size isn't known (Deskflow only logs it at debug level): it is drawn as
1920×1080, which only matters for partial edges.

## Finding and pairing other PCs

- **Discovery** (UDP, port 24801, only while on): a small JSON announcement
  (name, label, role, monitors, ports, certificate fingerprint) to each LAN's
  broadcast address, once a minute and when asked; Settings asks every 15 s
  while the page is open. A PC typed in by address is also asked directly
  (other subnets). Nothing in it is trusted: it only says where a PC is.
- **Pairing, Lingmo ↔ Lingmo** (TLS channel, port 24802): both sides present
  their certificate; each computes a 6-digit code from both fingerprints
  (SHA-256 of the sorted pair) and shows it in a dialog on its own screen.
  Someone in the middle would have to present a different certificate on each
  leg, and the codes would differ. Both people have to press *Parear*; only
  then each side stores the other's fingerprint (in both Deskflow trust files)
  and Deskflow restarts with it.
- **PCs with Deskflow/Synergy/Input Leap on another OS**: add them by name or
  IP. When one connects (or, as secundário, when we connect to it), Deskflow
  refuses the unknown certificate and reports its fingerprint; the daemon
  then asks *Confiar em …?* showing the SHA-256 fingerprint to compare with
  the one that Deskflow shows on the other PC (Deskflow › Settings ›
  Security). On the other PC the user accepts our fingerprint in its own
  Deskflow. A name typed in is used as its Deskflow screen name; when only an
  address was given, the name comes from Deskflow's `unrecognisedClient` at
  the first connection.
- **Unpair**: Settings › Computadores › *Desparear…*. The fingerprint leaves
  both trust files, Deskflow restarts (the connection drops at once), and a
  Lingmo peer is told to forget us too. A Deskflow PC that keeps retrying is
  not asked about again for 2 minutes.

## Files across PCs

Deskflow 1.27 has **no file drag and drop** (its protocol keeps the old
`DFTR`/`DDRG` messages but marks them "no longer implemented"), and while a
mouse button is held it keeps the cursor on the current PC ("locked by mouse
button"). So lingmo-continuity does it itself:

1. `DragWatcher` (xcb + XFixes) sees a file drag start: someone takes
   `XdndSelection` while the left button is down.
2. For as long as the drag lasts it maps four 2-pixel XDND drop targets
   along the screen edges (override-redirect, above everything, gone at
   release). The drag source talks to them like to any drop target; they
   accept (copy) only where the layout has a paired Lingmo PC at that point of
   the edge. When the drag rests there for 350 ms, or is dropped there, it asks
   the source for `text/uri-list` with the timestamp of the source's last
   `XdndPosition`, as XDND requires (Qt refuses any other time), and keeps the
   local files.
3. It sends them to that PC: with `lingmo-share send --to <address> <files…>`
   when lingmo-share is installed (if it fails or doesn't know `--to`, the
   built-in way is used), otherwise over the paired TLS channel. The source
   shows *Enviando para Notebook*.
4. The receiving PC saves them in Downloads (`name (2).ext` when it exists,
   `.part` while arriving, refused without room) and shows a glass
   *Recebido de Desktop* card that flies out of its cursor, with *Abrir* /
   *Mostrar na pasta*.

What works: regular files from any XDND source (Qt, GTK, file managers) on
X11; several files at once; principal ↔ secundário in both directions.
Not (yet): folders; Windows/macOS PCs (use their own sharing); between two
secundários that aren't paired to each other; Wayland. The files land when the
drag reaches the edge; dropping them somewhere in particular on the other PC
isn't possible (the drag itself stays on the first PC).

### The lingmo-share hook

lingmo-share (KDE Connect + LocalSend, another repo) wasn't in the workspace
when this was written. The daemon calls, only if `lingmo-share` is on `PATH`:

    lingmo-share send --to <host-or-address> <file>…

and expects exit code 0 on success; anything else falls back to the built-in
transfer. If lingmo-share ends up with a different syntax, change
`Service::sendFilesTo()` (one place). When lingmo-share delivers the files,
its own drop bubble shows them on the other PC.

## Status bar

While connected the daemon starts an activity in the status bar's Island
(`com.lingmo.Island` `/Island`: `StartWithHints(appId, kind, title, subtitle,
icon, progress, actions, hints)`, `UpdateFields(id, fields)`, `Finish(id,
message)`, as LingmoUI's `IslandActivity` uses it): *Teclado e mouse
compartilhados · Com Notebook* / *Agora em Notebook*, finished with
*Desconectado*. Without the Island it sends a notification when the
connection starts and ends (not on every switch). The toast *Agora em:
Notebook* appears next to the cursor on the PC it arrives at.

## Security

- TLS always on (Deskflow and the channel), TLS 1.2+; Deskflow runs with
  `checkPeerFingerprints=true`: only paired certificates get in, both ways.
- Pairing needs a person on each PC; Lingmo PCs compare a shared code,
  Deskflow PCs a SHA-256 fingerprint.
- The channel accepts nothing from an unpaired certificate except a pairing
  request (one at a time, 2-minute timeout), and refuses to send to an address
  whose certificate isn't the paired one.
- Received file names are reduced to a base name (no paths, no hidden files).
- **Off means off**: with the feature off nothing listens (no Deskflow, no
  discovery, no channel); at login the daemon quits at once.
- The firewall is never changed. When ufw or firewalld is active, Settings
  shows the ports (24800-24802/tcp and 24801/udp on the principal; 24802/tcp
  and 24801/udp on a secundário) and the command to open them, with a Copy
  button.
- Files: `~/.config/lingmo-continuity/` (state and Deskflow files, 0600; the
  private key never leaves it), `$XDG_RUNTIME_DIR/lingmo-continuity/` (0700,
  Deskflow's IPC socket).

## D-Bus

`com.lingmo.Continuity` `/Continuity`, see `data/com.lingmo.Continuity.xml`.
`State()`/`StateChanged(json)` carry everything Settings shows. `Preview(what)`
shows the toast, the dialog or the fly-in without doing anything, to try them.

## Trying it on one machine

Every port comes from `LINGMO_CONTINUITY_PORT` (default 24800), so two daemons
can run side by side, each with its own `HOME`, `XDG_RUNTIME_DIR`, session bus
and Xvfb display. Other knobs for tests: `LINGMO_CONTINUITY_DESKFLOW` (path to
`deskflow-core`), `LINGMO_CONTINUITY_DISCOVERY` (comma-separated
`address:port` to talk to instead of broadcasting), `LINGMO_CONTINUITY_NAME`,
`LINGMO_CONTINUITY_LABEL`, `LINGMO_CONTINUITY_FIREWALL=ufw|firewalld|none`,
`LINGMO_CONTINUITY_NO_SHARE`, `LINGMO_CONTINUITY_TRANSLATIONS`,
`LINGMO_CONTINUITY_DEBUG` (Deskflow at debug level, logged). `tests/dragsource`
(built with the tests) starts a file drag on mouse press, to try the edge drag
with `xdotool`.
