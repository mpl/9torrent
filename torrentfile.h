

typedef struct Bestring Bestring;
static struct Bestring {
	long length;
	Rune *value;
};

/*
typedef struct belistel belistel;
struct belistel {
	uint type;
	bestring *astring;
	beint *anint;
	bedictel *adict;
	belistel *alist;
	belistel *next;	
};

typedef struct bedictel bedictel;
struct bedictel {
	uint valtype;
	bestring *key;
	bestring *astring;
	beint *anint;
	belistel *alist;
	bedictel *adict;
	bedictel *next;	
};
*/

static enum Btfilekeys {
	BTannounceurl,
	BTannouncelist,
	BTinfo,
	BTcreationdate,
	BTcomment,
	BTcreatedby,
	BTencoding,
	BTpieces,
	BTpiecelength,
	BTprivate,
	BTname, BTutf8name,
	BTlength,
	BTmd5sum,
	BTfiles,
	BTpath, BTutf8path,
	BTcodepage,
	BTpublisher, BTutf8publisher, BTpublisherurl, BTutf8publisherurl,
	BTnodes,
	BTunknown,
};

static Bestring * getbestr(long toread, Biobuf *bin);
static uvlong readbignumber(Biobuf *bin, Rune stop);
static long readnumber(Biobuf *bin, Rune stop);
static int getkey(Biobuf *bin);
uchar *getinfohash(char *file, int toread);
static int getelement(int keytype, Biobuf *bin, Torrent *tor);
void parsebtfile(char *file, Torrent *tor);

