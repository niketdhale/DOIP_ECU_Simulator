/**
 * @file doip_config_test.h
 * @brief Configuration overrides for integration tests.
 *
 * Injected via -include before config/doip_config.h. Everything not listed
 * here comes from the real config/doip_config.h.
 *
 *  - Port 23400 instead of 13400, so the tests do not conflict with a running
 *    doip_ecu_sim.
 *  - Vehicle announcements go to the loopback address instead of the multicast
 *    group and come every 200 ms: nothing leaves the machine, and a test can
 *    receive them without depending on multicast routing.
 */
#define DOIP_UDP_PORT               23400U
#define DOIP_TCP_PORT               23400U
#define DOIP_MULTICAST_ADDR         "127.0.0.1"
#define DOIP_ANNOUNCE_INTERVAL_MS   200U
