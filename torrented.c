#include <u.h>
#include <libc.h>

void
main(int argc, char *argv[])
{
	int fdr, fdw, lenr, lenw, pos;
	char buf[1];
	char ori[] = "45:http://tracker.openbittorrent.com:80/announce13:announce-listll45:http://tracker.openbittorrent.com:80/announce44:udp://tracker.openbittorrent.com:80/announceel39:http://tracker.publicbt.com:80/announce38:udp://tracker.publicbt.com:80/announceee";
	char repl[] = "35:http://smgl.fr.eu.org:6969/announce";
	int n;
	
	pos = 11;
	lenr = strlen(ori);
	lenw = strlen(repl);
	fdr = open("/usr/glenda/cite.torrent", OREAD);
	fdw = create("/usr/glenda/cite2.torrent", OWRITE, 0755);

	for(int i=0; i<pos; i++){
		n = read(fdr, buf, 1);
		write(fdw, buf, 1);
	}

	for(int i=0; i<lenr; i++){
		n = read(fdr, buf, 1);
	}
	write(fdw, repl, lenw);

	while(n > 0){
		n = read(fdr, buf, 1);
		write(fdw, buf, 1);
	}
	close(fdr);
	close(fdw);
	exits(0);
}
