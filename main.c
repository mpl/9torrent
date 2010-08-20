/* BitTorrentfs - a BitTorrent client */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <fcall.h>
#include <thread.h> 
#include <9p.h>
#include <pool.h>
#include <ip.h>
#include "misc.h"
#include "dat.h"
#include "fns.h"

Torrent *torrents[1];
char mypeerid[PEERIDLEN+1];
char *port;
char *datadir;
int verbose = 0;
int onlycall = 0;
int onlylisten = 0;
int maxpeers = 30;

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

//TODO: we'll have to call again all the trackers inbefore their interval, so we'll have to set the timeout according to how many we have to call.
static void
poketrackers(void *arg)
{
	struct Params{ Torrent *tor; char *reqtype; Channel *c;} *params;
	int interval;
	int firstcall = 1;
	uchar chanmsg[1] = {1};

	params = arg;
	for (;;){
		interval = params->tor->interval * 95 / 100;
		calltrackers(params->tor, params->reqtype, interval);
// we attempt to call again when 95% of the interval time has passed,
// to allow for various delays
		if (firstcall){
			send(params->c, chanmsg);
			chanfree(params->c);
			firstcall = 0;
		}
//		sleep(interval * 1000);
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
	proccreate(poketrackers, params, STACK);
	// wait for the first call to the tracker to finish
	recv(c, chanmsg);
	// start the callers
	if (!onlylisten)
		proccreate(callers, torrents[0], STACK);
	threadexits(0);
}
