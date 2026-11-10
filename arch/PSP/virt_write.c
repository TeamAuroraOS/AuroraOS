#include "virt_addr.h"


int write_vector_psp_begin(const char* write){
*(volatile unsigned char *)RESET_WRITE_ADDR_START = (unsigned char)*write;
}

int psp_kuo_check(int check){
if (KUO_START == check){
return true;
}
}



