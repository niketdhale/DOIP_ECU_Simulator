CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_DEFAULT_SOURCE \
         -I. -Iconfig -Icore -Itransport -Istate
LDFLAGS = -lpthread

SRC_CORE = core/doip_frame.c core/doip_api.c core/doip_log.c
SRC_UDP  = transport/doip_udp.c
SRC_TCP  = transport/doip_tcp.c
SRC_UDS  = transport/doip_uds.c
SRC_MAIN = main.c

OBJS = $(SRC_CORE:.c=.o) $(SRC_UDP:.c=.o) $(SRC_TCP:.c=.o) $(SRC_UDS:.c=.o) $(SRC_MAIN:.c=.o)
TARGET = doip_ecu_sim

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean