/* host stand-in: storage.h names the I2C bus handle in one prototype,
 * and nothing on the host calls it. */
#pragma once
typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
/* And audio_out.h a device handle, which ui.h brings in for uireqtest
 * (MPD.md step 5). Nothing on the host calls that either. */
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;
