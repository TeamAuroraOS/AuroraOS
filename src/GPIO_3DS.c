#include "GPIO_3DS.h"
#define GPIO_MEM 0x10147000h //GPIO memory value
// vscode gives a error here for no reason FYI

int GPIO_write(const char* c){
 const char* c = (char *)(GPIO_MEM);
}
