#include "data.h"

int GPIO_write_psp(const char *uart){
GPIO_PSP = (unsigned short)*uart;
return 0;
}
