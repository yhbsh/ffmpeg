BIN := /tmp/ffmpeg_build
CFLAGS ?= -O2 -Wall

.PHONY: all run clean

all: run

$(BIN): build.c build_srcs.h
	$(CC) $(CFLAGS) -o $@ build.c

run: $(BIN)
	$(BIN) $(ARGS)

clean:
	rm -f $(BIN)
