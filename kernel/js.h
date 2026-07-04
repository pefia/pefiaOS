/* kernel/js.h
 *
 * Tree-walking JS interpreter, subset-of-a-subset, wired up against the DOM.
 * Ints only (no floats), var/let/const, if/while/for, plain functions, and
 * enough of document/element/Math/console to make onclick handlers and
 * <script> blocks from ordinary pages do something. No arrays, no objects
 * literals, no closures over anything but the global scope worth speaking of.
 * It won't run React. It'll run a page that does document.write and pokes at
 * a few elements, which covers most of what shows up in the wild.
 *
 * Bounded on purpose: a step counter kills runaway loops and a recursion
 * cap kills stack-eating recursion, so a bad script degrades to "nothing
 * happened" instead of hanging or crashing the OS.
 */
#ifndef PEFIA_JS_H
#define PEFIA_JS_H

/* Wipe interpreter state - string arena, env pool, globals. Call once per
 * document, after the DOM exists and before running any of its scripts. */
void js_engine_reset(void);

/* Parse + run one script against whatever DOM is currently loaded. Returns 1
 * if it touched the DOM (caller should re-layout), 0 if not. Bad input just
 * aborts the script - never brings the kernel down with it. */
int  js_run(const char *src, int len);

#endif /* PEFIA_JS_H */
