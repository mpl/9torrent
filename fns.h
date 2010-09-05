
uchar *getinfohash(char *file, int toread);
void parsebtfile(char *file, Torrent *tor);

int checkpiece(char *data, Torrent *tor, int index);
void preppieceslist(Torrent *tor);
void writedata(ulong index, char *data, Torrent *tor, Peer *peer);
void scanpieces(Torrent *tor, char *datadir);
void preppeerspieces(Torrent *tor, Peer *peer);
void readdata(ulong index, char *data, Torrent *tor, Peer *peer);
int updatepeerspieces(Torrent *tor, Peer *peer, int index, char op);

void poketrackers(void *arg);

void callees(void *arg);
void callers(void *arg);

static int readlenpref(int ctlfd);
static char *xfer(int from);
static int readmsg(int ctlfd, char *data);
static int writemsg(int ctlfd, int id, char *data, Torrent *tor);
static int hello1(Peer *peer, Torrent *tor, Channel *c);
static int leech(Peer *peer, Torrent *tor, Channel *c);
void chatpeer(Torrent *tor,  Peer *peer, Channel *c, char mode);
void listener(void *arg);
static int hello2(Peer *peer, Torrent *tor, Channel *c);
static int sharepieces(Peer *peer, Torrent *tor, Channel *c);

