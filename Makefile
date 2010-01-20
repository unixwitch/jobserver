SUBDIRS	= jobserverd logwriter job

default: all

all clean install:
	@for d in $(SUBDIRS); do \
		echo "$@ ==> $$d"; \
		cd "$$d" && $(MAKE) "$@" || exit 1; \
		cd ..; \
		echo "$@ <== $$d"; \
	done
