# Portable renderer port specification — Phase 2 (de-risking)

Status: design/spec only. This document traces the Microsoft Comic Chat 2.5
source algorithms behind the four unstarted Phase 2 renderer items in
[`ROADMAP.md`](ROADMAP.md) (2.1 balloon geometry, 2.2 `CBodyUnary` compositing,
2.3 expert placement, 2.4 `CLabel` font bounding box) and specifies how each
maps onto the portable Cairo/FreeType canvas and how each is gated on Linux.

## Source of truth and how to read it

The behavioral source of truth is Microsoft's released 2.5-beta-1 tree. Two
readable copies exist:

- Archival branch: `git -C . show version/v2.5-beta-1:panel.cpp` (do not modify;
  provenance-locked per ROADMAP guiding constraints).
- Compile-describing mirror on `main`: `v2.5-beta-1-modern/` — the same logic,
  C++26-modernized so it can be read and (under MSVC CI) compiled. **All
  `file:line` citations below are into `v2.5-beta-1-modern/`** and were read at
  the tree state of this writing; re-anchor by symbol if lines drift.

Portable target files: `portable/src/render.cpp`,
`portable/include/comicchat/render.hpp`, `portable/src/layout.cpp`,
`portable/include/comicchat/layout.hpp`, `portable/src/source_raster.cpp`,
`portable/src/avatar_assets.cpp`,
`portable/include/comicchat/avatar_assets.hpp`, `portable/src/text.cpp`.
Existing tests to extend: `portable/tests/source_raster_test.cpp`,
`portable/tests/render_test.cpp`.

## 0. The logical-coordinate model (shared foundation)

Every Phase 2 item lives in the same coordinate system; port it once.

- **Units.** `UNITSPERINCH = 1440` (`defines.h:115`), i.e. Win32 `MM_TWIPS`.
  The retained comic page is drawn in these logical units and only transformed
  for the display/print device (`pageview.cpp:189` `OnDraw`, which sets
  `MM_TWIPS` and `LPtoDP`; `pageview.cpp:166,177,458`). The portable canvas
  already adopts this: `render.cpp:19` `source_panel_units = 2300.0`,
  `render.cpp:156` `render_title_panel` computes `scale = panel_size /
  source_panel_units` and converts at the final canvas scale only.
- **Y is up.** In `MM_TWIPS` positive Y is up, so a panel's interior runs from
  `top = 0` down to `bottom = -m_unitHeight`. This sign convention is pervasive:
  `GetBalloonRect()` returns `top = 0`, `bottom = -m_unitHeight/2`
  (`panel.cpp:842`); bodies get `top = -m_unitHeight + height`
  (`panel.cpp:772`). The portable `Rect` (`layout.hpp:17`) already stores
  `bottom`/`top` with `bottom < top`, and `page_bounds`/`panel_rect`
  (`layout.cpp:9,17`) already subtract rows going up. **Keep this sign model** —
  do not flip to screen-space Y until the final Cairo device transform.
- **Panel grid.** Panel is `m_unitWidth × m_unitHeight`, default/minimum
  `MINUNITPANELWIDTH = MINUNITPANELHEIGHT = 2300` (`panel.h:152`), with
  `m_hInterstice = m_vInterstice = 144` gutters (`panel.cpp:61`) and 2 panels
  per row. Portable mirrors this exactly: `logical_panel_width =
  logical_panel_height = 2300`, `logical_interstice = 144`, `panels_per_row = 2`
  (`layout.hpp:12`).
- **Font logical heights.** `nFontHeightTitle = -576`, `nFontHeightShout =
  -252` (`defines.h:137,138`), scaled by `reduction = m_unitWidth / 4860`
  (`fonts.cpp:102`). Portable carries the same constants:
  `source_title_font_height = 576.0`, `source_shout_font_height = 252.0`,
  `source_reference_width = 4860.0` (`render.cpp:25,26,24`).

Implication for Phase 2: introduce a **panel-local logical drawing pass** in
`render.cpp` that works entirely in twips with Y-up, then apply one Cairo
`translate(origin)·scale(scale)·scale(1,-1)` device transform, exactly as
`render_title_panel` already does for the title panel (`render.cpp:159-164`).
All four items below emit primitives into that logical pass.

