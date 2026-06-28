#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* Start background task that talks to adityaelectronicsolutions.com */
void remote_server_start(void);

/* Called by web_server when relay command comes from local dashboard */
void remote_server_notify_relay_change(void);

#ifdef __cplusplus
}
#endif
