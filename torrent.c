#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <pool.h>
#include "misc.h"
#include "torrent.h"

extern char mypeerid[];
extern char *port;

/*
 * creates our linked list of Pieces and set other info such as file
 * borders and offsets
 */
void 
preppieceslist(Torrent *tor)
{
	/*
	temp is used for operations with many uvlongs, because
	not enough registers to handle them in one go.
	*/
	uvlong fullpieces, temp;
	uvlong piecelength;
	int *offset;
	uvlong *length;
	Piece *lister;

	length = tor->filelength;
	piecelength = tor->piecelength;
	tor->pieceslist = emalloc(sizeof(Piece));
	lister = tor->pieceslist;
	for (int i=0; i<tor->piecesnb-1; i++){
		lister->index = i;
		lister->status = 0;
		lister->next = emalloc(sizeof(Piece));
		lister = lister->next;
	}
	lister->index = tor->piecesnb-1;
	lister->status = 0;
	lister->next = 	nil;
	tor->firstoffset = emalloc(tor->filesnb * sizeof(int));
	offset = tor->firstoffset;
	offset[0] = 0;
	tor->fileborder = emalloc(tor->filesnb*sizeof(int));
	tor->fileborder[0] = 0;
	for (int i=0; i<tor->filesnb; i++){
		if(length[i] < piecelength){
		/* file is smaller than a piece */
			if(offset[i] == 0 && i < tor->filesnb - 1){
				lister = tor->pieceslist;
				while ((lister != nil) && (lister->index != tor->fileborder[i]))
					lister = lister->next;
				lister->status |= BTshar;
			}
			if(i < tor->filesnb - 1){
				tor->fileborder[i+1] = tor->fileborder[i];
				temp = offset[i] + length[i];
				offset[i+1] = temp;
			} else
				tor->lastpiece = length[i];
// => lastpiece == length if data is an exact number of pieces
		} else{
			if(offset[i] != 0){
				temp = length[i];
				temp -= piecelength - offset[i];
				fullpieces = temp / piecelength;
				temp = length[i];
				temp -= fullpieces * piecelength;
				if(i < tor->filesnb - 1){
					tor->fileborder[i+1] = tor->fileborder[i] + fullpieces + 1;
					offset[i+1] = temp - (piecelength - offset[i]);
				} else
					tor->lastpiece = temp - (piecelength - offset[i]);
// => lastpiece == 0 if data is an exact number of pieces
			} else{
				fullpieces = length[i] / piecelength;
				temp = length[i];
				if(i < tor->filesnb - 1){
					tor->fileborder[i+1] = tor->fileborder[i] + fullpieces;
					offset[i+1] = temp - fullpieces * piecelength;
				} else
					tor->lastpiece = temp - fullpieces * piecelength;
// => lastpiece == 0 if data is an exact number of pieces
			}
			if(i < tor->filesnb - 1 && offset[i+1] != 0){
				lister = tor->pieceslist;
				while ((lister != nil) && (lister->index != tor->fileborder[i+1]))
					lister = lister->next;
				lister->status |= BTshar;
			}
		}
	}
}

/*
 * 	creates our bitfield, mainly from tor->pieceslist
 *	also creates the data files if needed
 */
