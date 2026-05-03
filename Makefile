CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_DEFAULT_SOURCE \
         -I. -Iconfig -Icore -Itransport -Istate
LDFLAGS = -lpthread

# ===== Server (ECU Simulator) =====
SRC_CORE = core/doip_frame.c core/doip_log.c core/doip_det.c
SRC_API  = doip_api.c
SRC_UDP  = transport/doip_udp.c
SRC_TCP  = transport/doip_tcp.c
SRC_UDS  = transport/doip_uds.c
SRC_FSM  = state/doip_fsm.c
SRC_MAIN = main.c

OBJS   = $(SRC_CORE:.c=.o) $(SRC_API:.c=.o) \
         $(SRC_UDP:.c=.o) $(SRC_TCP:.c=.o) $(SRC_UDS:.c=.o) \
         $(SRC_FSM:.c=.o) $(SRC_MAIN:.c=.o)
TARGET = doip_ecu_sim

# ===== Client (DoIP Tester) =====
CLIENT_OBJS   = core/doip_frame.o core/doip_log.o \
                client/doip_client.o client/main_client.o
CLIENT_TARGET = doip_client_demo

# ===== Top-level =====
all: $(TARGET) $(CLIENT_TARGET)

# ----- Server link -----
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ----- Client link -----
$(CLIENT_TARGET): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ----- Compile rules -----
client/%.o: client/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) $(CLIENT_OBJS) $(CLIENT_TARGET)

.PHONY: all clean
