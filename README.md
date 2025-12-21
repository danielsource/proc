proc (work-in-progress)
====


```make
# This language is like a very dumb C (only int, if, else, while, proc...)
proc main(argc, argv) {
    int memcap = MAX_FIB + 1; # The only data type is signed 64-bit integer
    int memfib[memcap];       # Variables are zeroed out by default

    if argc != 2 {
        return 1;
    }

    # There aren't character strings, so argv[] is wasteful,
    # since each "string" contains each character in 8 bytes (instead of 1).

    # Print a Fibonacci sequence number
    PutInt(fibonacci(StrToInt(argv[1]), memfib));

    # Print memfib[]
    int i = 2;
    if memfib[i] { PutChar('\n'); }
    while memfib[i] && i < memcap {
        # Unspecified arguments in call are zero
        PutInt(i, 1);  # Set 1 to not print newline
        PutChar(' '); PutChar(':'); PutChar(' ');
        PutInt(memfib[i]);
        i += 1;
    }
    # In the absence of the return statement, "return 0;" is implied
}

int MAX_FIB = 92;

proc fibonacci(n, mem) { # Arguments are passed by value (mem is a pointer)
    Assert(n <= MAX_FIB);
    if n < 2 {
        return n;
    }
    if !mem[n] {
        # Operator precedence is similar to C's
        mem[n] = fibonacci(n - 1, mem) +
                 fibonacci(n - 2, mem);
    }
    return mem[n];
}

# Builtins:
#   x = StrToInt(digits)        (exits on error)
#   err = PutInt(i, no_newline) (-1 on error)
#   err = PutHex(i, no_newline) (-1 on error)
#   err = PutChar(c)            (-1 on error)
#   c = GetChar()               (-1 on error)
#   x = Rand(seed)
#   Exit(code)
#   Assert(expression)

# There is no fancy features, no record/classes, no bound checking, no strings.
# This is pretty much useless, just for fun!
```


---

My attempt to make a [toy programming language](https://en.wikipedia.org/wiki/Esoteric_programming_language).
Inspired by [_B_](https://web.archive.org/web/20240425202455/https://www.bell-labs.com/usr/dmr/www/kbman.html).

`proc.c` contains the interpreter (parser + evaluator) which is the language specification itself.

`examples` directory contains examples of programs in _proc_.


# Usage

```
General: proc  FILE-TO-BE-INTERPRETED  [ARGUMENTS-PASSED-TO-FILE...]

Example: proc  ./addnums.proc  1 2 3
```


# How to build (unix)

`make`

or just

`cc -o proc proc.c`


# How to build (windows with [TCC](https://en.wikipedia.org/wiki/Tiny_C_Compiler))

`tcc -o proc.exe proc.c`
