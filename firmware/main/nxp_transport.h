#ifndef KEYLIGHT_NXP_TRANSPORT_H
#define KEYLIGHT_NXP_TRANSPORT_H
#include "okl_nxp.h"
#include "esp_err.h"
esp_err_t app_nxp_transport_init(okl_nxp *driver, const uint8_t mac[6]);
#endif
