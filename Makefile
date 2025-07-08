CC = gcc
CFLAGS = -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include \
         -Wall -Wextra -O3 -g -march=native -mtune=native \
         -msse4.2 -mavx2 -flto -ffast-math -funroll-loops \
         -finline-functions -fomit-frame-pointer \
         -DNDEBUG -D_GNU_SOURCE
LDFLAGS = -pthread -lglib-2.0 -lnuma -flto

TARGET = server
SOURCES = main.c worker.c connection.c http_handler.c timer.c \
          worker_optimized.c lockfree_pool.c
OBJECTS = $(SOURCES:.c=.o)
HEADERS = connection.h worker.h http_handler.h timer.h \
          simd_utils.h lockfree_pool.h

.PHONY: all clean debug profile benchmark install

all: $(TARGET)

debug: CFLAGS = -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include \
                -Wall -Wextra -O0 -g3 -fsanitize=address -fsanitize=undefined \
                -D_GNU_SOURCE -DDEBUG
debug: LDFLAGS = -pthread -lglib-2.0 -lnuma -fsanitize=address -fsanitize=undefined
debug: $(TARGET)

profile: CFLAGS = -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include \
                  -Wall -Wextra -O2 -g -pg -march=native -D_GNU_SOURCE -DPROFILE
profile: LDFLAGS = -pthread -lglib-2.0 -lnuma -pg
profile: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

benchmark: $(TARGET)
	@echo "Running performance benchmarks..."
	@./benchmark.sh

install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/
	sudo setcap cap_net_bind_service=+ep /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJECTS) gmon.out *.gcov *.gcda *.gcno
