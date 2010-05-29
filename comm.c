#include <u.h>
#include <libc.h>
#include <bio.h>
#include <pool.h>
#include <thread.h>
#include <ip.h>
#include "misc.h"
#include "torrent.h"
#include "comm.h"

char tmpfile[] = "/tmp/tracker_reply";
extern char mypeerid[];
extern char *port;
extern maxpeers;

QLock l;
//Ioproc *io;

static char *
xfer(int from)
{
	int datasize;
	int n, offset;
	char *to = nil;
	
	datasize = XFERSIZE;
	to = emallocz(datasize,1);
	offset = 0;
	while((n = readn(from, to+offset, XFERSIZE)) > 0){
		if (n < XFERSIZE) 
			break;
		datasize = datasize + XFERSIZE;
		to = erealloc(to, datasize);
		offset = offset + XFERSIZE;
	}
	if(n < 0) {
		free(to);
		sysfatal("read failed: %r");
	}
	return to;
}

static long 
readnumber(char *data, int *offset, char stop)
{
	int val;
	long nr;

	val = 0;
	while ((nr = data[*offset]) != stop)
	{
		if (nr == 'e') 
			/* enf of metainfo */
			return 0;
		if (nr <= 0)
			sysfatal("eof in readnumber");
		val = 10 * val + (nr - 48);	
		(*offset)++;
	}
	(*offset)++;
	return val;
}

static int
getkey(char *data, int *offset)
{
	uint length;
	char *key;
	int keytype;
	
	if ((length = readnumber(data, offset, ':')) == 0) 
		return -1;
	key = emalloc((length+1)*sizeof(char));
	memmove(key,&data[*offset],length);
	key[length]='\0';
	*offset = *offset + length;
	for (int i = 0; i < nkeys; i++) {
		if (strcmp(key, keystab[i]) == 0) {
			keytype=i;
			dbgprint(1, "[tracker]key found: %s\n", keystab[i]);
			break;
		}
	}
	free(key);
	return keytype;
}

//getelement(int keytype, char *data, int *offset, Peerinfo ***peers)
static int
getelement(int keytype, char *data, int *offset, Torrent *tor)
{
	int length;
	int newkeytype;
	static int index = 0;
	uchar buf[4];

	switch(keytype){
	case BTinterval:
		if (data[*offset] != 'i'){
			print("Not a valid beencoded integer\n");
			return -1;
		}
		(*offset)++;
		tor->interval = readnumber(data, offset, 'e');
		dbgprint(1, "interval: %d\n", tor->interval);
		break;
//TODO: add BTpeers6 ?
	case BTpeers:
		if (data[*offset] != 'l'){
			// assume it's binary model data and not an error 
			dbgprint(1, "binary model data\n");
			if ((length = readnumber(data, offset, ':')) == -1) 
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
		}
		else{
			// dict model data 
			(*offset)++;
			if (data[*offset] == 'e')
				return 0;
			while (data[*offset] != 'e'){
				if (data[*offset] != 'd'){
					print("Not a valid dict in peers list\n");
					return -1;
				}
				(*offset)++;
				// ip, peer_id, port 
				tor->peersinfo = erealloc(tor->peersinfo, (index+1) * sizeof(Peerinfo *));
				tor->peersinfo[index] = emalloc(sizeof(Peerinfo));
				for (int i=0; i<3; i++){
					if ((newkeytype = getkey(data, offset)) == -1)
						return -1;
					if ((getelement(newkeytype, data, offset, tor)) == -1)
						return -1;
				}
				index++;
				if (data[*offset] != 'e'){
					print("Bad ending for dict in peers list\n");
					return -1;
				}
				(*offset)++;
			}
			(*offset)++;
		}
		break;
	case BTpeeraddress:
		if ((length = readnumber(data, offset, ':')) == -1) 
			return -1;
		tor->peersinfo[index]->address = emalloc((length+1)*sizeof(char));
		memmove(tor->peersinfo[index]->address, &data[*offset], length);
		tor->peersinfo[index]->address[length]='\0';
		dbgprint(1, "peer address: %s\n", tor->peersinfo[index]->address);
		*offset = *offset + length;
		break;
	case BTpeerid:
		if ((length = readnumber(data, offset, ':')) == -1) 
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
		length = readnumber(data, offset, 'e');
		tor->peersinfo[index]->port = length;
		dbgprint(1, "peer port: %d\n", tor->peersinfo[index]->port);
		break;
	case BTcomplete:
		if (data[*offset] != 'i')
			return -1;
		(*offset)++;
//TODO: use that info 
		readnumber(data, offset, 'e');
		break;
	case BTincomplete:
		if (data[*offset] != 'i')
			return -1;
		(*offset)++;
//TODO: use that info?
		readnumber(data, offset, 'e');
//		peers[index]->seeder = readnumber(data, offset, 'e');
//		if (peers[index]->seeder == 0)
//			peers[index]->seeder = 1;
		break;
	case BTmininterval:
		if (data[*offset] != 'i'){
			print("Not a valid beencoded integer\n");
			return -1;
		}
		(*offset)++;
		length = readnumber(data, offset, 'e');
		/*store that somewhere, maybe make a tracker struct*/
		dbgprint(1, "min interval: %d\n", length);
		break;
	case BTdownloaded:
		if (data[*offset] != 'i'){
			print("Not a valid beencoded integer\n");
			return -1;
		}
		(*offset)++;
		length = readnumber(data, offset, 'e');
		/*use that info later*/
		dbgprint(1, "downloaded: %d\n", length);	
		break;
	default:
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
		if ((keytype = getkey(reply, offset)) == -1){
			if (reply[*offset] == 'e') 
				break;
			free(offset);
			dbgprint(1, "getkey in parsetrackerreply");
			return -1;
		}
//		if ((n = getelement(keytype, reply, offset, &tor->peersinfo)) == -1){
		if ((n = getelement(keytype, reply, offset, tor)) == -1){
			free(offset);
			dbgprint(1, "parsetrackerreply: getelement");
			return -1;
		}
	}
	tor->peersinfonb = n;
	
	return tor->peersinfonb;
}

