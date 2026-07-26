# pefiaOS devlog

---

# Browser engine rewrite, audited bug fixes, and a build that works anywhere

Date: 2026-07-26

Two things drove this pass: "tell me 20 things to improve", and "make the web
browser way better - real search, input boxes, video, a full JS and CSS
engine". The first was answered by auditing the tree with a fleet of readers
and verifying every claim against the source before acting on it. The second
is most of the diff.

Everything below **builds warning-free and was booted in QEMU**, and the
browser work was driven through the QEMU monitor (scripted mouse and
keystrokes) with screenshots read back at each step, so the claims here are
things that were seen on screen, not things that ought to work.

## Answering the Mac VM question first

Someone booted this in a Mac VM configured as *"Intel ICH9 based PC (2009,
x86_64)"* and it did not come up. **That configuration is not the mistake.**
q35/ICH9 is fine, and picking an x86_64 VM is fine too - a 64-bit VM CPU runs
a 32-bit kernel without complaint. The setting that actually matters is
**BIOS vs UEFI firmware**, and the details are under "VM compatibility"
below, along with what now happens instead of a black screen.

## How it was verified

- `make` -> zero warnings, ISO produced.
- Booted headless, screenshotted: desktop, taskbar, browser, boot self-test
  green (`interp 84/2=42 OK  VFS rw OK  thr=2  tk=212  disk[0..3]=PEFI`).
- Scripted the mouse over the monitor socket to click through
  `about:test-engine`, `about:test-js`, `about:test-forms`, and typed a live
  search URL into the address bar.
