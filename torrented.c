#include <u.h>
#include <libc.h>

void
main(int argc, char *argv[])
{
	int fdr, fdw, lenr, lenw;
	char buf[1];
	char ori[] = "40:http://tracker.thepiratebay.org/announce";
	char repl[] = "34:http://bittrk.appspot.com/announce";
	int n;
	
	lenr = strlen(ori);
	lenw = strlen(repl);
	fdr = open("/usr/glenda/ebooks.0.torrent", OREAD);
	fdw = create("/usr/glenda/ebooks.torrent", OWRITE, 0755);

	for(int i=0; i<11; i++){
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