void
calltracker(Torrent *tor, char *reqtype)
{
	int tmpfd;
	char *reply = nil;
	char *msg = nil;
	char *buf = nil;
	Peerinfo **peers = nil;
	Peerinfo *peer = nil;
	int i = 0;
	char *hgetargs[] = {"/bin/hget/", nil, nil};

//TODO: now we're just overwriting the peersinfos at everycall; we should do better. Also we should try to get peers from as many trackers as possible, not stop as soon as we get a tracker reply.
	/*
	try all trackers if necessary then get out.
	calltracker will be recalled later anyway.
	*/
	for(;;){
		if (tor->annlistsize > 0)
			tor->announce = tor->announcelist[i];
		msg = forgerequest(tor, reqtype);

//TODO: skip the udp ones

		// call the tracker
		if (fork() ==  0){
			if ((tmpfd = create(tmpfile, OWRITE, 0644)) < 0)
				sysfatal("couldn't open %s: %r", tmpfile);
			hgetargs[1] = smprint("%s", msg);
			dup(tmpfd, 1);
			exec("/bin/hget", hgetargs);
		}
		if (waitpid() < 0)
			sysfatal("waitpid %r");

		// check the reply.
		if ((tmpfd = open(tmpfile, OREAD)) < 0)
			sysfatal("open %r");
//TODO: do it simpler and get rid of xfer
		reply = xfer(tmpfd);
		close(tmpfd);
		dbgprint(1, "tracker reply: %s\n", reply);
		buf = emalloc(13);
		memmove(buf, reply, 13);
		buf[12] = '\0';
		if (strstr(buf, "hget") != 0){
			dbgprint(1, "query to %s failed \n", tor->announce);
			if (tor->annlistsize == 0)
				break;
			if (i < tor->annlistsize - 1)
				i++;
			else
				break;
			sleep(1000);
		} else {
			if (strstr(buf, "fail") != 0){
				dbgprint(1, "%s replied with failure \n", tor->announce);
				if (tor->annlistsize == 0)
					break;				
				if (i < tor->annlistsize - 1)
					i++;
				else
					break;				
				sleep(1000);
			}
			else 
				break;
		}	

	}
	free(buf);				

	if (parsetrackerreply(reply, tor) <= 0){
		dbgprint(1, "no peers harvested\n");
		return;
	}
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
//TODO: change that: assume announce starts with "http://" and fqdn ends with ":"
			free(peer->address);
			peer->address = smprint("%s", strtok(&msg[7], ":"));
			dbgprint(1, "peer->address: %s\n",peer->address);
		}
	}
	free(reply);
	free(msg);
}

static int
readlenpref(int peerfd)
{
	int m, n, length;
	uchar buf[4];

	n = sizeof(buf);
	if ((m = readn(peerfd, buf, n)) != n){
		dbgprint(1, "problem reading length prefix \n");
		dbgprint(1, "m was: %d\n", m);
		return -1;
	}
	length = buf[0] * (int)pow(2,24) + buf[1] * (int)pow(2,16) + buf[2] * (int)pow(2,8) + buf[3];
	return length;
}

