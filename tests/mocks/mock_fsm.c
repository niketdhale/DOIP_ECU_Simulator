#include "mock_fsm.h"
#include "state/doip_fsm.h"  /* for matching signatures */

/* Observable counters */
int     g_mock_session_change_calls = 0;
uint8_t g_mock_last_session_id      = 0;
int     g_mock_diag_activity_calls  = 0;

void mock_fsm_reset(void) {
    g_mock_session_change_calls = 0;
    g_mock_last_session_id      = 0;
    g_mock_diag_activity_calls  = 0;
}

/* Stub: record call but do nothing else (no socket binding) */
void DoIP_Fsm_OnUdsSessionChange(uint8_t session_id) {
    g_mock_session_change_calls++;
    g_mock_last_session_id = session_id;
}

void DoIP_Fsm_OnDiagnosticActivity(void) {
    g_mock_diag_activity_calls++;
}
