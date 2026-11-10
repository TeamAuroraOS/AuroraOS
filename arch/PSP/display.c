#define DISPLAY_MEM (*(volatile unsigned short*)0xBE740000)
#define LCD_S (*(volatile unsigned short*)0xBE5C0000)


int LCD_screen_init(){
LCD_S[0] = 0x10;
return 0;
}
