all: proc

proc: proc.c
	$(CC) -std=c89 -g3 -fno-strict-aliasing -Wall -Wextra -o $@ proc.c

install: all
	install proc /usr/local/bin/proc

clean:
	rm -f proc