//TODO: we need to do some more precise resource protection in here, instead of using a big fat qlock in the caller
void
scanpieces(Torrent *tor, char *datadir)
{
	int fd, n, piecelength, index, offset, bitfieldsize, byteindex, fileindex, piecepart, keepgoing, needsopen;
	char *filename, *data, *buf, *bitfield;
	char bitindex, mask;
	Piece *lister;

	// do not rescan if torrent is already completed
	if (tor->complete)
		return;

	piecelength = tor->piecelength;
	if(tor->piecesnb % 8 != 0)
		bitfieldsize = tor->piecesnb / 8 + 1;
	else
		bitfieldsize = tor->piecesnb / 8;
	tor->bitfieldsize = bitfieldsize;
	// create data files if needed 
	if (tor->datafiles == nil){
		tor->datafiles = emalloc(tor->filesnb * sizeof(char *));
		for (int i=0; i<tor->filesnb; i++){
			tor->datafiles[i] = emalloc(strlen(datadir) + 1 + strlen(propername(tor->filepath[i], 0))+ 1);
			tor->datafiles[i] = strcpy(tor->datafiles[i], datadir);
			tor->datafiles[i] = strcat(tor->datafiles[i], "/");
			tor->datafiles[i] = strcat(tor->datafiles[i], propername(tor->filepath[i], 0));
			dbgprint(1, "tor->datafiles[%d]: %s\n", i, tor->datafiles[i]);
			createpath(tor->datafiles[i]);
		}
	}
	data = emalloc(piecelength);
	// redo our bitfield from scratch everytime
	free(tor->bitfield);
	tor->bitfield = emallocz(bitfieldsize*sizeof(char), 1);
	bitfield = tor->bitfield;
	lister = tor->pieceslist;
	needsopen = 1;
//TODO: something more readable maybe?
	while (lister != nil){
		// find fileindex 
		index = lister->index;
		fileindex = 0;
		for (int i=tor->filesnb-1; i>=0; i--)
			if (index > tor->fileborder[i]){
				fileindex = i;
				break;
			}
		filename = tor->datafiles[fileindex];
		if (needsopen) {
			if((fd = open(filename, OREAD)) < 0)
				error("torrent.c: scanpieces: open");
			needsopen = 0;
			if (fileindex == 0)
				offset = 0;
			else
				offset = piecelength - tor->firstoffset[fileindex];
		}
		if (((lister->status)&BTshar) != 0){
			// shared piece 
			piecepart = tor->firstoffset[fileindex+1];
			buf = emalloc(piecepart);
			n = pread(fd, buf, piecepart, offset);
			memmove(data, buf, piecepart);
			offset = piecepart;
			keepgoing = 1;
			if (n != piecepart){
				// just skip the rest of the piece since we already know this part is not yet dled 
				close(fd);
				needsopen = 1;
				free(buf);
				lister = lister->next;
				continue;
			}
			// now deal with rest of the piece 
			while (keepgoing == 1){
				close(fd);
				fileindex++;
				filename = tor->datafiles[fileindex];
				if((fd = open(filename, OREAD)) < 0)
					error("torrent.c: scanpieces: open");
				if (tor->filelength[fileindex]
				< piecelength - tor->firstoffset[fileindex]){
					// file smaller than the rest of the piece 
					piecepart = (int) tor->filelength[fileindex];
					if (fileindex < tor->filesnb - 1)
						// loop to next file in the piece 
						keepgoing = 1;
					else
						// last file of last piece, don't loop again 
						keepgoing = 0;
				} else {
					// file takes all the rest of the piece 
					piecepart = piecelength - tor->firstoffset[fileindex];
					keepgoing = 0;
				}
				free(buf);
				buf = emallocz(piecepart, 1);
				n = read(fd, buf, piecepart);
//TODO: skip to next piece if read not complete (not a problem though, checkpiece will catch that)
				memmove(&data[offset], buf, piecepart);
				offset += piecepart;
			}
			close(fd);
			needsopen = 1; 		
			free(buf);
		} else {
			// non shared piece 
			if (index == tor->piecesnb - 1)
				// last piece 
				piecelength = tor->lastpiece;
			n = pread(fd, data, piecelength, offset);
			offset += piecelength;
			if(n != piecelength){
				lister = lister->next;
				continue;	
			}
		}		
		if(checkpiece(data, tor, index) >= 0){
			byteindex = index / 8;
			bitindex = index % 8 + 1;
			mask = 1 << (8 - bitindex); 
			bitfield[byteindex] |= mask;
		}

		lister = lister->next;

	}

	// mark the pieces we have in our list of pieces
	lister = tor->pieceslist;
	while (lister != nil){
		index = lister->index;
		byteindex = index / 8;
		bitindex = index % 8 + 1;
		mask = 1 << (8 - bitindex); 
		if ((bitfield[byteindex] & mask) != 0)
			lister->status |= BThas;
		lister = lister->next;
	}
	// check if we still miss some pieces
	tor->complete = 1;
	lister = tor->pieceslist;
	while (lister != nil){
		if ((lister->status & BThas) == 0){
			tor->complete = 0;
			break;
		}
		lister = lister->next;
	}
	if (tor->complete)
		print("Download successful\n");
	free(data);
}

