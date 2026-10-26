# Independent 1P / 2P side (local camera side) — online and offline

> **한국어 요약** — 각 PC가 자기 화면에서만 자기 캐릭터를 원하는 쪽(LEFT 1P / RIGHT 2P)에 보이게
> 하는 기능입니다. 네트워크 팀 소유나 게임 결과는 바뀌지 않습니다. Player Match 방에서 두 사람이
> READY 하면 PLAYER SIDE SELECT 창이 뜨고, 각자 고른 쪽에 맞춰 대전 카메라·체력바(HUD)·좌우 입력·
> W/S 횡이동·캐릭터 선택 화면이 바뀝니다. 위쪽 플레이어 이름표는 아직 원래 자리에 남습니다(미해결).
> 이 브랜치는 원본 v0.1.6 위에 만들었습니다. 게임 데이터나 재컴파일된 게임 코드는 포함하지 않습니다.

This branch adds a **local, per-machine side choice** to the port: every player can see their own
fighter on the side they prefer (LEFT = 1P, RIGHT = 2P) on their own screen, offline and in online
Player Matches, without changing which team they control, the network team ownership, or the
simulation the two machines share.

It is built on top of upstream **v0.1.6** (`2edec5c`). Nothing derived from the game is included
(same rule as the main project: see `.gitignore`).

## What changes on the local screen

| Area | Offline (Practice / VS) | Online Player Match |
|---|---|---|
| Battle camera | rendered from the opposite side (180° about the target) | same |
| Stage objects / culling / fade | follow the rendered camera | same |
| Staged cameras (intro, win pose, stage) | original view (camera slots 2 / 3 / 6) | same |
| HUD (gauges, names, win marks) | each side shows the team standing there | same |
| Left / right input | side latch (`fighter+1028`) | raw pad mirror before the input is sent |
| W / S sidestep | side requirements 144..149 see the virtual side | flag bit in the networked input, both machines flip alike |
| Character select | — (Practice uses BATTLE POSITION) | UI drawn with the two sides traded |
| Character select name plates | — | **not yet** (stay on the native side) |

## How it is used

- **Online:** in a Player Match room both players press READY as usual. A *PLAYER SIDE SELECT*
  window then opens on each machine (10 s). UP/DOWN chooses LEFT [1P] / RIGHT [2P], **A confirms at
  once** (no cancel). When time runs out, the highlighted side is used. A player who confirms early
  sees *WAITING FOR THE OTHER PLAYER*. The game's own battle-ready wait is extended by the skipped
  time, so the early player does not time out. Then character select and the battle follow.
- **Offline:** the side select runs before Practice / VS. Practice maps it onto the game's own
  BATTLE POSITION option.
- Both machines must run the same build. The online W/S fix travels inside the shared input.

Switches (environment variables, all default on):

| Variable | Effect |
|---|---|
| `TTT2_ONLINE_SIDE_SELECT=0` | no online side select |
| `TTT2_CHARASELECT_SIDE_SWAP=0` | online character select drawn as the game lays it out |
| `TTT2_HUD_SIDE_SWAP=0` | no HUD team swap |
| `TTT2_CAMERA_SLOT_MASK=0x0FFE` | also turn the staged camera slots (pre-12-D behaviour) |
| `TTT2_CAMERA_INPUT_STRATEGY=XINPUT_LR` | legacy raw L/R mirror offline instead of the side latch |

## How it works (key findings)

Guest addresses refer to the TU3 image the port recompiles. Function names are the recompiler's
`sub_XXXXXXXX`.

### Camera (render only, `camInst` is never written)
- `sub_823C8B30` fills the scene's camera pass slots. The three accessors it calls return a view /
  view-projection / eye rotated 180° about `camInst+32` (the target). Reflections, previous-frame
  view and inverse matrices derive from that slot.
- Stage meshes cull against a second view getter (`sub_82398448`) and a parameter-block eye. The
  stage fade block `0x82DE69A0` (forward / eye / target) and mob culling read the camera inline.
  They get mirrored copies or a shadow camera manager at allow-listed return addresses.
- Camera manager slots (`camMgr+340+4*slot`): 1 battle, 2 / 3 sequence & motion script (intro, win
  pose), 4 / 5 tag & hit tracking, 6 stage staging, 7 practice, 8 throw. Default mask `0x0FB2`
  leaves 2 / 3 / 6 in their original view.

### Input
- Offline: `sub_823038E0` latches `fighter+1024` (side from the active camera) into `+1028`, which
  the L/R table (`sub_82303400`) and the side requirements 144..149 (`sub_8230DAB8`, they pick
  SSL / SSR) read. `+1028` is shown as the opposite side only while input is interpreted.
