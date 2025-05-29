//
// External EEPROM routines
//
// I2C_xxx constants defined in wifi_modem.h
//
void initEEPROM(void)
{
    i2c_init(i2c0, I2C_BAUD);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
}

bool readSettings(SETTINGS_T *p)
{
    const uint8_t addr[2] = {0, 0};
    int err;
    bool ok = false;

    if (i2c_write_blocking(i2c0, I2C_ADDR, addr, 2, true) == 2)
    {
        ok = i2c_read_blocking(i2c0, I2C_ADDR, (uint8_t *)p, sizeof(SETTINGS_T), false) == sizeof(SETTINGS_T);
    }
    return ok;
}

bool writeSettings(SETTINGS_T *p)
{
    uint8_t data[3];
    struct Settings current;
    bool ok = true;

    readSettings(&current);

    for (int i = 0; i < sizeof(SETTINGS_T) && ok; ++i)
    {
        // only write changed bytes
        if (((uint8_t *)p)[i] != ((uint8_t *)&current)[i])
        {
            data[0] = i >> 8;
            data[1] = i & 0xFF;
            data[2] = ((uint8_t *)p)[i];
            ok = i2c_write_blocking(i2c0, I2C_ADDR, data, 3, false) == 3;
            sleep_ms(5);
        }
    }
    return ok;
}
