/* BitTorrentfs - a BitTorrent client */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <fcall.h>
#include <thread.h> 
#include <9p.h>
#include <pool.h>
#include "ip.h"
#include "torrent.h"
#include "torrentfile.h"
#include "misc.h"
#include "comm.h"

Torrent *torrents[1];
char mypeerid[PEERIDLEN+1];
char *port;
char *datadir;
int verbose = 0;
int onlycall = 0;
int onlylisten = 0;
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
	params->tor->p_callersnb--;
	freepeer(params->peer, &(params->tor->p_callers));
//	chanfree(params->c);
//	free(params);
	print("callee [%d] terminated\n", threadid());
	threadexits("My job here is done\n"); 
}

static void
callees(void *arg)
{
	Torrent *tor;
	Peer *peer;
	Alt *a = 0;
	// 4 bytes for the accept fd, and 16 for the ip address 
	uchar chanm[20];
	int counter = 1;
	int n;
	struct Params{Torrent *tor; Peer *peer; Channel *c;} *params;
	int pfd;
	int num = 1;

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

//TODO: manage the table of alts ?
	// Then start a new callee thread everytime the listener receives a call
	for(;;){
		n = alt(a);
		if((n<0) || (n>counter)){
			dbgprint(1, "error with alt");
			error("with alt");
		}
		if ( n == 0){
		// message from teh listener 
			dbgprint(1, "msg from the listener");
//			pfd = chanm[0];
			memmove(&pfd, chanm, sizeof(int));
			// allow only for a total of maxpeers peers
			if (tor->p_callersnb + tor->p_calleesnb < maxpeers){
				// add a new Alt entry 
				counter++;
				a = erealloc(a, (counter+1)*sizeof(Alt));
				a[counter].v = nil;
				a[counter].c = nil;
				a[counter].op = CHANEND;
				a[counter-1].v = chanm;
				a[counter-1].c = chancreate(sizeof(chanm), 0);
				a[counter-1].op = CHANRCV;
	
				// add a new caller peer to the list
				if (tor->p_callersnb == 0){
					// head of the list
					tor->p_callers = emalloc(sizeof(Peer));
					peer = tor->p_callers;
				}
				else {
					peer->next = emalloc(sizeof(Peer));
					peer = peer->next;
				}
				peer->next = nil;
				peer->num = num;
				tor->p_callersnb++;
				// use odds here and evens in callers
				num += 2;
				
				// prepare params for the thread
				params = emalloc(sizeof(struct Params));
				params->peer = peer;
				params->peer->fd = pfd;
				params->tor = tor;
				params->c = a[counter-1].c;
				params->peer->peerinfo = emalloc(sizeof(Peerinfo));
				params->peer->peerinfo->address = smprint("%V", &chanm[4]);
				params->peer->peerinfo->id = nil;
				params->peer->busy = 1;
				threadcreate(callee, params, STACK);
				dbgprint(1, "callee thread #%d created\n", counter-1);
			}
			else 
				close(pfd);
		}
//		else
//			dbgprint(1, "callee #%d taking over\n",n);
	}
}

void 
initstuff(Torrent *tor)
{
	tor->peersinfonb = 0;
	tor->peersinfo = nil;
	tor->p_callersnb = 0;
	tor->p_calleesnb = 0;
	tor->p_callees = nil;
	tor->p_callers = nil;
	tor->interval = 1800;

	fmtinstall('V', eipfmt);
	fmtinstall('I', eipfmt);
}

