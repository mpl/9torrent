#include <u.h>
#include <libc.h>
#include <pool.h>
#include <thread.h>
#include "misc.h"

extern int verbose;

/* remove abort when debugging's over */
void
error(char *s)
{
	fprint(2, "%s: %r\n", s);
	//abort();
	threadexitsall("Cannot recover, terminate.");
}

void*
erealloc(void *v, ulong n)
{
	v = realloc(v, n);
	if(v == nil)
		error("realloc failed");
	setmalloctag(v, getcallerpc(&n));
	return v;
}

void*
emalloc(ulong n)
{
	void *v;

	v = malloc(n);
	if(v == nil)
		error("malloc failed");
	setmalloctag(v, getcallerpc(&n));
	v = memset(v, 0, n);
	return v;
}

void*
emallocz(ulong n, int clr)
{
	void *v;

	v = mallocz(n, clr);
	if(v == nil)
		error("mallocz failed");
	setmalloctag(v, getcallerpc(&n));
	return v;
}

void
dbgprint(int printid, ...)
{
	if (verbose == 0)
		return;
	Fmt f;
	char buf[64];
	int id = threadid();
	va_list arg;
	char *fmt;

	fmtfdinit(&f, 1, buf, sizeof buf);
//	va_start(arg, fmt);
	va_start(arg, printid);
	if (printid)
		fmtprint(&f, "[%d]: ", id);
	fmt = va_arg(arg, char *);
	fmtvprint(&f, fmt,	arg);
	va_end(arg);
	fmtfdflush(&f);
}

ulong
hton(ulong *il)
{
	uchar c[4];
	ulong l;	

	memmove(c,il,4);
	l = (c[0]&0xFF)<<24;
	l |= (c[1]&0xFF)<<16;
	l |= (c[2]&0xFF)<<8;
	l |= (c[3]&0xFF)<<0;

	return l;
}

int
pickrand(int min, int max)
{
	double rn;

	srand((long)nsec());
	rn = min + frand() * (max - min);
	return floor(rn);
}

//TODO: is that really needed?
char *
ipcharstostring(char *data)
{
	char *buf = malloc(16);

	buf = strcpy(buf,smprint("%d",(uchar)data[0]));
	buf = strcat(buf,".");
	buf = strcat(buf,smprint("%d",(uchar)data[1]));
	buf = strcat(buf,".");
	buf = strcat(buf,smprint("%d",(uchar)data[2]));
	buf = strcat(buf,".");
	buf = strcat(buf,smprint("%d",(uchar)data[3]));

	return buf;
}

void printbits(char n)
{
	char mask, masked;

	for(int i=7; i>=0; i--){
		mask = 1 << i;
		masked = n & mask;
		if (masked == 0) 
			print("0");
		else
			print("1");
	}
}

char *
propername(Rune *name, uchar maxsize)
{
	char *buf;
	uchar namelen;

	/*
	pass 0 as a maxsize for no maxsize
	*/
	if ((namelen = runestrlen(name)) > maxsize && maxsize != 0)
		namelen = maxsize;
	buf = emalloc(namelen+1);
	for (int i=0; i<namelen; i++){
		runetochar(&(buf[i]),&(name[i]));	
		if (buf[i] == ' ')
			buf[i] = '_';
	}
	buf[namelen] = '\0';
	return buf;
}

void
createpath(char *path)
{
	int length,fd;
	Dir *dir;
	
	length = strlen(path);
	for(int i=0; i<length; i++){
		if(path[i] == '/'){
			path[i] = '\0';
			if ((dir = dirstat(path)) == nil){
				fd = create(path,OREAD,DMDIR|0755);		
				close(fd);
			}
			free(dir);
			path[i] = '/';
		}
	}
	if ((dir = dirstat(path)) == nil){
		fd = create(path,OEXCL,0600);
		close(fd);
	}
	else
		dbgprint(1, "%s already exists, not creating it. \n", path);
	free(dir);
}

void
bigE(int N, uchar *buf)
{
	buf[0] = N / (int)pow(2,24);
	buf[1] = (N - buf[0]*(int)pow(2,24)) / (int)pow(2,16);
	buf[2] = (N - buf[0]*(int)pow(2,24) - buf[1]*(int)pow(2,16)) 
	/ (int)pow(2,8);
	buf[3] = N - buf[0]*(int)pow(2,24) - buf[1]*(int)pow(2,16)
	- buf[2]*(int)pow(2,8);
}

void
freeall(int num, ...)
{
	va_list arg;
	char *p;

	va_start(arg, num);
	for (int i = 0; i++; i<num){
		p = va_arg(arg, char *);
		free(p);
	}
	va_end(arg);
}

