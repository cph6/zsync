
index.html	:	paper.xml
	xsltproc /usr/share/xml/docbook/stylesheet/nwalsh/html/chunk.xsl $<
	perl -pi -e 's/\@/&#64;/g' index.html
