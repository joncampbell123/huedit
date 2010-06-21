PDFS=$(shell ls *.latex | sed 's/\.latex/.pdf/')
DVIS=$(shell ls *.latex | sed 's/\.latex/.dvi/')
PSS=$(shell ls *.latex | sed 's/\.latex/.ps/')

all: $(PDFS) $(DVIS) $(PSS)

%.ps : %.dvi
	@echo Building $@
	@dvips -o $@ $< >$@.err 2>&1

%.pdf : %.latex
	@echo Building $@
	@mkdir -p $@.tmp
	@pdflatex --output-directory=$@.tmp $< >$@.err 2>&1 </dev/null
	@cp -l $@.tmp/$@ $@ || (cat $@.err && false)

%.dvi : %.latex
	@echo Building $@
	@mkdir -p $@.tmp
	@latex --output-directory=$@.tmp $< >$@.err 2>&1
	@cp -l $@.tmp/$@ $@

clean:
	@rm -Rf *.{pdf,dvi,ps}{,.tmp,.err}

distclean: clean
	@find -name \*~ -delete
