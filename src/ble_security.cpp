
#include "ble_security.h"

uint32_t NotificationSecurityCallbacks::onPassKeyRequest()
{
    return 123456;
}

void NotificationSecurityCallbacks::onPassKeyNotify(uint32_t pass_key)
{
    (void)pass_key;
}

bool NotificationSecurityCallbacks::onSecurityRequest()
{
    return true;
}

bool NotificationSecurityCallbacks::onConfirmPIN(uint32_t pin)
{
    (void)pin;
    return true;
}

void NotificationSecurityCallbacks::onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl)
{
    (void)cmpl;
}
