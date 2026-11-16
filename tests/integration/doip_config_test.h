/**
 * @file doip_config_test.h
 * @brief Port override for integration tests.
 *
 * Injected via -include before config/doip_config.h. Redirects TCP + UDP
 * traffic to port 23400 so integration tests do not conflict with a
 * running doip_ecu_sim instance on port 13400. Everything else comes
 * from the real config/doip_config.h.
 */
#define DOIP_UDP_PORT               23400U
#define DOIP_TCP_PORT               23400U
