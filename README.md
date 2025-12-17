# proc (work-in-progress)

```
#!/usr/bin/env proc

proc main(argc, argv) {
    # all variables are C `int64_t` integers
    int memo[92]; # zeroed out by default
    if (argc != 2) {
        return 1;
    }
    PutInt(fibonacci(StrToInt(argv[1]), memo));
    PutChar(10); # newline
    # in absence of the return statement, "return 0;" is implied for all procedures
}

proc fibonacci(n, memo) { # arguments are passed by value (memo is a pointer)
    if n <= 1 {
        return n;
    }
    if (memo[n]) {
        return memo[n];
    }
    # operator precedence is similar to C's
    memo[n] = fibonacci(n - 1, memo) + fibonacci(n - 2, memo);
    return memo[n];
}
```

My attempt to make a [toy programming language](https://en.wikipedia.org/wiki/Esoteric_programming_language).
Inspired by [_B_](https://web.archive.org/web/20240425202455/https://www.bell-labs.com/usr/dmr/www/kbman.html).

`proc.c` contains (will contain) the interpreter (parser + evaluator) which is the language specification itself.

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
