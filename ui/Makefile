TARGET = mapping_ui
OBJS = mapping_ui.o mapping_patch.o controller_map.o dualsense.o
INCDIR = ../include
CFLAGS = -std=gnu99 -Os -G0 -Wall -Wextra -Werror -fno-pic -MMD -MP
BUILD_PRX = 1
PRX_EXPORTS = exports.exp
LDFLAGS = -nostartfiles
PSPSDK := $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
mapping_patch.o: ../src/mapping_patch.c
	$(CC) $(CFLAGS) -I../include -c $< -o $@
controller_map.o: ../src/controller_map.c
	$(CC) $(CFLAGS) -I../include -c $< -o $@
dualsense.o: ../src/dualsense.c
	$(CC) $(CFLAGS) -I../include -c $< -o $@
-include $(OBJS:.o=.d)
