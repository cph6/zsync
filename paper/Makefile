
index.html	:	paper.xml
	xsltproc /usr/local/share/xsl/docbook/xhtml/chunk.xsl ${.ALLSRC}
	perl -pi -e 's/\@/&#64;/g' index.html

upload	:
	rsync --exclude .svn -rptve ssh *.html *.png *.svg math moria:/var/www/zsync/paper

