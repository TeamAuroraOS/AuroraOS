#define VIRT_WRITE_H
#ifdef VIRT_WRITE_H
int write_vector_psp_begin(const char* write); /* only use this function in early boot */
int psp_kuo_check(int check);
#endif
