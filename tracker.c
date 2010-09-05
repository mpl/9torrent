#include <u.h>
#include <libc.h>
#include <bio.h>
#include <pool.h>
#include <thread.h>
#include <ip.h>
#include "misc.h"
#include "dat.h"
#include "fns.h"

static int nkeys = 9 ;
char tmpfile[] = "/tmp/tracker_reply";
extern char mypeerid[];
extern char *port;

static char *
readfile(int from)
{
	int readsize;
	int n, offset;
	char *to = nil;
	
	readsize = 32;
	to = emallocz(readsize,1);
	offset = 0;
	for(;;){
		n = read(from, &to[offset], readsize);
		if (n < readsize && n > 0){
			to = erealloc(to, offset + n);
			break;
		}
		offset+=readsize;
		to = erealloc(to, offset + readsize);
	}
	if(n < 0) {
		free(to);
		sysfatal("read failed: %r");
	}
	return to;
}

static long 
readnb(char *data, int *offset, char stop)
{
	int val;
	long nr;

	val = 0;
	while ((nr = data[*offset]) != stop)
	{
		if (nr == 'e') 
			/* enf of metainfo */
			return 0;
		if (nr <= 0){
			fprint(2, "unexpected eof in readnb \n");
			return -1;
		}
		val = 10 * val + (nr - 48);	
		(*offset)++;
	}
	(*offset)++;
	return val;
}

static int
getky(char *data, int *offset)
{
	uint length;
	char *key;
	int keytype;
	
	length = readnb(data, offset, ':');
	if ( length <= 0){
		free(key);
		return length;
	}
	key = emalloc((length+1)*sizeof(char));
	memmove(key,&data[*offset],length);
	key[length]='\0';
	*offset = *offset + length;
	for (int i = 0; i < nkeys; i++) {
		if (strcmp(key, trackerkeys[i]) == 0) {
			keytype=i;
			dbgprint(1, "[tracker]key found: %s\n", trackerkeys[i]);
			break;
		}
	}
	free(key);
	return keytype;
}

static int
getelem(int keytype, char *data, int *offset, Torrent *tor)
{
	int length;
	int newkeytype;
	static int index = 0;
	uchar buf[4];

	switch(keytype){
	case BTEOF:
		return 0;
	case BTinterval:
		if (data[*offset] != 'i'){
			fprint(2, "Not a valid beencoded integer\n");
			return -1;
		}
		(*offset)++;
		tor->interval = readnb(data, offset, 'e');
		dbgprint(1, "interval: %d\n", tor->interval);
		break;
//TODO: add BTpeers6 ?
	case BTpeers:
		if (data[*offset] != 'l'){
			// assume it's binary model data and not an error 
			dbgprint(1, "binary model data\n");
			if ((length = readnb(data, offset, ':')) == -1) 
				return -1;
			// ip and port on 4+2 bytes, network order 
			length = length / 6;
			for (index=0; index<length; index++){
				tor->peersinfo = erealloc(tor->peersinfo, (index+1) * sizeof(Peerinfo *));
				tor->peersinfo[index] = emalloc(sizeof(Peerinfo));
//				tor->peersinfo[index]->address = ipcharstostring(&data[*offset]);
				for (int i=0; i<4; i++)
					buf[i] = (uchar)data[*offset + i];
				tor->peersinfo[index]->address = smprint("%V", buf);
				(*offset)+=4;
				tor->peersinfo[index]->port = (uchar)data[*offset] * 256;
				(*offset)++;
				tor->peersinfo[index]->port += (uchar)data[*offset];
				(*offset)++;
				tor->peersinfo[index]->id = nil;
				dbgprint(1, "peers[%d]: %s:%d\n", index, tor->peersinfo[index]->address, tor->peersinfo[index]->port);
			}
			tor->peersinfonb = length;
		}
		else{
			// dict model data 
			(*offset)++;
			if (data[*offset] == 'e')
				return 0;
			while (data[*offset] != 'e'){
				if (data[*offset] != 'd'){
					fprint(2, "Not a valid dict in peers list\n");
					return -1;
				}
				(*offset)++;
				// ip, peer_id, port 
				tor->peersinfo = erealloc(tor->peersinfo, (index+1) * sizeof(Peerinfo *));
				tor->peersinfo[index] = emalloc(sizeof(Peerinfo));
				for (int i=0; i<3; i++){
					if ((newkeytype = getky(data, offset)) == -1)
						return -1;
					if ((getelem(newkeytype, data, offset, tor)) == -1)
						return -1;
				}
				index++;
				tor->peersinfonb = index;
				if (data[*offset] != 'e'){
					fprint(2, "Bad ending for dict in peers list\n");
					return -1;
				}
				(*offset)++;
			}
			(*offset)++;
		}
		break;
	case BTpeeraddress:
		if ((length = readnb(data, offset, ':')) == -1) 
			return -1;
		tor->peersinfo[index]->address = emalloc((length+1)*sizeof(char));
		memmove(tor->peersinfo[index]->address, &data[*offset], length);
		tor->peersinfo[index]->address[length]='\0';
		dbgprint(1, "peer address: %s\n", tor->peersinfo[index]->address);
		*offset = *offset + length;
		break;
	case BTpeerid:
		if ((length = readnb(data, offset, ':')) == -1) 
			return -1;
		tor->peersinfo[index]->id = emalloc((length+1)*sizeof(char));
		memmove(tor->peersinfo[index]->id,&data[*offset],length);
		tor->peersinfo[index]->id[length]='\0';
		dbgprint(1, "peer id: %s\n", tor->peersinfo[index]->id);
		*offset = *offset + length;
		break;
	case BTpeerport:
		if (data[*offset] != 'i')
			return -1;
		(*offset)++;
		length = readnb(data, offset, 'e');
		tor->peersinfo[index]->port = length;
		dbgprint(1, "peer port: %d\n", tor->peersinfo[index]->port);
		break;
	case BTcomplete:
		if (data[*offset] != 'i')
			return -1;
		(*offset)++;
//TODO: use that info 
		if (readnb(data, offset, 'e') < 0 )
			return -1;
		break;
	case BTincomplete:
		if (data[*offset] != 'i')
			return -1;
		(*offset)++;
//TODO: use that info?
		readnb(data, offset, 'e');
//		peers[index]->seeder = readnb(data, offset, 'e');
//		if (peers[index]->seeder == 0)
//			peers[index]->seeder = 1;
		break;
	case BTmininterval:
		if (data[*offset] != 'i'){
			fprint(2, "Not a valid beencoded integer\n");
			return -1;
		}
		(*offset)++;
		length = readnb(data, offset, 'e');
		/*store that somewhere, maybe make a tracker struct*/
		dbgprint(1, "min interval: %d\n", length);
		break;
	case BTdownloaded:
		if (data[*offset] != 'i'){
			fprint(2, "Not a valid beencoded integer\n");
			return -1;
		}
		(*offset)++;
		length = readnb(data, offset, 'e');
		/*use that info later*/
		dbgprint(1, "downloaded: %d\n", length);	
		break;
	default:
//TODO: we do not want to return -1 if it's just an unsupported key
		print("No match!\n");
		return -1;
	}
	return index;
}

