
#define PSTRLEN 19
#define REQSIZE 16384
#define POOLSIZE 20
/*
<pstrlen><pstr><reserved><info_hash><peer_id>
*/
#define HANDSHAKE 1 + PSTRLEN + 8 + HASHSIZE + PEERIDLEN
#define CHOKED_TIME 10000

static char *pstr = "BitTorrent protocol";
static int nkeys = 9 ;

static char *keystab[] = {
	"interval",
	"peers",
	"ip",
	"peer id",
	"port",
	"complete",
	"incomplete",
	"min interval",
	"downloaded",
};

enum Bttrackerkeys{
	BTinterval,
	BTpeers,
	BTpeeraddress,
	BTpeerid,
	BTpeerport,
	BTcomplete,
	BTincomplete,
	BTmininterval,
	BTdownloaded,
};

enum BTmessages {
	BTchoke,
	BTunchoke,
	BTinterested,
	BTnotinterested,
	BThave,
	BTbitfield,
	BTrequest,
	BTpiece,
	BTcancel,
	BTport,
	BTkeepalive, /* not official, use 10 for id */
};


static char *xfer(int from);
static int parsetrackerreply(char *reply, Torrent *tor);
void calltracker(Torrent *tor, char *reqtype);
static int readlenpref(int ctlfd);
static int readmsg(int ctlfd, char *data);
static int writemsg(int ctlfd, int id, char *data, Torrent *tor);
static int hello1(Peer *peer, Torrent *tor, Channel *c);
static int leech(Peer *peer, Torrent *tor, Channel *c);
void chatpeer(Torrent *tor,  Peer *peer, Channel *c, char mode);
void listener(void *arg);
static int hello2(Peer *peer, Torrent *tor, Channel *c);
static int sharepieces(Peer *peer, Torrent *tor, Channel *c);
