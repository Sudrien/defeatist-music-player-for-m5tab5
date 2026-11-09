# SPDX-License-Identifier: MIT
#
# boxwin's measuring code on a host (u-defeatist-0002): main/boxwin_run.c as
# it goes into the box, over ../../test_audio_files. See host.c.
#
# storage_io.h is feckless_storage_handler's, which the component manager
# puts in ../managed_components/ when boxwin is configured; a checkout of
# feckless-libs does as well:
#
#     make FECKLESS_STORAGE=/path/to/feckless-libs/components/feckless_storage_handler
#
# minimp3.h is fetched by ../../cmake/vendored.cmake or
# ../../tools/fetch_vendored.sh, as for texttest.

CC ?= cc

FECKLESS_STORAGE ?= ../managed_components/feckless_storage_handler
MINIMP3_DIR      := ../../components/minimp3
AUDIO            := ../../test_audio_files

INC := -I../main -I../../main -I$(MINIMP3_DIR) -I$(FECKLESS_STORAGE)/include

# The firmware's level and errors (CLAUDE.md), then the run under the
# sanitizers. minimp3.h is not this project's to hold to -Wextra.
WARNCFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
              -Wno-unused-function -D_GNU_SOURCE $(INC)
CFLAGS     := -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
              -fsanitize=address,undefined -fno-sanitize-recover=all \
              -fno-omit-frame-pointer -D_GNU_SOURCE $(INC)
LDFLAGS    := -fsanitize=address,undefined -lm

SRCS := host.c ../main/boxwin_run.c
DEPS := $(SRCS) ../main/boxwin.h ../../main/ape.h ../../main/dsd.h $(MINIMP3_DIR)/minimp3.h

.PHONY: all run clean
all: run

boxwinhost: $(DEPS)
	$(CC) $(WARNCFLAGS) -c ../main/boxwin_run.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c host.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

run: boxwinhost
	./boxwinhost $(AUDIO)/*.mp3 $(AUDIO)/*.mp2 $(AUDIO)/*.ape $(AUDIO)/*.dsf $(AUDIO)/*.dff

clean:
	rm -f boxwinhost
