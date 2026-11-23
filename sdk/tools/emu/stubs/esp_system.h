// The one firmware call the update screen can make that ends its world. The emulator
// has nothing to restart into, so it says so and carries on.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void esp_restart(void);

#ifdef __cplusplus
}
#endif
