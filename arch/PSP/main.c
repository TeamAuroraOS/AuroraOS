#include "data.h"
#include "display.h"
#include "GPIO_write.h"
int main(){
psp_screen_init_slim();
int gpio_debug = GPIO_write_psp("AuroraOS PSP booted!");
}