static int 
readmsg(int peerfd, char *data)
{
	int n, len, pos;
	char id;
	ulong index, begin, length;

	if ((len = readlenpref(peerfd)) < 0)
		return -1;
	if (len == 0){
		dbgprint(1, "Keepalive. \n");
		return 10;	
	}
	if ((n = readn(peerfd, &id, 1)) < 0){
		dbgprint(1, "Problem when reading message id. \n");
		return -1;
	}
	dbgprint(1, "id: %d -> ", id);
	switch(id){
	case BTchoke:
		dbgprint(1, "BTchoke received \n");
		break;
	case BTunchoke:
		dbgprint(1, "BTunchoke received \n");
		break;
	case BTinterested:
		dbgprint(1, "BTinterested received \n");
		break;
	case BTnotinterested:
		dbgprint(1, "BTnotinterested received \n");
		break;
	case BThave:
		dbgprint(1, "BThave received \n");
		if ((n = readn(peerfd, &index, sizeof(int))) != sizeof(int)){
			dbgprint(1, "comm.c: readmsg(): BThave: index\n");
			return -1;
		}
		index = hton(&index);
		pos = 0;
		memmove(&data[pos], &index, sizeof(int));
		break;
	case BTbitfield:
		len--;
		dbgprint(1, "Bitfield payload: %d bytes to read.\n", len);
//		if ((n = readn(peerfd, data, len)) != len) {
		if ((n = readn(peerfd, data, len)) != len) {
			dbgprint(1, "Problem when reading bitfield\n");
			return -1;
		}
		break;
	case BTrequest:
		dbgprint(0, "BTrequest received: ");
		if ((n = readn(peerfd, &index, sizeof(int))) != sizeof(int)){
			dbgprint(1, "comm.c: readmsg(): BTrequest: index\n");
			return -1;
		}
		index = hton(&index);
		if ((n = readn(peerfd, &begin, sizeof(int))) != sizeof(int)){
			dbgprint(1, "comm.c: readmsg(): BTrequest: begin\n");
			return -1;
		}
		begin = hton(&begin);
		if ((n = readn(peerfd, &length, sizeof(int))) != sizeof(int)) {
			dbgprint(1, "comm.c: readmsg(): BTrequest: length\n");
			return -1;
		}
		dbgprint(0, "%d, %d \n", index, begin);
		length = hton(&length);
		/* disallow request for blocks larger than REQSIZE for now */
		if (length > REQSIZE)
			length = REQSIZE;
		pos = 0;
		memmove(&data[pos], &index, sizeof(int));
		pos += sizeof(int);
		memmove(&data[pos], &begin, sizeof(int));
		pos += sizeof(int);
		memmove(&data[pos], &length, sizeof(int));
		break;
	case BTpiece:
		dbgprint(0, "BTpiece received: ");
		len -= 9;
		/* we don't want to overflow. should we allow bigger sizes than REQSIZE anyway? */
		if (len > REQSIZE){
			dbgprint(1, "comm.c: readmsg: BTpiece: len > REQSIZE \n");
			return -1;
		}
		dbgprint(0, "%d bytes, ", len);
		if ((n = readn(peerfd, &index, sizeof(int))) != sizeof(int)){
			dbgprint(1, "Problem when reading index of piece\n");
			return -1;
		}
		index = hton(&index);
		dbgprint(0, "%d, ", index);
		memmove(data, &index, sizeof(int));
		if ((n = readn(peerfd, &begin, sizeof(int))) != sizeof(int)){
			dbgprint(1, "Problem when reading begin of piece\n");
			return -1;
		}
		begin = hton(&begin);
		dbgprint(0, "%d \n", begin);
		pos =  sizeof(int);
		memmove(&data[pos], &begin, sizeof(int));
		pos += sizeof(int);
		if ((n = readn(peerfd, &data[pos], len)) != len) {
			dbgprint(1, "Problem when reading piece payload");
			return -1;
		}
		break;
	case BTcancel:
		break;
	case BTport:
		break;
	default:
		dbgprint(1, "Unknown id, could be the end of the handshake.\n");
		/* we put in data the 20 bytes which could be the peer id of the handshake */
		data = realloc(data, PEERIDLEN);
//		bigE(len, (uchar *)data);
		hnputl((uchar *)data, len);
		pos = 4;
		memmove(&data[pos], &id, sizeof(char));
		pos++;
		if ((n = readn(peerfd, &data[pos], PEERIDLEN - 5)) != PEERIDLEN - 5){
			dbgprint(1, "comm.c: readmsg: default\n");
			threadexits("comm.c: readmsg: default\n");
		}		
		return -1;
	}

	return id;
}

