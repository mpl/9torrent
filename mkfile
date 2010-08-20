</$objtype/mkfile

OFILES=\
	misc.$O			\
	pieces.$O		\
	torrentfile.$O	\
	tracker.$O		\
	talk.$O 		\
	peers.$O		\
	fs.$O	 		\
	main.$O		

default:V:
	mk tidy
	mk btfs

%.$O:	%.c 
		$CC $CFLAGS $stem.c

btfs:	$OFILES
		$LD $LDFLAGS -o btfs $OFILES

clean:V:
	rm *.$O
	rm btfs

tidy:V:
	broke | rc
	kill btfs | rc

torrented:V:
	$CC $CFLAGS torrented.c
	$LD $LDFLAGS -o torrented torrented.$O

ps:	btfs.ms
	troff -ms -o1-3 btfs.ms | lp -dstdout > /tmp/btfs.ps

pdf:	/tmp/btfs.ps
		cat /sys/doc/docfonts /tmp/btfs.ps > /tmp/_btfs.ps
		ps2pdf /tmp/_btfs.ps /tmp/btfs.pdf
		
todo:V:
	echo 'repeat call tracker; manage callees; fix mem/chan frees'

peers:V:
	dircp /usr/glenda/seeder/ /usr/glenda/leecher1/
	cd /usr/glenda/leecher1/; rm -rf kryptonite.flv portal/
	dircp /usr/glenda/seeder/ /usr/glenda/leecher2/
	cd /usr/glenda/leecher2/; rm -rf funky portal/
	dircp /usr/glenda/seeder/ /usr/glenda/leecher3/
	cd /usr/glenda/leecher3/; rm -rf funky kryptonite.flv

