CFLAGS += -I../..

LDFLAGS += -L../../framework -lmlt -lpthread -lrt

include ../../../config.mak

TARGET = ../libmltplaycast$(LIBSUF)

OBJS = factory.o producer_pcingest.o

SRCS := $(OBJS:.o=.c)

all: 	$(TARGET)

$(TARGET): $(OBJS)
		$(CC) $(SHFLAGS) -o $@ $(OBJS) $(LDFLAGS)

depend:	$(SRCS)
		$(CC) -MM $(CFLAGS) $^ 1>.depend

distclean:	clean
		rm -f .depend config.mak

clean:
		rm -f $(OBJS) $(TARGET)

install: all
	install -m 755 $(TARGET) "$(DESTDIR)$(moduledir)"
	install -d "$(DESTDIR)$(mltdatadir)/playcast"
	install -m 644 *.yml "$(DESTDIR)$(mltdatadir)/playcast"

ifneq ($(wildcard .depend),)
include .depend
endif
