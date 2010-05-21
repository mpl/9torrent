/* BitTorrentfs - a BitTorrent client */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <fcall.h>
#include <thread.h> 
#include <9p.h>
#include <pool.h>
#include "torrent.h"
#include "torrentfile.h"
#include "misc.h"
#include "comm.h"

Torrent *torrents[10];
char mypeerid[PEERIDLEN+1];
char *port;
char *datadir;
int verbose = 0;
int nocalling = 0;
int nolisten = 0;
int maxpeers = 30;
QLock l;

extern Srv fs;

static void
setpeerid(void)
{
	/*
	peer id = 20 bytes = <"-p9btfs-"><12 random chars>
	*/
	char prefix[] = "-p9btfs-";
	//char prefix[] = "M3-4-2--";
	long prefixsz = strlen(prefix);
	int suffixsz = PEERIDLEN - prefixsz;

	strncpy(mypeerid, prefix, prefixsz);
	for (int i=0;i<suffixsz;i++)
		mypeerid[i+prefixsz] = (char)pickrand(65,90);
	mypeerid[PEERIDLEN] = '\0';
}

static void
setdatadir(char *dir)
{
	uchar length = strlen(dir);
	datadir = emalloc(length+1);
	datadir = strcpy(datadir,dir);
}

static void
tortotree(File *root, Torrent *tor)
{
	File *torrent;
	char *buf;
	File *parent;
	File *tracker;
	File *announce;
	File *files;

	buf = emalloc(2 * HASHSIZE + 1);
	for (int i=0; i<HASHSIZE; i++)
		sprint(&(buf[2*i]),"%.2ux", tor->infohash[i]);
	parent = root;
	incref(parent);
	parent = walkfile(parent, "torrents");
	if(parent == nil){
		free(buf);
		closefile(parent);
		return;
	}
	torrent = createfile(parent, buf, parent->uid, DMDIR|0644, nil);
	tracker = createfile(torrent, "tracker", torrent->uid, DMDIR|0644, nil);
	announce = createfile(tracker, "announce", tracker->uid, 0644, nil);
	files = createfile(torrent, "pieces", torrent->uid, DMDIR|0644, nil);
	/*
	files->aux = smprint("%s", "pieces");
	print("%s \n", files->aux);
	*/

	closefile(parent);
	free(buf);
}

static void 
filltree(File *root)
{
	File *ctl;
	File *torrents;

	ctl = createfile(root, "ctl", root->uid, 0644, nil);
	torrents = createfile(root, "torrents", root->uid, DMDIR|0644, nil);
}

static void
callee(void *arg)
{
	struct Params{Torrent *tor; Peer *peer; Channel *c;} *params;

	params = arg;
	qlock(&l);
	scanpieces(params->tor, datadir);
	qunlock(&l);
	print("callee [%d] starting\n", threadid());
	chatpeer(params->tor, params->peer, params->c, 2);
	freepeer(params->peer);
//	chanfree(params->c);
//	free(params);
	print("callee [%d] terminated\n", threadid());
	threadexits("My job here is done\n"); 
}

static void
callees(void *arg)
{
	Torrent *tor;
	Alt *a = 0;
	int chanm[1];
	int counter = 1;
	int n;
	struct Params{Torrent *tor; Peer *peer; Channel *c;} *params;
	int dfd;
	int *toto;

	tor = arg;
	// first start the listener 
	a = erealloc(a, (counter+1)*sizeof(Alt));
	a[0].v = chanm;
	a[0].c = chancreate(sizeof(chanm), 0);
	a[0].op = CHANRCV;
	a[1].v = nil;
	a[1].c = nil;
	a[1].op = CHANEND;
	proccreate(listener, a[0].c, STACK);

	/* Then start a new seeder everytime the listener tells us to. */
	for(;;){
		n = alt(a);
		if((n<0) || (n>counter)){
			dbgprint(1, "error with alt");
			error("with alt");
		}
		if ( n == 0){
		// message from teh listener 
			dbgprint(1, "msg from the listener");
			dfd = chanm[0];
			// add a new Alt entry 
			counter++;
			a = erealloc(a, (counter+1)*sizeof(Alt));
			a[counter].v = nil;
			a[counter].c = nil;
			a[counter].op = CHANEND;
			a[counter-1].v = chanm;
			a[counter-1].c = chancreate(sizeof(chanm), 0);
			a[counter-1].op = CHANRCV;
			params = emalloc(sizeof(struct Params));
//TODO: we should a have peers list akin to the one for the callers 
			params->peer = emalloc(sizeof(Peer));
			params->peer->fd = dfd;
			params->tor = tor;
			params->c = a[counter-1].c;
			threadcreate(callee, params, STACK);
			dbgprint(1, "callee thread #%d created\n", counter-1);
		}
//		else
//			dbgprint(1, "callee #%d taking over\n",n);
	}
}

Torrent *
addtorrent(char *torrentfile)
{
	Torrent *tor = nil;
	Piece *lister;

	print("Adding %s\n", torrentfile);
	tor = emalloc(sizeof(Torrent));
	parsebtfile(torrentfile, tor);
	tor->infohash = getinfohash(torrentfile, tor->infosize);
	for (int i = 0; i<20; i++)
		print("%%%.2ux", tor->infohash[i]);
	print("\n");
	tortotree(fs.tree->root, tor);
	preppieceslist(tor);
	/*
	lister = tor->pieceslist;
	while (lister != nil){
		dbgprint(0, "[%d, %d] ",lister->index, lister->status);
		lister = lister->next;	
	}
	dbgprint(1, "\n");
	*/
	tor->datafiles = nil;
	scanpieces(tor, datadir);
	/*
	for(int i=0; i < tor->bitfieldsize; i++)
		printbits(tor->bitfield[i]);
	*/

	return tor;
}

