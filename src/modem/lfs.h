
#ifdef __cplusplus
extern "C"
{
#endif
#include "littlefs/lfs.h"
#include "types.h"
    bool readSettings(SETTINGS_T *p);
    bool writeSettings(SETTINGS_T *p);
#ifdef __cplusplus
} // extern "C"
#endif