---

## Item 2.1 — Comic balloon geometry, tails, thought/whisper shapes

### 2.1.a The deterministic re-layout property (port first — it gates everything)

Balloon layout is **randomized but deterministic per panel**. Each `CPanel`
draws one `m_seed = rand()` at construction (`panel.cpp:558`); a cloned panel
copies the seed (`panel.cpp:565`). `LayoutBalloons` calls `srand(m_seed)` before
placing any balloon (`panel.cpp:870`, comment "always layout panel the same
random way"). Every stochastic choice downstream consumes `randfloat() =
rand()/RAND_MAX` (`balloon.cpp:444`): cloud goal width (`panel.cpp:902,906`),
x-placement (`panel.cpp:919`), per-line jitter `ShiftLines`
(`balloon.cpp:777,785`), title choice (`panel.cpp:471`), emotion
(`panel.cpp:133`).

**Port contract:** the portable renderer must own a **seedable PRNG that
reproduces `srand`/`rand` sequence semantics** (a value-identical LCG or a
recorded stream), not `std::mt19937`. A panel model carries a `u32 seed`; layout
re-runs `seed_rand(seed)` before laying out and draws `randfloat()` in the exact
call order above. This makes goldens reproducible and is itself the cheapest
Linux test (same seed ⇒ byte-identical frame; see Verification 2.1).

> Honesty note: exact bit-parity with MSVCRT's `rand()` is only guaranteed if we
> reimplement its LCG (`state = state*214013+2531011; return (state>>16)&0x7fff`).
> That LCG is well-documented and portable; use it so `m_seed` values captured
> from a real `.ccc` reproduce. This is verifiable on Linux.

### 2.1.b Panel free-rect and per-balloon flow

`LayoutBalloons` (`panel.cpp:858`): grabs the free rectangle
`GetBalloonRect()` (`panel.cpp:842` — full width minus border pen, top `0`,
bottom `-m_unitHeight/2`; balloons live in the **upper half**, avatars below),
stashes up to 10 balloons, `srand(m_seed)`, then for each balloon calls
`LayoutBalloon` (`panel.cpp:928`). If the only balloon in a fresh panel does not
fit, `ForceFitBalloon` splits it (`panel.cpp:877-883`).

`LayoutBalloon` (`panel.cpp:928`) is the core:
1. `GetCloudEstimate` (`panel.cpp:888`) picks a goal width. One-liners
   (`len <= ONELINETHRESHOLD = 500`, `panel.cpp:40,896`) use their natural
   length; otherwise a random width between the widest word
   (`CLabel::WidestWord`, `balloon.cpp:736`) and the max, biased by available
   height `LowestPreviousBottom(...) - freeRect.bottom + MINHOOKHEIGHT`
   (`panel.cpp:899`, `MINHOOKHEIGHT = 100`, `panel.cpp:41`). Area estimate is
   `1.3 * textWidth * (textHeight + lineHeight)` (`CLabel::AreaEstimate`,
   `balloon.cpp:722-732`). X is placed so the balloon overlaps its speaker:
   centered around `m_speaker->m_arrowX` with random offset, clamped to
   `freeRect` (`panel.cpp:914-923`).
2. `GetInterveningBBox` (`panel.cpp:167`) shifts/clips the candidate rect out of
   previously-placed balloons' *route regions* (`QueryRouteRgn`,
   `balloon.cpp:1430`) and lowers its top below any docked cloud to its left
   (`Dock`, `balloon.cpp:585`: `delta = TOPBORDER+YBORDER+HWAVEHEIGHT`).
3. `CBalloon::SetBBox` (`balloon.cpp:1468`) fits text at the chosen width. If
   width/height changed it calls `ComputeInternals` (`balloon.cpp:1904`:
   `BreakIntoLines` → `ShiftLines` → `CreateBalloonSpline` → `ComputeCloudBBox`)
   and derives a real bottom (`balloon.cpp:1483`). Failure ⇒ balloon can't be
   built that small ⇒ layout fails.
4. If the balloon is near the top it docks (`DockAtTop`, `balloon.cpp:1306`:
   `Top = height + TOPBORDER`, `TOPBORDER = -20`, `balloon.cpp:71`).
5. Compute the route region `GetCloudBBox(&m_routeRgn)` (`panel.cpp:943`), reject
   if it drops below `freeRect.bottom + MINHOOKHEIGHT` (`panel.cpp:944`), then
   `AdjustRouteRgns` subtracts this balloon's column from earlier balloons
   (`panel.cpp:248,946`).

`RearrangeBalloons` (`panel.cpp:962`) is an **empty stub** in the shipped source
— no second pass exists. Do not invent one.

`m_arrowX` (the tail anchor, where the balloon points at its speaker) is set in
`LayoutAvatars`: `m_arrowX = bbox.Left + round(arrowX_frac * width)` where
`arrowX_frac` comes from the avatar's `faceX/width` (`panel.cpp:817`,
`avatar.cpp:73,112`).

### 2.1.c The cloud outline (say balloon)

`CreateBalloonSpline` (`balloon.cpp:1839`) builds the puffy outline:
- `GetFilters` (`balloon.cpp:482`) turns the per-line left/right text extents
  into left/right staircases (`RANGE` runs), so the cloud tightly binds ragged
  text.
- `PermuteFilters` (`balloon.cpp:531`) assigns Y to each step using the font
  metrics: first step `TOPBORDER + YBORDER + m_topOffset`, interior steps
  `±YBORDER`/`-YBORDER - m_baseAdd`, decrementing `baseY` by
  `nLinesInRun * m_lineHeight` (`balloon.cpp:539-543`). Constants:
  `XBORDER = 100`, `YBORDER = 40`, `TOPBORDER = -20` (`balloon.cpp:69,70,71`).
- `AddWavies` (`balloon.cpp:563`) inserts the scalloped bumps along each edge:
  amplitude `HWAVEHEIGHT/VWAVEHEIGHT = 70`, period `H/VWAVEINTERVAL = 300`
  (`balloon.cpp:60-63`), zig-zagging by a perpendicular `waveDiam` vector.
- The point list (`MAXPTS = 150`, `balloon.cpp:64`) becomes a **closed
  Kochanek–Bartels/beta spline** `CBeta(pts, nPts, TRUE)` (`balloon.cpp:1873`;
  `CBeta::defaultTension = 5.0`, `defaultBias = 1.0`, `spline.cpp:65,66`). This
  is what gives the rounded cartoon-cloud silhouette.

**Port:** the portable canvas has no `CBeta`. Two honest options:
1. **Faithful:** port `CBeta`/`CSpline` (`spline.cpp`, `splinutl.cpp`) to emit
   the same control-point → Bézier expansion, then stroke/fill the path with
   Cairo (`cairo_move_to`/`cairo_curve_to`/`cairo_close_path`,
   `cairo_fill_preserve` + `cairo_stroke`, as `render.cpp:196-199` already does
   for star circles). Highest fidelity; the beta-spline math is deterministic and
   Linux-testable at the control-point level.
2. **Contour-equivalent:** emit the same `AddWavies` polygon and render it with
   Cairo `curve_to` smoothing. Cheaper, visually close, but *not* byte-parity
   with MSVC GDI — mark such goldens `unverified-vs-MSVC-visual`.

### 2.1.d The tail (`AddArrow`)

`CBWoodringNormal::AddArrow` (`balloon.cpp:1538`): the tail runs from the cloud
bottom down to `(m_speaker->m_arrowX, m_speaker->m_bbox.Top + 200)`. It picks a
break point `xbreak` at the middle of the route region, nudged to stay under the
last text line (`balloon.cpp:1552-1559`), clamps the tail angle to ≤45° from
vertical (`balloon.cpp:1575-1581`), `BreakSpline` cuts a `gapwidth = 80` opening
in the cloud (`balloon.cpp:451,457`), and adds two `CArc` segments forming the
curved pointer with alt `0.05 * tailLen` (`balloon.cpp:1595-1601`). Minimum tail
height `MINTAILHEIGHT = 100` (`balloon.cpp:76,1564`).

### 2.1.e Whisper, think, and action-box variants

`MakeBalloon` (`panel.cpp:1039`) selects the subclass by mode
(`BM_SAY`→`CBWoodringNormal`, `BM_WHISPER`→`CBWoodringWhisper`,
`BM_THINK`→`CBWoodringThink`, `BM_ACTION*`→`CBWoodringBox`):

- **Whisper** (`balloon.cpp:1954`): same cloud as normal but constructed with
  `byteDashed = 1` and the whisper font (`m_fiWWhisper`). `Draw`
  (`balloon.cpp:1919`) strokes with `m_nimbusPen` first, fills white, then
  **re-strokes the trajectory dashed** via `m_traj->Dash(pdc)`
  (`balloon.cpp:1936-1940`). Port = same outline + a dashed Cairo stroke
  (`cairo_set_dash`).
- **Think** (`balloon.cpp:1966`): draws the normal cloud, then replaces the tail
  with a line of shrinking ellipses. `nBubbles = (deltaY + INTERBUBBLE) /
  (BUBBLEHEIGHT + INTERBUBBLE)` along the vector from the cloud to the speaker;
  `BUBBLEHEIGHT = 150`, `INTERBUBBLE = 100`, `ENDBUBBLEWIDTH = 400`
  (`balloon.cpp:57,58,59,1979-2004`). Each bubble is a `cairo_arc` circle.
- **Action box** (`balloon.cpp:2011`): a rectangle, not a cloud. `SetBalloonTraj`
  builds 4 `CLine` segments inset by `XBOXDELTA = 90`, `YBOXDELTA = 50`
  (`balloon.cpp:54,55,2018-2033`); `ComputeCloudBBox` is the text bbox ±those
  deltas (`balloon.cpp:2042`); it is left-justified (`FT_LEFT_JUSTIFY`,
  `balloon.cpp:2014`) and starts a new panel (`panel.cpp:1067`). No tail.

### 2.1.f Portable data model to add

Extend `render.hpp` with a panel model that mirrors the source: per-balloon
`{ mode, utf8 text, arrowX, bbox (twips), spline/polygon points, route region }`
and a `Panel { u32 seed; vector<Balloon>; vector<Body> }`. Add
`Canvas::render_panel(const Panel&, TextEngine&)` next to the existing
`render_title_panel` (`render.hpp:47`). Reuse `wrap_words`/`draw_shaped`
(`render.cpp:69,93`) for the text; add cloud-outline + tail emitters.

---

## Item 2.2 — `CBodyUnary` / avatar pose compositing from AVB

### 2.2.a What already exists in the portable tree

`portable/src/avatar_assets.cpp` + `avatar_assets.hpp` already decode the AVB
container into the exact record model the source uses:
`AvatarAsset{ kind, poses[], bodies[], faces[], torsos[], icon_pose_id }`
(`avatar_assets.hpp:55-69`), where each `AvatarPose{ drawing, mask, aura }`
(`avatar_assets.hpp:37-41`) matches the source `CPose::m_pdibs[3]`
(`avatar.h:56`, accessors `GetDrawing/GetMask/GetAura` at `avatar.h:31,33,35`),
and `AvatarComponent` carries `pose_id, center_x/y, center_delta_x/y, face_x/y`
(`avatar_assets.hpp:43-53`) matching `FACEREC`/`BODYREC` (`avatar.h:103-121`).
`render_avatar(asset, request)` with `AvatarRenderMode::legacy_exact`
(`avatar_assets.hpp:86,102`) is the intended compositing entry point. So Phase
2.2 is **compositing + geometry**, not decode.

### 2.2.b Simple avatar (`CBodySingle` / `CBodyUnary`) — one layer

`CBodyUnary` is a `CBodySingle` whose pose id is `m_bodyID` directly
(`avatar.h:169-178`; `GetPoseID()` returns `m_bodyID`). It is what the **title /
conversation-star roster** uses: `AddStars` news a `CBodyUnary`, sets
`m_bodyID = av->m_icon` (the icon pose), and boxes it at `ICONSIZE = 500`
(`panel.cpp:1438-1440`). This is the direct successor to the portable title
panel's placeholder "colored initial circle" (`render.cpp:190-206`).

`CBodySingle::DrawBody` (`bodycam.cpp:601`): resolve the pose
(`GetPoseFromID`, `avatar.cpp:117`, with neutral-then-anything substitution
`avatar.cpp:142-166`); compute the fitted rect `GetBodyBox`
(`bodycam.cpp:673`: scale the pose bitmap to fit `clientRect` preserving aspect,
centered horizontally and **bottom-aligned**, `bodycam.cpp:692-695`); flip
horizontally if `m_flip` (`bodycam.cpp:595`); draw aura with `MERGEPAINT`, then
drawing with `SRCAND` (`bodycam.cpp:614-621`).

**Raster-op semantics to preserve:** `SRCAND` (drawing) + `MERGEPAINT`
(mask/aura) is Windows 1-bit transparent-blit: the mask ANDs a hole, the drawing
is ANDed in. In straight-alpha ARGB (portable `AvatarBitmap` is 0xAARRGGBB,
`avatar_assets.hpp:31-35`) the equivalent is: mask defines alpha, drawing
supplies color; `render_avatar` should already implement this for
`legacy_exact`. Verify the AND/MERGEPAINT phase matches `bodycam.cpp:614-621`.

### 2.2.c Complex avatar (`CBodyDouble`) — head + torso composite

`CBodyDouble::DrawBody` (`bodycam.cpp:527`) composites two poses:
- resolve `headPose` + `torsoPose` (`GetPosesFromIDs`, `avatar.cpp:168`);
- `GetBodyBox` (`bodycam.cpp:632`) computes the union bitmap box from the head
  offset `xOffset = torso.xCX + face.delta_xCX - face.xCX`,
  `yOffset = torso.yCX + face.delta_yCX - face.yCX` (`bodycam.cpp:634,635`),
  scales the union to the client rect (`min(widthScale,heightScale)`,
  `bodycam.cpp:650-652`), then derives separate `headRect`/`torsoRect` in the
  scaled space (`bodycam.cpp:662-670`);
- draw order is flag-driven: `TORSOFIRST` (`avatar.h:186`) draws torso mask+
  drawing, then head; otherwise head, then torso (`bodycam.cpp:555-584`). Masks
  are gated by `HEADMASK`/`TORSOMASK` flags (`avatar.h:184,185`).

The Zig legacy fork already learned this exact rule (commit `8dc5ef3`:
"composite head+body whenever a head pose exists"); the C++ portable port must
reproduce the head-offset geometry (`bodycam.cpp:634-635`) and the
`TORSOFIRST`/mask flag ordering, not just overlay two bitmaps centered.

### 2.2.d Dimension feedback into layout

`GetDimInfo` (`avatar.cpp:55` single, `avatar.cpp:77` double) returns
`width,height,normHeight,headHeight,faceX` used by `LayoutAvatars`
(`panel.cpp:761`) to scale bodies to `m_unitHeight/1.9` (`panel.cpp:740`),
distribute horizontal margins (`panel.cpp:810`), and set `m_arrowX`
(`panel.cpp:817`). `faceX` flips under `m_flip` (`avatar.cpp:74,113`). Item 2.2
must expose the same dim info so Item 2.1's `m_arrowX` and Item 2.3's placement
have real geometry to consume. `normHeight` is a constant `100` in the shipped
source (`avatar.cpp:64,110`) — do not over-engineer per-pose normalization.

---

## Item 2.3 — Expert placement (avatar order + panel splitting)

### 2.3.a Roster ordering (title panel) — already ported

`AddStarsAux` (`panel.cpp:479`) orders the conversation-star roster: local user
(`g_puiSelf`) first (`panel.cpp:493-495`), present users before departed, then
descending send count (`panel.cpp:503-504`), skipping avatars with no icon
(`panel.cpp:490`). The portable `order_stars` (`layout.cpp:31`) already
reproduces this (self first, present-before-departed, `sends` descending,
`has_icon` filter, `max_stars` clamp) and is covered — this half of 2.3 is
**done**; keep `layout.cpp:31` as the reference.

### 2.3.b Conversation-panel placement — the unported half

The real remaining work is `LayoutAvatars`' greedy left-to-right ordering of
speakers within a conversation panel:

- `OrderAvatars` (`panel.cpp:426`): if fewer than 5 speakers, pull in "talk-to"
  partners (`AddTalkTos`, `panel.cpp:317`, capped at 5 per panel
  `panel.cpp:325`), then `DoGreedyOrdering`.
- `DoGreedyOrdering` (`panel.cpp:405`): insert each body at the position + facing
  direction that minimizes a rating; ties broken by the avatar's last direction
  (`panel.cpp:398-401`).
- `EvalPlacement` (`panel.cpp:359`) tries the body flipped both ways and scores
  every pair with `EvalPair` (`panel.cpp:280`): rewards facing the person you
  talk to (`m_udi.m_talkTos`, `panel.cpp:293,303`), penalizes facing away
  (`+40`, `panel.cpp:307`), plus a displacement penalty for breaking prior
  left/right adjacency (`ComputeDisplacementPenalty`, `panel.cpp:260`).
- `UpdateHistoresis` (`panel.cpp:437`) records each avatar's chosen direction and
  its left/right neighbors so the **next** panel is consistent
  (`m_lastDir/m_lastLeft/m_lastRight`, `panel.cpp:442-448`).

**Panel splitting** is not a geometric split; it is *flow control* in `AddLine`
(`panel.cpp:1061`): a new `CUnitPanel` starts when the current one has ≥5
elements, or the speaker is already in the panel, or fewer than 2 panels exist
(`panel.cpp:1082`); action boxes force a new panel (`panel.cpp:1067`); if
`LayoutBalloons` fails the panel is discarded and the line retried in a fresh
panel (`panel.cpp:1113-1119`), and any text that overflowed is re-added via the
`szLeftOverString` recursion (`panel.cpp:1131-1138`). The page-level panel
**grid** (2 per row, gutters) is already ported in `layout.cpp:9,17`.

**Port scope for 2.3b:** an `order_conversation(bodies, talk_tos)` in
`layout.cpp` implementing `DoGreedyOrdering`/`EvalPair`/`ComputeDisplacementPenalty`
plus a `should_start_new_panel(...)` predicate mirroring `panel.cpp:1082`. This
is pure integer/logic code (no rasterization) → highly Linux-testable.

---

## Item 2.4 — `CLabel` font bounding-box exactness

### 2.4.a What the portable text pass lacks today

`render.cpp` measures text by summing HarfBuzz glyph advances
(`shape_advance`, `render.cpp:58`; `wrap_words`, `render.cpp:69`) and stacks
lines at an ad-hoc `title_size * 1.12` (`render.cpp:170`). The source instead
derives a **font-metric-exact** bounding box from `TEXTMETRIC`, which every
balloon/label geometry above depends on. The gap:

- **`CFontInfo` metrics.** Built from `GetTextMetrics` (`balloon.cpp:606`):
  `m_lineHeight = tm.tmHeight + m_leading` (`balloon.cpp:640`),
  `m_leading`/`m_baseAdd` are per-font kern offsets (`fonts.cpp:89-91,135,137`;
  e.g. title `(-220*reduction, 120*reduction)`, shout `(0,0)`),
  `m_topOffset = FAREAST_TOPOFFSET(50)` or 0 (`balloon.cpp:635-638`),
  `m_continuationWidth` = width of the wrap-continuation glyph
  (`balloon.cpp:642,643`).
- **Bbox from line count.** `CLabel::GetBBox` (`balloon.cpp:1170`) runs
  `BreakIntoLines` (`balloon.cpp:685`) then sets
  `bbox.Bottom = bbox.Top - nLines*m_lineHeight - m_baseAdd`
  (`balloon.cpp:711`) and centers/left-justifies width around the widest line
  (`balloon.cpp:700-709`). The **whole cloud spline geometry** (`PermuteFilters`,
  `balloon.cpp:539-543,559`) is expressed in `m_lineHeight`, `m_baseAdd`,
  `m_topOffset` — so parity here is a precondition for pixel-accurate balloons,
  not a cosmetic nicety.
- **Line breaking + width.** `::BreakIntoLines`/`GetFormattedTextExtent`
  (`balloon.cpp:691,726`) and `WidestWord` (`balloon.cpp:736`) measure real
  device text extents; `AreaEstimate = 1.3 * cx * (cy + lineHeight)`
  (`balloon.cpp:732`).

### 2.4.b Port mapping to FreeType/HarfBuzz

- Read FreeType metrics per face/size: `FT_Size_Metrics.height` (26.6 fixed) is
  the analogue of `tm.tmHeight`; ascender/descender give the `m_leading`/
  `m_baseAdd` decomposition. Build a portable `FontInfo { line_height,
  base_add, top_offset, continuation_width }` populated once per (face,size)
  from `TextEngine::native_face()` (`text.hpp:36`).
- Replace the ad-hoc `title_size*1.12` line stacking (`render.cpp:170`) with
  `line_height`; replace advance-sum wrapping with a `BreakIntoLines` that also
  records per-line width (feeds Item 2.1's `GetFilters`).
- Compute label bbox exactly as `balloon.cpp:711`:
  `bottom = top - nLines*line_height - base_add`, width from the widest shaped
  line.
- **`CStarLabel` ellipsis** (`balloon.cpp:1184`): the roster label draws single
  line with `DT_END_ELLIPSIS` (`balloon.cpp:1197`). The portable title panel
  already ellipsizes conceptually; make it exact by measuring against the column
  width `maxWidth` (`panel.cpp:1425-1428`) and truncating with a middle/right
  ellipsis at `line_height`.

### 2.4.c Honest limits

`tm.tmHeight`/`tmExternalLeading` come from the GDI font mapper for the specific
installed comic face; the portable build ships Comic Neue
(`portable/assets/fonts`, cf. `render.cpp` font lookup via
`find_portable_comic_font`, `text.hpp:25`). Metrics will therefore be *self-
consistent and deterministic* but **not identical to Win32 GDI** unless the same
TTF and rasterizer are used. Gate 2.4 on *internal* determinism + the
line-count/bottom formula (Linux), and mark absolute-pixel parity vs. GDI as
MSVC-CI/visual-only. This is why ROADMAP already marks 2.4 as 🟡 partial rather
than ⬜.

---

## Verification strategy (per item) — Linux headless

All render items follow the two existing gates:

- **Reference-bitmap oracle** like `source_raster_test.cpp`
  (`portable/tests/source_raster_test.cpp`): decode Microsoft's own released
  assets in `portable/assets/source-raster/` and assert exact pixel/opaque-count
  invariants (e.g. `alpha_count`, `cell_alpha_count`,
  `source_raster_test.cpp:18,30,75,144-177`). This is the strongest gate because
  the expectation is Microsoft's own bytes.
- **Golden PNG** via the deterministic headless path:
  `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build/comic-chat --frames 2
  --png out.png` (`portable/README.md:50-53`), compared to a committed golden.
  Cairo already writes PNGs (`Canvas::write_png`, `render.cpp:213`).

Per item:

- **2.1 Balloon geometry.** (a) *Determinism test (pure, cheap):* seed the
  portable LCG, run `render_panel` twice, assert byte-identical frames; assert a
  captured `m_seed` yields the documented `randfloat()` call sequence. (b)
  *Geometry unit tests (pure):* assert `GetCloudEstimate` goal-width bounds
  (`panel.cpp:896-913`), `DockAtTop` (`Top == height + TOPBORDER`,
  `balloon.cpp:1310`), route-region rejection (`panel.cpp:944`), spline
  control-point coordinates from `PermuteFilters`/`AddWavies`
  (`balloon.cpp:531,563`) against hand-computed values — no raster needed. (c)
  *Golden PNG* for say/whisper/think/box shapes at fixed seed + fixed text.
  Verifiable on Linux to *self-consistency*; exact GDI-vs-Cairo cloud pixels are
  visual/MSVC-only (mark as such) unless the `CBeta` spline is ported bit-exact.

- **2.2 CBodyUnary compositing.** *Strongest:* an `avatar_assets` oracle test
  (an "avatar-oracle" harness already exists:
  `portable/build/comicchat-avatar-oracle`) that decodes a released `.avb`
  (e.g. `xeno.avb` in the Zig tree's testdata, or a bundled portable copy),
  renders `legacy_exact`, and asserts opaque-pixel counts per pose — same shape
  as `source_raster_test.cpp`'s `cell_alpha_count`. Add head+torso composite
  assertions (union-box dims from `bodycam.cpp:634-641`, `TORSOFIRST` ordering).
  *Unit:* `GetDimInfo`/`GetBodyBox` geometry against hand-computed rects
  (`bodycam.cpp:657-670`). Highly Linux-verifiable — the compositing math and
  the decoded bitmaps are both deterministic.

- **2.3 Expert placement.** Pure integer logic ⇒ fully Linux-testable, no
  raster. Table-driven tests over `order_conversation`: talk-to scenarios
  (`EvalPair` facing rewards/penalties, `panel.cpp:296-309`), historesis
  stability across two panels (`panel.cpp:442-448`), the ≥5 / speaker-already-
  present / <2-panel split predicate (`panel.cpp:1082`), and the roster order
  already covered by the existing `order_stars` tests (`layout.cpp:31`). This is
  the **most verifiable** item.

- **2.4 CLabel font bbox.** *Pure unit tests:* `FontInfo` line_height/base_add
  derivation is deterministic per bundled TTF; assert
  `bottom == top - nLines*line_height - base_add` (`balloon.cpp:711`), widest-
  line width, and `CStarLabel` ellipsis truncation width. *Golden PNG* for the
  title panel confirms integration. Internal determinism is Linux-gated;
  absolute GDI parity is MSVC/visual-only (be explicit in the test name).

---

## Recommended Phase 2 implementation order

Prioritized by **value × Linux-verifiability**. Do the pure-logic, fully-gated
items first so later raster work has correct geometry to consume.

1. **0. Logical-coordinate + seeded-PRNG foundation** (§0, §2.1.a). Small,
   unblocks everything, byte-exact testable. Port the MSVCRT LCG and the
   Y-up/twips panel-local drawing pass.
2. **2.3b Conversation placement + panel-split predicate** (§2.3.b). Pure
   integer logic, *most verifiable*, no raster; and Item 2.1 needs correct
   `m_arrowX`/body order to point tails. `order_stars` half is already done.
3. **2.4 CLabel font bbox** (§2.4). Pure metric logic, gates balloon geometry
   (`PermuteFilters` is expressed in `line_height`/`base_add`). Linux-gate the
   formula; flag GDI parity as MSVC-only.
4. **2.2 CBodyUnary/CBodyDouble compositing** (§2.2). Decode already exists;
   compositing is deterministic and strongly gated by an `.avb` opaque-count
   oracle (reuse the avatar-oracle harness). Replaces the placeholder circles in
   the title/roster panel.
5. **2.1 Balloon geometry** (§2.1) last: it consumes the arrowX (from 2.2/2.3)
   and the exact label bbox (from 2.4). Ship the deterministic-layout + pure
   geometry unit tests as the hard gate; ship say/whisper/think/box golden PNGs
   as the visual gate, honestly labeling cloud-spline pixel parity as
   MSVC/visual-only unless `CBeta` is ported bit-exact.

Honesty summary of what is *not* fully Linux-provable: exact GDI-vs-Cairo pixel
parity for balloon cloud splines (2.1) and absolute font-pixel parity vs. the
Win32 font mapper (2.4). Everything else — determinism, placement logic,
compositing opaque-counts, bbox arithmetic, coordinate math — is gated the same
way as the existing 11/11 suite.
