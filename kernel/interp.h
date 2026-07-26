#ifndef PEFIA_INTERP_H
#define PEFIA_INTERP_H

#include "wm.h"

#define INTERP_C  0
#define INTERP_PY 1

void *interp_new(int lang);
void  interp_paint(Window *w, int cx, int cy, int cw, int ch);
void  interp_key(Window *w, char c);

/* Evaluate a bare integer expression (no variables/assignment). Sets *ok to 1
 * on success, 0 on a parse/eval error. Used by the boot self-test. */
long  interp_eval(const char *s, int *ok);

/* Append + execute a chunk of Python source in an interp window's context.
 * Used by the terminal's `py <file>` command. */
void  interp_run_source(void *state, const char *src);

#endif
