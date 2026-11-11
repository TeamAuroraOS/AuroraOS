#include "data.h"

hardware_timer_write(const char *write){
HARDWARE_TIMER = (unsigned short)*write;
return 0;
}
L2_Cache_write(const char *write){
L2_CACHE = (unsigned short)*write;
return 0;
}