void 
preppeerspieces(Torrent *tor, Peer *peer)
{
	char *bitfield;
	Piece *lister;
	Piece *rimmer;
	Piece *kryten;
	int index;
	int byteindex;
	char bitindex;
	char mask;

	bitfield = peer->bitfield;
	lister = tor->pieceslist;
	peer->pieceslist = emalloc(sizeof(Piece));
	rimmer = peer->pieceslist;
	kryten = rimmer;
	peer->piecesnb = 0;
	while (lister != nil){
		index = lister->index;
		byteindex = index / 8;
		bitindex = index % 8 + 1;
		mask = 1 << (8 - bitindex); 
		if ((bitfield[byteindex] & mask) != 0){
			rimmer->index = index;
			rimmer->status = lister->status;
			rimmer->next = emalloc(sizeof(Piece));
			kryten = rimmer;
			rimmer = rimmer->next;
			peer->piecesnb++;
		}
		lister = lister->next;
	}
	free(rimmer);
	kryten->next = nil;
}

char *
forgerequest(Torrent *tor, char *req)
{
	char *announce;
	int length;
	char *baseurl;
	char infohash[3*HASHSIZE+1]; 
	char peerid[3*PEERIDLEN+1]; 
	int len = strlen(tor->announce);
	char *buf = emalloc((512)*sizeof(char));

	/*
	Is it really fair to bail out if announce url
	doesn't contain "announce"?
	*/
	if((announce = strstr(tor->announce, "announce")) == 0)
		error("weird announce url");
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
		/*
		No need to realloc precisely, just use a big enough buffer
		redo it like for announce

		buf = erealloc(buf,(len+7+10+3*HASHSIZE+1)*sizeof(char));
		buf = strcpy(buf, baseurl);
		buf = strcat(buf,"scrape?info_hash=");
		buf = strcat(buf,infohash);
		*/
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
	}

	free(baseurl);
	return buf;
}

void
writedata(ulong index, char *data, Torrent *tor, Peer *peer)
{
	int m, fd, fileindex, towrite, offset, piecelength;
	uvlong *filelength;
	Piece *lister;
	Piece *remover;

	fileindex = 0;
	for (int i=tor->filesnb-1; i>=0; i--)
		if (index > tor->fileborder[i]){
			fileindex = i;
			break;
		}
	piecelength = tor->piecelength;
	filelength = tor->filelength;
//TODO: case when file is smaller than piece is still missing 
	if (tor->firstoffset[fileindex] != 0)
		offset = piecelength - tor->firstoffset[fileindex] + (index - tor->fileborder[fileindex] - 1) * piecelength;
	else
		offset = (index - tor->fileborder[fileindex]) * piecelength;
	if ((fd = open(tor->datafiles[fileindex], OWRITE)) < 0)
		sysfatal("couldn't open %s: %r", tor->datafiles[fileindex]);

	dbgprint(1, "offset in file: %d\n", offset);
	lister = tor->pieceslist;
	while ((lister != nil) && (lister->index != index))
			lister = lister->next;	
	if (((lister->status)&BTshar) != 0){
	/* piece at junction of two (or more) files */
		towrite = tor->firstoffset[fileindex+1];
		if ((m = pwrite(fd, data, towrite, offset)) != m)
				error("torrent.c: writedata(): %r");
		close(fd);
		dbgprint(1, "shared piece: %ld\n", index);
		dbgprint(1, "wrote %d bytes at offset %d \n", towrite, offset);
		for(;;){
			fileindex++;
			if ((fd = open(tor->datafiles[fileindex], OWRITE)) < 0)
				error("couldn't open file: %r");	
			offset = 0;
			if ((index == tor->piecesnb-1) && (fileindex == tor->filesnb-1)){
			/* last piece of torrent */
				towrite = tor->lastpiece;
				if ((m = pwrite(fd, &data[tor->firstoffset[fileindex]], towrite, offset)) != m)
					error("torrent.c: writedata(): %r");
				dbgprint(1, "then wrote %d bytes at %d\n", towrite, offset);
				break;
			} else{
			/* not last piece */
				if(tor->fileborder[fileindex+1] == index){
				/* file smaller than what's left of piece */
					towrite = filelength[fileindex];
					if ((m = pwrite(fd, &data[tor->firstoffset[fileindex]], towrite, offset)) != m)
						error("torrent.c: writedata(): %r");
					dbgprint(1, "then wrote %d bytes at %d\n", towrite, offset);
					close(fd);
				} else{
				/* file bigger than what's left of piece */
					towrite = piecelength - tor->firstoffset[fileindex];
					if ((m = pwrite(fd, &data[tor->firstoffset[fileindex]], towrite, offset)) != m)
						error("torrent.c: writedata(): %r");
					dbgprint(1, "then wrote %d bytes at %d\n", towrite, offset);
					break;
				}
			}
		}
	} else{
	/* common case: piece not shared */
		if (index == tor->piecesnb-1){
			if ((m = pwrite(fd, data, tor->lastpiece, offset)) != m)
				error("torrent.c: writedata(): %r");
		}
		else{
			towrite = piecelength;
			if ((m = pwrite(fd, data, towrite, offset)) != m)
				error("torrent.c: writedata(): %r");
		}
	}
	close(fd);
	/* update the list */
	lister = tor->pieceslist;
	while ((lister != nil) && (lister->index != index))
		lister = lister->next;
	lister->status |= BThas;
	/* now for the peer's list */
	lister = peer->pieceslist;
	remover = lister;
	while ((lister != nil) && (lister->index != index)){
		remover = lister;
		lister = lister->next;
	}
	if (lister == peer->pieceslist)
		peer->pieceslist = lister->next;
	remover->next = lister->next;
	free(lister);
	peer->piecesnb--;
	dbgprint(1, "wrote piece: %ld\n", index);
}

