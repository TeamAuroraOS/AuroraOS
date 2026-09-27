
int emmc_write_partition_flags(const char *write_emmuc_partflags){
*(volatile unsigned char *)188h = (unsigned char)*write_emmuc_partflags;
}
int emmc_write(const char *write_emmuc){
*(volatile unsigned char *)18Fh = (unsigned char)*write_emmuc;
}
