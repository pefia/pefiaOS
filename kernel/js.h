#ifndef PEFIA_JS_H
#define PEFIA_JS_H

#include "domrt.h"

/* Wipe interpreter state - string arena, AST, envs, objects, handlers,
 * timers. Call once per document, after the DOM exists and before running
 * any of its scripts. */
void js_engine_reset(void);

/* Parse + run one script against whatever DOM is currently loaded. Returns 1
 * if it touched the DOM (caller should re-layout), 0 if not. Bad input just
 * aborts the script - never brings the kernel down with it. */
int  js_run(const char *src, int len);

/* Tell the engine the document's URL, so `location` has something to say. */
void js_set_location(const char *url);

/* Fire 'load'/'DOMContentLoaded' handlers (registered via addEventListener
 * or window.onload / <body onload>). Returns 1 if the DOM got dirty. */
int  js_fire_load(void);

/* Run click handlers for a node, bubbling up through its ancestors
 * (addEventListener('click'), element.onclick, and onclick="" attributes).
 * Returns 1 if the DOM got dirty. */
int  js_click(DomNode *n);

/* Does this node (or an ancestor) have any click handler? Cheap hit-test
 * helper so the renderer can decide whether a click is "interactive". */
int  js_has_click_handler(const DomNode *n);

/* Run timers that have come due (setTimeout/setInterval). Call once per
 * frame. Returns 1 if the DOM got dirty. */
int  js_pump(void);

/* One-shot reads of script side effects. Each returns 1 and fills out if
 * something is pending, clearing it; 0 otherwise. Nav = location.href
 * assignments / window.open; alert = alert() text for the status bar. */
int  js_take_nav(char *out, int cap);
int  js_take_alert(char *out, int cap);

/* Queue a navigation from outside the interpreter (the renderer uses this
 * for <meta http-equiv="refresh">) so it leaves through the same channel. */
void js_request_nav(const char *url);

#endif
