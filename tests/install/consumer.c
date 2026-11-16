/**
 * @file consumer.c
 * @brief Install check: a program that knows nothing but the installed tree.
 *
 * `make test-install` installs the library and headers into a scratch prefix,
 * then builds this file against <doip/doip.h> and libdoip only, and runs it.
 * It fails if a public header is missing, empty, or pulls in a private one.
 */
#include <doip/doip.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    /* Server API: two independent instances */
    DoIP_Handle_t *first = DoIP_Create(), *second = DoIP_Create();
    if (!first || !second) return 1;

    DoIP_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level       = 1;
    cfg.port            = 23420;
    cfg.logical_address = 0x1001;
    if (DoIP_Init(first, &cfg) != 0) return 2;
    cfg.port            = 23421;
    cfg.logical_address = 0x1002;
    if (DoIP_Init(second, &cfg) != 0) return 3;
    if (DoIP_SetDid(first, 0xF187, "PN-1", 4, false) != 0) return 4;
    for (int i = 0; i < 20; i++) { DoIP_Tick(first); DoIP_Tick(second); usleep(1000); }

    /* Client API */
    DoIP_Client_t *client = DoIP_Client_Create();
    if (!client) return 5;
    DoIP_Client_Destroy(client);

    DoIP_Destroy(first);
    DoIP_Destroy(second);
    printf("libdoip %s: installed headers and library work\n", DOIP_LIB_VERSION_STR);
    return 0;
}
