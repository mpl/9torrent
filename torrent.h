#define HASHSIZE 20
#define PEERIDLEN 20

enum {		
	BThas = (1<<0),		/* least sig bit: "has that piece" */
	BTshar = (1<<1),	/* 2nd least sig bit: "piece is shared" */
	BThashar = BThas|BTshar,
	BTnone = 0,
};

typedef struct Piece Piece;
struct Piece{
	int index;
	char status;
	Piece *next;
};

struct Peerinfo {
	char *address;
	char *id;
	int port;	
};
typedef struct Peerinfo Peerinfo;

//TODO: if it turns out the peer_id is too unreliable, maybe we should use an internal id to tag the peers
typedef struct Peer Peer;
struct Peer {
	Peerinfo *peerinfo;
	int fd;
	char seeder;
	Peer *next;
//TODO: get rid of busy?
	char busy;
	char *bitfield;
	Piece *pieceslist;
	int piecesnb;
	char am_choking;
	char am_interested;
	char peer_choking;
	char peer_interested;
// we want to recycle that one, or we will run out eventually
	int num;
};

/*
 * Torrent.fileborder[i]: index of piece where file i starts. fileborder[0] = 0.
 * Torrent.firstoffset[i]: offset (in bytes) where file i starts, relatively to the beginning of fileborder[i]. firstoffset[0] = 0.
 * Torrent.lastpiece: length (in bytes) of actual data within the last piece.
 * Typical piece length: 262144; max(piecesnb) = 2^31 - 1
 => max torrent size = 262144 * max(piecesnb) ~= 5e14 bytes
 => int should be enough for piecesnb for a long time.
*/

//TODO: is this struct getting bloated?
//TODO: do not store ip addresses in a string form?
struct Torrent {
	char *announce;
	char **announcelist;
	int annlistsize;
	Rune *comment;
	Rune *createdby;
	uvlong creationdate; 
	Rune *encoding;
	int multifile;
	uint piecelength;
	Rune *name;
	int piecesnb;
	uchar **sha1list;
	uint private;
	uvlong *filelength;
	Rune **filepath;
	Rune **filemd5sum;
	int filesnb;
	uchar *infohash;
	int infosize;	
	Piece *pieceslist;	 
	char **datafiles;
	int *fileborder;
	int *firstoffset;
	int lastpiece;
	Peer *p_callees;
	Peer *p_callers;
	uint p_callersnb;
	uint p_calleesnb;
	char *bitfield;
	int bitfieldsize;
	Peerinfo **peersinfo;
	char peersinfonb;
	char complete;
//TODO: maybe make a tracker struct ?
	int interval;
};
typedef struct Torrent Torrent;

char *forgerequest(Torrent *tor, char *req);
int checkpiece(char *data, Torrent *tor, int index);
void preppieceslist(Torrent *tor);
void writedata(ulong index, char *data, Torrent *tor, Peer *peer);
void scanpieces(Torrent *tor, char *datadir);
void preppeerspieces(Torrent *tor, Peer *peer);
void readdata(ulong index, char *data, Torrent *tor, Peer *peer);
int updatepeerspieces(Torrent *tor, Peer *peer, int index, char op);
void freepeer(Peer *peer, Peer **listhead);
