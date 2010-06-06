#include <u.h>
#include <libc.h>
#include <bio.h>
#include <libsec.h>
#include <pool.h>
#include "misc.h"
#include "torrent.h"
#include "torrentfile.h"

static int nkeys = 25 ;

static Rune *keystab[] = {
	L"announce",
	L"announce-list",
	L"info",
	L"creation date",
	L"comment",
	L"created by",
	L"encoding",
	L"pieces",
	L"piece length",
	L"private",
	L"name", L"name.utf-8",
	L"length",
	L"md5sum",
	L"files",
	L"path", L"path.utf-8",
	L"codepage",
	L"publisher", L"publisher.utf-8", L"publisher-url", L"publisher-url.utf-8",
	L"nodes",
	L"httpseeds",
	L"unknown",
};

static Bestring *
getbestr(long toread, Biobuf *bin){
	Bestring *bestr;
	int i,j;

	bestr = emalloc(sizeof(Bestring));
	bestr->value = emalloc((toread + 1)*sizeof(Rune));
	i = 0;
	j = 0;
	while(j<toread) {
		if ((bestr->value[i] = Bgetrune(bin)) <= 0) {
			werrstr("pb while filling Bestring from torrent file");
			free(bestr);
			return nil;
		}
		j+=runelen(bestr->value[i]);
		i++;
	}
	bestr->value[i]='\0';
	bestr->length=i;
	return bestr;
}

static uvlong 
readbignumber(Biobuf *bin, Rune stop)
{
	uvlong val;
	long nr;

	val = 0;
	while ((nr = Bgetrune(bin)) != stop)
	{
		if (nr == 'e'){
			/* end of metainfo; should be safe to use 0
			 since no key should be of zero length */
			return 0;
		}
		if (nr <= 0) 
			sysfatal("unexpected eof in readbignumber");
	val = 10 * val + (nr - 48);	
	}
	return val;
}

static long 
readnumber(Biobuf *bin, Rune stop)
{
	return (long)readbignumber(bin, stop);
}

static int
getkey(Biobuf *bin)
{
	uint length;
	Bestring *key;
	int keytype;

	// default is "unknown"
	keytype = nkeys-1;	
	if ((length = readnumber(bin,':')) == 0) 
		return -1;
	key = getbestr(length, bin);
	for (int i = 0; i < nkeys; i++) {
		if (runestrcmp(key->value, keystab[i]) == 0) {
			keytype=i;
			dbgprint(1, "key found: %S\n", keystab[i]);
			break;
		}
	}
	if (keytype == nkeys-1)
		dbgprint(1, "unknown key, skipping.\n");
	free(key->value);
	free(key);
	return keytype;
}

uchar *getinfohash(char *file, int toread)
{
	Biobuf *bin;
	int nr;
	int pos;
	Rune str[6 * sizeof(Rune)]; 
	Rune key[] = L":info";
	uchar *digest = emalloc(SHA1dlen);
	uchar *buf;

	if((bin = Bopen(file, OREAD)) == nil){
		werrstr("failed to open file");
		return nil;
	}
	if ((nr = Bgetrune(bin)) != 'd'){
		werrstr("Not a .torrent file");
		return nil;
	}		
	while ((nr = Bgetrune(bin)) > 0){ 
		if (nr == '4'){
			pos = Boffset(bin);
			for (int i = 0; i<5; i++) 
				str[i] = Bgetrune(bin);
			str[5]='\0';
			if (runestrcmp(str,key) == 0){
				pos = Boffset(bin);
				break;
			}
			Bseek(bin, pos, 0);
		}
	}

	buf = emalloc(toread * sizeof(uchar));
	nr = Bread(bin, buf, toread);
	sha1(buf, toread, digest, nil);
	/*
	for (int i = 0; i<20; i++)
		print("%.2ux", digest[i]);
	print("\n");
	*/

	free(buf);
	Bterm(bin);
	return digest;
}

