#ifndef MOCK_FSM_H
#define MOCK_FSM_H

#include <stdint.h>

/**
 * @file mock_fsm.h
 * @brief Stub for state/doip_fsm.c — lets UDS unit tests run without binding sockets.
 *
 * Link tests/mocks/mock_fsm.o instead of state/doip_fsm.o.
 * Call mock_fsm_reset() in every Unity setUp() to clear counters.
 */

/* Call counters — inspect these in assertions */
extern int     g_mock_session_change_calls;
extern uint8_t g_mock_last_session_id;
extern int     g_mock_diag_activity_calls;

/** Reset all counters to zero. Call in setUp(). */
void mock_fsm_reset(void);

#endif /* MOCK_FSM_H */