- Four real bugs were found *by* that loop and fixed (see "Found while
  testing" below). Two of them would have been invisible to code review.

---

## The audit: what a fleet of readers found

Five readers mapped the subsystems (browser, network, core kernel, UI, apps),
six finders swept for defects across different dimensions, and three
adversarial verifiers re-opened the cited files and rejected the claims that
did not survive. 44 improvements came out confirmed. The ones fixed in this
pass are marked DONE; the rest are the honest backlog.

**Correctness / crash-class**

1. DONE - `dechunk()` chunk-size overflow. The hex size accumulator took
   unlimited digits from the server, so `7FFFFFFF` overflowed `int`, made
   `in + size > len` go negative, skipped the clamp, and copied ~2 GB past a
   256 KB static buffer. Any HTTP server could trigger it. Now the
   accumulator stops growing past the buffer length and the clamp is
   `size > len - in` (which cannot overflow).
2. DONE - 16bpp framebuffer wrote 3 bytes per 2-byte pixel: every row
   corrupted, and the last row overran the `kmalloc`'d back buffer into the
   next heap header. Added the missing 2-byte branch to `fb_put_pixel`,
   `fb_get_pixel`, `fb_fill_rect`, `fb_blit`.
3. DONE - `kfree()` validated nothing. Any stale pointer flipped an arbitrary
   word to "free" and the coalescing pass then merged garbage into the free
   list - corruption that surfaces later as an unrelated crash. Header now
   carries a magic word; `kfree` range-checks the pointer, then no-ops on bad
   magic or double free.
4. DONE - `rtc_time()` had no update-in-progress guard. Worse than a
   flickering clock: `clock_init` calibrates the TSC by spinning on RTC
   seconds, so one torn read permanently skewed every timeout in the OS.
   Now waits out UIP and re-reads until two passes agree.
5. DONE - `rtl8139_send` never checked TX-slot completion, so the fifth
   in-flight frame clobbered a descriptor the NIC was still DMA-ing. Now
   bounded-spins on the OWN bit and returns an error instead, letting TCP
   retransmit handle it as ordinary packet loss.
6. DONE - the HTML tag scanner ended a tag at the first `>`, even inside a
   quoted attribute. `<div title="a > b">` leaked its tail into the page as
   text and stored a truncated attribute. The scan is quote-aware now.
7. DONE - Notepad silently truncated files over 8 KB at open, and saving wrote
   the truncated buffer back - permanent data loss on any large file. Load
   now records truncation and save refuses with
   `file too large - opened read-only`.
8. DONE - `sched_spawn` leaked an 8 KiB stack every time it reused a dead
   slot, and its capacity gate counted threads that had already exited, so
   spawning eventually bricked itself. Dead slots reuse their stack; the gate
   is the free-slot scan.

**Browser / web compatibility** (all DONE, detailed below)

9. Hidden inputs and `value=` defaults were dropped from every submission.
10. `method=post` silently fell back to GET.
11. No checkbox/radio state, no select, no password masking.
12. JS had no arrays, no object literals, no closures.
13. 256-byte URL ceiling - shorter than a real search result link.
14. Every navigation refetched every image over TLS (no cache across pages).
15. The CSS cascade re-scanned the whole rule list per applied declaration.
16. Animated GIFs decoded frame 0 and stopped; `<video>` laid out as nothing.

**Input and paint** (all DONE)

17. Mouse wheel: the PS/2 init never asked for the IntelliMouse extension, so
    there was no scroll wheel at all. The knock is in, 4-byte packets decode a
    signed Z delta, and the WM routes notches to the window under the cursor.
18. Arrow keys were dropped by the scancode decoder. They now come through as
    distinct codes (0x81-0x84, outside printable ASCII so every existing
    `c >= 32` filter ignores them), and the terminal recalls history with
    Up/Down.
19. The offscreen flip copied the whole 8.3 MB surface on **every** repaint -
    every keystroke, every mouse move, every animation frame. The framebuffer
    now tracks a dirty rectangle across all four writers and copies only that.

**Still open** (verified real, not attempted here): certificate verification
(TLS currently trusts any server cert), HTTP keep-alive, DNS caching and
NXDOMAIN fast-fail, timer-driven preemption, disk-backed VFS persistence,
UTF-8 text decoding, JS statement-level error recovery, an AC97 audio driver,
clipboard, bookmarks, screenshots.

---

## The browser

### CSS engine

`kernel/css.c` went from "type/class/id plus descendant" to something that
survives real stylesheets.

- **Real combinators**: child (`>`), adjacent sibling (`+`), general sibling
  (`~`), and descendant, split right-to-left with the combinator that
  precedes each compound.
- **Attribute selectors**: `[attr]`, `=`, `~=`, `^=`, `$=`, `*=`, `|=`.
- **Structural pseudo-classes**: `:first-child`, `:last-child`,
  `:only-child`, `:root`, `:empty`, `:checked`, `:disabled`, `:link`, and the
  common `:nth-child(odd|even|N)`. The interaction ones (`:hover`, `:focus`)
  deliberately never match - nothing tracks pointer state, and matching them
  unconditionally would paint every link in its hover style.
- **At-rules are structural now**: `@media` and `@supports` recurse into
  their block, and a print-only block is skipped instead of leaking print
  styles into the page. `@keyframes` and `@font-face` are skipped whole
  rather than having their inner declarations escape as page rules.
- **Box model**: 1-to-4 value `margin`/`padding` shorthands, borders (drawn),
  `width`, `line-height`, `text-transform`, `visibility`, `line-through`,
  and colors out of shorthands like `background: #fff url(x) no-repeat`.
- **Colors**: `rgb()`/`rgba()` (alpha 0 reads as "no color" instead of
  painting black over content), `hsl()`, and ~80 named colors.
- **Presentational attributes**: `bgcolor`, `color`, `align`, `hidden` -
  still all over the real web.
- **The cascade stopped being quadratic.** It used to rescan every rule to
  find the next one in (specificity, order) *per applied declaration*, per
  node. Rules are now sorted once per document into an index and the cascade
  is a single ordered pass.

### JavaScript engine

`kernel/js.c` was an expression evaluator with `if`/`while`/`for` and no data
structures. It now runs the kind of script server-rendered pages actually
ship:

- **Arrays and objects** with real property storage out of a shared pool -
  indexing, `length`, and `push/pop/shift/join/indexOf/includes/slice/concat/
  reverse/forEach/map/filter/find`.
- **Closures**: functions capture their defining environment, so the
  counter-factory idiom works. Verified on screen (`called 3x -> 3`).
- **Arrow functions**, both `x => expr` and `(a, b) => { ... }`.
- **`do/while`, `switch/case/default`, `for-in`, `for-of`, `try/catch/
  finally`, `throw`, `new`**, and the `??`/`?.` operators.
- **String methods**: `split`, `replace`/`replaceAll`, `includes`,
  `startsWith`, `endsWith`, `trim`, `repeat`, `concat`, `charCodeAt`,
  `lastIndexOf`, `localeCompare`, plus indexing.
- **DOM**: `querySelector`/`querySelectorAll`, `getElementsByTagName`/
  `ClassName` (returning real arrays), `classList` add/remove/toggle/contains,
  `children`, `createElement`/`createTextNode`, `appendChild`,
  `removeChild`/`remove`, attribute methods, `document.title`.
- **Events**: `addEventListener('click')`, `element.onclick`, and inline
  `onclick=""` attributes, dispatched with real bubbling up the ancestor
  chain. A click on the page goes to script before it goes to link
  navigation, the same order a real browser uses.
- **Timers**: `setTimeout`/`setInterval`/`clearTimeout`/`clearInterval` and
  `requestAnimationFrame`, pumped once per frame from the WM loop.
- **`location`** (href/host/pathname/search/hash, assignment navigates),
  `window.open`, `alert` (shown in the status bar), `JSON.stringify`,
  `Object.keys/values`, `Array.isArray`, `Date.now`, `encodeURIComponent`.
- Bounded exactly as before, plus a new **parser depth cap** - deeply nested
  source used to eat the kernel stack before a single statement ran.

The one structural change behind all of this: **the AST and DOM now persist
per document, not per script run.** A click handler registered by one
`<script>` holds pointers into both; the old code reset the AST arena on every
`js_run` and recycled the DOM node pool on every relayout, so any handler that
outlived its own script was pointing at recycled memory. Parsing and flowing
are separate stages now - a window resize re-flows without re-parsing.

### Form controls

Text boxes were already editable. Now the rest of a form works:

- **Password** fields mask with `*`.
- **Checkboxes and radios** hold state, draw it, and toggle on click; radios
  are exclusive within (form, name).
- **Selects** render the current option with a dropdown marker and cycle
  through options on click (no popup layer to hang a real menu on).
- **Hidden inputs** are recorded and submitted without being drawn.
- **`value=` seeds the field** on first layout, and only the first - later
  relayouts must not stomp what the user typed.
- **POST works.** `html_field_submit` returns GET-with-URL or
  POST-with-body, and `net_fetch_post` sends it with the right headers.
  Redirect semantics follow the spec: 301/302/303 after a POST continue as
  GET, 307/308 re-POST.
- Serialization follows the real rules: unchecked boxes are omitted, only the
  clicked submit button contributes its name, unnamed controls are skipped.

### Images, animation, video

- **Animated GIFs play.** The decoder was refactored into a frame loop with a
  persistent canvas: per-frame delays, transparent-index compositing, and
  frame disposal. The renderer keeps every scaled frame and picks one from the
  wall clock. Memory-guarded: a large animation degrades to a still rather
  than allocating 16 scaled copies.
- **`<video>` and `<audio>`** render a labelled placeholder box (with the
  poster image when there is one) that navigates to the media URL on click.
  There is no codec; laying out as nothing was the bug.
- The image cache now survives relayout, and images are keyed by URL.

### Network

- **Cookies.** A session jar parses every `Set-Cookie`, stores by host and
  name, and sends a `Cookie` header on matching requests, with `www.` stripped
  so a cookie set for `example.com` reaches `www.example.com`.
- **POST** support end to end.
- **User-Agent** is now `Mozilla/5.0 (compatible; pefiaOS/1.0)` plus an
  `Accept-Language`, which is what gets servers to serve their simple HTML.
- Buffers raised: 640 KB raw, 1 MB decompressed, 768 KB page, 512-byte URLs
  (a real search result link overflows 256 easily).
- `<meta http-equiv="refresh">` is followed - but never one inside
  `<noscript>` (see below).

---

## Found while testing

Three bugs that only a running system could show:

1. **Clicking `about:test-engine` navigated to `about:home/about:test-engine`.**
   The renderer resolves every href against the base URL at layout time, and
   the browser was resolving the already-resolved result a second time. The
   fix deleted a function rather than adding one.
2. **`setInterval` fired but nothing appeared.** The WM frame loop only
   repainted in response to input, so a timer that updated the DOM (or an
   animated GIF advancing a frame) changed the page invisibly. `browser_tick`
   now reports whether anything changed and the loop repaints on its behalf,
   with animation paced to ~25 fps so it does not repaint the desktop as fast
   as the loop spins.
3. **A fetched page bounced itself to "turn on JavaScript".** Script-heavy
   sites carry a `<meta http-equiv="refresh">` *inside `<noscript>`* as the
   fallback for browsers that run no scripts at all. We honored it and threw
   away content we had successfully fetched. Refreshes inside `<noscript>`
   are now ignored, and `<noscript>` content is not rendered - we do run
   scripts.

4. **A centered search box was drawn to the left of its own placeholder
   text, overlapping the Search button.** `align_line` shifted the text spans
   of a centered line but not the boxes of the form controls on it, so the
   chrome stayed at the left margin while the labels moved. The aligner now
   moves the control rects and their hit-test boxes with the line; block
   backgrounds are flagged separately since they already span the full
   content width.

5. **A real search page rendered at double size with 85px line spacing.**
   DuckDuckGo's stylesheet says `th,td{font-size:107.1%}` and
   `.result-snippet{font-size:77.4%}`. `parse_length_px` read "107.1%" as
   107 **pixels**, which crossed the 22px threshold into the 2x glyph scale
   and set a line height to match. Percentages are now resolved against the
   inherited size, and both font-size and line-height clamp to a range that
   a misread value cannot escape.

### Which search engines can render here, and why

The big search engines answer a simple browser with a page whose `<body>` is
a `<noscript>` fallback plus tens of kilobytes of compiled JavaScript that
builds every result at runtime. The fetch succeeds - DNS, TLS 1.3, HTTP 200,
90 KB - and there is still nothing to lay out. No engine renders that without
executing the site's own bundle.

Rather than guess, this was measured. A small host harness links the real
`domparse.c` / `domrt.c` / `css.c` and reports what layout would receive from
a saved live response:

| response | visible text nodes |
| --- | --- |
| script-built search page | **0** |
| DuckDuckGo Lite | **106** |
| Bing | **101** |

So the start page is built around **DuckDuckGo's lite endpoint**, which is
server-rendered. `about:home` is now a centered new-tab page: a search box
that submits straight to it, a row of one-click queries, a short list of
sites that render well here, and the engine test pages.

Verified the whole path from inside the guest, by screenshot rather than by
hope: click the box, type `osdev`, press Enter, and the results come back -
the zero-click info panel, four numbered results with snippets and their
URLs, correct wrapping.

---

## Build system

The i686-elf cross toolchain was not on this machine and building it from
source costs about half an hour, so the Makefile now **falls back to the host
gcc in 32-bit mode** (`-m32 -fno-pie -fno-stack-protector`) when
`i686-elf-gcc` is absent. It emits the same ELF32 objects GRUB wants.

A host gcc without `gcc-multilib` has no 32-bit `libgcc` to link against, so
`kernel/intdiv.c` supplies the 64-bit division helpers the compiler expects
(`__udivdi3`, `__umoddi3`, `__divdi3`, `__moddi3`, `__udivmoddi4`). The cross
toolchain still wins when present - the linker only pulls an archive member
for a symbol that is still undefined.

## VM compatibility

Someone tried to boot this in a Mac VM set to *"Intel ICH9 based PC (2009,
x86_64)"*, which is QEMU's `q35` machine type.