Torrent *
addtorrent(char *torrentfile)
{
	Torrent *tor = nil;
	Piece *lister;

	print("Adding %s\n", torrentfile);
	tor = emalloc(sizeof(Torrent));
	parsebtfile(torrentfile, tor);
	initstuff(tor);
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
	uchar chanmsg[1];

	params = arg;
	print("caller [%d] starting\n", threadid());
	chatpeer(params->tor, params->peer, params->c, 1);
	chanmsg[0] = 7;
	send(params->c, chanmsg);
	freepeer(params->peer, &(params->tor->p_callees));
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
	Peer *peer;
	uchar m[1];
	int i;
	int n;
	Alt *a = 0;
	struct Params{Torrent *tor; Peer *peer; Channel *c;} *params;
	int num = 0;

	for (i = 0; i<tor->peersinfonb; i++){
		// allow only for a total of maxpeers peers
		if (tor->p_callersnb + tor->p_calleesnb < maxpeers){
			// add a new callee peer to the list
			if (tor->p_calleesnb == 0){
				tor->p_callees = emalloc(sizeof(Peer));
				peer = tor->p_callees;
			}
			else{
				peer->next = emalloc(sizeof(Peer));
				peer = peer->next;
			}
			peer->next = nil;
			peer->busy = 1;
			peer->peerinfo = emalloc(sizeof(Peerinfo));
			peer->peerinfo->address = smprint("%s", tor->peersinfo[i]->address);
			peer->peerinfo->port = tor->peersinfo[i]->port;
			if (tor->peersinfo[i]->id != nil)
				peer->peerinfo->id = smprint("%s", tor->peersinfo[i]->id);
			else
				peer->peerinfo->id = nil;
			peer->num = num;
			tor->p_calleesnb++;
			// use evens here and odds in callees
			num += 2;

			// add a new Alt entry 
			a = erealloc(a, (i+1)*sizeof(Alt));
			a[i].v = m;
			a[i].c = chancreate(sizeof(m), 0);
			a[i].op = CHANRCV;

			// prepare params for the thread
			params = emalloc(sizeof(struct Params));
			params->tor = tor;
			params->peer = peer;
			params->c = a[i].c;
			threadcreate(caller, params, STACK);
			dbgprint(1, "caller thread #%d started\n", i);
		}
		else
			break;
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

//TODO: we'll have to call again all the trackers inbefore their interval, so we'll have to set the timeout according to how many we have to call.
static void
poketracker(void *arg)
{
	struct Params{ Torrent *tor; char *reqtype; Channel *c;} *params;
	int interval;
	int firstcall = 1;
	uchar chanmsg[1] = {1};

	params = arg;
	for (;;){
		calltracker(params->tor, params->reqtype);
// we attempt to call again when 95% of the interval time has passed,
// to allow for various delays
		interval = params->tor->interval * 95 / 100;
		if (firstcall){
			send(params->c, chanmsg);
			chanfree(params->c);
			firstcall = 0;
		}
		sleep(interval * 1000);
	}
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
	uchar chanmsg[1];
	Channel *c;
	struct Params{ Torrent *tor; char *reqtype; Channel *c;} *params;

//TODO: add maxpeers, keep nocalling and nolisten (usefull for debugging) but maybe with other flags
	ARGBEGIN{
	case 'c':
		onlycall = 1;
		break;
	case 'd':
		datadir = ARGF();
		break;
	case 'l':
		onlylisten = 1;
		break;
	case 'm':
		mtpt = ARGF();
		break;
	case 'P':
		maxpeers = atoi(ARGF());
	case 'p':
		port = ARGF();
		break;
//TODO: make another level of verbose for just the few prints we have now when not verbose
	case 'v':
		verbose = 1;
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
	torrents[0] = addtorrent("/usr/glenda/local.torrent");
	// start the listeners
	if (!onlycall)
		proccreate(callees, torrents[0], STACK);
	// prepare the tracker caller
	c = chancreate(sizeof(chanmsg), 0);
	params->reqtype = smprint("announce");
	params->c = c;
	params->tor = torrents[0];
//TODO: maybe use a simple coroutine along with the callers instead of a standalone proc?
	proccreate(poketracker, params, STACK);
	// wait for the first call to the tracker to finish
	recv(c, chanmsg);
	// start the callers
	if (!onlylisten)
		proccreate(callers, torrents[0], STACK);
	threadexits(0);
}
