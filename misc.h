#define STACK 16384

void error(char *s);
void *erealloc(void *v, ulong n);
void *emalloc(ulong n);
void *emallocz(ulong n, int clr);
void dbgprint(int printid, ...);
ulong hton(ulong *il);
int pickrand(int min, int max);
char * ipcharstostring(char *data);
void printbits(char n);
char * propername(Rune *name, uchar maxsize);
void createpath(char *path);
void bigE(int N, uchar *buf);
void freeall(int num, ...);