static int 
writemsg(int peerfd, int id, char *data, Torrent *tor)
{
	int size, pos;
	uchar *msg;
	ulong index, begin, length;

	switch(id){
	case BTchoke:
		size = 5;
		msg = emalloc(size);
		msg[0]=0;
		msg[1]=0;
		msg[2]=0;
		msg[3]=1;
		msg[4]=0;
		dbgprint(1, "sending BTchoke \n");
		if (write(peerfd, msg, size) != size) {
			dbgprint(1, "remote side hung up \n");
			free(msg);
			return -1;
		}
		free(msg);
		break;
	case BTunchoke:
		size = 5;
		msg = emalloc(size);
		msg[0]=0;
		msg[1]=0;
		msg[2]=0;
		msg[3]=1;
		msg[4]=1;
		dbgprint(1, "sending BTunchoke \n");
		if (write(peerfd, msg, size) != size) {
			dbgprint(1, "remote side hung up \n");
			free(msg);
			return -1;
		}
		free(msg);
		break;
	case BTinterested:
		size = 5;
		msg = emalloc(size);
		msg[0]=0;
		msg[1]=0;
		msg[2]=0;
		msg[3]=1;
		msg[4]=2;
		dbgprint(1, "sending BTinterested \n");
		if (write(peerfd, msg, size) != size) {
			dbgprint(1, "remote side hung up \n");
			free(msg);
			return -1;
		}
		free(msg);
		break;
	case BTnotinterested:
		size = 5;
		msg = emalloc(size);
		msg[0]=0;
		msg[1]=0;
		msg[2]=0;
		msg[3]=1;
		msg[4]=3;
		dbgprint(1, "sending BTnotinterested \n");
		if (write(peerfd, msg, size) != size) {
			dbgprint(1, "remote side hung up \n");
			free(msg);
			return -1;
		}
		free(msg);
		break;
	case BThave:
		size = 9;
		msg = emalloc(size);
		msg[0]=0;
		msg[1]=0;
		msg[2]=0;
		msg[3]=5;
		msg[4]=4;
		pos = 0;
		dbgprint(1, "sending BThave: %d \n", index);
		memmove(&index, &data[pos], sizeof(int));
		index = hton(&index);
		pos = 5;
		memmove(&msg[pos], &index, sizeof(int));
		if (write(peerfd, msg, size) != size){
			dbgprint(1, "remote side hung up \n");
			free(msg);
			return -1;
		}
		free(msg);
		break;
	case BTbitfield:
		length = tor->bitfieldsize;
		size = 5 + length;
		msg = emalloc(size);
//		bigE(length+1, msg);
		hnputl(msg, length+1);
		msg[4] = 5;
		pos = 5;
		memmove(&msg[pos], data, length);
		dbgprint(1, "sending BTbitfield \n");
//		if (write(peerfd, msg, size) != size){
		if (write(peerfd, msg, size) != size){
			dbgprint(1, "remote side hung up \n");
			free(msg);
			return -1;
		}
		free(msg);		
		break;
	case BTrequest:
		size = 17;
		msg = emalloc(size);
		msg[0]=0;
		msg[1]=0;
		msg[2]=0;
		msg[3]=13;
		msg[4]=6;
		pos = 0;
		memmove(&index, &data[pos], sizeof(int));
		index = hton(&index);
		pos = pos + sizeof(int);
		memmove(&begin, &data[pos], sizeof(int));
		begin = hton(&begin);
		pos = pos + sizeof(int);
		memmove(&length, &data[pos], sizeof(int));
		length = hton(&length);
		pos = 5;
		memmove(&msg[pos], &index, sizeof(int));
		pos = pos + sizeof(int);
		memmove(&msg[pos], &begin, sizeof(int));
		pos = pos + sizeof(int);
		memmove(&msg[pos], &length, sizeof(int));
		
		dbgprint(1, "sending BTrequest: ");
		for (int i=0; i<size; i++)
			dbgprint(0, "%d ",msg[i]);
		dbgprint(0, "\n");
	
//		if (write(peerfd, msg, size) != size){
		if (write(peerfd, msg, size) != size){
			dbgprint(1, "remote side hung up \n");
			free(msg);
			return -1;
		}
		free(msg);
		break;
	case BTpiece:
		pos = 2*sizeof(int);
		memmove(&length, &data[pos], sizeof(int));
		size = 13 + length;
		msg = emalloc(size);
		size -= 4;
//		bigE(size, msg);
		hnputl(msg, size);
		msg[4]=7;
		size += 4;
		length = hton(&length);
		pos = 0;
		memmove(&index, &data[pos], sizeof(int));
		pos = sizeof(int);
		memmove(&begin, &data[pos], sizeof(int));
		dbgprint(1, "sending BTpiece: %d, %d \n", index, begin);
		index = hton(&index);
		begin = hton(&begin);

		pos = 5;
		memmove(&msg[pos], &index, sizeof(int));
		pos += sizeof(int);
		memmove(&msg[pos], &begin, sizeof(int));
		pos += sizeof(int);
		memmove(&msg[pos], &data[3*sizeof(int)], size - 13);

		if (write(peerfd, msg, size) != size){
			dbgprint(1, "remote side hung up \n");
			free(msg);
			return -1;
		}
		free(msg);
		break;
	case BTcancel:
		break;
	case BTport:
		break;
	case BTkeepalive:
		size = 4;
		msg = emallocz(size, 1);
		dbgprint(1, "sending BTkeepalive \n");
		if (write(peerfd, msg, size) != size){
			dbgprint(1, "remote side hung up \n");
			free(msg);
			return -1;
		}		
		break;
	default:
		dbgprint(1, "Unknown id, something went wrong.\n");
		free(msg);
		return -1;
	}
	
	return id;
}

static void
connect(void *arg)
{
	struct Params{ int *fd; char *address; int port; Channel *c;} *params;
	uchar chanmsg[1];
	char *addr;
	char *port;

	params = arg;
	addr = params->address;
	port = smprint("%d",params->port);
	dbgprint(1, "addr called: %s\n", netmkaddr(addr, "tcp", port));
	*(params->fd) = dial(netmkaddr(addr, "tcp", port), 0, 0, 0);
	chanmsg[0] = 1;
	send(params->c,chanmsg);
	threadexits(0); 
}


