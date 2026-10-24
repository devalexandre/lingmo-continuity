# lingmo-continuity

One keyboard and mouse for several computers side by side: move the cursor
past the edge of the screen and it continues on the next computer, with the
same keyboard, a shared clipboard and files dragged across. Like Universal
Control or Synergy, in Settings › **Teclado e mouse compartilhados**
(Shared Keyboard and Mouse).

It drives [Deskflow](https://deskflow.org/) (`deskflow` in Arch `[extra]`; the
open-source continuation of Synergy/Barrier), so the other computers can run
Lingmo, another Linux, Windows or macOS with Deskflow, Synergy or Input Leap.
Deskflow's own window is never shown.

- `lingmo-continuityd`: the session daemon (D-Bus `com.lingmo.Continuity`).
  It writes Deskflow's files from the layout, runs `deskflow-core`, finds and
  pairs Lingmo PCs, shows the "Agora em" toast, the pairing dialog and the
  "Recebido de" fly-in, and reports the connection to the status bar's Island.
- The Settings page lives in lingmo-settings (`src/qml/Continuity/`,
  `src/continuitymodule.*`) and only talks D-Bus.

How it works, what crosses between PCs and the security model:
[docs/continuity.md](docs/continuity.md).

Build: `cmake -B build -G Ninja && ninja -C build` (Qt 6, LingmoUI, libxcb,
xcb-xfixes). `ctest --test-dir build` runs the layout, config, pairing and
file-transfer tests on loopback.