static int
parsetrackerreply(char *reply, Torrent *tor)
{
	int keytype = 0;
	int *offset; 
	int n;

	// cleanup the peersinfo from the previous call
	for (int i = 0; i<tor->peersinfonb; i++){
		free((tor->peersinfo)[i]->address);
		free((tor->peersinfo)[i]->id);
		free((tor->peersinfo)[i]);
	}
	tor->peersinfonb = 0;
	if (reply[0] != 'd'){
		dbgprint(1, "Not a valid tracker reply");
		return -1;
	}

	offset = emalloc(sizeof(int));
	*offset = 1;
	for (;;) 
	{
		if ((keytype = getky(reply, offset)) <= 0)
			break;
		if ((n = getelem(keytype, reply, offset, tor)) <= 0)
			break;
	}
	free(offset);
	if (keytype < 0 || n < 0){
		return -1;
	}

	return tor->peersinfonb;
}

static int
forgerequest(Torrent *tor, char *req, char **msg)
{
	char *announce, *buf;
	int length;
	char *baseurl;
	char infohash[3*HASHSIZE+1]; 
	char peerid[3*PEERIDLEN+1]; 
	int len = strlen(tor->announce);

	buf = emalloc((512)*sizeof(char));
	/*
	Is it really fair to bail out if announce url
	doesn't contain "announce"?
	*/
	if((announce = strstr(tor->announce, "announce")) == 0){
		fprint(2, "weird announce url");
		return -1;
	}
	length = strlen(announce);
	len = len - length;

	baseurl = emalloc((len+1)*sizeof(char));
	baseurl = strncpy(baseurl,tor->announce,len);
	baseurl[len] = '\0';		

	for (int i = 0; i<HASHSIZE; i++){
		infohash[3*i] = '%';
		sprint(&(infohash[3*i+1]),"%.2ux", tor->infohash[i]);
	}
	infohash[3*HASHSIZE] = '\0';

	for (int i = 0; i<20; i++){
		peerid[3*i] = '%';
		sprint(&(peerid[3*i+1]),"%.2x", mypeerid[i]);
	}
	infohash[3*PEERIDLEN] = '\0';

	if (strcmp(req, "scrape") == 0){
//TODO: that case
		/*
		No need to realloc precisely, just use a big enough buffer
		redo it like for announce
		*/

		buf = strcpy(buf, baseurl);
		buf = strcat(buf,"scrape?info_hash=");
		buf = strcat(buf,infohash);
	}
	else if (strcmp(req, "announce") == 0){
		buf = strcpy(buf, baseurl);
		buf = strcat(buf, announce);
		buf = strcat(buf,"?info_hash=");
		buf = strcat(buf,infohash);
		buf = strcat(buf,"&port=");
		buf = strcat(buf,port);
		buf = strcat(buf,"&uploaded=0&downloaded=0&left=0&event=started");
/*
sending compact=0 will in fact result in compact replies! 
*/
		//buf = strcat(buf,"&compact=0");
		//buf = strcat(buf,"&ip=127.0.0.1");
		buf = strcat(buf,"&peer_id=");
		buf = strcat(buf,peerid);
		print("%s", buf);
	}

	*msg = erealloc(*msg, (512)*sizeof(char));
	*msg = strcpy(*msg, buf);
	free(buf);
	free(baseurl);
	return 1;
}

