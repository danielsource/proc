# proc (WIP)

---
```
#!/usr/bin/env proc

proc fibonacci(n, memo) {
    if n <= 1 {
        return n;
    }
    if (memo[n]) {
        return memo[n];
    }
    memo[n] = fibonacci(n - 1, memo) * fibonacci(n - 2, memo);
    return memo[n];
}

proc main(argc, argv) {
    # all variables are 64-bit signed integers
    int memo[92]; # zeroed out by default
    if (argc != 2) {
        return 1;
    }
    PutInt(fibonacci(StrToInt(argv[1]), &memo));
    PutChar(10); # newline
    # in absence of the return statement, "return 0;" is implied for all procedures
}
```
---

My attempt to make a [_toy programming language_](https://en.wikipedia.org/wiki/Esoteric_programming_language).
Inspired by [B](https://web.archive.org/web/20240425202455/https://www.bell-labs.com/usr/dmr/www/kbman.html).

`proc.c` contains the interpreter which is the language specification itself.

`examples` directory contains examples of programs in _proc_.


# usage

```
General: proc  FILE-TO-BE-INTERPRETED  [ARGUMENTS-PASSED-TO-FILE...]

Example: proc  ./addnums.proc  1 2 3
```


# how to build (unix)

`$ make`

or just

`$ cc -o proc proc.c`


<!-- keep it simple!!! -->
