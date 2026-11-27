/* Force-included into updater/main.c by run.py: its files go to the folder
 * standing in for the SD card, and its main() becomes updater_main(). */
#include <stdio.h>
#include <unistd.h>

FILE *host_fopen(const char *path, const char *mode);
int host_rename(const char *from, const char *to);
int host_remove(const char *path);
int host_rmdir(const char *path);

#define fopen  host_fopen
#define rename host_rename
#define remove host_remove
#define rmdir  host_rmdir
#define main   updater_main
