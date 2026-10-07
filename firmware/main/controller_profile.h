#ifndef KEYLIGHT_CONTROLLER_PROFILE_H
#define KEYLIGHT_CONTROLLER_PROFILE_H
/* Explicit operator-selected profile. Role1 alone never selects a bench
 * action; production has no diagnostic profile. Persisted values are stable. */
enum {
    APP_CONTROLLER_PROFILE_NONE = 0,
    APP_CONTROLLER_PROFILE_OFF = 1,
    APP_CONTROLLER_PROFILE_LOW = 2
};
#endif
