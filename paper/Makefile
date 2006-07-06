STSHEET ?= chunk
STSHEETDIR := /usr/share/xml/docbook/stylesheet/nwalsh/html

index.html	:	paper.xml
	xsltproc $(STSHEETDIR)/$(STSHEET).xsl ${.ALLSRC}
	perl -pi -e 's/\@/&#64;/g' index.html

upload	:
	rsync --exclude .svn -rptve ssh *.html *.png *.svg math moria:/var/www/zsync/paper

clean	:
	rm *.html