//use messages to sync with other threads from callers():
// 0 means we're working on peersinfo, 1 means not working on it, 2 means not working on it, but no peers collected yet
static void
calltrackers(Torrent *tor, char *reqtype, int interval, Channel *c)
{
	int tmpfd;
	char *reply = emalloc(1);
	char *msg;
	char buf[13];
	Peerinfo **peers = nil;
	Peerinfo *peer = nil;
	int i = 0;
	char *hgetargs[] = {"/bin/hget/", nil, nil};
	uchar m[1];

//TODO: now we're just overwriting the peersinfos at everycall; we should do better. Also we should try to get peers from as many trackers as possible, not stop as soon as we get a tracker reply.
	/*
	try all trackers if necessary then get out.
	calltracker will be recalled later anyway.
	*/
	for(;;){
		for(;;) {
			if (tor->annlistsize > 0)
				tor->announce = tor->announcelist[i];
			if (forgerequest(tor, reqtype, &msg) < 0) 
				sysfatal("");
	
	//TODO: skip the udp ones
			// call the tracker
			if (fork() ==  0){
				if ((tmpfd = create(tmpfile, OWRITE, 0644)) < 0)
					sysfatal("couldn't open %s: %r", tmpfile);
				hgetargs[1] = smprint("%s", msg);
				dup(tmpfd, 1);
				dup(tmpfd, 2);
				exec("/bin/hget", hgetargs);
			}
			if (waitpid() < 0)
				sysfatal("waitpid %r");
	
			// check the reply.
			if ((tmpfd = open(tmpfile, OREAD)) < 0)
				sysfatal("open %r");
			free(reply);
			reply = readfile(tmpfd);
			close(tmpfd);
			dbgprint(1, "tracker reply: %s\n", reply);
			memmove(buf, reply, 13);
			buf[12] = '\0';
			if (strstr(buf, "hget") != 0){
				dbgprint(1, "query to %s failed \n", tor->announce);
				if (i < tor->annlistsize - 1){
					i++;
				} else {
					if (tor->annlistsize > 0) 
						i = 0;
					sleep(interval);
				}
				continue;
			} else {
				if (strstr(buf, "fail") != 0){
					dbgprint(1, "%s replied with failure \n", tor->announce);			
					if (i < tor->annlistsize - 1) {
						i++;
					} else {
						if (tor->annlistsize > 0) 
							i = 0;
						sleep(interval);
					}
					continue;
				} else 
					break;
			}
		}
		m[0] = 0;
		send(c, m);
		if (parsetrackerreply(reply, tor) <= 0){
			dbgprint(1, "no peers harvested with %s \n", tor->announce);
			m[0] = 2;
			send(c, m);
			sleep(interval);
		} else {
			break;
		}
	}
	free(reply);

	peers = tor->peersinfo;

/*
 *	when the tracker uses binary model for the reply, there's no
 *	peer id, so in that case we can't discriminate between the
 *	tracker sending 127.0.0.1 for a peer on the same host (because
 *	it's bound to 0.0.0.0) and an actual peer on localhost.
 */
	for(int i=0; i<tor->peersinfonb; i++){
		peer = peers[i];
		if ((strcmp("127.0.0.1", peer->address) == 0)
		&& peer->id != nil
		&& (strcmp(mypeerid, peer->id) != 0)){
/* 
 *	check if tracker sent "127.0.0.1" because it's bound to
 *	0.0.0.0 and there's a peer running on the same host
 */
			dbgprint(1, "tracker says peer on 127.0.0.1\n");
//TODO: we assume announce starts with "http://" and fqdn ends with ":", change that.
			free(peer->address);
			peer->address = smprint("%s", strtok(&msg[7], ":"));
			dbgprint(1, "peer->address: %s\n",peer->address);
		}
	}
	free(msg);
	m[0] = 1;
	send(c, m);
}

//TODO: we'll have to call again all the trackers inbefore their interval, so we'll have to set the timeout according to how many we have to call.
void
poketrackers(void *arg)
{
	struct Params{ Torrent *tor; char *reqtype; Channel *c;} *params;
	int interval;
	uchar m[1];

	params = arg;
	for (;;){
		// we attempt to call again when 95% of the interval time has passed,
		// to allow for various delays
		interval = params->tor->interval * 950;
		m[0] = 0;
		calltrackers(params->tor, params->reqtype, interval, params->c);
	}
}

