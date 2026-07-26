#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "interp.c"

/* vfs stubs: one fake file for run() */
static VNode fake = { "lib.py", 0, 0, "def double(n):\n    return n * 2\n", 0 };
int vfs_count(void) { return 1; }
const VNode *vfs_node(int i) { return i == 0 ? &fake : 0; }

static Repl *R;

static const char *last(int back)
{
    assert(R->count > back);
    return R->lines[R->count - 1 - back];
}

static void run(const char *src) { run_source(R, src, 1); }

int main(void)
{
    R = interp_new(INTERP_PY);
    assert(R);

    run("x = 6 * 7");
    run("print(x)");
    assert(!strcmp(last(0), "42"));

    run("print(2 + 3 * 4, 10 // 3, 10 % 3, -5)");
    assert(!strcmp(last(0), "14 3 1 -5"));

    run("s = \"ab\" + 'cd'");
    run("print(s, len(s), s[1], s[-1])");
    assert(!strcmp(last(0), "abcd 4 b d"));

    run("print(str(12) + '!')");
    assert(!strcmp(last(0), "12!"));
    run("print(int('  -37') + 1)");
    assert(!strcmp(last(0), "-36"));
    run("print(chr(ord('A') + 1))");
    assert(!strcmp(last(0), "B"));

    run("1 < 2");
    assert(!strcmp(last(0), "1"));
    run("'hi'");
    assert(!strcmp(last(0), "'hi'"));

    run("x = 15\nif x > 20:\n    print('big')\nelif x > 10:\n    print('mid')\nelse:\n    print('small')");
    assert(!strcmp(last(0), "mid"));

    /* while with break/continue, augmented assignment */
    run("total = 0\ni = 0\nwhile True:\n    i += 1\n    if i > 10:\n        break\n    if i % 2 == 0:\n        continue\n    total += i");
    run("print(total)");
    assert(!strcmp(last(0), "25"));

    run("acc = ''\nfor i in range(3):\n    acc = acc + str(i)\nfor i in range(5, 7):\n    acc = acc + str(i)\nfor i in range(9, 3, -3):\n    acc = acc + str(i)");
    run("print(acc)");
    assert(!strcmp(last(0), "0125696"));

    /* functions + recursion, locals don't clobber globals */
    run("def fib(n):\n    if n < 2:\n        return n\n    return fib(n-1) + fib(n-2)");
    run("n = 999");
    run("print(fib(10))");
    assert(!strcmp(last(0), "55"));
    run("print(n)");
    assert(!strcmp(last(0), "999"));

    /* short-circuit really skips the dead arm (no div-by-zero, no call) */
    run("d = 0");
    run("print(d != 0 and 10 / d > 0)");
    assert(!strcmp(last(0), "0"));
    run("print(d == 0 or fib(-1000000) == 1)");
    assert(!strcmp(last(0), "1"));

    run("print(not 0, not 'x', 3 >= 3)");
    assert(!strcmp(last(0), "1 0 1"));

    run("print(1 / 0)");
    assert(strstr(last(0), "division by zero"));
    run("nope");
    assert(strstr(last(0), "undefined"));
    run("while True:\n    pass");
    assert(strstr(last(0), "too many steps"));

    /* run("file") pulls a def out of the fake VFS */
    run("run('lib.py')");
    run("print(double(21))");
    assert(!strcmp(last(0), "42"));

    /* interp_eval kept for the boot self-test */
    int ok = 0;
    assert(interp_eval("84/2", &ok) == 42 && ok);
    assert(interp_eval("(2+3)*4-8", &ok) == 12 && ok);
    assert(interp_eval("100%7", &ok) == 2 && ok);
    interp_eval("1 +", &ok);
    assert(!ok);

    printf("all interp tests passed\n");
    return 0;
}