static int
hello1(Peer *peer, Torrent *tor, Channel *c)
{
	char *port;
	int peerfd;
	int pos, m, n, bitfieldsize;
	Channel *cio;
	uchar chanmsg[1];
	struct Params{ int *fd; char *address; int port; Channel *c;} *params;
	uchar handshake[HANDSHAKE+1];

	bitfieldsize = tor->bitfieldsize;
	cio = chancreate(sizeof(chanmsg), 0);
	params = emalloc(sizeof(struct Params));
	params->fd = emalloc(sizeof(int));
	params->address = peer->peerinfo->address;
	params->port = peer->peerinfo->port;
	params->c = cio;
	/* dunno why dialing with an ioproc does not work,
	 * so we create an extra proc just for that
	 */
	proccreate(connect, params, STACK);
	recv(cio,chanmsg);
	peerfd = *(params->fd);
	chanfree(cio);
	free(params->fd);
	free(params);
	if(peerfd < 0){
		fprint(2, "can't dial %s %r\n", peer->peerinfo->address);
		return -1;
	}
	chanmsg[0] = 1;
	send(c, chanmsg);

	/*
	<pstrlen><pstr><reserved><info_hash><peer_id>
	*/
	/* send the peer id after we got their handshake */
	n = HANDSHAKE - PEERIDLEN;
	handshake[0] = PSTRLEN;
	pos = 1;
	memmove(&handshake[pos], pstr, PSTRLEN);
	pos = 1 + PSTRLEN + 8;
	memmove(&handshake[pos], tor->infohash, HASHSIZE);
//	pos = 1 + PSTRLEN + 8 + HASHSIZE;
//	memmove(&handshake[pos], mypeerid, PEERIDLEN);

	if (write(peerfd, handshake, n) != n){
		dbgprint(1, "remote side hung up\n");
		return -1;
	}
	for (int i = 0; i < n; i++)
		dbgprint(0, "%c", handshake[i]);
	dbgprint(0, "\n");

//TODO: Do something for the case of clients not sending their peerids ( Vuze anonymity option? )
	n = HANDSHAKE;
	if ((m = readn(peerfd, handshake, n)) <= 0){
		dbgprint(1, "remote side hung up\n");
		return -1;
	}
	for (int i=0; i<m; i++)
		dbgprint(0, "%c",handshake[i]);
	dbgprint(0, "\n");

	// set the peer->id if it hasn't been done at calltracker time 
	if (peer->peerinfo->id == nil){
		peer->peerinfo->id = emalloc(PEERIDLEN + 1);
		memmove(peer->peerinfo->id, &handshake[48], PEERIDLEN);
		peer->peerinfo->id[PEERIDLEN] = '\0';
	}
//TODO: we should not have to check that, since the other side already drops the connection in that case. Why don't we see the connection as closed from here? beats me...
	if (strcmp(mypeerid, peer->peerinfo->id) == 0){
		dbgprint(1, "Called self, drop connection (caller side) \n");
		close(peerfd);
		return -1;
	}

//TODO: we are not checking if the peerid is the same as the one announced by the tracker; spec says we should. (I think it's moot since it's not announced when in binary model).
	// sending end of handshake (peer id) 
	pos = 1 + PSTRLEN + 8 + HASHSIZE;
	n = PEERIDLEN;
	if (write(peerfd, mypeerid, n) != n){
		dbgprint(1, "remote side hung up\n");
		return -1;
	}

//TODO: it should fail here because the other side closed the connection. why can it still send over the connection? :(
	/* send own bitfield */
	if (writemsg(peerfd, BTbitfield, tor->bitfield, tor) != BTbitfield){
		dbgprint(1, "Problem when sending bitfield \n");
		return -1;
	}

	// read, might be a bitfield 
	peer->bitfield = emalloc(bitfieldsize);
	if ((m = readmsg(peerfd, peer->bitfield)) != BTbitfield){
		dbgprint(1, "No bitfield sent \n");
		return -1;
	}

	return peerfd;

}

//TODO: we often get the last piece at the end, something's not quite so random here.
// pick a piece at random amongst the ones left to get
static int
pickpiece(Torrent *tor, Peer *peer)
{
	int index;
	Piece *lister, *rimmer;
	
	/*
	lister = peer->pieceslist;
	dbgprint(1, "pieces to get from this peer: \n");
	while (lister != nil){
		dbgprint(0, "[ %d ] ", lister->index);
		lister = lister->next;
	}
	dbgprint(0, "\n");
	*/
	index = -1;
	while(peer->piecesnb != 0){
		dbgprint(1, "pieces left: %d\n", peer->piecesnb);
		index = pickrand(0,peer->piecesnb-1);
		dbgprint(1, "pickrand: %d -> ", index);
		lister = peer->pieceslist;
		for (int i=0; i<index; i++)
			lister = lister->next;
		index = lister->index;
		dbgprint(1, "index: %d\n", index);
		lister = tor->pieceslist;
		while ((lister != nil) && (lister->index != index))
			lister = lister->next;
//TODO: this should not happen so it should be safe to remove it...	
		if (lister == nil)
			error("main list empty before peer's list");
		// Do we already have this piece? 
		if ((lister->status & BThas) == 0)
			break;
		dbgprint(1, "Already got this piece, drop it.\n");
		// update the peer's list 
		lister = peer->pieceslist;
		rimmer = lister;
		while ((lister != nil) && (lister->index != index)){
			rimmer = lister;
			lister = lister->next;
		}
//TODO: should not happen either, so remove when sure
		if (lister == nil)
			error("peer's list empty whereas we checked before");
		if (lister == peer->pieceslist)
			peer->pieceslist = lister->next;
		rimmer->next = lister->next;
		free(lister);
		peer->piecesnb--;
	}

	return index;
}