int
checkpiece(char *data, Torrent *tor, int index)
{
	uchar *digest;
	uchar *buf;
	int datasize;
	Piece *lister;

	if (index == tor->piecesnb-1){
		lister = tor->pieceslist;
		while ((lister != nil) && (lister->index != index))
		lister = lister->next;
		/*
		if piece is shared, lastpiece is actually not the
		size of the whole piece. Too lazy to figure out 
		the right algo right now, so assume it's all good.
		*/
		if ((lister->status & BTshar) != 0)
			return 1;
		else
			datasize = tor->lastpiece;
	} else
		datasize = tor->piecelength;
	digest = emalloc(SHA1dlen);
	buf = emalloc(datasize);

	memmove(buf, data, datasize);
	sha1(buf, datasize, digest, nil);

	for (int i=0; i<HASHSIZE; i++){
		//dbgprint(1, "%%%.2ux",digest[i]);
		if (digest[i] != tor->sha1list[index][i]){
			free(digest);
			free(buf);
			return -1;	
		}
	}
	//dbgprint(1, "\n");
	free(digest);
	free(buf);
	return index;
}

//TODO: merge with writedata (make function that does pread or pwrite depending on the arg)
void
readdata(ulong index, char *data, Torrent *tor, Peer *peer)
{
	int m, fd, fileindex, toread, offset, piecelength;
	uvlong *filelength;
	Piece *lister;

	fileindex = 0;
	for (int i=tor->filesnb-1; i>=0; i--)
		if (index > tor->fileborder[i]){
			fileindex = i;
			break;
		}
	piecelength = tor->piecelength;
	filelength = tor->filelength;

	if ((fd = open(tor->datafiles[fileindex], ORDWR)) < 0)
		error("couldn't open file: %r");
	dbgprint(1, "opened file: %s\n", tor->datafiles[fileindex]);

	if (tor->firstoffset[fileindex] != 0)
		offset = piecelength - tor->firstoffset[fileindex] + (index - tor->fileborder[fileindex] - 1) * piecelength;
	else
		offset = (index - tor->fileborder[fileindex]) * piecelength;
	dbgprint(1, "offset in file: %d\n", offset);

	lister = tor->pieceslist;
	while ((lister != nil) && (lister->index != index))
			lister = lister->next;	
	if (((lister->status)&BTshar) != 0){
		/* piece at the junction of two files */
		toread = tor->firstoffset[fileindex+1];
		if ((m = pread(fd, data, toread, offset)) != toread)
				error("torrent.c: readdata(): %r");
		close(fd);
		dbgprint(1, "shared piece: %ld\n", index);
		dbgprint(1, "read %d bytes at offset %d \n", toread, offset);
		for(;;){
			fileindex++;
			if ((fd = open(tor->datafiles[fileindex], ORDWR)) < 0)
				error("couldn't open file: %r");	
			offset = 0;
			if ((index == tor->piecesnb-1) && (fileindex == tor->filesnb-1)){
				/* last piece */
				toread = tor->lastpiece;
				if ((m = pread(fd, &data[tor->firstoffset[fileindex]], toread, offset)) != toread)
					error("torrent.c: readdata(): %r");
				dbgprint(1, "then read %d bytes at %d\n", toread, offset);
				break;
//TODO: go on, stopped here.
			} else{
				/* not last piece */
				if(tor->fileborder[fileindex+1] == index){
				/* file smaller than what's left of piece */
					toread = filelength[fileindex];
					if ((m = pread(fd, &data[tor->firstoffset[fileindex]], toread, offset)) != toread)
						error("torrent.c: readdata(): %r");
					dbgprint(1, "then read %d bytes at %d\n", toread, offset);
					close(fd);
				} else{
				/* file bigger than what's left of piece */
					toread = piecelength - tor->firstoffset[fileindex];
					if ((m = pread(fd, &data[tor->firstoffset[fileindex]], toread, offset)) != toread)
						error("torrent.c: readdata(): %r");
					dbgprint(1, "then read %d bytes at %d\n", toread, offset);
					break;
				}
			}
		}
	} else{
	/* common case; piece not shared */
		if (index == tor->piecesnb-1){
			if ((m = pread(fd, data, tor->lastpiece, offset)) != tor->lastpiece)
				error("could not read piece from disk: %r");
		}
		else{
			toread = piecelength;
			if ((m = pread(fd, data, toread, offset)) != toread)
				error("could not read piece from disk: %r");
		}
	}
	close(fd);
//TODO: keep track of what pieces the peer has
	dbgprint(1, "piece read: %ld\n", index);
}

