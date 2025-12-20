all: proc

proc: proc.c
	$(CC) -std=c89 -g3 -Wall -Wextra -Wpedantic -Wdeclaration-after-statement -Wno-missing-field-initializers -o $@ proc.c

install: all
	install proc /usr/local/bin/proc

clean:
	rm -f proc