static int
checkinterest(Torrent *tor, Peer *peer){
	Piece *lister, *rimmer;

	lister = tor->pieceslist;
	rimmer = peer->pieceslist;
	while(lister != nil){
		// check if there's at least one piece the peer has that we don't have
		if ((lister->status & BThas) == 0){
			while (rimmer != nil && rimmer->index < lister->index)
				rimmer = rimmer->next;
			if (rimmer != nil && rimmer->index == lister->index)
				return lister->index;
		}
		lister = lister->next;		
	}
	return -1;
}

void
chatpeer(Torrent *tor, Peer *peer, Channel *c, char mode)
{
	dbgprint(1, "in chatpeer\n");
//	io = ioproc();
	int chanm[1];
	int n;

	peer->am_interested = 1;
	peer->am_choking = 1;
	peer->peer_choking = 1;
	peer->peer_interested = 0;

	switch(mode){
	case 1:
		// we are the caller 
		if ((peer->fd = hello1(peer, tor, c)) < 0){
			dbgprint(1, "Problem during hello1\n");
//TODO: cleanup
			return;
		}
		break;
	case 2:
		// we are being called 
		if (hello2(peer, tor, c) < 0){
			dbgprint(1, "Problem during hello2\n");
//TODO: cleanup
			return;
		}
		break;
	default:
		error("comm.c: chatpeer(): unexpected case");

	}
	preppeerspieces(tor, peer);
	if ((n = checkinterest(tor, peer)) >= 0){
		dbgprint(1, "Peer has at least piece %d, which we want\n", n);
		peer->am_interested = 1;
	} else {
		dbgprint(1, "Peer has no interesting piece \n", n);
		peer->am_interested = 0;
	}
	sharepieces(peer, tor, c);
	peer->busy = 0;
	close(peer->fd);

//TODO: figure out why this faults 
//	closeioproc(io);
	dbgprint(1, "end of chatpeer\n");
}

void
listener(void *arg)
{
	int acfd, lcfd, dfd;
	char adir[40], ldir[40];
	Channel *c;
	uchar chanm[20];
	char *address;
	NetConnInfo *info;

	c = arg;
	address = smprint("%s", strcat("tcp!*!", port));
	acfd = announce(address, adir);
	if(acfd < 0)
		error("");

	for(;;){
		// listen for a call 
		dbgprint(1, "waiting for a call\n");
		lcfd = listen(adir, ldir);
		if(lcfd < 0){
			dbgprint(1, "pb with lcfd\n");
			error("");
		}
		dbgprint(1, "call received\n");
		// accept the call and open the data file 
		dfd = accept(lcfd, ldir);
		if(dfd < 0){
			dbgprint(1, "pb with dfd\n");
			error("");
		}
//		chanm[0] = dfd;
		info = getnetconninfo(ldir, dfd);
		if (info == nil){
			dbgprint(1, "could not get conninfo\n");
			error("");
		}			
		memmove(chanm, &dfd, sizeof(int));
		// yay, ipv6 ready!
		if (parseip(&chanm[4], info->rsys) != 6)
			v4parseip(&chanm[4], info->rsys);
		send(c, chanm);
		dbgprint(1, "call accepted! \n");
	}
	close(lcfd);
	close(acfd);
	threadexits(0);	
}

