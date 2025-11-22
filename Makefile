all: proc

proc: proc.c
	$(CC) -std=c89 -g3 -Wall -Wextra -Wpedantic -Wdeclaration-after-statement -Wno-missing-field-initializers -o $@ proc.c

clean:
	rm -f proc
