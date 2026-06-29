/* kernel/js.h
 * -----------------------------------------------------------------------------
 * A small tree-walking JavaScript interpreter that runs against the live DOM.
 * It is deliberately a subset: numbers are 32-bit integers, and it supports
 * variables, arithmetic/string/logical expressions, if/while/for, functions,
 * and a useful slice of the DOM/host API (document.getElementById, innerHTML,
 * textContent, getAttribute/setAttribute, element.style.*, document.write,
 * console.log, Math.*, parseInt, ...). It will NOT run a modern web app, but it
 * runs hand-written and lightly-generated scripts. Everything is bounded: a
 * step budget stops runaway loops and any error aborts the one script safely.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_JS_H
#define PEFIA_JS_H

/* Clear all interpreter state (globals, pools). Call once per document, after
 * the DOM is built and before running its scripts. */
void js_engine_reset(void);

/* Execute one script against the current DOM. Returns 1 if the DOM may have
 * been mutated (caller should re-layout), 0 otherwise. Never crashes the OS:
 * syntax errors, unsupported features and runaway loops all abort cleanly. */
int  js_run(const char *src, int len);

#endif /* PEFIA_JS_H */
