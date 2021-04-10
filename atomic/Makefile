all:
	$(CC) -o atomic thread_pi.c main.c -g -lpthread -lm

test:
	number=1 ; while [ $$number -le 1000 ] ; do\
	       echo $$number ;  ./atomic ;\
	       number=$$((number + 1)) ;\
	done

massif:
	valgrind --tool=massif ./atomic

perf:
	sudo perf stat --repeat 10000 -e cache-misses,cache-references,instructions,cycles ./atomic

clean:
	rm -rf atomic
