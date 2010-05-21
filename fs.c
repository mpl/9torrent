#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h> 
#include <9p.h>
#include <pool.h>
#include "misc.h"
#include "torrent.h"

/* Note for self: the Files get a corresponding qid.path directly correlated to the order in which they are created. starting from 0. */

extern Torrent *addtorrent(char *torrentfile);
extern void callers(void *arg);

enum files {
	Qctl = 0,
	Qtorrents = 1,
};

static char Ebadmsg[] = "bad btfs control message";

static void
dummyread(Req *r)
{
	char s[512];
	char *p, *e;
	Fid *fid;
	ulong	path;
	ulong	vers;
	uchar	type;

	fid = r->fid;
	path = (ulong)fid->qid.path;
	vers = fid->qid.vers;
	type = fid->qid.type;
	p = s;
	e = s + sizeof s;
	*p = '\0';
	p = seprint(p, e, "Qid: { %ld, %ld, %d }\n",path, vers, type);
	readstr(r, s);
}

static void
fsread(Req *r)
{
	ulong path;
//	char *hint;

	path = (ulong)r->fid->qid.path;
	if (path == 0)
		dummyread(r);
/*
	hint = r->fid->file->aux;
	if (strcmp(hint, "pieces") == 0)
		print("Feature not yet available \n");
*/
	respond(r, nil);
}

static char*
writectl(void *v, long count)
{
	char buf[256];
	char *f[10];
	int nf;
	Torrent *tor;

	if(count >= sizeof(buf))
		count = sizeof(buf)-1;
	memmove(buf, v, count);
	buf[count] = '\0';

	nf = tokenize(buf, f, nelem(f));
	if(nf == 0)
		return Ebadmsg;

	if(strcmp(f[0], "add") == 0){
		if(nf == 1)
			return Ebadmsg;
		tor = addtorrent(f[1]);
		callers(tor);
	}
	else
		return Ebadmsg;
	
	return 0;
}

static char *
simplewrite(Req *r)
{
	print("dirname: %s\n", r->d.name);
	return r->d.name;	
}

static void
fswrite(Req *r)
{
	Fid *fid;

	fid = r->fid;
	r->ofcall.count = r->ifcall.count;
	if(fid->qid.path == Qctl) {
		respond(r, writectl(r->ifcall.data, r->ifcall.count));
		return;
	}
	else
		respond(r, simplewrite(r));
	//	respond(r, nil);
	return;
}

Srv fs = {
	.read = fsread,
	.write = fswrite,
};
