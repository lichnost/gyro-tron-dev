// board/board.h includes the vendor's I2C header for the one handle type its API
// returns. The emulator never touches the bus, so the type is all that is needed.
#pragma once

typedef void *i2c_master_bus_handle_t;