static int
getelement(int keytype, Biobuf *bin, Torrent *tor)
{
	long hint;
	long length;
	Bestring *bestr = nil;
//	Rune **annlist = nil;
	char **annlist = nil;
	int listsize;
	int newkeytype;
	Rune *buf = nil;

	switch(keytype){
	case BTannounceurl:
		length = readnumber(bin,':');
		bestr = getbestr(length, bin);
		tor->announce = emalloc((length+1)*sizeof(char));
		for (int i=0; i<length+1; i++) 
			runetochar(&(tor->announce[i]),&(bestr->value[i]));
		dbgprint(1, "%s \n", tor->announce);
		free(bestr->value);
		free(bestr);
		break;
	case BTannouncelist:
		listsize = 0;
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'l'){
			print("pb with list prefix\n");
			return -1;
		}
		while ((hint = Bgetrune(bin)) != 'e'){
			if (hint != 'l'){
				print("pb with list prefix\n");
				return -1;
			}
			while ((hint = Bgetrune(bin)) != 'e'){
				// we can have several urls in the same list
				if (hint != 'l')
					Bungetrune(bin);
				length = readnumber(bin,':');
				bestr = getbestr(length, bin);
				listsize++;
				//annlist = erealloc(annlist, listsize * sizeof(Rune *));
				annlist = erealloc(annlist, listsize * sizeof(char *));
				annlist[listsize - 1] = emalloc((length+1)*sizeof(char));
				for (int i=0; i<length+1; i++) 
					runetochar(&(annlist[listsize - 1][i]),&(bestr->value[i]));
				free(bestr->value);
				free(bestr);
			}
		}
		tor->announcelist = annlist;
		tor->annlistsize = listsize;
		// since we have the list, we'll set announce with it instead.
		if (listsize > 0)
			free(tor->announce);
		break;
	case BTcomment:
		length = readnumber(bin,':');
		bestr = getbestr(length, bin);
		tor->comment=bestr->value;
		dbgprint(1, "%S \n", tor->comment);
		free(bestr);
		break;
	case BTcreatedby:
		length = readnumber(bin,':');
		bestr = getbestr(length, bin);
		tor->createdby=bestr->value;
		dbgprint(1, "%S \n", tor->createdby);
		free(bestr);
		break;
	case BTcreationdate:
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'i'){
			print("pb with integer prefix\n");
			return -1;
		}
		tor->creationdate = readbignumber(bin,'e');
		dbgprint(1, "%d \n", tor->creationdate);
		break;
	case BTencoding:
		length = readnumber(bin,':');
		bestr = getbestr(length, bin);
		tor->encoding=bestr->value;
		dbgprint(1, "%S \n", tor->encoding);
		free(bestr);
		break;
	case BTinfo:
		tor->infosize = Boffset(bin);
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'd'){
			print("pb with info dict prefix\n");
			return -1;
		}
		while ((hint = Bgetrune(bin)) != 'e'){
			Bungetrune(bin);
			newkeytype = getkey(bin);	
			getelement(newkeytype, bin, tor);
		}
		tor->infosize = Boffset(bin) - tor->infosize;
		break;
	case BTfiles:
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'l'){
			print("pb with files list prefix\n");
			return -1;
		}
		tor->multifile = 1;
		tor->filesnb = 0;
		while ((hint = Bgetrune(bin)) == 'd'){
			tor->filesnb++;
			while ((hint = Bgetrune(bin)) <= 0 || hint != 'e'){
				Bungetrune(bin);
			//for (int i=0; i<3; i++) {
				newkeytype = getkey(bin);	
				getelement(newkeytype, bin, tor);
			}
			/*
			if ((hint = Bgetrune(bin)) <= 0 || hint != 'e'){
				print("pb with files dict suffix\n");
				return -1;
			}
			*/
		}
		if (hint <= 0 || hint != 'e'){
			print("pb with files list suffix\n");
			return -1;
		}		
		break;	
	case BTlength:
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'i'){
			print("pb with length integer prefix\n");
			return -1;
		}
		tor->filelength = erealloc(tor->filelength, tor->filesnb * sizeof(uvlong));
		tor->filelength[tor->filesnb-1] = readbignumber(bin,'e');	
		dbgprint(1, "%lld \n", tor->filelength[tor->filesnb-1]);
		break;
	case BTmd5sum:
		tor->filemd5sum = erealloc(tor->filemd5sum, tor->filesnb*sizeof(Rune *));
		length = readnumber(bin,':');
		bestr = getbestr(length, bin);
		tor->filemd5sum[tor->filesnb-1]=bestr->value;
		dbgprint(1, "%S \n", tor->filemd5sum[tor->filesnb-1]);
		free(bestr);
		break;
	case BTpath:
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'l'){
			print("pb with path list prefix\n");
			return -1;
		}	
		tor->filepath = erealloc(tor->filepath, tor->filesnb * sizeof(Rune *));
		tor->filepath[tor->filesnb-1] = emalloc(sizeof(Rune));
		buf = tor->filepath[tor->filesnb-1];
		buf[0] = '\0';
		while ((hint = Bgetrune(bin)) != 'e'){
			Bungetrune(bin);
			length = readnumber(bin,':');
			bestr = getbestr(length, bin);
			length = runestrlen(buf);
			buf = erealloc(buf,(length+bestr->length+2)*sizeof(Rune));
			if (length != 0){
				buf[length] = '/';
				buf[length+1] = '\0';
			}
			buf = runestrcat(buf, bestr->value); 
			free(bestr->value);
			free(bestr); 
		}
		tor->filepath[tor->filesnb-1] = buf;
		dbgprint(1, "%S \n", tor->filepath[tor->filesnb-1]);
		break;
	case BTutf8path:
		free(tor->filepath[tor->filesnb-1]);
		getelement(BTpath, bin, tor);
		break;
	case BTname:
		length = readnumber(bin,':');
		bestr = getbestr(length, bin);
		tor->name = bestr->value;
		dbgprint(1, "%S \n", tor->name);
		free(bestr);
		if (tor->multifile != 1){
			tor->filepath = emalloc(sizeof(Rune *));
			tor->filepath[tor->filesnb-1] = tor->name;
		}
		break;
	case BTutf8name:
		free(tor->name);
		getelement(BTname, bin, tor);
		break;
	case BTpiecelength:
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'i'){
			print("pb with piecelength integer prefix\n");
			return -1;
		}
		tor->piecelength=readnumber(bin,'e');
		dbgprint(1, "%d \n", tor->piecelength);
		break;		
	case BTpieces:
		tor->sha1list = nil;
		length = readnumber(bin,':');
		listsize = length / HASHSIZE; 
		tor->sha1list = emalloc(listsize * sizeof(uchar *));
		for (int i=0; i < listsize; i++){
			//tor->sha1list[i] = emalloc(50);
			tor->sha1list[i] = emalloc(HASHSIZE * sizeof(uchar));
			if ((hint = Bread(bin, tor->sha1list[i], HASHSIZE* sizeof(uchar))) <=0 ){
				print("pb at %d\n", i);
				sysfatal("pb while reading hashes\n");
			}
		}
		tor->piecesnb = listsize;
		dbgprint(1, "%d \n", tor->piecesnb);
		break;
	case BTprivate:
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'i'){
			print("pb with private integer prefix\n");
			return -1;
		}
		tor->private=readnumber(bin,'e');
		break;
	case BTcodepage:
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'i')
			error("pb with integer prefix\n");
		dbgprint(1, "%d \n", readnumber(bin,'e'));	
		break;
	case BTpublisher:
		length = readnumber(bin,':');
		bestr = getbestr(length, bin);
		dbgprint(1, "%S \n", bestr->value);	
		free(bestr);
		break;
	case BTutf8publisher:
		getelement(BTpublisher, bin, tor);
		break;
	case BTpublisherurl:
		length = readnumber(bin,':');
		bestr = getbestr(length, bin);
		dbgprint(1, "%S \n", bestr->value);	
		free(bestr);
		break;
	case BTutf8publisherurl:
		getelement(BTpublisherurl, bin, tor);
		break;
	case BTnodes:
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'l')
			error("pb with list prefix\n");
		while ((hint = Bgetrune(bin)) == 'l'){
			length = readnumber(bin,':');
			bestr = getbestr(length, bin);
			dbgprint(1, "%S \n", bestr->value);	
			free(bestr);
			if ((hint = Bgetrune(bin)) <= 0 || hint != 'i')
				error("pb with integer prefix\n");
			dbgprint(1, "%d \n", readnumber(bin,'e'));
			if ((hint = Bgetrune(bin)) != 'e')
				error("e was expected at end of list\n");
		}
		if (hint != 'e')
			error("e was expected at end of list\n");
		break;
	case BThttpseeds:
		if ((hint = Bgetrune(bin)) <= 0 || hint != 'l')
			error("pb with list prefix\n");
		for(;;){
			getelement(BTunknown, bin, tor);
			if ((hint = Bgetrune(bin)) == 'e')
				break;
			else
				Bungetrune(bin);
		}
		break;	
	case BTunknown:
		/*
		try and skip to the next element because we just
		do not care. yay for recursion!
		*/
		hint = Bgetrune(bin);
		switch(hint){
		case 'i':
			length = readnumber(bin,'e');
			dbgprint(1, "integer value: %d\n", length);
			break;
		case 'l':
			for(;;){
				getelement(BTunknown, bin, tor);
				if ((hint = Bgetrune(bin)) == 'e'){
					dbgprint(1, "end of unknown list\n");
					break;
				}
				else
					Bungetrune(bin);
			}
			break;
		case 'd':
			for(;;){ 
				if ((keytype = getkey(bin)) == -1)
					/* end of metainfo */
					break;
				getelement(keytype, bin, tor);
			}
			break;
		default:
			Bungetrune(bin);
			length = readnumber(bin,':');
			bestr = getbestr(length, bin);
			dbgprint(1, "%S\n", bestr->value);
			free(bestr->value);
			free(bestr);
			break;
		}
		break;
	default:
		error("getelement should never reach that case.");
	}	
}

void 
parsebtfile (char *file, Torrent *tor)
{
	Biobuf *bin;
	long nr;
	tor->filesnb = 1;
	tor->multifile = 0;
	tor->filepath = nil;
	tor->filemd5sum = nil;
	tor->filelength = nil;
	int keytype = 0;

	if((bin = Bopen(file, OREAD)) == nil)
		error("failed to open file");

	if ((nr = Bgetrune(bin)) != 'd')
		error("Not a .torrent file");

	for (;;) 
	{ 
		if ((keytype = getkey(bin)) == -1)
			/* end of metainfo */
			break;
		getelement(keytype, bin, tor);
	}
	Bterm(bin);
}
