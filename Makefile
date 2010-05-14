PDFS=$(shell ls *.latex | sed 's/\.latex/.pdf/')
DVIS=$(shell ls *.latex | sed 's/\.latex/.dvi/')
PSS=$(shell ls *.latex | sed 's/\.latex/.ps/')

all: $(PDFS) $(DVIS) $(PSS)

%.ps : %.dvi
	dvips -o $@ $<

%.pdf : %.latex
	mkdir -p $@.tmp
	pdflatex --output-directory=$@.tmp $<
	cp -lv $@.tmp/$@ $@

%.dvi : %.latex
	mkdir -p $@.tmp
	latex --output-directory=$@.tmp $<
	cp -lv $@.tmp/$@ $@

clean:
	rm -Rf *.pdf *.pdf.tmp *.dvi *.dvi.tmp *.ps *.ps.tmp

distclean: clean
	find -name \*~ -delete
