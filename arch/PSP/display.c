#define REGISTER_32(value) (*(volatile unsigned int*)value)
#define DISPLAY_CONTROL REGISTER_32(0xBE740000)
#define DISPLAY_MEM REGISTER_32(0xBE740010)
#define TV_OUT REGISTER_32(0xBE5C0000)
#define LCD_SLIM REGISTER_32(0xBE140000)
int psp_screen_init_slim(){
__asm__ volatile ("sync");
LCD_SLIM = 0x1;
DISPLAY_MEM = 0x04000000;
return 0;
}