static void
caller(void *arg)
{
	struct Params{ Torrent *tor; Peer *peer; Channel *c;} *params;
	Piece *lister, *rimmer;
	int m[1];

	params = arg;
	print("caller [%d] starting\n", threadid());
	chatpeer(params->tor, params->peer, params->c, 1);
	m[0] = 7;
	send(params->c, m);
	freepeer(params->peer);
//TODO: freeing the chan makes next call to alt() fail. why? Not really a pb in itself tho, as I can/will reuse those channels. But it might be a hint at something amiss.
//	chanfree(params->c);
//	free(params);
	print("caller [%d] terminated\n", threadid());
	threadexits("My job here is done\n"); 
}

//TODO: update the list regularly with a call to the tracker. question is, how to preserve the current peers while doing that?
void
callers(void *arg)
{
	Torrent *tor = arg;
	int m[1];
	int i;
	int n;
	Alt *a = 0;
	struct Params{Torrent *tor; Peer *peer; Channel *c;} *params;

//TODO: not peersinfonb but rather maxpeers
	for (i = 0; i<tor->peersinfonb; i++){
		tor->peers = erealloc(tor->peers, (i+1) * sizeof(Peer *));
		tor->peers[i] = emalloc(sizeof(Peer));
		tor->peers[i]->peerinfo = emalloc(sizeof(Peerinfo));
		tor->peers[i]->peerinfo->address = smprint("%s", tor->peersinfo[i]->address);
		tor->peers[i]->peerinfo->port = tor->peersinfo[i]->port;
		if (tor->peersinfo[i]->id != nil)
			tor->peers[i]->peerinfo->id = smprint("%s", tor->peersinfo[i]->id);
		else
			tor->peers[i]->peerinfo->id = nil;
		a = erealloc(a, (i+1)*sizeof(Alt));
		a[i].v = m;
		a[i].c = chancreate(sizeof(m), 0);
		a[i].op = CHANRCV;
		params = emalloc(sizeof(struct Params));
		params->tor = tor;
		params->peer = tor->peers[i];
		params->c = a[i].c;
		tor->peers[i]->busy = 1;
		threadcreate(caller, params, STACK);
		dbgprint(1, "caller thread #%d started\n", i);
	}
	a = erealloc(a, (i+1)*sizeof(Alt));
	a[i].v = nil;
	a[i].c = nil;
	a[i].op = CHANEND;
	for(;;){
		n = alt(a);
		if((n<0) || (n>i))
			error("with alt");
//		dbgprint(1, "caller #%d taking over\n",n);
		if (m[0] == 7){
			// update our bitfield
//TODO: a less barbaric way would be directly in scanpieces?
			qlock(&l);
			scanpieces(params->tor, datadir);
			qunlock(&l);
		}
	}
	threadexits(0);	
}

static void
usage(void)
{
	fprint(2, "usage: btfs [-d datadir] [-m mntpt] ");
	exits("usage");
}

void
threadmain(int argc, char **argv)
{
	mainmem->flags |= POOL_PARANOIA;
	char *mtpt = "/n/btfs";
	//char *datadir = getenv("home");
	char *datadir = "/usr/glenda/torrents";
	port = smprint("%s", "6889");
	Dir *dir;
	int fd;

//TODO: add maxpeers, keep nocalling and nolisten (usefull for debugging) but maybe with other flags
	ARGBEGIN{
	case 'm':
		mtpt = ARGF();
		break;
	case 'd':
		datadir = ARGF();
		break;
	case 'p':
		port = ARGF();
		break;
	case 'v':
		verbose = 1;
		break;
	case 'c':
		nocalling = 1;
		break;
	case 'l':
		nolisten = 1;
		break;
	default:
		break;
	}ARGEND

	if ((dir = dirstat(mtpt)) == nil) {
		print("%s does not exist, creating it. \n", mtpt);
		fd = create(mtpt,OREAD,DMDIR|0755);		
		close(fd);
	}
	free(dir);

	setdatadir(datadir);
	if ((dir = dirstat(datadir)) == nil) {
		print("%s does not exist, creating it. \n", datadir);
		fd = create(datadir,OREAD,DMDIR|0755);	
		close(fd);	
	}
	free(dir);

	fs.tree = alloctree(nil, nil, DMDIR|0777, nil);
	filltree(fs.tree->root);
	threadpostmountsrv(&fs, nil, mtpt, MREPL|MCREATE);
	setpeerid();
	torrents[0] = addtorrent("/usr/glenda/clips-local.torrent");
//	torrents[0] = addtorrent("/usr/glenda/district9.torrent");
	if (!nolisten)
		proccreate(callees, torrents[0], STACK);
//TODO make a proc in charge of just calling the tracker(s)
	calltracker(torrents[0], "announce");
	if (!nocalling)
		proccreate(callers, torrents[0], STACK);
	threadexits(0);
}
