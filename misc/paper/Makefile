STSHEET ?= chunk
STSHEETDIR := /usr/share/xml/docbook/stylesheet/nwalsh/html

all	:	index.html paper.html
	ruby -pi -e '$$_.gsub!("@","&#64;")' *.html

index.html	:	paper.xml
	xmlto html $<

paper.html	:	paper.xml
	xmlto html-nochunks $<

upload	:
	rsync --exclude .svn -rptve ssh *.html *.png *.svg math www-uploader@moria:/var/www/zsync/paper

clean	:
	rm *.html
