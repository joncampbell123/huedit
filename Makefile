
all:
	make -C hackipedia

clean:
	make -C hackipedia clean

distclean: clean
	@find -name \*~ -delete
	make -C hackipedia/software/huedit distclean