- Online the latch would desync the two simulations. Instead:
  - L / R: the raw pad is mirrored before the game sends it.
  - W / S: the OPPOSITE-view machine ORs `0x0800` (an unused pad bit) into its battle input word
    (`sub_82664CE0` → `sub_826F0CD0`, the 16 bits are sent as-is by `sub_826EFAD8`). Both machines
    strip it in the getters `sub_826EFA48` / `sub_826EFA90` (NACE+5984 / +5996) and flip side
    requirements 144..149 for the team whose agreed-frame slot carries it.

### Who is local online
- Not the online manager's `+7488` / `+7492` (layout mode, `2` / `-2` on both machines), not host
  status, not the name table (empty online).
- The sync engine (`NACE = 0x82E6C5C8`, `kSync = NACE+64`): nodes at `+4828`, count `+4856`.
  Node 0 is this machine, node `+744` is its battle slot = team (`sub_822745B0`: logical ids 9 / 11
  → slot 0, 10 / 12 → slot 1). Character select asks the same value (`sub_8266C740`).
- Teams alternate between matches, so the owner is read every frame.

### Room flow
- `state_room_main` (`sub_826BF870`) and the READY countdown are untouched. When both are READY the
  room switches to `state_battle_ready` (`sub_826BF778`). That state is held while the side select
  runs and released on confirm or after 10 s.
- `battle_ready_wait` (`sub_826BEF60`) counts `room+180` to 600 frames for the peer. An early
  client's counter is held back by the frames it did not hold (+1 s).

### Online character select (display only)
The Lumen select controller (update `sub_82644588`, draw `sub_82641308`), the TAG / SOLO panels
(`sub_8263D038`, `sub_8263D698`), the owner's side getters (`sub_8263CF88` / `CFD8` / `D008`) and
`sub_82644C98` read each side's select state from `data+48` / `data+56`. While the chosen side is not
the team's native one, those calls run with:
- the two states traded,
- the team profile index traded (`sub_82653478`),
- the pad ids 9 ↔ 10 / 11 ↔ 12 traded (`sub_8228DA18`),
- bits 0 / 1 of the local-human mask `*(*(*(0x82E6C5A8)+240)+84)` traded (it gates the browsing
  portrait in `sub_82643A08`).

Everything is restored on return.

A first attempt traded p1 / p2 element *names* in the Lumen lookup (`sub_82B62B30`). It broke the
big portraits, because they sit in per-side subtrees, so it was replaced by the data trade.

## Status

| Item | Result |
|---|---|
| Offline OPPOSITE camera, input, W/S, stage objects | confirmed in game |
| Practice side via BATTLE POSITION | confirmed in game |
| Online Player Match join, READY → side select → character select | confirmed (local two-instance) |
| Online local team identification (sync engine node 0) | confirmed in game |
| Online camera / HUD / A-D / W-S | confirmed in game |
| Online character select layout (cursor, portraits, TAG/SOLO) | confirmed in game |
| Character select name plates | **open**: not drawn through `sub_8256FE28` (never called there) nor as Lumen text; next step is the game's own text draw (`sub_8237ECB0`) |
| Two different PCs over Steam | not yet tested |
| Rebase onto v0.1.7 | not done |

## Files

- `port/src/runtime/kernel/side_camera_diag.{h,cpp}`: camera, input, side policy, online owner,
  W/S flag, HUD and character select hooks. It also holds the opt-in camera capture diagnostics.
- `port/src/runtime/kernel/player_side_select.h`: side select windows, room hold, battle-ready
  wait extension (header-only, unit-testable).
- `port/src/runtime/kernel/menu_ext.cpp`: the side select UI in the game's dialog window, room
  hooks, mode-change handling.
- `port/src/runtime/kernel/side_camera_ppc_probe.cpp`, `side_camera_writer_probe.cpp`,
  `port/tools/side_camera_*`, `test_side_camera_*`: opt-in camera research probes and their tests.
- `steam_bridge.cpp`, `matchmaking.cpp`, `xsession.cpp`, `xsocket.cpp`: a helper-process Steam
  bridge with frame pipes on the command line (fixes joins failing when Steam's own text corrupted
  stdout frames), plus extra logging.

## Credits

The port itself is
[shihabal3amri/tekken-tag-tournament-2-pc](https://github.com/shihabal3amri/tekken-tag-tournament-2-pc).
This branch only adds the side feature on top of it.