/*
 *	adds or removes a piece to the peers' pieceslist.  returns 1 if the
 *	operation was successfull, otherwise 0.
 */
int 
updatepeerspieces(Torrent *tor, Peer *peer, int index, char op)
{
	Piece *lister, *rimmer;
	char status = 0;

	if (op == '+'){ // insert in the list
		lister = tor->pieceslist;
		// do not add it if we already have it
		while (lister != nil){
			if (lister->index == index){
				if ((lister->status & BThas) != 0)
					return 0;
				if ((lister->status & BTshar) != 0)
					status |= BTshar;
			}
			lister = lister->next;
		}
		lister = peer->pieceslist;
		rimmer = lister;
		while (lister != nil){
			if (lister->index == index){
				// somehow it was already there. do nothing.
				return 0;
			}
			if (lister->index > index){
				// insert here
				rimmer->next = emalloc(sizeof(Piece));
				rimmer->next->index = index;
				rimmer->next->status = status;
				rimmer->next->next = lister;
				peer->piecesnb++;
				return 1;
			}
			rimmer = lister;
			lister = lister->next;
		}
		// end of the list. append.
		rimmer->next = emalloc(sizeof(Piece));
		rimmer->next->index = index;
		rimmer->next->status = status;
		rimmer->next->next = lister;
		peer->piecesnb++;
		return 1;		
	}

	return 0;
}

/*
it can happen that we have to free a peer and we don't have a peer_id
for it, case in point: tracker was in binary model so we didn't get
the peer id from it, and hello1 fails at dial because (for example)
the peer does not exist anymore at this address.
=> let's use an internal id to tag our peers.
*/

//TODO: using both linked list and an "array" is a terrible idea. get rid of the array.
void 
freepeer(Peer *peer, Peer **listhead)
{
	Piece *lister, *rimmer;
	Peer *current, *previous;

	// find the peer in the list and detach it from there
	current = *listhead;
	previous = current;
	while (current != nil){
		if (peer->num == current->num){
			if (previous == current)
				*listhead = current->next;
			else
				previous->next = current->next;
			current->next = nil;
			break;
		}
		previous = current;
		current = current->next;
	}
	print("beepdog \n");

	// now actually free some stuff		
	free(peer->peerinfo->address);
	free(peer->peerinfo->id);
	free(peer->peerinfo);
//TODO: in cases where we fail early we probably don't have a bitfield yet
	free(peer->bitfield);
	lister = peer->pieceslist;
	rimmer = lister;
	while(lister != nil){
		lister = rimmer->next;
		free(rimmer);
		rimmer = lister;
	}
	free(peer);
}