//TODO: make it return an int for error handling
static int
hello2(Peer *peer, Torrent *tor, Channel *c)
{
	
	int pos, m, n, bitfieldsize;
	uchar handshake[HANDSHAKE];
	int peerfd = peer->fd;

	dbgprint(1, "in hello2\n");
	bitfieldsize = tor->bitfieldsize;

	// read handshake 
	// at least mainline and rtorrent send their peerid after they got our handshake, so let's read it in two times 
	n = HANDSHAKE - PEERIDLEN;
	if ((m = readn(peerfd, handshake, n)) <= 0){
		dbgprint(1, "remote side hung up\n");
		return -1;
	}
	dbgprint(1, "got handshake: ");
	for (int i=0; i<m; i++)
		dbgprint(0, "%c",handshake[i]);
	dbgprint(0, "\n");

	/*
	 * send handshake
	 * <pstrlen><pstr><reserved><info_hash><peer_id>
	 * 1 + 19 + 8 + 20 + 20
	 */
	n = HANDSHAKE;
	handshake[0] = PSTRLEN;
	pos = 1;
	memmove(&handshake[pos], pstr, PSTRLEN);
	pos = 1 + PSTRLEN + 8;
	memmove(&handshake[pos], tor->infohash, HASHSIZE);
	pos +=  HASHSIZE;
	memmove(&handshake[pos], mypeerid, PEERIDLEN);
	dbgprint(1, "sending handshake: ");
	for (int i = 0; i < n; i++)
		dbgprint(0, "%c", handshake[i]);
	dbgprint(0, "\n");
	if (write(peerfd, handshake, n) != n){
		dbgprint(1, "remote side hung up\n");
		return -1;
	}

	// read, should be bitfield or end of handshake (peer id) 
	peer->bitfield = emalloc(bitfieldsize);
	if ((m = readmsg(peerfd, peer->bitfield)) != BTbitfield){
		// Maybe it was the end of the handshake 
		if (peer->peerinfo->id == nil){
			peer->peerinfo->id = emalloc(PEERIDLEN + 1);
			memmove(peer->peerinfo->id, peer->bitfield, PEERIDLEN);
			peer->peerinfo->id[PEERIDLEN] = '\0';
			dbgprint(1, "Peer id? %s \n", peer->peerinfo->id);
		}
		if (strcmp(mypeerid, peer->peerinfo->id) == 0){
			dbgprint(1, "Called by self, drop connection (listener side). \n");
			close(peerfd);
			return -1;
		}
		peer->bitfield = realloc(peer->bitfield, bitfieldsize);
		if ((m = readmsg(peerfd, peer->bitfield)) != BTbitfield){
			if (m < 0){
				dbgprint(1, "remote side hung up\n");
//TODO: we should clean up at least the Peer
				return -1;
			}
			dbgprint(1, "No bitfield sent \n");
		}
	}

	/* send own bitfield */
	if (writemsg(peerfd, BTbitfield, tor->bitfield, tor) != BTbitfield){
		dbgprint(1, "Problem when sending bitfield \n");
		return -1;
	}

	return 0;

}

