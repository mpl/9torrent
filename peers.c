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

extern int maxpeers;
extern char *datadir;
QLock l;

/*
it can happen that we have to free a peer and we don't have a peer_id
for it, case in point: tracker was in binary model so we didn't get
the peer id from it, and hello1 fails at dial because (for example)
the peer does not exist anymore at this address.
=> let's use an internal id to tag our peers.
*/

static void
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
	freepeer(params->peer, &(params->tor->p_callers));
	params->tor->p_callersnb--;
	chanfree(params->c);
	free(params);
	print("callee [%d] terminated\n", threadid());
}

void
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

static void
caller(void *arg)
{
	struct Params{ Torrent *tor; Peer *peer; Channel *c;} *params;
	uchar chanmsg[1];

	params = arg;
	print("caller [%d] starting\n", threadid());
	chatpeer(params->tor, params->peer, params->c, 1);
	freepeer(params->peer, &(params->tor->p_callees));
//TODO: freeing the chan makes next call to alt() fail. why? Not really a pb in itself tho, as I can/will reuse those channels. But it might be a hint at something amiss.
	params->tor->p_calleesnb--;
	chanmsg[0] = 7;
	send(params->c, chanmsg);
	chanfree(params->c);
	free(params);
	print("caller [%d] terminated\n", threadid());
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
	struct Params2{Torrent *tor; char *reqtype; Channel *c;} *params2;
	struct Params{Torrent *tor; Peer *peer; Channel *c;} *params;
	static int num = 0;
	int lastcalled = 0;
	int wait = 0;

	// first proc is for the tracker caller
	a = emalloc(sizeof(Alt));
	a[0].v = m;
	a[0].c = chancreate(sizeof(m), 0);
	a[0].op = CHANRCV;
	params2 = emalloc(sizeof(struct Params));
	params2->tor = tor;
	params2->reqtype = smprint("announce");
	params2->c = a[0].c;
	proccreate(poketrackers, params2, STACK);
	dbgprint(1, "tracker thread started\n", i);	
	// wait for the first call to the tracker to finish
	recv(a[0].c, m);
	while (m[0] != 1)
		recv(a[0].c, m);

	// now for the callers
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
			a = erealloc(a, (i+2)*sizeof(Alt));
			a[i+1].v = m;
			a[i+1].c = chancreate(sizeof(m), 0);
			a[i+1].op = CHANRCV;

			// prepare params for the thread
			params = emalloc(sizeof(struct Params));
			params->tor = tor;
			params->peer = peer;
			params->c = a[i+1].c;
			threadcreate(caller, params, STACK);
			dbgprint(1, "caller thread #%d started\n", i);
		}
		else {
			// keep track what peersinfo we were at, for later
			lastcalled = i;
			break;
		}
	}
	a = erealloc(a, (i+1)*sizeof(Alt));
	a[i].v = nil;
	a[i].c = nil;
	a[i].op = CHANEND;
	for(;;){
		n = alt(a);
		if((n<0) || (n>i))
			error("with alt");
		if (n == 0) {
		// message from teh tracker caller 
			if (m[0] == 0) {
				dbgprint(1, "tracker caller is working \n");
				wait = 1;
			} else {
				dbgprint(1, "tracker caller is done \n");
				wait = 0;
			}
		}
		if (m[0] == 7){
			// a caller has terminated.
			// update our bitfield
//TODO: a less barbaric way would be directly in scanpieces?
			qlock(&l);
			scanpieces(params->tor, datadir);
			qunlock(&l);

			// if tracker caller is not busy with peersinfo, 
			// try to contact a new one
			if (!wait) {
				if (tor->p_callersnb + tor->p_calleesnb < maxpeers){
					i = lastcalled;
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
		
		//TODO: reuse the "freed" entries instead of creating new ones
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
					lastcalled++;
					dbgprint(1, "caller thread #%d started\n", i);
				}
			} else {
//TODO: see later about that.
			}
		}
	}
	threadexits(0);	
}



