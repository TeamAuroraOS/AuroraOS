#define REGISTER_32(value) (*(volatile unsigned int*)value)
#define DISPLAY_MEM REGISTER_32(0xBE740000)
#define LCD_S REGISTER_32(0xBE5C0000)
#define LCD_FAT REGISTER_32(0xBE140000)
int psp_screen_init_slim(){
__asm__ volatile ("sync");
LCD_S = 0x10000000;
DISPLAY_MEM = 0x04000000;
return 0;
}

int psp_screen_init_fat(){
__asm__ volatile ("sync");
LCD_FAT = 0x10000000;
DISPLAY_MEM = 0x04000000;
return 0;
}