**That choice is fine, and so is the 64-bit CPU.** The kernel is 32-bit but
runs on any x86_64 host CPU, and q35 works. What actually breaks a boot is
**UEFI firmware**: `grub-mkrescue` only puts an EFI path on the image when
GRUB's EFI modules are installed, and on this build host only `i386-pc` is
present, so the ISO is BIOS/CSM only. Under UEFI it does not boot at all -
and it failed *silently*, because a kernel that halts looks exactly like a
kernel that crashed.

- `kernel_main` now paints a **VGA text-mode diagnostic** when `fb_init`
  fails, naming UEFI as the likely cause and saying q35/ICH9 and a 64-bit VM
  CPU are both fine. Text mode is a safe assumption there precisely because
  graphics mode is what failed.
- The build **warns when it produced a BIOS-only ISO**, instead of leaving
  that to be discovered by a black screen.
- `make run-q35` reproduces the reported setup (q35 + e1000 + AHCI-attached
  disk) so it can be tested directly. Everything comes up there; the boot
  self-test reports `disk=none`, because q35 has no legacy IDE ports and the
  ATA PIO driver only speaks to those.

## Comments

Every comment shorter than six words was removed, along with every
top-of-file banner comment, across all 93 kernel sources plus the NASM files.
The vendored `PureDOOM.h` and the generated `font8x16.h` were left alone.