//TODO: maybe the interested/choke status should directly be set in readmsg and writemsg
static int
sharepieces(Peer *peer, Torrent *tor, Channel *c)
{
	int pos, block, blocks, blocksgot, pieceup, piecedown;
	char *msg, *buf, *upload, *download;
	int index, begin, length, m, peerfd;
	uchar chanmsg[1];
	// for now: 0->block not requested, 1->requested, 2->we got it. change to use bitfields later.
	char *requested = nil;

	msg = emalloc(tor->piecelength);
	upload = emalloc(tor->piecelength);
	download = emalloc(tor->piecelength);
	buf = emalloc(REQSIZE + 3*sizeof(int));
	pieceup = -1;
	piecedown = -1;
	chanmsg[0] = 0;
	peerfd = peer->fd;
	block = 0;
	blocks = 0;
	blocksgot = 0;

	if (peer->am_interested){
		// Send "interested" 
		if (writemsg(peer->fd, BTinterested, nil, nil) != BTinterested){
			dbgprint(1, "remote side hung up \n");
			return;
		}
	}
	else{
		if (writemsg(peer->fd, BTnotinterested, nil, nil) != BTnotinterested){
			dbgprint(1, "remote side hung up \n");
			return;
		}
	}

	for (;;){
		m = readmsg(peerfd, msg);
//TODO: all the cases
		switch(m){
		case BTchoke:
			peer->peer_choking = 1;
			break;
		case BTunchoke:
			peer->peer_choking = 0;
			break;
		case BTinterested:
			dbgprint(1, "peer is interested; unchoking. \n");
			peer->peer_interested = 1;
			// Send "unchoke" 
			if (writemsg(peerfd, BTunchoke, nil, nil) != BTunchoke){
				freeall(4, msg, buf, upload, download);
				threadexits("comm.c: seed(): sending unchoke \n");
			}
			peer->am_choking = 0;
			break;
		case BTnotinterested:
			peer->peer_interested = 0;
			break;
		case BThave:
			pos = 0;
			memmove(&index, &msg[pos], sizeof(int));
			updatepeerspieces(tor, peer, index, '+');
			if (peer->am_interested == 0){
				if (checkinterest(tor, peer) >= 0){
					if (writemsg(peerfd, BTinterested, nil, nil) != BTinterested){
						dbgprint(1, "remote side hung up \n");
						freeall(5, msg, buf, upload, download, requested);
						return -1;
					}
					peer->am_interested = 1;
				} 
			}
			break;
		case BTrequest:
			// ignore request if we are choking.
			if (peer->am_choking != 0)
				break;
			pos = 0;
			memmove(&index, &msg[pos], sizeof(int));
			pos += sizeof(int);
			memmove(&begin, &msg[pos], sizeof(int));
			pos += sizeof(int);
			memmove(&length, &msg[pos], sizeof(int));
//TODO: we can probably do some even better "caching" for that, but I'm assuming other peers usually request preferably blocks of the same piece in one go.
			// do not read the piece again from the file if we already did so last time 
			if (index != pieceup){
//TODO: a less barbaric way would be directly in readdata?
				qlock(&l);
				readdata(index, upload, tor, peer);
				qunlock(&l);
				if (checkpiece(upload, tor, index) < 0){
					dbgprint(1, "bad piece read: #%ld\n", index);
					freeall(4, msg, buf, upload, download);
					threadexits("bad piece read \n");
				}
				pieceup = index;
			}
			/* send the requested block */
			memmove(buf, msg, 3*sizeof(int));
			pos = 3*sizeof(int);
			memmove(&buf[pos], &upload[begin], length);
			if (writemsg(peerfd, BTpiece, buf, nil) != BTpiece) {
				freeall(4, msg, buf, upload, download);
				threadexits("comm.c: seed: writemsg: BTpiece\n");
			}
			chanmsg[0] = 0;
			send(c, chanmsg);
			break;
		case BTpiece:
			memmove(&index, msg, sizeof(int));
			if (index != piecedown){
//TODO: cache that block instead of discarding it and keep it for later?
				dbgprint(1, "not the index we asked for, discard.\n");
				break;
			}
			pos = sizeof(int);
			memmove(&begin, &msg[pos], sizeof(int));
			block = begin / REQSIZE;
//TODO: investigate last piece case (ignore for now)
			if (piecedown < tor->piecesnb-1){
				if (requested[block] != 1){
					dbgprint(1, "not a block we asked for, discard.\n");
					break;
				}
			}
			pos += sizeof(int);
			if ((piecedown == tor->piecesnb-1) && (block == blocks - 1))
				length = tor->lastpiece - (tor->lastpiece / REQSIZE)*REQSIZE;
			else
				length = REQSIZE;
			memmove(&download[begin], &msg[pos], length);
			requested[block] = 2;
			blocksgot++;
			if (blocksgot == blocks){
				if (checkpiece(download, tor, piecedown) < 0){
					print("checkpiece failed for #%d\n", piecedown);
					threadexits("bad piece");
//TODO: put that piece back in the pool and reget it
				}
				else{
					qlock(&l);
					writedata(piecedown, download, tor, peer);
					qunlock(&l);
				}
			}
			chanmsg[0] = 1;
			send(c, chanmsg);
			break;
		default:
//TODO: we in fact end up here when something goes wrong on the other side -> improve resilience
			dbgprint(1, "msg type: %d\n", m);
			if (writemsg(peerfd, BTkeepalive, nil, nil) != BTkeepalive){
				dbgprint(1, "comm.c: seed(): sending keepalive \n");
				freeall(5, msg, buf, upload, download, requested);
				return -1;
			}
			break;
		}

		// check whether there's still something to do/get
		if (peer->piecesnb == 0){
//TODO: send a specific message to alt so we can do something in main()?
			if (peer->am_interested != 0){
				peer->am_interested = 0;
				if (writemsg(peerfd, BTnotinterested, nil, nil) != BTnotinterested){
					dbgprint(1, "remote side hung up \n");
					freeall(5, msg, buf, upload, download, requested);
					return -1;
				}
			}
			// terminate if both the peer and us are done
			if (peer->peer_interested == 0)
				break;
		}
				
		// ask for a block, if suitable 
		if (peer->am_interested == 1 && peer->peer_choking == 0){
			// start getting a new piece if we finished the previous one 
			if (blocksgot == blocks){
				piecedown = pickpiece(tor, peer);
//TODO: I should not have to check for that, because peer->piecesnb is checked right before. And yet it happens. why?
				if (piecedown < 0){
					dbgprint(1, "Peer is out of pieces. \n");
					freeall(4, msg, buf, upload, download);
					return -1;
				}
				if (piecedown == tor->piecesnb-1)
					blocks = tor->lastpiece / REQSIZE + 1;
				else
					blocks = tor->piecelength / REQSIZE;
				blocksgot = 0;
				if (requested != nil)
					free(requested);
				requested = emallocz(blocks, 1);
			}
				
//TODO: allow for other block sizes than REQSIZE
			length = REQSIZE;
			block = -1;
//TODO: think of something to reask for some blocks if they never arrive, probably use a timer
			// look for a block we don't have and we didn't ask for yet
			for (int i = 0; i<blocks; i++){
				if (requested[i] == 0){
					block = i;
					break;
				}
			}
			if (block == -1){
				continue;
			}

			pos = 0;
			begin = length * block;
			// last block of last piece 
			if ((piecedown == tor->piecesnb-1) && (block == blocks - 1))
				length = tor->lastpiece - (tor->lastpiece / REQSIZE)*REQSIZE;
			memmove(&msg[pos], &piecedown, sizeof(int));
			pos += sizeof(int);
			memmove(&msg[pos], &begin, sizeof(int));
			pos += sizeof(int);
			memmove(&msg[pos], &length, sizeof(int));
			if (writemsg(peerfd, BTrequest, msg, nil) != BTrequest) {
				print("comm.c: seed: writemsg: BTrequest\n");	
				freeall(4, msg, buf, upload, download);
				return -1;	
			}
			requested[block] = 1;

		}
		chanmsg[0] = 2;
		send(c, chanmsg);
	}
	freeall(5, msg, buf, upload, download, requested);
	return 1;

}
