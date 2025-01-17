all:
	$(CC) -o bpp thread_pi.c main.c -g -lpthread -lm

one:
	./bpp


test:
	number=1 ; while [ $$number -le 1000 ] ; do\
	       ./bpp; echo $$number ;\
	       number=$$((number + 1)) ;\
	done

massif:
	valgrind --tool=massif ./bpp

perf:
	sudo perf stat --repeat 10000 -e cache-misses,cache-references,instructions,cycles ./bpp

clean:
	rm -rf bpp