The pass was done with a scanner that tracks string and char literal state,
because a regex would have eaten the `//` in every `"http://..."` in the tree
(and there are plenty). It was rehearsed on a scratch copy first and
compile-checked there before being run on the repo. That rehearsal caught one
real consequence: `json_out` in `js.c` relied on a `/* fallthrough */` comment
to silence `-Wimplicit-fallthrough`, so removing comments made the build
noisy. Fixed properly in code (the case returns through a helper) rather than
by putting the comment back.

After the strip: `build.bat` builds clean, zero warnings, and the browser test
pages render pixel-identically to the run before it.

## What this pass did not fix

Named plainly, because a screenshot will show them:

- **Non-ASCII text renders as `?`.** The font is an 8x16 ASCII cell and text
  emission is byte-oriented, so a UTF-8 multibyte sequence becomes one `?`
  per byte. Visible on any real page with an arrow or an emoji. The fix is
  UTF-8 decoding plus a small mapping to ASCII lookalikes.
- **TLS does not verify certificates.** The handshake works and the traffic
  is encrypted, but nothing checks that the server is who it claims to be.
- **No HTTP keep-alive**, so every image and stylesheet on a page costs a
  fresh TCP+TLS handshake.
- **Scheduling is still cooperative**, the VFS is still RAM-only, and there
  is still no paging or ring 3.

## Build & run

```
build.bat       # Windows: runs make inside WSL
make            # ISO; uses i686-elf-gcc if present, else host gcc -m32
make run        # QEMU, i440FX + RTL8139 + IDE disk
make run-q35    # QEMU, q35/ICH9 + e1000 (the Mac VM setup)
```
